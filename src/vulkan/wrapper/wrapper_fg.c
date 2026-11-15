/*
 * Frame generation for the WL swapchain (wsi_wl_fg_ops, wsi_common_wl.h), built into the
 * wrapper: WFG (wrapper_wfg.c, MIT, Copyright 2026 qwertypower (DEVAR Entertainment LLC),
 * see LICENSE.wfg) on the underlying driver.
 *
 * Per game frame, in vkQueuePresentKHR on the application's queue, after the present's own
 * submit: the game's image is analysed (WFG copies it into its own pyramid) and the frames
 * between the previous game frame and it are rendered into the swapchain's outputs, one
 * submit, one sync file for the swapchain to show them by. The application's queue is
 * externally synchronized for the call, and the work lands right behind the frame it reads,
 * ahead of the game's next one: no queue of its own, no lock.
 *
 * Every Vulkan call here and in WFG goes straight to the driver (driver handles, the wrapper
 * device's dispatch tables): the command buffers are never the application's.
 *
 * Copyright © 2026 WinLite
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "vulkan/wsi/wsi_common_wl.h"
#include "util/bitscan.h"
#include "util/libsync.h"
#include "util/macros.h"
#include "util/simple_mtx.h"

#define WRAPPER_WFG_VK_NO_REDIRECT
#include "wrapper_wfg_vk.h"
#include "wrapper_wfg.h"

/* WFG's own table: filled once from the first device that runs it. */
VkDispatch vkd;
static simple_mtx_t wfg_bind_mutex = SIMPLE_MTX_INITIALIZER;
static bool wfg_bound;

#define FG_RING 3
#define FG_DEFAULT_FLOW_SIDE 180u
#define FG_BASE_EXTENT 4u

struct fg_slot {
   VkCommandBuffer cmd;
   VkFence fence;
   bool submitted;
};

struct wsi_wl_fg {
   struct wrapper_device *device;
   VkDevice dev;                          /* the driver's */
   const struct vk_device_dispatch_table *vk;
   VkFormat format;
   uint32_t width, height;

   VkrDis *dis;
   VkRenderPass pass;                     /* compatible with WFG's interpolation pass */
   uint32_t output_count;
   VkImage outputs[WL_DIRECT_MAX_IMAGES];
   VkImageView views[WL_DIRECT_MAX_IMAGES];
   VkFramebuffer fbs[WL_DIRECT_MAX_IMAGES];
   VkSemaphore waits[WL_DIRECT_MAX_IMAGES];   /* an output's release fence, imported */

   /* What WFG samples outside the content rect: the content is the whole image, so never;
    * a small black image keeps its descriptor valid. */
   VkImage base;
   VkDeviceMemory base_mem;
   VkImageView base_view;
   bool base_ready;

   uint32_t family;                       /* of the pool, UINT32_MAX: none yet */
   VkCommandPool pool;
   struct fg_slot ring[FG_RING];
   uint32_t ring_next;
   VkSemaphore signal;                    /* exportable as a sync file */

   uint64_t source_frames;
   bool paused;                           /* below fg_min_fps */
   bool broken;
};

static bool
wfg_bind(struct wrapper_device *device)
{
   simple_mtx_lock(&wfg_bind_mutex);
   if (!wfg_bound) {
      const struct vk_physical_device_dispatch_table *pd = &device->physical->dispatch_table;
      const struct vk_device_dispatch_table *d = &device->dispatch_table;
      struct wrapper_instance *instance = device->physical->instance;
      memset(&vkd, 0, sizeof(vkd));
      vkd.GetInstanceProcAddr = instance->dispatch_table.GetInstanceProcAddr;
      vkd.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
         instance->dispatch_table.GetInstanceProcAddr(instance->dispatch_handle,
                                                      "vkGetDeviceProcAddr");
      vkd.GetPhysicalDeviceProperties = pd->GetPhysicalDeviceProperties;
      vkd.GetPhysicalDeviceMemoryProperties = pd->GetPhysicalDeviceMemoryProperties;
      vkd.GetPhysicalDeviceFormatProperties = pd->GetPhysicalDeviceFormatProperties;
#define FROM_DEVICE(name) vkd.name = d->name
      FROM_DEVICE(AllocateMemory);
      FROM_DEVICE(FreeMemory);
      FROM_DEVICE(CreateImage);
      FROM_DEVICE(DestroyImage);
      FROM_DEVICE(BindImageMemory);
      FROM_DEVICE(GetImageMemoryRequirements);
      FROM_DEVICE(CreateImageView);
      FROM_DEVICE(DestroyImageView);
      FROM_DEVICE(CreateSampler);
      FROM_DEVICE(DestroySampler);
      FROM_DEVICE(CreateDescriptorSetLayout);
      FROM_DEVICE(DestroyDescriptorSetLayout);
      FROM_DEVICE(CreateDescriptorPool);
      FROM_DEVICE(DestroyDescriptorPool);
      FROM_DEVICE(AllocateDescriptorSets);
      FROM_DEVICE(UpdateDescriptorSets);
      FROM_DEVICE(CreatePipelineLayout);
      FROM_DEVICE(DestroyPipelineLayout);
      FROM_DEVICE(CreateComputePipelines);
      FROM_DEVICE(DestroyPipeline);
      FROM_DEVICE(CreateShaderModule);
      FROM_DEVICE(DestroyShaderModule);
      FROM_DEVICE(CmdBindPipeline);
      FROM_DEVICE(CmdBindDescriptorSets);
      FROM_DEVICE(CmdPushConstants);
      FROM_DEVICE(CmdPipelineBarrier);
      FROM_DEVICE(CmdBlitImage);
      FROM_DEVICE(CmdDispatch);
      FROM_DEVICE(CreateRenderPass);
      FROM_DEVICE(DestroyRenderPass);
      FROM_DEVICE(CreateGraphicsPipelines);
      FROM_DEVICE(CmdBeginRenderPass);
      FROM_DEVICE(CmdEndRenderPass);
      FROM_DEVICE(CmdSetViewport);
      FROM_DEVICE(CmdSetScissor);
      FROM_DEVICE(CmdDraw);
#undef FROM_DEVICE
      wfg_bound = vkd.GetPhysicalDeviceFormatProperties && vkd.GetPhysicalDeviceMemoryProperties &&
                  vkd.CreateComputePipelines && vkd.CmdDispatch && vkd.CreateRenderPass &&
                  vkd.CreateGraphicsPipelines && vkd.CmdBeginRenderPass && vkd.CmdBlitImage &&
                  vkd.CmdDraw;
      if (!wfg_bound)
         WRAPPER_LOG(error, "WFG: the driver lacks entry points it needs");
   }
   const bool ok = wfg_bound;
   simple_mtx_unlock(&wfg_bind_mutex);
   return ok;
}

static uint32_t
fg_memory_type(const struct wsi_wl_fg *fg, uint32_t bits, VkMemoryPropertyFlags flags)
{
   const VkPhysicalDeviceMemoryProperties *mp = &fg->device->physical->memory_properties;
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return UINT32_MAX;
}

/* Waits for every submit of the ring. */
static void
fg_wait_idle(struct wsi_wl_fg *fg)
{
   for (uint32_t i = 0; i < FG_RING; i++) {
      struct fg_slot *s = &fg->ring[i];
      if (s->submitted && s->fence != VK_NULL_HANDLE) {
         fg->vk->WaitForFences(fg->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
         s->submitted = false;
      }
   }
}

static void
fg_destroy_pool(struct wsi_wl_fg *fg)
{
   for (uint32_t i = 0; i < FG_RING; i++) {
      struct fg_slot *s = &fg->ring[i];
      if (s->cmd != VK_NULL_HANDLE)
         fg->vk->FreeCommandBuffers(fg->dev, fg->pool, 1, &s->cmd);
      s->cmd = VK_NULL_HANDLE;
   }
   if (fg->pool != VK_NULL_HANDLE)
      fg->vk->DestroyCommandPool(fg->dev, fg->pool, NULL);
   fg->pool = VK_NULL_HANDLE;
   fg->family = UINT32_MAX;
}

static bool
fg_ensure_pool(struct wsi_wl_fg *fg, uint32_t family)
{
   if (fg->pool != VK_NULL_HANDLE && fg->family == family)
      return true;
   fg_wait_idle(fg);
   fg_destroy_pool(fg);

   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = family,
   };
   if (fg->vk->CreateCommandPool(fg->dev, &pci, NULL, &fg->pool) != VK_SUCCESS) {
      fg->pool = VK_NULL_HANDLE;
      return false;
   }
   for (uint32_t i = 0; i < FG_RING; i++) {
      const VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = fg->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      if (fg->vk->AllocateCommandBuffers(fg->dev, &ai, &fg->ring[i].cmd) != VK_SUCCESS) {
         fg->ring[i].cmd = VK_NULL_HANDLE;
         fg_destroy_pool(fg);
         return false;
      }
   }
   fg->family = family;
   return true;
}

static void
wrapper_fg_destroy(struct wsi_wl_fg *fg)
{
   if (!fg)
      return;
   fg_wait_idle(fg);
   vkr_dis_destroy(fg->dis);
   fg_destroy_pool(fg);
   for (uint32_t i = 0; i < FG_RING; i++) {
      if (fg->ring[i].fence != VK_NULL_HANDLE)
         fg->vk->DestroyFence(fg->dev, fg->ring[i].fence, NULL);
   }
   for (uint32_t i = 0; i < fg->output_count; i++) {
      if (fg->fbs[i] != VK_NULL_HANDLE)
         fg->vk->DestroyFramebuffer(fg->dev, fg->fbs[i], NULL);
      if (fg->views[i] != VK_NULL_HANDLE)
         fg->vk->DestroyImageView(fg->dev, fg->views[i], NULL);
      if (fg->waits[i] != VK_NULL_HANDLE)
         fg->vk->DestroySemaphore(fg->dev, fg->waits[i], NULL);
   }
   if (fg->signal != VK_NULL_HANDLE)
      fg->vk->DestroySemaphore(fg->dev, fg->signal, NULL);
   if (fg->pass != VK_NULL_HANDLE)
      fg->vk->DestroyRenderPass(fg->dev, fg->pass, NULL);
   if (fg->base_view != VK_NULL_HANDLE)
      fg->vk->DestroyImageView(fg->dev, fg->base_view, NULL);
   if (fg->base != VK_NULL_HANDLE)
      fg->vk->DestroyImage(fg->dev, fg->base, NULL);
   if (fg->base_mem != VK_NULL_HANDLE)
      fg->vk->FreeMemory(fg->dev, fg->base_mem, NULL);
   free(fg);
}

static bool
fg_create_view(struct wsi_wl_fg *fg, VkImage image, VkImageView *view)
{
   const VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = fg->format,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   return fg->vk->CreateImageView(fg->dev, &vi, NULL, view) == VK_SUCCESS;
}

/* The render pass vkr_dis_generate_into's framebuffers must be compatible with (wrapper_wfg.h):
 * one attachment of the format, one sample, one subpass. */
static bool
fg_create_pass(struct wsi_wl_fg *fg)
{
   const VkAttachmentDescription att = {
      .format = fg->format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
   };
   const VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   const VkSubpassDescription sub = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &ref,
   };
   const VkSubpassDependency dep = {
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
   };
   const VkRenderPassCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &att,
      .subpassCount = 1,
      .pSubpasses = &sub,
      .dependencyCount = 1,
      .pDependencies = &dep,
   };
   return fg->vk->CreateRenderPass(fg->dev, &ci, NULL, &fg->pass) == VK_SUCCESS;
}

static bool
fg_create_base(struct wsi_wl_fg *fg)
{
   const VkImageCreateInfo ic = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fg->format,
      .extent = { FG_BASE_EXTENT, FG_BASE_EXTENT, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (fg->vk->CreateImage(fg->dev, &ic, NULL, &fg->base) != VK_SUCCESS) {
      fg->base = VK_NULL_HANDLE;
      return false;
   }
   VkMemoryRequirements req;
   fg->vk->GetImageMemoryRequirements(fg->dev, fg->base, &req);
   const uint32_t type = fg_memory_type(fg, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (type == UINT32_MAX)
      return false;
   const VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if (fg->vk->AllocateMemory(fg->dev, &ai, NULL, &fg->base_mem) != VK_SUCCESS) {
      fg->base_mem = VK_NULL_HANDLE;
      return false;
   }
   if (fg->vk->BindImageMemory(fg->dev, fg->base, fg->base_mem, 0) != VK_SUCCESS)
      return false;
   return fg_create_view(fg, fg->base, &fg->base_view);
}

static struct wsi_wl_fg *
wrapper_fg_create(VkDevice _device, VkFormat format, uint32_t width, uint32_t height,
                  uint32_t output_count, const VkImage *outputs)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   if (!device || output_count == 0 || output_count > WL_DIRECT_MAX_IMAGES || !wfg_bind(device))
      return NULL;

   struct wsi_wl_fg *fg = calloc(1, sizeof(*fg));
   if (!fg)
      return NULL;
   fg->device = device;
   fg->dev = device->dispatch_handle;
   fg->vk = &device->dispatch_table;
   fg->format = format;
   fg->width = width;
   fg->height = height;
   fg->family = UINT32_MAX;
   fg->output_count = output_count;

   if (!fg->vk->ImportSemaphoreFdKHR || !fg->vk->GetSemaphoreFdKHR) {
      WRAPPER_LOG(error, "WFG: no sync file semaphores on this driver; frame generation off");
      goto fail;
   }

   fg->dis = vkr_dis_create(fg->dev, device->physical->dispatch_handle);
   if (!fg->dis)
      goto fail;
   if (!fg_create_pass(fg) || !fg_create_base(fg))
      goto fail;

   const VkSemaphoreCreateInfo plain = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   for (uint32_t i = 0; i < output_count; i++) {
      fg->outputs[i] = outputs[i];
      if (!fg_create_view(fg, outputs[i], &fg->views[i]))
         goto fail;
      const VkFramebufferCreateInfo fi = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = fg->pass,
         .attachmentCount = 1,
         .pAttachments = &fg->views[i],
         .width = width,
         .height = height,
         .layers = 1,
      };
      if (fg->vk->CreateFramebuffer(fg->dev, &fi, NULL, &fg->fbs[i]) != VK_SUCCESS ||
          fg->vk->CreateSemaphore(fg->dev, &plain, NULL, &fg->waits[i]) != VK_SUCCESS)
         goto fail;
   }

   const VkExportSemaphoreCreateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   const VkSemaphoreCreateInfo exportable = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &export_info,
   };
   if (fg->vk->CreateSemaphore(fg->dev, &exportable, NULL, &fg->signal) != VK_SUCCESS)
      goto fail;
   const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   for (uint32_t i = 0; i < FG_RING; i++) {
      if (fg->vk->CreateFence(fg->dev, &fci, NULL, &fg->ring[i].fence) != VK_SUCCESS)
         goto fail;
   }

   WRAPPER_LOG(info, "WFG: frame generation for a %ux%u swapchain (format %d, %u outputs)",
               width, height, (int)format, output_count);
   return fg;

fail:
   WRAPPER_LOG(error, "WFG: frame generation unavailable for this swapchain");
   wrapper_fg_destroy(fg);
   return NULL;
}

static void
fg_image_barrier(struct wsi_wl_fg *fg, VkCommandBuffer cmd, VkImage image,
                 VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                 VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
   const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   fg->vk->CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* A release fence into the output's wait semaphore; false: there was none to wait for (or it
 * was waited for on the CPU). Takes the fd. */
static bool
fg_import_wait(struct wsi_wl_fg *fg, uint32_t output, int fd)
{
   if (fd < 0)
      return false;
   const VkImportSemaphoreFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      .semaphore = fg->waits[output],
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .fd = fd,
   };
   if (fg->vk->ImportSemaphoreFdKHR(fg->dev, &info) == VK_SUCCESS)
      return true;   /* the semaphore owns it now */
   sync_wait(fd, 1000);
   close(fd);
   return false;
}

static void
wrapper_fg_frame(struct wsi_wl_fg *fg, VkQueue _queue, VkImage source,
                 const struct wsi_wl_fg_params *p, uint32_t free_mask, int *release_fds,
                 struct wsi_wl_fg_result *res)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   res->count = 0;
   res->fence_fd = -1;
   res->interval_ns = 0;
   res->state = WL_FG_STATE_OFF;
   if (fg->broken || !queue)
      return;

   if (!fg_ensure_pool(fg, queue->vk.queue_family_index)) {
      WRAPPER_LOG(error, "WFG: no command pool; frame generation off for this swapchain");
      fg->broken = true;
      return;
   }

   vkr_dis_configure(fg->dis, p->flow_side ? p->flow_side : FG_DEFAULT_FLOW_SIDE, p->fast_interp,
                     p->target_fps, p->multiplier, p->refresh_hz);
   const VkrDisContentRect content = { 0, 0, fg->width, fg->height };
   if (vkr_dis_needs_rebuild(fg->dis, fg->width, fg->height, fg->format, content, fg->base_view)) {
      fg_wait_idle(fg);
      if (!vkr_dis_prepare(fg->dis, fg->width, fg->height, fg->format, content, fg->base_view)) {
         fg->broken = true;
         return;
      }
   }

   uint32_t capacity = util_bitcount(free_mask);
   if (capacity > VKR_DIS_MAX_GENERATIONS)
      capacity = VKR_DIS_MAX_GENERATIONS;
   const uint32_t planned = vkr_dis_plan(fg->dis, capacity, ++fg->source_frames);
   const uint64_t interval = vkr_dis_source_interval_ns(fg->dis);
   res->interval_ns = interval;

   /* The adaptive floor of wl.control (a fixed xN is the user's own choice and always runs). */
   if (p->multiplier < 2 && p->min_fps && interval) {
      const double rate = 1.0e9 / (double)interval;
      const uint32_t resume = MAX2(p->resume_fps, p->min_fps);
      if (!fg->paused && rate < (double)p->min_fps)
         fg->paused = true;
      else if (fg->paused && rate >= (double)resume)
         fg->paused = false;
   } else {
      fg->paused = false;
   }

   if (fg->paused || (planned == 0 && vkr_dis_idle(fg->dis))) {
      /* Nothing generated, and the frame not needed either: shown as it is. */
      vkr_dis_skip_frame(fg->dis);
      res->state = WL_FG_STATE_PAUSED;
      return;
   }

   struct fg_slot *slot = &fg->ring[fg->ring_next];
   if (slot->submitted) {
      if (fg->vk->WaitForFences(fg->dev, 1, &slot->fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
         vkr_dis_skip_frame(fg->dis);   /* the GPU is a second behind: not now */
         res->state = WL_FG_STATE_PAUSED;
         return;
      }
      slot->submitted = false;
   }
   fg->vk->ResetFences(fg->dev, 1, &slot->fence);
   fg->vk->ResetCommandBuffer(slot->cmd, 0);
   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (fg->vk->BeginCommandBuffer(slot->cmd, &bi) != VK_SUCCESS) {
      fg->broken = true;
      return;
   }
   VkCommandBuffer cmd = slot->cmd;

   if (!fg->base_ready) {
      fg_image_barrier(fg, cmd, fg->base, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
      const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      fg->vk->CmdClearColorImage(cmd, fg->base, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &range);
      fg_image_barrier(fg, cmd, fg->base, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      fg->base_ready = true;
   }

   /* The game's image, presented by the submit before this one on the same queue: GENERAL for
    * WFG's copy, PRESENT_SRC again after it. */
   fg_image_barrier(fg, cmd, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
   vkr_dis_process(fg->dis, cmd, source, fg->width, fg->height, planned);
   fg_image_barrier(fg, cmd, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);

   VkSemaphore waits[WL_DIRECT_MAX_IMAGES];
   VkPipelineStageFlags wait_stages[WL_DIRECT_MAX_IMAGES];
   uint32_t wait_count = 0;
   uint32_t made = 0;
   for (uint32_t o = 0; o < fg->output_count && made < planned; o++) {
      if (!(free_mask & (1u << o)))
         continue;
      if (!vkr_dis_generate_into(fg->dis, cmd, made, fg->fbs[o], fg->width, fg->height))
         break;
      if (fg_import_wait(fg, o, release_fds[o])) {
         waits[wait_count] = fg->waits[o];
         wait_stages[wait_count++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      }
      release_fds[o] = -1;
      res->outputs[made++] = o;
   }

   if (fg->vk->EndCommandBuffer(cmd) != VK_SUCCESS) {
      fg->broken = true;
      return;
   }
   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = wait_count,
      .pWaitSemaphores = waits,
      .pWaitDstStageMask = wait_stages,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &fg->signal,
   };
   VkResult r = fg->vk->QueueSubmit(queue->dispatch_handle, 1, &si, slot->fence);
   if (r != VK_SUCCESS) {
      WRAPPER_LOG(error, "WFG: submit failed (%d); frame generation off for this swapchain", r);
      fg->broken = true;
      return;
   }
   slot->submitted = true;
   fg->ring_next = (fg->ring_next + 1) % FG_RING;

   const VkSemaphoreGetFdInfoKHR gi = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = fg->signal,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   int fd = -1;
   if (fg->vk->GetSemaphoreFdKHR(fg->dev, &gi, &fd) != VK_SUCCESS || fd < 0) {
      /* No sync file: this batch is waited for on the CPU (the swapchain then has nothing to
       * wait for), and none comes after it, as the semaphore may still be signaled. */
      fg->vk->WaitForFences(fg->dev, 1, &slot->fence, VK_TRUE, UINT64_MAX);
      WRAPPER_LOG(error, "WFG: no sync file from the driver; frame generation off for this swapchain");
      fg->broken = true;
      fd = -1;
   }
   res->fence_fd = fd;
   res->count = made;
   res->state = WL_FG_STATE_ACTIVE;
}

const struct wsi_wl_fg_ops wrapper_wl_fg_ops = {
   .create = wrapper_fg_create,
   .destroy = wrapper_fg_destroy,
   .frame = wrapper_fg_frame,
};
