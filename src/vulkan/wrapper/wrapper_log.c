#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "wrapper_log.h"

/* Default output directory; override the log file with WRAPPER_LOG_FILE
 * (a path or "stdout"). */
#define WRAPPER_DIR                 "/sdcard/wrapper"
#define WRAPPER_SHADER_LOG_PATH     WRAPPER_DIR "/shaders"
#define WRAPPER_LOG_PATH            WRAPPER_DIR "/logs"
#define WRAPPER_VALIDATION_LOG_PATH WRAPPER_DIR "/validation"

struct wrapper_log_option {
   const char *name;
   uint64_t value;
};

static const struct wrapper_log_option wrapper_log_options[] = {
   { "info",       WRAPPER_LOG_INFO },
   { "error",      WRAPPER_LOG_ERROR },
   { "shader",     WRAPPER_LOG_SHADER },
   { "validation", WRAPPER_LOG_VALIDATION },
   { "bcn",        WRAPPER_LOG_BCN },
   { "apidump",    WRAPPER_LOG_APIDUMP },
   { NULL, 0 },
};

static uint64_t wrapper_log_mask;

static void
create_folder(const char *folder)
{
   struct stat sb;
   if (stat(folder, &sb) == 0 && S_ISDIR(sb.st_mode))
      return;

   mode_t old_mask = umask(0);
   mkdir(folder, 0770);
   umask(old_mask);
}

static void
get_formatted_date_time(char *buf, size_t length)
{
   time_t rawtime = time(NULL);
   struct tm tm;
   localtime_r(&rawtime, &tm);
   strftime(buf, length, "%F_%H-%M-%S", &tm);
}

const char *
get_executable_name(void)
{
   static char name[256];
   static int initialized;

   if (initialized)
      return name;
   initialized = 1;

   strcpy(name, "unknown");

   char cmdline[1024];
   int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return name;

   ssize_t len = read(fd, cmdline, sizeof(cmdline) - 1);
   close(fd);
   if (len <= 0)
      return name;
   cmdline[len] = '\0'; /* argv[0] ends at the first NUL anyway */

   const char *base = cmdline;
   const char *p = strrchr(base, '/');
   if (p)
      base = p + 1;
   p = strrchr(base, '\\');
   if (p)
      base = p + 1;

   if (*base) {
      strncpy(name, base, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
   }

   return name;
}

static uint64_t
get_debug_flag(const char *option)
{
   for (int i = 0; wrapper_log_options[i].name != NULL; i++) {
      if (!strcmp(wrapper_log_options[i].name, option))
         return wrapper_log_options[i].value;
   }

   return 0;
}

static void
parse_wrapper_debug_str(const char *env)
{
   wrapper_log_mask = 0;
   if (!env)
      return;

   char *copy = strdup(env);
   if (!copy)
      return;

   char *saveptr = NULL;
   for (char *option = strtok_r(copy, ",", &saveptr); option != NULL;
        option = strtok_r(NULL, ",", &saveptr))
      wrapper_log_mask |= get_debug_flag(option);

   free(copy);
}

int
get_wrapper_log_level(const char *option)
{
   return (wrapper_log_mask & get_debug_flag(option)) != 0;
}

void
write_to_logfile(const char *fmt, const char *level, ...)
{
   static FILE *wrapper_log_file = NULL;
   static int open_failed;

   if (!wrapper_log_file && !open_failed) {
      const char *filename = getenv("WRAPPER_LOG_FILE");
      char path[PATH_MAX];

      if (!filename) {
         char date[64];
         get_formatted_date_time(date, sizeof(date));
         snprintf(path, sizeof(path), "%s/%s_%s", WRAPPER_LOG_PATH,
                  get_executable_name(), date);
         filename = path;
      }

      if (!strcmp("stdout", filename))
         wrapper_log_file = stdout;
      else if (!strcmp("stderr", filename))
         wrapper_log_file = stderr;
      else
         wrapper_log_file = fopen(filename, "w");

      if (!wrapper_log_file)
         open_failed = 1;
   }

   if (!wrapper_log_file)
      return;

   va_list va_args;
   va_start(va_args, level);
   fprintf(wrapper_log_file, "[%s]: ", level);
   vfprintf(wrapper_log_file, fmt, va_args);
   fprintf(wrapper_log_file, "\n");
   fflush(wrapper_log_file);
   va_end(va_args);
}

void
init_wrapper_logging(void)
{
   parse_wrapper_debug_str(getenv("WRAPPER_LOG_LEVEL"));

   /* Only touch the storage when some logging was actually requested. */
   if (!wrapper_log_mask)
      return;

   create_folder(WRAPPER_DIR);
   create_folder(WRAPPER_LOG_PATH);
   if (wrapper_log_mask & WRAPPER_LOG_SHADER)
      create_folder(WRAPPER_SHADER_LOG_PATH);
   if (wrapper_log_mask & WRAPPER_LOG_VALIDATION)
      create_folder(WRAPPER_VALIDATION_LOG_PATH);
}

void
dump_shader_code(const uint32_t *code, size_t size)
{
   static int index = 0;
   char file[PATH_MAX];

   snprintf(file, sizeof(file), "%s/%s_shader_%d.spv",
            WRAPPER_SHADER_LOG_PATH, get_executable_name(), index++);

   FILE *fp = fopen(file, "wb");
   if (fp) {
      fwrite(code, 1, size, fp);
      fclose(fp);
   }
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wrapper_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                              void *userData)
{
   static FILE *vvl_log_file = NULL;

   if (!vvl_log_file) {
      char date[64];
      char path[PATH_MAX];

      get_formatted_date_time(date, sizeof(date));
      snprintf(path, sizeof(path), "%s/%s_%s", WRAPPER_VALIDATION_LOG_PATH,
               get_executable_name(), date);
      vvl_log_file = fopen(path, "w");
   }

   if (vvl_log_file) {
      fprintf(vvl_log_file, "[%s] Code %i : %s\n",
              callbackData->pMessageIdName ? callbackData->pMessageIdName : "",
              callbackData->messageIdNumber,
              callbackData->pMessage ? callbackData->pMessage : "");
      fflush(vvl_log_file);
   }

   return VK_FALSE;
}
