#ifndef WRAPPER_LOG_H
#define WRAPPER_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define WRAPPER_LOG_INFO       (1ull << 0)
#define WRAPPER_LOG_ERROR      (1ull << 1)
#define WRAPPER_LOG_SHADER     (1ull << 2)
#define WRAPPER_LOG_VALIDATION (1ull << 3)
#define WRAPPER_LOG_APIDUMP    (1ull << 5)

#ifdef __cplusplus
extern "C" {
#endif

/* Parses WRAPPER_LOG_LEVEL (comma separated: info,error,shader,validation,
 * apidump).  Called once before anything else is logged. */
void
init_wrapper_logging(void);

void
dump_shader_code(const uint32_t *code, size_t size);

int
get_wrapper_log_level(const char *option);

void
write_to_logfile(const char *fmt, const char *level, ...)
   __attribute__((format(printf, 1, 3)));

/* Name of the running executable (basename of argv[0], also stripped of a
 * Windows path for Wine processes).  The returned string is static. */
const char *
get_executable_name(void);

VKAPI_ATTR VkBool32 VKAPI_CALL
wrapper_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                              void *userData);

#ifdef __cplusplus
}
#endif

#define WRAPPER_LOG_LEVEL(s) (get_wrapper_log_level(#s))

#define WRAPPER_LOG(level, fmt, ...) \
do { \
   if (WRAPPER_LOG_LEVEL(level)) \
      write_to_logfile(fmt, #level, ##__VA_ARGS__); \
} while (0)

#endif
