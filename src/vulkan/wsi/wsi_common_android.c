/*
 * AHardwareBuffer backed swapchain images.
 *
 * Used by the Vulkan wrapper on Android: the X11 swapchain images are
 * allocated as AHardwareBuffers, imported into the underlying Vulkan driver
 * through VK_ANDROID_external_memory_android_hardware_buffer, and handed to
 * the emulator's X server over a unix socket (see x11_image_init()).
 *
 * SPDX-License-Identifier: MIT
 */

#include "wsi_common.h"
#include "wsi_common_private.h"
#include "vk_alloc.h"
#include "util/log.h"
#include "util/macros.h"

#include <android/hardware_buffer.h>
#include <assert.h>

/* HAL_PIXEL_FORMAT_BGRA_8888, not part of the public NDK enum */
#define WSI_AHB_FORMAT_B8G8R8A8_UNORM 5

/* Modifier value the emulator's DRI3 implementation expects for
 * AHardwareBuffer backed pixmaps. */
#define WSI_AHB_DRM_MODIFIER 1255

enum wsi_swapchain_blit_type
wsi_get_android_blit_type(const struct wsi_device *wsi,
                          const struct wsi_base_image_params *params,
                          VkDevice device)
{
   if (wsi->needs_blit)
      return WSI_SWAPCHAIN_IMAGE_BLIT;

   return WSI_SWAPCHAIN_NO_BLIT;
}

static VkResult
wsi_get_ahardware_buffer_properties(const struct wsi_swapchain *chain,
                                    struct wsi_image *image,
                                    VkAndroidHardwareBufferPropertiesANDROID *props,
                                    VkAndroidHardwareBufferFormatPropertiesANDROID *format_props)
{
   const struct wsi_device *wsi = chain->wsi;

   *format_props = (VkAndroidHardwareBufferFormatPropertiesANDROID) {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
   };
   *props = (VkAndroidHardwareBufferPropertiesANDROID) {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      .pNext = format_props,
   };

   if (wsi->GetAndroidHardwareBufferPropertiesANDROID == NULL) {
      mesa_loge("wsi: driver lacks vkGetAndroidHardwareBufferPropertiesANDROID");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   VkResult result =
      wsi->GetAndroidHardwareBufferPropertiesANDROID(chain->device,
                                                     image->ahardware_buffer,
                                                     props);
   if (result != VK_SUCCESS)
      mesa_loge("wsi: failed to get AHardwareBuffer properties (%d)", result);

   return result;
}

/* Direct rendering: the presented VkImage is bound to the AHardwareBuffer. */
static VkResult
wsi_create_ahardware_buffer_image_mem(const struct wsi_swapchain *chain,
                                      const struct wsi_image_info *info,
                                      struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   VkAndroidHardwareBufferFormatPropertiesANDROID format_props;
   VkAndroidHardwareBufferPropertiesANDROID props;
   result = wsi_get_ahardware_buffer_properties(chain, image, &props,
                                                &format_props);
   if (result != VK_SUCCESS)
      return result;

   /* Images with an external format can't be mutable, so recreate the image
    * accordingly before binding the AHardwareBuffer memory.
    */
   VkImageCreateInfo image_create_info = info->create;
   if (format_props.externalFormat)
      image_create_info.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

   VkImage new_image;
   result = wsi->CreateImage(chain->device, &image_create_info,
                             &chain->alloc, &new_image);
   if (result != VK_SUCCESS) {
      mesa_loge("wsi: failed to create AHardwareBuffer image (%d)", result);
      return result;
   }

   wsi->DestroyImage(chain->device, image->image, &chain->alloc);
   image->image = new_image;

   const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkImportAndroidHardwareBufferInfoANDROID import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
      .pNext = &memory_dedicated_info,
      .buffer = image->ahardware_buffer,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = props.allocationSize,
      .memoryTypeIndex =
         wsi_select_device_memory_type(wsi, props.memoryTypeBits),
   };

   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS) {
      mesa_loge("wsi: failed to import AHardwareBuffer memory (%d)", result);
      return result;
   }

   image->num_planes = 1;
   image->drm_modifier = WSI_AHB_DRM_MODIFIER;

   return VK_SUCCESS;
}

/* Blit path: the application renders into a regular VkImage which gets
 * blitted into a linear image bound to the AHardwareBuffer. */
static VkResult
wsi_create_ahardware_buffer_blit_context(const struct wsi_swapchain *chain,
                                         const struct wsi_image_info *info,
                                         struct wsi_image *image)
{
   assert(chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT);
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   VkAndroidHardwareBufferFormatPropertiesANDROID format_props;
   VkAndroidHardwareBufferPropertiesANDROID props;
   result = wsi_get_ahardware_buffer_properties(chain, image, &props,
                                                &format_props);
   if (result != VK_SUCCESS)
      return result;

   const VkExternalMemoryImageCreateInfo external_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   const VkImageCreateInfo blit_image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external_memory_info,
      .flags = 0u,
      .extent = info->create.extent,
      .format = format_props.format,
      .imageType = VK_IMAGE_TYPE_2D,
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = info->create.queueFamilyIndexCount,
      .pQueueFamilyIndices = info->create.pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = wsi->CreateImage(chain->device, &blit_image_info,
                             &chain->alloc, &image->blit.image);
   if (result != VK_SUCCESS) {
      mesa_loge("wsi: failed to create blit image (%d)", result);
      return result;
   }

   const VkMemoryDedicatedAllocateInfo blit_mem_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->blit.image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkImportAndroidHardwareBufferInfoANDROID import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
      .pNext = &blit_mem_dedicated_info,
      .buffer = image->ahardware_buffer,
   };
   const VkMemoryAllocateInfo blit_mem_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = props.allocationSize,
      .memoryTypeIndex =
         wsi_select_device_memory_type(wsi, props.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &blit_mem_info,
                                &chain->alloc, &image->blit.memory);
   if (result != VK_SUCCESS) {
      mesa_loge("wsi: failed to import AHardwareBuffer blit memory (%d)",
                result);
      return result;
   }

   result = wsi->BindImageMemory(chain->device, image->blit.image,
                                 image->blit.memory, 0);
   if (result != VK_SUCCESS) {
      mesa_loge("wsi: failed to bind blit image memory (%d)", result);
      return result;
   }

   /* Memory for the image the application renders into. */
   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex =
         wsi_select_device_memory_type(wsi, reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS) {
      mesa_loge("wsi: failed to allocate swapchain image memory (%d)", result);
      return result;
   }

   image->num_planes = 1;
   image->drm_modifier = WSI_AHB_DRM_MODIFIER;

   return VK_SUCCESS;
}

static uint32_t
wsi_vk_format_to_ahardware_buffer_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return WSI_AHB_FORMAT_B8G8R8A8_UNORM;
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
   default:
      return 0;
   }
}

VkResult
wsi_configure_android_image(const struct wsi_swapchain *chain,
                            const VkSwapchainCreateInfoKHR *pCreateInfo,
                            const struct wsi_base_image_params *params,
                            struct wsi_image_info *info)
{
   assert(params->image_type == WSI_IMAGE_TYPE_ANDROID);
   assert(chain->blit.type == WSI_SWAPCHAIN_NO_BLIT ||
          chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT);

   const bool blit = chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT;
   const VkExternalMemoryHandleTypeFlags handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

   VkResult result = wsi_configure_image(chain, pCreateInfo,
                                         blit ? 0 : handle_type, info);
   if (result != VK_SUCCESS)
      return result;

   /* wsi_configure_image() clears the whole info struct. */
   info->image_type = WSI_IMAGE_TYPE_ANDROID;

   const uint32_t ahb_format = blit ?
      AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM :
      wsi_vk_format_to_ahardware_buffer_format(info->create.format);
   if (ahb_format == 0) {
      mesa_loge("wsi: swapchain format %d can't be backed by an "
                "AHardwareBuffer", info->create.format);
      wsi_destroy_image_info(chain, info);
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   info->ahardware_buffer_desc =
      vk_zalloc(&chain->alloc, sizeof(AHardwareBuffer_Desc), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!info->ahardware_buffer_desc) {
      wsi_destroy_image_info(chain, info);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *info->ahardware_buffer_desc = (AHardwareBuffer_Desc) {
      .width = pCreateInfo->imageExtent.width,
      .height = pCreateInfo->imageExtent.height,
      .layers = pCreateInfo->imageArrayLayers,
      .format = ahb_format,
      .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
               AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
               AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
   };

   if (blit) {
      wsi_configure_image_blit_image(chain, info);
      info->create_mem = wsi_create_ahardware_buffer_blit_context;
   } else {
      info->create_mem = wsi_create_ahardware_buffer_image_mem;
   }

   return VK_SUCCESS;
}
