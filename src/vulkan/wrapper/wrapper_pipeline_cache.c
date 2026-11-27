/*
 * Persistent pipeline cache for the wrapper (WRAPPER_PIPELINE_CACHE).
 *
 * Mesa drivers (Turnip) keep their own on-disk shader cache, so a game that
 * compiled its pipelines once starts fast the next time.  The proprietary
 * Qualcomm driver keeps nothing between runs: on Android it is up to the
 * application to hand the driver a VkPipelineCache and to save it, and in the
 * emulator's stack nobody does (vkd3d-proton's own cache only holds
 * DXBC/DXIL -> SPIR-V).  Every launch then compiles every pipeline again.
 *
 * What this file does, below the application and above the driver:
 *
 *  - One persistent cache of the wrapper's own is created at device creation,
 *    primed from the file the last run saved.  Pipelines created with
 *    VK_NULL_HANDLE as the cache use it, and so do pipelines created with an
 *    application cache that was created empty (or with only the header of an
 *    empty one): vkd3d-proton makes one such cache per pipeline, and giving
 *    each of them the whole file would cost far more than it saves.  Those
 *    application caches stay empty, merges into them are dropped.  Caches with
 *    real data, flags or a pNext chain belong to the application and are
 *    passed through untouched.  (WRAPPER_PIPELINE_CACHE_IMPLICIT=0 turns the
 *    whole thing off.)
 *  - The cache is written back WRAPPER_PIPELINE_CACHE_DELAY_MS after the last
 *    pipeline was created (never more often than every
 *    WRAPPER_PIPELINE_CACHE_INTERVAL_S), at least every
 *    WRAPPER_PIPELINE_CACHE_FORCE_S while pipelines keep
 *    coming and when the device goes away.  A process that is killed keeps what was saved, which is why the
 *    saving does not wait for the exit.  A save only happens when the
 *    driver's data grew.
 *
 * One file per (game, vendor, device, driver version, pipelineCacheUUID),
 * written with a header of the wrapper's own (size and hash of the
 * data) next to the driver's, through a temporary file that is synced before
 * the rename.  A truncated or damaged file is deleted, never given to the
 * driver.  The identity in the key and in the checks is the real driver's, not
 * the one the application sees (WRAPPER_VENDOR_ID and friends).
 *
 * vkGetPipelineCacheData does not run at the same time as pipeline creation
 * (a small gate in the creation entrypoints), the proprietary compiler has been
 * seen to crash with concurrent calls.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_debug.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

#define WPC_FILE_MAGIC        0x48435057u /* "WPCH" */
#define WPC_FILE_VERSION      1u
#define WPC_PATH_MAX          1024
#define WPC_MAX_APP_CACHES    3
#define WPC_ENTRY_IMPLICIT    0
#define WPC_NUM_ENTRIES       (1 + WPC_MAX_APP_CACHES)
#define WPC_POLL_US           100000
#define WPC_DEFAULT_INTERVAL_S 120
#define WPC_DEFAULT_FORCE_S   600
#define WPC_TEMP_MAX_AGE_SEC  120
#define WPC_DEFAULT_DELAY_MS  10000
#define WPC_DEFAULT_MAX_MB    512
/* A cache created with at most this much data is an empty one (the header
 * of an empty cache is 32 bytes) */
#define WPC_EMPTY_DATA_MAX    256

/* On-disk header, followed by the data of vkGetPipelineCacheData */
struct wpc_file_header {
   uint32_t magic;
   uint32_t version;
   uint32_t vendor_id;
   uint32_t device_id;
   uint32_t driver_version;
   uint8_t uuid[VK_UUID_SIZE];
   uint32_t reserved;
   uint64_t data_size;
   uint64_t data_hash;
};
_Static_assert(sizeof(struct wpc_file_header) == 56,
               "the file header layout is part of the file format");

/* Start of the data every driver returns (VkPipelineCacheHeaderVersionOne) */
struct wpc_vk_header {
   uint32_t header_size;
   uint32_t header_version;
   uint32_t vendor_id;
   uint32_t device_id;
   uint8_t uuid[VK_UUID_SIZE];
};

enum wpc_entry_status {
   WPC_ENTRY_FREE = 0,
   WPC_ENTRY_RESERVED,
   WPC_ENTRY_ACTIVE,
};

struct wpc_entry {
   enum wpc_entry_status status;       /* mtx */
   bool dirty;                         /* mtx: pipelines made since the last save */
   VkPipelineCache cache;              /* mtx */
   size_t saved_size;                  /* save_mtx: size of the data last saved or loaded */
   bool too_big_logged;                /* save_mtx */
   char path[WPC_PATH_MAX];            /* set while RESERVED, constant while ACTIVE */
};

struct wrapper_pipeline_cache_state {
   struct wrapper_device *device;

   char dir[WPC_PATH_MAX];
   char key[96];
   uint64_t max_bytes;
   int64_t idle_ns;
   int64_t interval_ns;                /* least time between two saves */
   int64_t force_ns;                   /* save at least this often while pipelines keep coming */
   bool save_on_exit_only;
   /* WRAPPER_PIPELINE_CACHE_REQUEST: the launcher writes a token into this
    * file to ask for a save, the answer is the same token in <path>.done and
    * <path>.alive.<pid> says someone is listening.  Thread only. */
   char request_path[WPC_PATH_MAX];
   char request_token[64];
   bool use_implicit;

   /* Entry 0 is the wrapper's own cache for pipelines created without one,
    * 1.. are the application's (slot = index - 1) */
   struct wpc_entry entries[WPC_NUM_ENTRIES];
   /* Never changes after init (VK_NULL_HANDLE: not used) */
   VkPipelineCache implicit_cache;

   /* Caches the application created empty (vkd3d-proton makes one per
    * pipeline): pipelines made with them go to implicit_cache instead.
    * Protected by mtx. */
   VkPipelineCache *shadow;
   unsigned shadow_count, shadow_cap;

   /* Gate between pipeline creation and vkGetPipelineCacheData, and the
    * bookkeeping of the entries */
   pthread_mutex_t mtx;
   pthread_cond_t cond;
   uint32_t in_flight;                 /* pipeline creations inside the driver */
   bool saving;                        /* a save has the exclusive turn */
   int64_t last_activity_ns;           /* last successful pipeline creation */
   int64_t last_pass_ns;               /* last time the entries were saved */
   bool had_pass;                      /* the first save isn't held back by the interval */
   bool stop;
   bool thread_running;
   pthread_t thread;

   /* One save at a time; also keeps a cache from being destroyed under a
    * save.  Taken before mtx. */
   pthread_mutex_t save_mtx;
};

/* ------------------------------------------------------------------------ */
/* Files                                                                    */
/* ------------------------------------------------------------------------ */

static const VkPhysicalDeviceProperties *
wpc_props(const struct wrapper_pipeline_cache_state *st)
{
   /* The real driver's values, not the identity reported to the app */
   return &st->device->physical->properties2.properties;
}

static void
wpc_entry_path(const struct wrapper_pipeline_cache_state *st, unsigned index,
               char *out, size_t size)
{
   const VkPhysicalDeviceProperties *props = wpc_props(st);
   const uint64_t uuid_hash = XXH64(props->pipelineCacheUUID, VK_UUID_SIZE, 0);
   char slot[16];

   if (index == WPC_ENTRY_IMPLICIT)
      snprintf(slot, sizeof(slot), "i");
   else
      snprintf(slot, sizeof(slot), "c%u", index - 1);

   snprintf(out, size, "%s/%s_%08x_%08x_%08x_%016llx_%s.vkpc", st->dir, st->key,
            props->vendorID, props->deviceID, props->driverVersion,
            (unsigned long long)uuid_hash, slot);
}

static bool
wpc_read_all(int fd, void *dst, size_t size)
{
   uint8_t *p = dst;
   while (size) {
      ssize_t n = read(fd, p, size);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      p += n;
      size -= n;
   }
   return true;
}

static bool
wpc_write_all(int fd, const void *src, size_t size)
{
   const uint8_t *p = src;
   while (size) {
      ssize_t n = write(fd, p, size);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      p += n;
      size -= n;
   }
   return true;
}

static bool
wpc_vk_header_matches(const VkPhysicalDeviceProperties *props,
                      const void *data, size_t size)
{
   struct wpc_vk_header h;

   if (size < sizeof(h))
      return false;
   memcpy(&h, data, sizeof(h));
   return h.header_size >= sizeof(h) && h.header_size <= size &&
          h.header_version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
          h.vendor_id == props->vendorID && h.device_id == props->deviceID &&
          !memcmp(h.uuid, props->pipelineCacheUUID, VK_UUID_SIZE);
}

/* Returns the buffer to free (the file contents), with the driver's data in
 * *data / *data_size.  NULL: no usable file (a bad one is deleted). */
static uint8_t *
wpc_load_file(const struct wrapper_pipeline_cache_state *st, const char *path,
              const uint8_t **data, size_t *data_size)
{
   const VkPhysicalDeviceProperties *props = wpc_props(st);
   struct wpc_file_header h;
   struct stat sb;
   uint8_t *buf = NULL;
   bool ok = false;

   int fd = open(path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return NULL;

   if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) &&
       (uint64_t)sb.st_size > sizeof(h) &&
       (uint64_t)sb.st_size <= sizeof(h) + st->max_bytes) {
      const size_t total = sb.st_size;
      buf = malloc(total);
      if (buf && wpc_read_all(fd, buf, total)) {
         const uint8_t *payload = buf + sizeof(h);
         const size_t payload_size = total - sizeof(h);

         memcpy(&h, buf, sizeof(h));
         ok = h.magic == WPC_FILE_MAGIC && h.version == WPC_FILE_VERSION &&
              h.vendor_id == props->vendorID && h.device_id == props->deviceID &&
              h.driver_version == props->driverVersion &&
              !memcmp(h.uuid, props->pipelineCacheUUID, VK_UUID_SIZE) &&
              h.data_size == payload_size &&
              h.data_hash == XXH64(payload, payload_size, 0) &&
              wpc_vk_header_matches(props, payload, payload_size);
         if (ok) {
            *data = payload;
            *data_size = payload_size;
         }
      }
   }
   close(fd);

   if (!ok) {
      if ((uint64_t)sb.st_size > sizeof(h) + st->max_bytes)
         WRAPPER_LOG(info, "Pipeline cache: %s is %lld bytes, over the limit of "
                     "%llu (WRAPPER_PIPELINE_CACHE_MAX_MB): starting empty",
                     path, (long long)sb.st_size,
                     (unsigned long long)st->max_bytes);
      else
         WRAPPER_LOG(info, "Pipeline cache: dropping unusable file %s", path);
      free(buf);
      unlink(path);
      return NULL;
   }
   return buf;
}

static bool
wpc_write_file(const struct wrapper_pipeline_cache_state *st, const char *path,
               const void *data, size_t size)
{
   const VkPhysicalDeviceProperties *props = wpc_props(st);
   struct wpc_file_header h;
   char tmp[WPC_PATH_MAX + 16];

   memset(&h, 0, sizeof(h));
   h.magic = WPC_FILE_MAGIC;
   h.version = WPC_FILE_VERSION;
   h.vendor_id = props->vendorID;
   h.device_id = props->deviceID;
   h.driver_version = props->driverVersion;
   memcpy(h.uuid, props->pipelineCacheUUID, VK_UUID_SIZE);
   h.data_size = size;
   h.data_hash = XXH64(data, size, 0);

   snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
   int fd = mkstemp(tmp);
   if (fd < 0)
      return false;

   bool ok = wpc_write_all(fd, &h, sizeof(h)) && wpc_write_all(fd, data, size) &&
             fsync(fd) == 0;
   if (close(fd) != 0)
      ok = false;
   if (ok && rename(tmp, path) != 0)
      ok = false;
   if (!ok)
      unlink(tmp);
   return ok;
}

static void
wpc_mkdir_p(const char *dir)
{
   char tmp[WPC_PATH_MAX];

   snprintf(tmp, sizeof(tmp), "%s", dir);
   for (char *p = tmp + 1; *p; p++) {
      if (*p == '/') {
         *p = '\0';
         mkdir(tmp, 0770);
         *p = '/';
      }
   }
   mkdir(tmp, 0770);
}

/* Temporary files of a save that was killed half way; a live save is
 * finished within seconds, so only old ones go. */
static void
wpc_cleanup_temp_files(const struct wrapper_pipeline_cache_state *st)
{
   DIR *d = opendir(st->dir);
   if (!d)
      return;

   const size_t key_len = strlen(st->key);
   const time_t now = time(NULL);
   struct dirent *e;

   while ((e = readdir(d)) != NULL) {
      if (strncmp(e->d_name, st->key, key_len) != 0 ||
          e->d_name[key_len] != '_' || !strstr(e->d_name, ".vkpc."))
         continue;

      char path[WPC_PATH_MAX * 2];
      struct stat sb;
      snprintf(path, sizeof(path), "%s/%s", st->dir, e->d_name);
      if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) &&
          now - sb.st_mtime > WPC_TEMP_MAX_AGE_SEC)
         unlink(path);
   }
   closedir(d);
}

/* ------------------------------------------------------------------------ */
/* Saving                                                                   */
/* ------------------------------------------------------------------------ */

/* Takes the exclusive turn (no pipeline creation inside the driver) around
 * vkGetPipelineCacheData.  Called with save_mtx held.  Returns the data to
 * free, NULL when there's nothing (new) to save. */
static void *
wpc_snapshot(struct wrapper_pipeline_cache_state *st, struct wpc_entry *e,
             size_t *out_size)
{
   struct wrapper_device *device = st->device;
   void *buf = NULL;
   size_t size = 0;

   pthread_mutex_lock(&st->mtx);
   st->saving = true;
   while (st->in_flight)
      pthread_cond_wait(&st->cond, &st->mtx);
   e->dirty = false;
   pthread_mutex_unlock(&st->mtx);

   VkResult res = device->dispatch_table.GetPipelineCacheData(
      device->dispatch_handle, e->cache, &size, NULL);
   if (res == VK_SUCCESS && size > e->saved_size) {
      if (size > st->max_bytes) {
         if (!e->too_big_logged) {
            WRAPPER_LOG(info, "Pipeline cache: %zu bytes is over the limit "
                        "(WRAPPER_PIPELINE_CACHE_MAX_MB), not saving %s",
                        size, e->path);
            e->too_big_logged = true;
         }
      } else {
         buf = malloc(size);
         if (buf) {
            size_t got = size;
            res = device->dispatch_table.GetPipelineCacheData(
               device->dispatch_handle, e->cache, &got, buf);
            if (res == VK_SUCCESS) {
               size = got;
            } else {
               free(buf);
               buf = NULL;
            }
         }
      }
   }

   pthread_mutex_lock(&st->mtx);
   st->saving = false;
   pthread_cond_broadcast(&st->cond);
   pthread_mutex_unlock(&st->mtx);

   *out_size = size;
   return buf;
}

/* Called with save_mtx held */
static void
wpc_save_entry(struct wrapper_pipeline_cache_state *st, struct wpc_entry *e)
{
   size_t size;
   void *blob = wpc_snapshot(st, e, &size);

   if (!blob)
      return;

   struct stat sb;
   if (stat(e->path, &sb) == 0 && S_ISREG(sb.st_mode) &&
       (uint64_t)sb.st_size >= sizeof(struct wpc_file_header) + size) {
      /* Another device of the process (or an earlier run) already saved at
       * least as much: never replace a bigger file with a smaller one */
      WRAPPER_LOG(info, "Pipeline cache: %s already holds %lld bytes, not "
                  "replacing it with %zu", e->path, (long long)sb.st_size, size);
      e->saved_size = size;
   } else if (!wpc_vk_header_matches(wpc_props(st), blob, size)) {
      WRAPPER_LOG(error, "Pipeline cache: the driver's data has an unexpected "
                  "header, not saving %s", e->path);
   } else if (wpc_write_file(st, e->path, blob, size)) {
      e->saved_size = size;
      WRAPPER_LOG(info, "Pipeline cache: saved %zu bytes to %s", size, e->path);
   } else {
      WRAPPER_LOG(error, "Pipeline cache: failed to write %s (errno %d)",
                  e->path, errno);
   }
   free(blob);
}

static void
wpc_save_all(struct wrapper_pipeline_cache_state *st)
{
   pthread_mutex_lock(&st->save_mtx);

   for (unsigned i = 0; i < WPC_NUM_ENTRIES; i++) {
      struct wpc_entry *e = &st->entries[i];

      pthread_mutex_lock(&st->mtx);
      const bool want = e->status == WPC_ENTRY_ACTIVE && e->dirty;
      pthread_mutex_unlock(&st->mtx);

      if (want)
         wpc_save_entry(st, e);
   }

   pthread_mutex_lock(&st->mtx);
   st->last_pass_ns = os_time_get_nano();
   st->had_pass = true;
   pthread_mutex_unlock(&st->mtx);

   pthread_mutex_unlock(&st->save_mtx);
}

/* Returns true when the launcher asked for a save that wasn't answered yet */
static bool
wpc_request_pending(struct wrapper_pipeline_cache_state *st, char *token,
                    size_t token_size)
{
   if (!st->request_path[0])
      return false;

   int fd = open(st->request_path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return false;
   ssize_t n = read(fd, token, token_size - 1);
   close(fd);
   if (n <= 0)
      return false;
   token[n] = '\0';
   return strcmp(token, st->request_token) != 0;
}

static void
wpc_answer_request(struct wrapper_pipeline_cache_state *st, const char *token)
{
   char done[WPC_PATH_MAX + 16];

   snprintf(st->request_token, sizeof(st->request_token), "%s", token);
   snprintf(done, sizeof(done), "%s.done", st->request_path);
   int fd = open(done, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
   if (fd >= 0) {
      if (write(fd, token, strlen(token)) < 0)
         WRAPPER_LOG(error, "Pipeline cache: can't answer the save request");
      close(fd);
   }
}

static void
wpc_alive_marker(struct wrapper_pipeline_cache_state *st, bool create)
{
   char path[WPC_PATH_MAX + 32];

   if (!st->request_path[0])
      return;
   snprintf(path, sizeof(path), "%s.alive.%d", st->request_path, (int)getpid());
   if (create) {
      int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
      if (fd >= 0)
         close(fd);
   } else {
      unlink(path);
   }
}

static void *
wpc_thread_main(void *arg)
{
   struct wrapper_pipeline_cache_state *st = arg;

   for (;;) {
      os_time_sleep(WPC_POLL_US);

      pthread_mutex_lock(&st->mtx);
      const bool stop = st->stop;
      bool due = false;
      if (!stop && !st->save_on_exit_only) {
         bool any_dirty = false;
         for (unsigned i = 0; i < WPC_NUM_ENTRIES; i++)
            any_dirty |= st->entries[i].status == WPC_ENTRY_ACTIVE &&
                         st->entries[i].dirty;
         if (any_dirty) {
            const int64_t now = os_time_get_nano();
            const bool spaced = !st->had_pass ||
                                now - st->last_pass_ns >= st->interval_ns;
            due = spaced &&
                  (now - st->last_activity_ns >= st->idle_ns ||
                   now - st->last_pass_ns >= st->force_ns);
         }
      }
      pthread_mutex_unlock(&st->mtx);

      if (stop)
         break;

      char token[sizeof(st->request_token)];
      if (wpc_request_pending(st, token, sizeof(token))) {
         WRAPPER_LOG(info, "Pipeline cache: save requested by the launcher");
         wpc_save_all(st);
         wpc_answer_request(st, token);
      } else if (due) {
         wpc_save_all(st);
      }
   }
   return NULL;
}

/* Called with mtx held */
static void
wpc_start_thread_locked(struct wrapper_pipeline_cache_state *st)
{
   if (st->thread_running)
      return;
   if (pthread_create(&st->thread, NULL, wpc_thread_main, st) == 0)
      st->thread_running = true;
   else
      WRAPPER_LOG(error, "Pipeline cache: failed to start the saving thread, "
                  "caches are saved when they are destroyed only");
}

/* ------------------------------------------------------------------------ */
/* Saving at process exit                                                   */
/* ------------------------------------------------------------------------ */

/* Wine usually ends a game without vkDestroyDevice, so the saving at device
 * teardown never runs.  The live states are kept in a small registry and
 * saved from an atexit handler too. */
#define WPC_MAX_LIVE 8
static pthread_mutex_t wpc_live_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct wrapper_pipeline_cache_state *wpc_live[WPC_MAX_LIVE];
static bool wpc_atexit_registered;

static void wpc_save_all(struct wrapper_pipeline_cache_state *st);

void
wrapper_pipeline_cache_atexit(void)
{
   pthread_mutex_lock(&wpc_live_mtx);
   for (unsigned i = 0; i < WPC_MAX_LIVE; i++) {
      if (wpc_live[i])
         wpc_save_all(wpc_live[i]);
   }
   pthread_mutex_unlock(&wpc_live_mtx);
}

static void
wpc_live_add(struct wrapper_pipeline_cache_state *st)
{
   pthread_mutex_lock(&wpc_live_mtx);
   for (unsigned i = 0; i < WPC_MAX_LIVE; i++) {
      if (!wpc_live[i]) {
         wpc_live[i] = st;
         break;
      }
   }
   if (!wpc_atexit_registered) {
      wpc_atexit_registered = true;
      atexit(wrapper_pipeline_cache_atexit);
   }
   pthread_mutex_unlock(&wpc_live_mtx);
}

static void
wpc_live_remove(struct wrapper_pipeline_cache_state *st)
{
   pthread_mutex_lock(&wpc_live_mtx);
   for (unsigned i = 0; i < WPC_MAX_LIVE; i++) {
      if (wpc_live[i] == st)
         wpc_live[i] = NULL;
   }
   pthread_mutex_unlock(&wpc_live_mtx);
}

/* ------------------------------------------------------------------------ */
/* Creating the caches                                                      */
/* ------------------------------------------------------------------------ */

/* Creates the driver's cache for entry 'index' (RESERVED, path set), primed
 * from its file when there is a usable one.  On success the entry is ACTIVE. */
static VkResult
wpc_create_entry_cache(struct wrapper_pipeline_cache_state *st, unsigned index,
                       const VkPipelineCacheCreateInfo *app_info,
                       const VkAllocationCallbacks *alloc,
                       VkPipelineCache *out_cache)
{
   struct wrapper_device *device = st->device;
   struct wpc_entry *e = &st->entries[index];
   VkPipelineCacheCreateInfo info = *app_info;
   const uint8_t *data = NULL;
   size_t data_size = 0;

   uint8_t *file = wpc_load_file(st, e->path, &data, &data_size);
   if (file) {
      info.initialDataSize = data_size;
      info.pInitialData = data;
   }

   VkResult res = device->dispatch_table.CreatePipelineCache(
      device->dispatch_handle, &info, alloc, out_cache);
   if (res != VK_SUCCESS && file) {
      /* The driver refused the data (a driver update that kept its UUID) */
      WRAPPER_LOG(info, "Pipeline cache: the driver refused %s (res %d)",
                  e->path, res);
      unlink(e->path);
      free(file);
      file = NULL;
      info = *app_info;
      res = device->dispatch_table.CreatePipelineCache(
         device->dispatch_handle, &info, alloc, out_cache);
   }
   free(file);

   if (res != VK_SUCCESS)
      return res;

   if (data_size)
      WRAPPER_LOG(info, "Pipeline cache: loaded %zu bytes from %s", data_size,
                  e->path);
   else
      WRAPPER_LOG(info, "Pipeline cache: no saved data yet for %s", e->path);

   pthread_mutex_lock(&st->mtx);
   e->cache = *out_cache;
   e->dirty = false;
   e->status = WPC_ENTRY_ACTIVE;
   if (!st->save_on_exit_only || st->request_path[0])
      wpc_start_thread_locked(st);
   pthread_mutex_unlock(&st->mtx);

   /* saved_size is only used under save_mtx */
   pthread_mutex_lock(&st->save_mtx);
   e->saved_size = data_size;
   e->too_big_logged = false;
   pthread_mutex_unlock(&st->save_mtx);

   return VK_SUCCESS;
}

static void
wpc_sanitize_key(char *dst, size_t size, const char *src)
{
   size_t n = 0;

   for (; src && *src && n + 1 < size; src++) {
      const unsigned char c = *src;
      dst[n++] = (isalnum(c) || c == '.' || c == '_' || c == '-') ? c : '_';
   }
   dst[n] = '\0';
   if (!n)
      snprintf(dst, size, "unknown");
}

VkResult
wrapper_pipeline_cache_device_init(struct wrapper_device *device)
{
   const struct wrapper_physical_device *pdevice = device->physical;
   const int64_t mode = debug_get_num_option("WRAPPER_PIPELINE_CACHE", 0);

   /* 0: off, 1: every driver, 2: only the proprietary Qualcomm driver.
    * Nothing here may fail the device. */
   if (mode == 0)
      return VK_SUCCESS;
   if (mode == 2 &&
       pdevice->driver_properties.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
      WRAPPER_LOG(info, "Pipeline cache: not the Qualcomm driver, disabled");
      return VK_SUCCESS;
   }

   const char *dir = getenv("WRAPPER_PIPELINE_CACHE_PATH");
   if (!dir || !*dir)
      dir = getenv("WRAPPER_CACHE_PATH");
   if (!dir || !*dir || strlen(dir) >= WPC_PATH_MAX - 128) {
      WRAPPER_LOG(info, "Pipeline cache: WRAPPER_PIPELINE_CACHE_PATH / "
                  "WRAPPER_CACHE_PATH not set, disabled");
      return VK_SUCCESS;
   }

   struct wrapper_pipeline_cache_state *st = calloc(1, sizeof(*st));
   if (!st)
      return VK_SUCCESS;

   st->device = device;
   snprintf(st->dir, sizeof(st->dir), "%s", dir);
   const char *key = getenv("WRAPPER_CACHE_KEY");
   wpc_sanitize_key(st->key, sizeof(st->key),
                    (key && *key) ? key : get_executable_name());

   int64_t max_mb = debug_get_num_option("WRAPPER_PIPELINE_CACHE_MAX_MB",
                                         WPC_DEFAULT_MAX_MB);
   int64_t delay_ms = debug_get_num_option("WRAPPER_PIPELINE_CACHE_DELAY_MS",
                                           WPC_DEFAULT_DELAY_MS);
   st->max_bytes = (uint64_t)CLAMP(max_mb, 1, 4096) << 20;
   st->idle_ns = CLAMP(delay_ms, 100, 600000) * 1000000ll;
   st->interval_ns = CLAMP(debug_get_num_option(
      "WRAPPER_PIPELINE_CACHE_INTERVAL_S", WPC_DEFAULT_INTERVAL_S), 0, 86400) *
      1000000000ll;
   st->force_ns = MAX2(CLAMP(debug_get_num_option(
      "WRAPPER_PIPELINE_CACHE_FORCE_S", WPC_DEFAULT_FORCE_S), 1, 86400) *
      1000000000ll, st->interval_ns);
   const char *save = getenv("WRAPPER_PIPELINE_CACHE_SAVE");
   st->save_on_exit_only = save && !strcmp(save, "exit");
   const char *request = getenv("WRAPPER_PIPELINE_CACHE_REQUEST");
   if (request && *request && strlen(request) < WPC_PATH_MAX - 32)
      snprintf(st->request_path, sizeof(st->request_path), "%s", request);
   st->use_implicit = debug_get_bool_option("WRAPPER_PIPELINE_CACHE_IMPLICIT", true);
   st->last_activity_ns = st->last_pass_ns = os_time_get_nano();

   pthread_mutex_init(&st->mtx, NULL);
   pthread_cond_init(&st->cond, NULL);
   pthread_mutex_init(&st->save_mtx, NULL);

   wpc_mkdir_p(st->dir);
   wpc_cleanup_temp_files(st);

   if (st->use_implicit) {
      const VkPipelineCacheCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      };
      struct wpc_entry *e = &st->entries[WPC_ENTRY_IMPLICIT];

      e->status = WPC_ENTRY_RESERVED;
      wpc_entry_path(st, WPC_ENTRY_IMPLICIT, e->path, sizeof(e->path));
      if (wpc_create_entry_cache(st, WPC_ENTRY_IMPLICIT, &info, NULL,
                                 &st->implicit_cache) != VK_SUCCESS) {
         WRAPPER_LOG(error, "Pipeline cache: failed to create the implicit cache");
         e->status = WPC_ENTRY_FREE;
         st->implicit_cache = VK_NULL_HANDLE;
      }
   }

   device->pipeline_cache = st;
   wpc_live_add(st);
   wpc_alive_marker(st, true);
   WRAPPER_LOG(info, "Pipeline cache: enabled, dir %s, key %s, save after "
               "%lld ms idle, implicit cache %s", st->dir, st->key,
               (long long)(st->idle_ns / 1000000),
               st->implicit_cache ? "on" : "off");
   return VK_SUCCESS;
}

void
wrapper_pipeline_cache_device_finish(struct wrapper_device *device)
{
   struct wrapper_pipeline_cache_state *st = device->pipeline_cache;

   if (!st)
      return;

   wpc_live_remove(st);
   wpc_alive_marker(st, false);

   pthread_mutex_lock(&st->mtx);
   st->stop = true;
   const bool joinable = st->thread_running;
   pthread_mutex_unlock(&st->mtx);
   if (joinable)
      pthread_join(st->thread, NULL);

   /* The caches the application leaked go with the device, but not before
    * they are saved */
   wpc_save_all(st);

   if (st->implicit_cache != VK_NULL_HANDLE &&
       device->dispatch_handle != VK_NULL_HANDLE) {
      device->dispatch_table.DestroyPipelineCache(device->dispatch_handle,
                                                  st->implicit_cache, NULL);
   }

   free(st->shadow);
   pthread_mutex_destroy(&st->save_mtx);
   pthread_cond_destroy(&st->cond);
   pthread_mutex_destroy(&st->mtx);
   device->pipeline_cache = NULL;
   free(st);
}

/* ------------------------------------------------------------------------ */
/* Entrypoints                                                              */
/* ------------------------------------------------------------------------ */

/* Pipeline creation and merging into a cache run side by side, but never
 * with a save of the cache data (see wpc_snapshot) */
static void
wpc_begin_use(struct wrapper_pipeline_cache_state *st)
{
   pthread_mutex_lock(&st->mtx);
   while (st->saving)
      pthread_cond_wait(&st->cond, &st->mtx);
   st->in_flight++;
   pthread_mutex_unlock(&st->mtx);
}

static void
wpc_end_use(struct wrapper_pipeline_cache_state *st, VkPipelineCache cache,
            bool new_data)
{
   pthread_mutex_lock(&st->mtx);
   if (new_data) {
      st->last_activity_ns = os_time_get_nano();
      for (unsigned i = 0; i < WPC_NUM_ENTRIES; i++) {
         if (st->entries[i].status == WPC_ENTRY_ACTIVE &&
             st->entries[i].cache == cache)
            st->entries[i].dirty = true;
      }
   }
   if (--st->in_flight == 0)
      pthread_cond_broadcast(&st->cond);
   pthread_mutex_unlock(&st->mtx);
}

static bool
wpc_is_shadow_locked(const struct wrapper_pipeline_cache_state *st,
                     VkPipelineCache cache)
{
   for (unsigned i = 0; i < st->shadow_count; i++) {
      if (st->shadow[i] == cache)
         return true;
   }
   return false;
}

/* The cache pipelines created with 'cache' really go to */
static VkPipelineCache
wpc_resolve_cache(struct wrapper_pipeline_cache_state *st, VkPipelineCache cache)
{
   if (st->implicit_cache == VK_NULL_HANDLE)
      return cache;
   if (cache == VK_NULL_HANDLE)
      return st->implicit_cache;

   pthread_mutex_lock(&st->mtx);
   const bool shadowed = wpc_is_shadow_locked(st, cache);
   pthread_mutex_unlock(&st->mtx);
   return shadowed ? st->implicit_cache : cache;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreatePipelineCache(VkDevice _device,
                            const VkPipelineCacheCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipelineCache *pPipelineCache)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_pipeline_cache_state *st = device->pipeline_cache;

   /* An empty cache (or one that only holds the header of an empty one, as
    * vkd3d-proton gets back from a shadowed cache) carries nothing worth
    * keeping: it's created empty and shadowed by the wrapper's persistent
    * cache.  Caches with real data, flags or extensions are the application's. */
   if (!st || st->implicit_cache == VK_NULL_HANDLE ||
       pCreateInfo->initialDataSize > WPC_EMPTY_DATA_MAX ||
       (pCreateInfo->flags &
        ~VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT) != 0 ||
       pCreateInfo->pNext != NULL) {
      return device->dispatch_table.CreatePipelineCache(
         device->dispatch_handle, pCreateInfo, pAllocator, pPipelineCache);
   }

   VkPipelineCacheCreateInfo info = *pCreateInfo;
   info.initialDataSize = 0;
   info.pInitialData = NULL;

   VkResult res = device->dispatch_table.CreatePipelineCache(
      device->dispatch_handle, &info, pAllocator, pPipelineCache);
   if (res != VK_SUCCESS)
      return res;

   pthread_mutex_lock(&st->mtx);
   if (st->shadow_count == st->shadow_cap) {
      const unsigned cap = st->shadow_cap ? st->shadow_cap * 2 : 64;
      VkPipelineCache *n = realloc(st->shadow, cap * sizeof(*n));
      if (n) {
         st->shadow = n;
         st->shadow_cap = cap;
      }
   }
   if (st->shadow_count < st->shadow_cap)
      st->shadow[st->shadow_count++] = *pPipelineCache;
   pthread_mutex_unlock(&st->mtx);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyPipelineCache(VkDevice _device, VkPipelineCache pipelineCache,
                             const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_pipeline_cache_state *st = device->pipeline_cache;

   if (st && pipelineCache != VK_NULL_HANDLE) {
      pthread_mutex_lock(&st->mtx);
      for (unsigned i = 0; i < st->shadow_count; i++) {
         if (st->shadow[i] == pipelineCache) {
            st->shadow[i] = st->shadow[--st->shadow_count];
            break;
         }
      }
      pthread_mutex_unlock(&st->mtx);
   }

   device->dispatch_table.DestroyPipelineCache(device->dispatch_handle,
                                               pipelineCache, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_MergePipelineCaches(VkDevice _device, VkPipelineCache dstCache,
                            uint32_t srcCacheCount,
                            const VkPipelineCache *pSrcCaches)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_pipeline_cache_state *st = device->pipeline_cache;

   if (!st) {
      return device->dispatch_table.MergePipelineCaches(
         device->dispatch_handle, dstCache, srcCacheCount, pSrcCaches);
   }

   /* Shadowed caches hold nothing: merging into one is dropped */
   if (st->implicit_cache != VK_NULL_HANDLE &&
       wpc_resolve_cache(st, dstCache) == st->implicit_cache)
      return VK_SUCCESS;

   wpc_begin_use(st);
   VkResult res = device->dispatch_table.MergePipelineCaches(
      device->dispatch_handle, dstCache, srcCacheCount, pSrcCaches);
   wpc_end_use(st, dstCache, res == VK_SUCCESS);
   return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateGraphicsPipelines(VkDevice _device, VkPipelineCache pipelineCache,
                                uint32_t createInfoCount,
                                const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                const VkAllocationCallbacks *pAllocator,
                                VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_pipeline_cache_state *st = device->pipeline_cache;

   if (!st) {
      return device->dispatch_table.CreateGraphicsPipelines(
         device->dispatch_handle, pipelineCache, createInfoCount, pCreateInfos,
         pAllocator, pPipelines);
   }

   pipelineCache = wpc_resolve_cache(st, pipelineCache);

   wpc_begin_use(st);
   VkResult res = device->dispatch_table.CreateGraphicsPipelines(
      device->dispatch_handle, pipelineCache, createInfoCount, pCreateInfos,
      pAllocator, pPipelines);
   wpc_end_use(st, pipelineCache, res == VK_SUCCESS);
   return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateComputePipelines(VkDevice _device, VkPipelineCache pipelineCache,
                               uint32_t createInfoCount,
                               const VkComputePipelineCreateInfo *pCreateInfos,
                               const VkAllocationCallbacks *pAllocator,
                               VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_pipeline_cache_state *st = device->pipeline_cache;

   if (!st) {
      return device->dispatch_table.CreateComputePipelines(
         device->dispatch_handle, pipelineCache, createInfoCount, pCreateInfos,
         pAllocator, pPipelines);
   }

   pipelineCache = wpc_resolve_cache(st, pipelineCache);

   wpc_begin_use(st);
   VkResult res = device->dispatch_table.CreateComputePipelines(
      device->dispatch_handle, pipelineCache, createInfoCount, pCreateInfos,
      pAllocator, pPipelines);
   wpc_end_use(st, pipelineCache, res == VK_SUCCESS);
   return res;
}
