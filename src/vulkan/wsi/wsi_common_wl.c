/*
 * WinLite: the WL swapchain's link to the emulator. See wsi_common_wl.h.
 *
 * Copyright © 2026 WinLite
 * SPDX-License-Identifier: MIT
 */
#include "wsi_common_wl.h"

#include <android/hardware_buffer.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "util/log.h"

uint64_t
wl_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* $<env>, else $TMPDIR/<name>; NULL if neither is set. */
static const char *
wl_path(const char *env, const char *name, char *buf, size_t size)
{
   const char *p = getenv(env);
   if (p && *p)
      return p;
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !*tmp)
      return NULL;
   if ((size_t)snprintf(buf, size, "%s/%s", tmp, name) >= size)
      return NULL;
   return buf;
}

static char sock_buf[256];
static const char *sock_path;
static once_flag sock_once = ONCE_FLAG_INIT;

static void
sock_init(void)
{
   sock_path = wl_path(WL_DIRECT_SOCK_ENV, WL_DIRECT_SOCK_NAME, sock_buf, sizeof(sock_buf));
}

const char *
wl_direct_socket_path(void)
{
   call_once(&sock_once, sock_init);
   return sock_path;
}

/* ================================================================ WL Direct */

struct wl_direct {
   int fd;
   uint32_t seq;
   uint32_t client_id;
};

struct wl_direct *
wl_direct_connect(const char *path, uint32_t x_window, uint32_t image_count,
                  struct AHardwareBuffer *const *images, uint32_t width, uint32_t height,
                  uint32_t ahb_format, uint64_t ahb_usage, bool ubwc,
                  uint32_t *granted, uint32_t *reason)
{
   if (!path || image_count == 0 || image_count > WL_DIRECT_MAX_IMAGES)
      return NULL;

   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   if (strlen(path) >= sizeof(addr.sun_path))
      return NULL;
   strcpy(addr.sun_path, path);

   int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
   if (fd < 0)
      return NULL;
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(fd);
      return NULL;
   }
   struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

   struct wl_direct_hello hello;
   memset(&hello, 0, sizeof(hello));
   wl_direct_hdr_init(&hello.hdr, WL_DIRECT_HELLO, 1);
   hello.magic = WL_DIRECT_MAGIC;
   hello.version = WL_DIRECT_VERSION;
   hello.flags = ubwc ? WL_HELLO_UBWC : 0;
   hello.x_window = x_window;
   hello.image_count = image_count;
   hello.width = width;
   hello.height = height;
   hello.ahb_format = ahb_format;
   hello.pid = (uint32_t)getpid();
   hello.ahb_usage = ahb_usage;
   if (wl_direct_send(fd, &hello.hdr, -1) != 0)
      goto fail;
   for (uint32_t i = 0; i < image_count; i++) {
      if (AHardwareBuffer_sendHandleToUnixSocket(images[i], fd) != 0)
         goto fail;
   }

   _Alignas(8) unsigned char buf[WL_DIRECT_MAX_PACKET];
   int got_fd = -1;
   ssize_t n = wl_direct_recv(fd, buf, &got_fd);
   if (got_fd >= 0)
      close(got_fd);
   const struct wl_direct_welcome *w = (const struct wl_direct_welcome *)buf;
   if (n != (ssize_t)sizeof(*w) || w->hdr.type != WL_DIRECT_WELCOME || w->version != WL_DIRECT_VERSION) {
      mesa_logw("wsi(wl): WL Direct server at %s refused the swapchain", path);
      goto fail;
   }

   /* Blocking from now on: a reader thread waits on it. Sends keep their timeout. */
   tv.tv_sec = 0;
   tv.tv_usec = 0;
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   struct wl_direct *link = calloc(1, sizeof(*link));
   if (!link)
      goto fail;
   link->fd = fd;
   link->seq = 1;
   link->client_id = w->client_id;
   if (granted)
      *granted = w->granted;
   if (reason)
      *reason = w->reason;
   mesa_logi("wsi(wl): WL Direct client %u (window 0x%x, %u images %ux%u)",
             w->client_id, x_window, image_count, width, height);
   return link;

fail:
   close(fd);
   return NULL;
}

int
wl_direct_read(struct wl_direct *link, struct wl_direct_event *ev)
{
   for (;;) {
      _Alignas(8) unsigned char buf[WL_DIRECT_MAX_PACKET];
      int fd = -1;
      ssize_t n = wl_direct_recv(link->fd, buf, &fd);
      if (n <= 0)
         return (int)n;
      memset(ev, 0, sizeof(*ev));
      ev->fd = -1;
      const struct wl_direct_hdr *h = (const struct wl_direct_hdr *)buf;
      ev->type = h->type;
      switch (h->type) {
      case WL_DIRECT_GRANT: {
         const struct wl_direct_grant *m = (const void *)buf;
         ev->granted = m->granted;
         ev->reason = m->reason;
         ev->refresh_ns = m->refresh_ns;
         ev->capabilities = m->capabilities;
         return 1;
      }
      case WL_DIRECT_RELEASE: {
         const struct wl_direct_release *m = (const void *)buf;
         ev->image = m->image_index;
         ev->fd = fd;
         return 1;
      }
      case WL_DIRECT_TIMING: {
         const struct wl_direct_timing *m = (const void *)buf;
         ev->frame_id = m->frame_id;
         ev->latch_ns = m->latch_ns;
         ev->present_ns = m->present_ns;
         ev->timing_flags = m->flags;
         return 1;
      }
      default:
         /* Nothing else is meant for a client: ignored. */
         if (fd >= 0)
            close(fd);
         continue;
      }
   }
}

int
wl_direct_frame(struct wl_direct *link, uint32_t image, int fence_fd, uint64_t frame_id,
                uint64_t desired_present_ns, bool generated)
{
   struct wl_direct_frame m;
   memset(&m, 0, sizeof(m));
   wl_direct_hdr_init(&m.hdr, WL_DIRECT_FRAME, ++link->seq);
   m.image_index = image;
   m.flags = (generated ? WL_FRAME_GENERATED : 0) | (fence_fd >= 0 ? WL_FRAME_HAS_FENCE : 0);
   m.frame_id = frame_id;
   m.desired_present_ns = desired_present_ns;
   return wl_direct_send(link->fd, &m.hdr, fence_fd);
}

void
wl_direct_shutdown(struct wl_direct *link)
{
   if (link)
      shutdown(link->fd, SHUT_RDWR);
}

void
wl_direct_close(struct wl_direct *link)
{
   if (!link)
      return;
   struct wl_direct_bye bye;
   wl_direct_hdr_init(&bye.hdr, WL_DIRECT_BYE, ++link->seq);
   wl_direct_send(link->fd, &bye.hdr, -1);
   close(link->fd);
   free(link);
}

/* ================================================================ wl.control */

static mtx_t ctl_mutex;
static once_flag ctl_once = ONCE_FLAG_INIT;
static struct wl_control *ctl_map;      /* NULL until mapped */
static uint64_t ctl_next_try_ns;

static void
ctl_init(void)
{
   mtx_init(&ctl_mutex, mtx_plain);
}

static struct wl_control *
wl_control_get(void)
{
   call_once(&ctl_once, ctl_init);
   struct wl_control *c = __atomic_load_n(&ctl_map, __ATOMIC_ACQUIRE);
   if (c)
      return c;

   mtx_lock(&ctl_mutex);
   c = ctl_map;
   const uint64_t now = wl_now_ns();
   if (!c && now >= ctl_next_try_ns) {
      ctl_next_try_ns = now + 2000000000ull;
      char buf[256];
      const char *path = wl_path(WL_CONTROL_PATH_ENV, WL_CONTROL_NAME, buf, sizeof(buf));
      int fd = path ? open(path, O_RDWR | O_CLOEXEC) : -1;
      struct stat st;
      if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= (off_t)WL_CONTROL_SIZE) {
         void *p = mmap(NULL, WL_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
         if (p != MAP_FAILED) {
            if (wl_control_valid((struct wl_control *)p)) {
               c = (struct wl_control *)p;
               __atomic_store_n(&ctl_map, c, __ATOMIC_RELEASE);
               mesa_logi("wsi(wl): wl.control at %s", path);
            } else {
               mesa_logw("wsi(wl): %s is not wl.control version %u: ignored", path, WL_CONTROL_VERSION);
               munmap(p, WL_CONTROL_SIZE);
               ctl_next_try_ns = UINT64_MAX;   /* another version: it will not change */
            }
         }
      }
      if (fd >= 0)
         close(fd);
   }
   mtx_unlock(&ctl_mutex);
   return c;
}

bool
wl_control_read_settings_now(struct wl_control_settings *out)
{
   struct wl_control *c = wl_control_get();
   return c && wl_control_read_settings(c, out, NULL);
}

bool
wl_control_read_grid(uint64_t now_ns, uint64_t *period_ns, uint64_t *anchor_ns)
{
   struct wl_control *c = wl_control_get();
   struct wl_control_clock clock;
   *period_ns = 0;
   *anchor_ns = 0;
   if (!c || !wl_control_read_clock(c, &clock, NULL))
      return false;

   /* The period is useful even without a fresh anchor: FG's display-rate cap and the limiter's
    * interval snapping must not fall back to 60 Hz just because display timing is unavailable.
    * Only the anchor becomes invalid when feedback stops. */
   *period_ns = clock.period_ns;
   if (!clock.period_ns || !clock.anchor_ns)
      return false;
   if (now_ns > clock.anchor_ns && now_ns - clock.anchor_ns > 2000000000ull)
      return false;
   *anchor_ns = clock.anchor_ns;
   return true;
}

void
wl_control_write_status_now(const struct wl_control_status *status)
{
   struct wl_control *c = wl_control_get();
   if (c)
      wl_control_write_status(c, status);
}

/* ================================================================ the FPS limit */

uint64_t
wl_limiter_next_slot(uint64_t last_ns, uint64_t now_ns, uint32_t fps,
                     uint64_t period_ns, uint64_t anchor_ns)
{
   if (!fps)
      return now_ns;
   uint64_t interval = 1000000000ull / fps;
   bool grid = false;
   if (period_ns) {
      const uint64_t n = (interval + period_ns / 2) / period_ns;
      const uint64_t snapped = n * period_ns;
      const uint64_t diff = snapped > interval ? snapped - interval : interval - snapped;
      if (n >= 1 && diff * 20 <= interval) {
         interval = snapped;
         grid = anchor_ns != 0;
      }
   }
   uint64_t slot = last_ns ? last_ns + interval : now_ns;
   /* More than a frame late: start over from now, no burst of frames to catch up. */
   if (slot + interval <= now_ns)
      slot = now_ns;
   if (grid) {
      /* On the nearest refresh: frames that start on the grid are shown evenly. */
      const int64_t rel = (int64_t)(slot - anchor_ns);
      const int64_t p = (int64_t)period_ns;
      const int64_t k = rel >= 0 ? (rel + p / 2) / p : -((-rel + p / 2) / p);
      slot = (uint64_t)((int64_t)anchor_ns + k * p);
      if (last_ns && slot <= last_ns)
         slot += period_ns;
   }
   return slot;
}

uint64_t
wl_limiter_wait(struct wl_limiter *lim, uint32_t fps, uint64_t preferred_period_ns,
                uint64_t max_wait_ns)
{
   const uint64_t now = wl_now_ns();
   if (!fps) {
      lim->last_ns = 0;
      return 0;
   }
   uint64_t grid_period = 0, grid_anchor = 0;
   wl_control_read_grid(now, &grid_period, &grid_anchor);
   const uint64_t period = preferred_period_ns ? preferred_period_ns : grid_period;
   const uint64_t anchor = period == grid_period ? grid_anchor : 0;
   uint64_t slot = wl_limiter_next_slot(lim->last_ns, now, fps, period, anchor);
   uint64_t slept = 0;
   if (slot > now) {
      uint64_t until = slot;
      if (until - now > max_wait_ns)
         until = now + max_wait_ns;
      struct timespec ts = { .tv_sec = (time_t)(until / 1000000000ull),
                             .tv_nsec = (long)(until % 1000000000ull) };
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
      slept = until - now;
   }
   lim->last_ns = slot;
   return slept;
}
