/*
 * WinLite: the WL swapchain's link to the emulator (wl_proto.h).
 *
 *   WL Direct   the swapchain's frames go straight to the emulator's SurfaceFlinger layer
 *               while the emulator grants it (the X window is alone on the screen); otherwise
 *               they go through X (DRI3 + Present) as before
 *   wl.control  settings from the emulator (FPS limit, frame generation, output mode), the
 *               display's refresh grid, and the swapchain's status for its HUD
 *   limiter     the FPS limit, applied in vkAcquireNextImageKHR: the game waits for its
 *               next frame slot before it starts drawing (lower latency than a limit at
 *               present time); xcore does not limit a window with the _WL_SWAPCHAIN property
 *
 * Copyright © 2026 WinLite
 * SPDX-License-Identifier: MIT
 */
#ifndef WSI_COMMON_WL_H
#define WSI_COMMON_WL_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#include "wl_proto.h"

struct AHardwareBuffer;

#ifdef __cplusplus
extern "C" {
#endif

/* Where the emulator listens: $WL_DIRECT_SOCK, else $TMPDIR/.wl-direct; NULL if neither. */
const char *wl_direct_socket_path(void);

struct wl_direct;

struct wl_direct_event {
   uint16_t type;            /* WL_DIRECT_GRANT, _RELEASE, _TIMING */
   uint32_t granted;         /* GRANT */
   uint32_t reason;          /* GRANT */
   uint64_t refresh_ns;      /* GRANT */
   uint32_t capabilities;    /* GRANT: WL_GRANT_* */
   uint32_t image;           /* RELEASE */
   int      fd;              /* RELEASE: the release fence (owned by the caller), or -1 */
   uint64_t frame_id;        /* TIMING */
   uint64_t latch_ns;        /* TIMING */
   uint64_t present_ns;      /* TIMING */
   uint32_t timing_flags;    /* TIMING: WL_TIMING_* */
};

/* Connects and hands over the swapchain's images (HELLO, the images, WELCOME); waits at most
 * a few hundred milliseconds. NULL when nobody listens or the server refuses. */
struct wl_direct *
wl_direct_connect(const char *path, uint32_t x_window, uint32_t image_count,
                  struct AHardwareBuffer *const *images, uint32_t width, uint32_t height,
                  uint32_t ahb_format, uint64_t ahb_usage, bool ubwc,
                  uint32_t *granted, uint32_t *reason);

/* Blocks for the next event. 1: *ev is filled; 0: the server closed; < 0: -errno. */
int wl_direct_read(struct wl_direct *link, struct wl_direct_event *ev);

/* Sends a frame; `fence_fd` (not consumed) is the acquire sync file, -1 if rendered already.
 * 0 or -errno. */
int wl_direct_frame(struct wl_direct *link, uint32_t image, int fence_fd, uint64_t frame_id,
                    uint64_t desired_present_ns, bool generated);

/* Wakes a reader blocked in wl_direct_read() (it returns 0). */
void wl_direct_shutdown(struct wl_direct *link);

/* BYE and close. */
void wl_direct_close(struct wl_direct *link);

/* ---- wl.control (process-wide; the file is mapped on first use, looked for again every
 * two seconds while it is missing) */

/* false: no file (or not version 1): the caller uses its defaults. */
bool wl_control_read_settings_now(struct wl_control_settings *out);
/* The refresh grid the emulator published; false if its anchor is none or stale (older than two
 * seconds). `period_ns` is still filled when the period is known without a fresh anchor;
 * `anchor_ns` is then 0. */
bool wl_control_read_grid(uint64_t now_ns, uint64_t *period_ns, uint64_t *anchor_ns);
void wl_control_write_status_now(const struct wl_control_status *status);

/* ---- the FPS limit */

struct wl_limiter {
   uint64_t last_ns;     /* the slot the last frame got */
};

/* Sleeps until the next frame slot for `fps` (0: none) and claims it. The interval snaps to a
 * whole number of refreshes when it is within 5% of one, and the slots then sit on the refresh
 * grid. A nonzero `preferred_period_ns` (the latest Direct GRANT) takes precedence over
 * wl.control; its anchor is used only when both periods agree. At most `max_wait_ns`.
 * Returns the time slept. */
uint64_t wl_limiter_wait(struct wl_limiter *lim, uint32_t fps, uint64_t preferred_period_ns,
                         uint64_t max_wait_ns);

/* The slot computation of wl_limiter_wait: when the frame after one at `last_ns` may start, at
 * `now_ns`, for `fps` on a grid (period, anchor; 0 = no grid). */
uint64_t wl_limiter_next_slot(uint64_t last_ns, uint64_t now_ns, uint32_t fps,
                              uint64_t period_ns, uint64_t anchor_ns);

uint64_t wl_now_ns(void);

/* ---- frame generation
 *
 * The WL swapchain generates frames between the game's with an engine the layered driver
 * supplies (the Vulkan wrapper's WFG, wrapper_fg.c): wsi_device::wl_fg. The swapchain owns a
 * pool of extra images (outputs) the engine renders the generated frames into; it presents them
 * like the game's own, through X or WL Direct, at the times it plans. */

struct wsi_wl_fg;

/* From wl.control, for one game frame. */
struct wsi_wl_fg_params {
   uint32_t multiplier;       /* >= 2: fixed xN; 0: chase target_fps */
   uint32_t target_fps;       /* 0: the display refresh rate */
   uint32_t flow_side;        /* px on the shorter side the motion is analysed at, 0: default */
   bool fast_interp;
   float refresh_hz;          /* 0: unknown */
   uint32_t min_fps;          /* adaptive: pauses below this source rate (0: no floor)... */
   uint32_t resume_fps;       /* ...and resumes above this one */
};

struct wsi_wl_fg_result {
   uint32_t count;                              /* frames generated */
   uint32_t outputs[WL_DIRECT_MAX_IMAGES];      /* the output each was rendered into, in order */
   int fence_fd;                                /* signaled once all of it (and the source's
                                                 * layout transitions) is done; -1: nothing
                                                 * submitted, the game frame is as it was */
   uint64_t interval_ns;                        /* the game's measured frame interval, 0 unknown */
   uint32_t state;                              /* enum wl_fg_state */
};

struct wsi_wl_fg_ops {
   /* `device` is the application's (the layered driver's) handle. The outputs and every
    * source are images of `format`, `width` x `height`, usable as colour attachment, transfer
    * source and sampled. NULL: no frame generation on this device. */
   struct wsi_wl_fg *(*create)(VkDevice device, VkFormat format, uint32_t width, uint32_t height,
                               uint32_t output_count, const VkImage *outputs);
   /* Waits for its own GPU work first. */
   void (*destroy)(struct wsi_wl_fg *fg);
   /* One game frame, from vkQueuePresentKHR on the application's queue (externally synchronized
    * by the application), after the present's own submit: `source` (in PRESENT_SRC, left
    * there) is analysed and frames between the previous game frame and it are rendered into
    * free outputs (bit i of `free_mask`: output i may be written). `release_fds[i]`: the fence
    * output i is still read until (-1: none); the engine takes the fds of the outputs it uses
    * and sets them to -1. */
   void (*frame)(struct wsi_wl_fg *fg, VkQueue queue, VkImage source,
                 const struct wsi_wl_fg_params *params, uint32_t free_mask, int *release_fds,
                 struct wsi_wl_fg_result *result);
};

#ifdef __cplusplus
}
#endif

#endif
