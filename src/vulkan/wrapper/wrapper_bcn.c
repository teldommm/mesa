/*
 * BCn texture support for Vulkan drivers that lack it (WRAPPER_EMULATE_BCN).
 *
 * Ported from the Mesa 25 based wrapper (wrapper_bcdec.c and the BCn parts
 * of wrapper_device.c / wrapper_physical_device.c) and reworked:
 *
 *  - format support is decided per format: a BCn format the driver samples
 *    natively is left alone in auto mode;
 *  - emulated images are created with an uncompressed format (RGBA8 for
 *    BC1-3/7, R8 / RG8 for BC4 / BC5, RGBA16F for BC6H); format lists and
 *    image views are translated without touching the application's structs;
 *  - format / image format properties of emulated formats are the driver's
 *    properties of the uncompressed format, restricted to what a compressed
 *    format can do; vkGetPhysicalDeviceFormatProperties2 (used by DXVK 2.x)
 *    is handled too;
 *  - vkCmdCopyBufferToImage and vkCmdCopyBufferToImage2 decompress the
 *    source data into one staging buffer per call, honouring
 *    bufferRowLength / bufferImageHeight, array layers / depth and partial
 *    edge blocks (the old code wrote past the end of the staging buffer for
 *    mips smaller than 4x4);
 *  - staging buffers belong to the command buffer that uses them and are
 *    released when it is begun again, reset or freed (the old code tied
 *    them to the fence of the previous submission and leaked them without
 *    one);
 *  - the source data is read through the application's own mapping when the
 *    memory is mapped (DXVK keeps its staging memory persistently mapped,
 *    a second vkMapMemory would fail).
 *
 * Limitation (as before): the data is decompressed when the copy is
 * recorded, so it has to be in the buffer by then.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "vk_alloc.h"
#include "vk_util.h"
#include "c11/threads.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_math.h"

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "util/xxhash.h"

#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#define WRAPPER_BCN_ALL_FORMATS \
   ((1u << (WRAPPER_BCN_LAST_FORMAT - WRAPPER_BCN_FIRST_FORMAT + 1)) - 1)

/* What an (emulated) compressed format can be used for */
#define WRAPPER_BCN_FORMAT_FEATURES \
   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
    VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

#define WRAPPER_BCN_IMAGE_USAGE \
   (VK_IMAGE_USAGE_SAMPLED_BIT | \
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
    VK_IMAGE_USAGE_TRANSFER_DST_BIT)

#define WRAPPER_BCN_MAX_VIEW_FORMATS 16

struct wrapper_bcn_image {
   VkFormat format;        /* the application's BCn format */
   uint32_t array_layers;
};

struct wrapper_bcn_buffer {
   VkDeviceMemory memory;
   VkDeviceSize offset;
};

struct wrapper_bcn_staging {
   struct list_head link;
   VkBuffer buffer;
   VkDeviceMemory memory;
};

/* ------------------------------------------------------------------------ */
/* Formats                                                                  */
/* ------------------------------------------------------------------------ */

static VkFormat
bcn_decompressed_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      return VK_FORMAT_R8G8B8A8_SRGB;
   case VK_FORMAT_BC4_UNORM_BLOCK:
      return VK_FORMAT_R8_UNORM;
   case VK_FORMAT_BC4_SNORM_BLOCK:
      return VK_FORMAT_R8_SNORM;
   case VK_FORMAT_BC5_UNORM_BLOCK:
      return VK_FORMAT_R8G8_UNORM;
   case VK_FORMAT_BC5_SNORM_BLOCK:
      return VK_FORMAT_R8G8_SNORM;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      /* RGB16F is rarely sampleable on mobile GPUs, RGBA16F always is */
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   default:
      return VK_FORMAT_R8G8B8A8_UNORM;
   }
}

static uint32_t
bcn_texel_size(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
      return 1;
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
      return 2;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      return 8;
   default:
      return 4;
   }
}

static uint32_t
bcn_block_size(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
      return 8;
   default:
      return 16;
   }
}

static VkFormat
bcn_translate_format(const struct wrapper_physical_device *pdevice,
                     VkFormat format)
{
   return wrapper_bcn_is_emulated(pdevice, format) ?
      bcn_decompressed_format(format) : format;
}

/* ------------------------------------------------------------------------ */
/* Physical device                                                          */
/* ------------------------------------------------------------------------ */

static const char *
bcn_mode_name(enum wrapper_bcn_mode mode)
{
   switch (mode) {
   case WRAPPER_BCN_OFF:  return "off";
   case WRAPPER_BCN_FAKE: return "report only";
   case WRAPPER_BCN_FULL: return "full";
   default:               return "auto";
   }
}

void
wrapper_bcn_setup_physical_device(struct wrapper_physical_device *pdevice)
{
   const VkDriverId driver_id = pdevice->driver_properties.driverID;
   const uint32_t driver_version = pdevice->properties2.properties.driverVersion;
   int64_t env = debug_get_num_option("WRAPPER_EMULATE_BCN", WRAPPER_BCN_AUTO);
   enum wrapper_bcn_mode mode =
      (env < WRAPPER_BCN_OFF || env > WRAPPER_BCN_AUTO) ? WRAPPER_BCN_AUTO :
                                                          (enum wrapper_bcn_mode)env;

   /* Formats the driver samples by itself */
   uint32_t native = 0;
   for (VkFormat f = WRAPPER_BCN_FIRST_FORMAT; f <= WRAPPER_BCN_LAST_FORMAT; f++) {
      VkFormatProperties props = { 0 };
      pdevice->dispatch_table.GetPhysicalDeviceFormatProperties(
         pdevice->dispatch_handle, f, &props);
      if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
         native |= wrapper_bcn_format_bit(f);
   }

   if (mode == WRAPPER_BCN_AUTO) {
      if (driver_id == VK_DRIVER_ID_MESA_TURNIP) {
         mode = WRAPPER_BCN_OFF;
      } else if (driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
                 driver_version >= VK_MAKE_VERSION(512, 530, 0)) {
         /* The hardware decodes BCn, newer drivers just may not say so */
         mode = pdevice->base_supported_features.textureCompressionBC ?
            WRAPPER_BCN_OFF : WRAPPER_BCN_FAKE;
      }
   }

   switch (mode) {
   case WRAPPER_BCN_OFF:
      pdevice->bcn_fake_mask = 0;
      pdevice->bcn_emulate_mask = 0;
      break;
   case WRAPPER_BCN_FAKE:
      pdevice->bcn_fake_mask = WRAPPER_BCN_ALL_FORMATS & ~native;
      pdevice->bcn_emulate_mask = 0;
      break;
   case WRAPPER_BCN_FULL:
      pdevice->bcn_fake_mask = WRAPPER_BCN_ALL_FORMATS;
      pdevice->bcn_emulate_mask = WRAPPER_BCN_ALL_FORMATS;
      break;
   case WRAPPER_BCN_AUTO:
      /* Covers the old Qualcomm drivers (< 512.530), Mali, PowerVR, and
       * Xclipse, where only the BCn formats the driver lacks (usually
       * BC4-7) get decompressed. */
      pdevice->bcn_fake_mask = WRAPPER_BCN_ALL_FORMATS & ~native;
      pdevice->bcn_emulate_mask = WRAPPER_BCN_ALL_FORMATS & ~native;
      break;
   }

   pdevice->bcn_mode = mode;

   /* Masked with what the driver supports in vkCreateDevice */
   if (mode != WRAPPER_BCN_OFF)
      pdevice->vk.supported_features.textureCompressionBC = true;

   /* Host image copies would bypass the decompression */
   if (pdevice->bcn_emulate_mask &&
       pdevice->vk.supported_extensions.EXT_host_image_copy) {
      WRAPPER_LOG(info, "BCn: hiding VK_EXT_host_image_copy");
      pdevice->vk.supported_extensions.EXT_host_image_copy = false;
      pdevice->vk.supported_features.hostImageCopy = false;
   }

   WRAPPER_LOG(info, "BCn: mode %s, native 0x%04x, reported 0x%04x, decompressed 0x%04x",
               bcn_mode_name(mode), native, pdevice->bcn_fake_mask,
               pdevice->bcn_emulate_mask);
}

static void
bcn_fill_fake_format_properties(VkFormatProperties *props)
{
   props->optimalTilingFeatures |= WRAPPER_BCN_FORMAT_FEATURES;
}

static VkFormatFeatureFlags
bcn_mask_features(VkFormatFeatureFlags features)
{
   return features & WRAPPER_BCN_FORMAT_FEATURES;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                          VkFormat format,
                                          VkFormatProperties *pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   const uint32_t bit = wrapper_bcn_format_bit(format);

   if (bit & pdevice->bcn_emulate_mask) {
      pdevice->dispatch_table.GetPhysicalDeviceFormatProperties(
         pdevice->dispatch_handle, bcn_decompressed_format(format),
         pFormatProperties);
      pFormatProperties->linearTilingFeatures =
         bcn_mask_features(pFormatProperties->linearTilingFeatures);
      pFormatProperties->optimalTilingFeatures =
         bcn_mask_features(pFormatProperties->optimalTilingFeatures);
      pFormatProperties->bufferFeatures = 0;
      return;
   }

   pdevice->dispatch_table.GetPhysicalDeviceFormatProperties(
      pdevice->dispatch_handle, format, pFormatProperties);

   if (bit & pdevice->bcn_fake_mask)
      bcn_fill_fake_format_properties(pFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                           VkFormat format,
                                           VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   const uint32_t bit = wrapper_bcn_format_bit(format);
   const bool emulated = (bit & pdevice->bcn_emulate_mask) != 0;
   const bool fake = !emulated && (bit & pdevice->bcn_fake_mask) != 0;

   pdevice->dispatch_table.GetPhysicalDeviceFormatProperties2(
      pdevice->dispatch_handle,
      emulated ? bcn_decompressed_format(format) : format,
      pFormatProperties);

   if (!emulated && !fake)
      return;

   VkFormatProperties *props = &pFormatProperties->formatProperties;
   if (emulated) {
      props->linearTilingFeatures = bcn_mask_features(props->linearTilingFeatures);
      props->optimalTilingFeatures = bcn_mask_features(props->optimalTilingFeatures);
      props->bufferFeatures = 0;
   } else {
      bcn_fill_fake_format_properties(props);
   }

   vk_foreach_struct(ext, pFormatProperties->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3: {
         VkFormatProperties3 *props3 = (void *)ext;
         if (emulated) {
            props3->linearTilingFeatures &= WRAPPER_BCN_FORMAT_FEATURES;
            props3->optimalTilingFeatures &= WRAPPER_BCN_FORMAT_FEATURES;
            props3->bufferFeatures = 0;
         } else {
            props3->optimalTilingFeatures |= WRAPPER_BCN_FORMAT_FEATURES;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT: {
         VkDrmFormatModifierPropertiesListEXT *list = (void *)ext;
         list->drmFormatModifierCount = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT: {
         VkDrmFormatModifierPropertiesList2EXT *list = (void *)ext;
         list->drmFormatModifierCount = 0;
         break;
      }
      default:
         break;
      }
   }
}

/* Synthetic properties for "report only" formats, the driver would refuse
 * the query. */
static VkResult
bcn_fake_image_format_properties(const struct wrapper_physical_device *pdevice,
                                 VkImageType type, VkImageTiling tiling,
                                 VkImageUsageFlags usage, VkImageCreateFlags flags,
                                 VkImageFormatProperties *props)
{
   const VkPhysicalDeviceLimits *limits = &pdevice->properties2.properties.limits;

   if (usage & ~WRAPPER_BCN_IMAGE_USAGE)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (tiling != VK_IMAGE_TILING_OPTIMAL)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   memset(props, 0, sizeof(*props));

   switch (type) {
   case VK_IMAGE_TYPE_1D:
      props->maxExtent = (VkExtent3D) { limits->maxImageDimension1D, 1, 1 };
      props->maxArrayLayers = limits->maxImageArrayLayers;
      break;
   case VK_IMAGE_TYPE_2D: {
      const uint32_t dim = (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ?
         limits->maxImageDimensionCube : limits->maxImageDimension2D;
      props->maxExtent = (VkExtent3D) { dim, dim, 1 };
      props->maxArrayLayers = limits->maxImageArrayLayers;
      break;
   }
   case VK_IMAGE_TYPE_3D: {
      const uint32_t dim = limits->maxImageDimension3D;
      props->maxExtent = (VkExtent3D) { dim, dim, dim };
      props->maxArrayLayers = 1;
      break;
   }
   default:
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   uint32_t max_dim = MAX3(props->maxExtent.width, props->maxExtent.height,
                           props->maxExtent.depth);
   props->maxMipLevels = util_logbase2(max_dim) + 1;
   props->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->maxResourceSize = 1ull << 31;

   return VK_SUCCESS;
}

static bool
bcn_usage_supported(VkImageUsageFlags usage, VkImageTiling tiling)
{
   return !(usage & ~WRAPPER_BCN_IMAGE_USAGE) &&
          tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
}

static VkImageCreateFlags
bcn_translate_image_flags(VkImageCreateFlags flags)
{
   /* Only valid with a compressed format */
   return flags & ~(VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT |
                    VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                    VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                    VK_IMAGE_CREATE_SPARSE_ALIASED_BIT);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
                                               VkFormat format,
                                               VkImageType type,
                                               VkImageTiling tiling,
                                               VkImageUsageFlags usage,
                                               VkImageCreateFlags flags,
                                               VkImageFormatProperties *pImageFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   const uint32_t bit = wrapper_bcn_format_bit(format);

   if (bit & pdevice->bcn_emulate_mask) {
      if (!bcn_usage_supported(usage, tiling))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      return pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties(
         pdevice->dispatch_handle, bcn_decompressed_format(format), type,
         tiling, usage, bcn_translate_image_flags(flags),
         pImageFormatProperties);
   }

   if (bit & pdevice->bcn_fake_mask)
      return bcn_fake_image_format_properties(pdevice, type, tiling, usage,
                                              flags, pImageFormatProperties);

   return pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties(
      pdevice->dispatch_handle, format, type, tiling, usage, flags,
      pImageFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                                const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
                                                VkImageFormatProperties2 *pImageFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   const uint32_t bit = wrapper_bcn_format_bit(pImageFormatInfo->format);

   if (bit & pdevice->bcn_emulate_mask) {
      if (!bcn_usage_supported(pImageFormatInfo->usage, pImageFormatInfo->tiling))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;

      VkPhysicalDeviceImageFormatInfo2 info = *pImageFormatInfo;
      info.format = bcn_decompressed_format(pImageFormatInfo->format);
      info.flags = bcn_translate_image_flags(pImageFormatInfo->flags);
      return pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties2(
         pdevice->dispatch_handle, &info, pImageFormatProperties);
   }

   if (bit & pdevice->bcn_fake_mask) {
      /* External memory, YCbCr, ... aren't supported for these. */
      if (pImageFormatInfo->pNext != NULL)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      return bcn_fake_image_format_properties(pdevice, pImageFormatInfo->type,
                                              pImageFormatInfo->tiling,
                                              pImageFormatInfo->usage,
                                              pImageFormatInfo->flags,
                                              &pImageFormatProperties->imageFormatProperties);
   }

   return pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties2(
      pdevice->dispatch_handle, pImageFormatInfo, pImageFormatProperties);
}

/* ------------------------------------------------------------------------ */
/* Device                                                                   */
/* ------------------------------------------------------------------------ */

VkResult
wrapper_bcn_device_init(struct wrapper_device *device)
{
   device->bcn_active = device->physical->bcn_emulate_mask != 0 &&
                        device->vk.enabled_features.textureCompressionBC;
   if (!device->bcn_active)
      return VK_SUCCESS;

   device->bcn_image_table = _mesa_hash_table_u64_create(NULL);
   device->bcn_buffer_table = _mesa_hash_table_u64_create(NULL);
   device->map_table = _mesa_hash_table_u64_create(NULL);
   if (!device->bcn_image_table || !device->bcn_buffer_table ||
       !device->map_table) {
      wrapper_bcn_device_finish(device);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   WRAPPER_LOG(info, "BCn: CPU decompression enabled");
   return VK_SUCCESS;
}

static void
bcn_free_table(struct wrapper_device *device, struct hash_table_u64 *table)
{
   if (!table)
      return;
   hash_table_u64_foreach(table, entry)
      vk_free(&device->vk.alloc, entry.data);
   _mesa_hash_table_u64_destroy(table);
}

/* After the command buffers were destroyed (their staging is released then) */
void
wrapper_bcn_device_finish(struct wrapper_device *device)
{
   bcn_free_table(device, device->bcn_image_table);
   bcn_free_table(device, device->bcn_buffer_table);
   if (device->map_table)
      _mesa_hash_table_u64_destroy(device->map_table);
   device->bcn_image_table = NULL;
   device->bcn_buffer_table = NULL;
   device->map_table = NULL;
   device->bcn_active = false;
}

void
wrapper_bcn_track_map(struct wrapper_device *device, VkDeviceMemory memory,
                      VkDeviceSize offset, void *ptr)
{
   if (!device->bcn_active || ptr == NULL)
      return;

   simple_mtx_lock(&device->resource_mutex);
   _mesa_hash_table_u64_insert(device->map_table, (uint64_t)(uintptr_t)memory,
                               (char *)ptr - offset);
   simple_mtx_unlock(&device->resource_mutex);
}

void
wrapper_bcn_untrack_map(struct wrapper_device *device, VkDeviceMemory memory)
{
   if (!device->bcn_active)
      return;

   simple_mtx_lock(&device->resource_mutex);
   _mesa_hash_table_u64_remove(device->map_table, (uint64_t)(uintptr_t)memory);
   simple_mtx_unlock(&device->resource_mutex);
}

/* Called with resource_mutex held */
void
wrapper_bcn_release_staging(struct wrapper_command_buffer *wcb)
{
   struct wrapper_device *device = wcb->device;

   if (!wcb->on_staging_list)
      return;

   list_for_each_entry_safe(struct wrapper_bcn_staging, staging,
                            &wcb->staging_list, link) {
      device->dispatch_table.DestroyBuffer(device->dispatch_handle,
                                           staging->buffer, NULL);
      device->dispatch_table.FreeMemory(device->dispatch_handle,
                                        staging->memory, NULL);
      list_del(&staging->link);
      vk_free(&device->vk.alloc, staging);
   }

   list_del(&wcb->staging_link);
   wcb->on_staging_list = false;
}

/* Image format list of an emulated image: the uncompressed formats.  With a
 * single one left the image doesn't need to be mutable. */
static void
bcn_translate_format_list(const struct wrapper_physical_device *pdevice,
                          const VkImageFormatListCreateInfo *list,
                          VkFormat image_format,
                          VkFormat *formats, uint32_t *count)
{
   *count = 0;
   formats[(*count)++] = image_format;

   for (uint32_t i = 0; list && i < list->viewFormatCount; i++) {
      VkFormat f = list->pViewFormats[i];
      if (!wrapper_is_bcn_format(f))
         continue; /* uncompressed block-texel views can't be kept */
      f = bcn_translate_format(pdevice, f);

      bool dup = false;
      for (uint32_t j = 0; j < *count; j++)
         dup |= formats[j] == f;
      if (!dup && *count < WRAPPER_BCN_MAX_VIEW_FORMATS)
         formats[(*count)++] = f;
   }
}

/* Translates an emulated image's create info.  The format list struct of
 * the application is patched in place and restored by bcn_restore_image(). */
struct bcn_image_patch {
   VkImageFormatListCreateInfo *list;
   uint32_t saved_count;
   const VkFormat *saved_formats;
   VkFormat formats[WRAPPER_BCN_MAX_VIEW_FORMATS];
};

static void
bcn_patch_image(const struct wrapper_physical_device *pdevice,
                const VkImageCreateInfo *in, VkImageCreateInfo *out,
                struct bcn_image_patch *patch)
{
   *out = *in;
   out->format = bcn_decompressed_format(in->format);
   out->flags = bcn_translate_image_flags(in->flags);

   patch->list = (VkImageFormatListCreateInfo *)
      vk_find_struct_const(in->pNext, IMAGE_FORMAT_LIST_CREATE_INFO);

   if (!(in->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
      /* Not mutable: a format list may only name the image format */
      if (patch->list) {
         patch->saved_count = patch->list->viewFormatCount;
         patch->saved_formats = patch->list->pViewFormats;
         patch->formats[0] = out->format;
         patch->list->viewFormatCount = patch->list->viewFormatCount ? 1 : 0;
         patch->list->pViewFormats = patch->formats;
      }
      return;
   }

   if (!patch->list)
      return; /* stays mutable, views are translated */

   uint32_t count;
   bcn_translate_format_list(pdevice, patch->list, out->format,
                             patch->formats, &count);
   patch->saved_count = patch->list->viewFormatCount;
   patch->saved_formats = patch->list->pViewFormats;
   patch->list->viewFormatCount = count;
   patch->list->pViewFormats = patch->formats;
   if (count <= 1)
      out->flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
}

static void
bcn_restore_image(struct bcn_image_patch *patch)
{
   if (!patch->list)
      return;
   patch->list->viewFormatCount = patch->saved_count;
   patch->list->pViewFormats = patch->saved_formats;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateImage(VkDevice _device,
                    const VkImageCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImage *pImage)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (!device->bcn_active ||
       !wrapper_bcn_is_emulated(device->physical, pCreateInfo->format)) {
      return device->dispatch_table.CreateImage(device->dispatch_handle,
                                                pCreateInfo, pAllocator, pImage);
   }

   struct wrapper_bcn_image *record =
      vk_alloc(&device->vk.alloc, sizeof(*record), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!record)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkImageCreateInfo create_info;
   struct bcn_image_patch patch = { 0 };
   bcn_patch_image(device->physical, pCreateInfo, &create_info, &patch);

   VkResult result = device->dispatch_table.CreateImage(device->dispatch_handle,
                                                        &create_info,
                                                        pAllocator, pImage);
   bcn_restore_image(&patch);

   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "BCn: failed to create %ux%u image (format %d -> %d), res %d",
                  pCreateInfo->extent.width, pCreateInfo->extent.height,
                  pCreateInfo->format, create_info.format, result);
      vk_free(&device->vk.alloc, record);
      return result;
   }

   record->format = pCreateInfo->format;
   record->array_layers = pCreateInfo->arrayLayers;

   WRAPPER_LOG(bcn, "Emulating %ux%ux%u BCn image %d as %d (mips %u, layers %u)",
               pCreateInfo->extent.width, pCreateInfo->extent.height,
               pCreateInfo->extent.depth, pCreateInfo->format,
               create_info.format, pCreateInfo->mipLevels,
               pCreateInfo->arrayLayers);

   simple_mtx_lock(&device->resource_mutex);
   _mesa_hash_table_u64_insert(device->bcn_image_table,
                               (uint64_t)(uintptr_t)*pImage, record);
   simple_mtx_unlock(&device->resource_mutex);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyImage(VkDevice _device, VkImage image,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (device->bcn_active && image != VK_NULL_HANDLE) {
      simple_mtx_lock(&device->resource_mutex);
      struct wrapper_bcn_image *record =
         _mesa_hash_table_u64_search(device->bcn_image_table,
                                     (uint64_t)(uintptr_t)image);
      if (record)
         _mesa_hash_table_u64_remove(device->bcn_image_table,
                                     (uint64_t)(uintptr_t)image);
      simple_mtx_unlock(&device->resource_mutex);
      vk_free(&device->vk.alloc, record);
   }

   device->dispatch_table.DestroyImage(device->dispatch_handle, image, pAllocator);
}

static bool
bcn_lookup_image(struct wrapper_device *device, VkImage image,
                 struct wrapper_bcn_image *out)
{
   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_bcn_image *record =
      _mesa_hash_table_u64_search(device->bcn_image_table,
                                  (uint64_t)(uintptr_t)image);
   if (record && out)
      *out = *record;
   simple_mtx_unlock(&device->resource_mutex);
   return record != NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateImageView(VkDevice _device,
                        const VkImageViewCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkImageView *pView)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkImageViewCreateInfo create_info;
   const VkImageViewCreateInfo *info = pCreateInfo;

   if (device->bcn_active && wrapper_is_bcn_format(pCreateInfo->format) &&
       bcn_lookup_image(device, pCreateInfo->image, NULL)) {
      create_info = *pCreateInfo;
      create_info.format = bcn_translate_format(device->physical,
                                                pCreateInfo->format);
      info = &create_info;
   }

   VkResult result = device->dispatch_table.CreateImageView(device->dispatch_handle,
                                                            info, pAllocator, pView);
   if (result != VK_SUCCESS)
      WRAPPER_LOG(error, "Failed to create image view (format %d), res %d",
                  info->format, result);
   return result;
}

/* vkGetDeviceImageMemoryRequirements & co take an image create info too */
VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceImageMemoryRequirements(VkDevice _device,
                                         const VkDeviceImageMemoryRequirements *pInfo,
                                         VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (!device->bcn_active || !pInfo->pCreateInfo ||
       !wrapper_bcn_is_emulated(device->physical, pInfo->pCreateInfo->format)) {
      device->dispatch_table.GetDeviceImageMemoryRequirements(
         device->dispatch_handle, pInfo, pMemoryRequirements);
      return;
   }

   VkImageCreateInfo create_info;
   struct bcn_image_patch patch = { 0 };
   bcn_patch_image(device->physical, pInfo->pCreateInfo, &create_info, &patch);

   VkDeviceImageMemoryRequirements info = *pInfo;
   info.pCreateInfo = &create_info;
   device->dispatch_table.GetDeviceImageMemoryRequirements(
      device->dispatch_handle, &info, pMemoryRequirements);
   bcn_restore_image(&patch);
}

static void
bcn_track_buffer(struct wrapper_device *device, VkBuffer buffer,
                 VkDeviceMemory memory, VkDeviceSize offset)
{
   struct wrapper_bcn_buffer *record =
      vk_alloc(&device->vk.alloc, sizeof(*record), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!record)
      return;
   record->memory = memory;
   record->offset = offset;

   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_bcn_buffer *old =
      _mesa_hash_table_u64_search(device->bcn_buffer_table,
                                  (uint64_t)(uintptr_t)buffer);
   _mesa_hash_table_u64_insert(device->bcn_buffer_table,
                               (uint64_t)(uintptr_t)buffer, record);
   simple_mtx_unlock(&device->resource_mutex);

   vk_free(&device->vk.alloc, old);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_BindBufferMemory(VkDevice _device, VkBuffer buffer,
                         VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   VkResult result = device->dispatch_table.BindBufferMemory(
      device->dispatch_handle, buffer, memory, memoryOffset);

   if (result == VK_SUCCESS && device->bcn_active)
      bcn_track_buffer(device, buffer, memory, memoryOffset);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                          const VkBindBufferMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   VkResult result = device->dispatch_table.BindBufferMemory2(
      device->dispatch_handle, bindInfoCount, pBindInfos);

   if (device->bcn_active) {
      for (uint32_t i = 0; i < bindInfoCount; i++) {
         /* maintenance6: per-bind status when the call fails */
         const VkBindMemoryStatus *status =
            vk_find_struct_const(pBindInfos[i].pNext, BIND_MEMORY_STATUS);
         if (result != VK_SUCCESS &&
             (!status || !status->pResult || *status->pResult != VK_SUCCESS))
            continue;
         bcn_track_buffer(device, pBindInfos[i].buffer, pBindInfos[i].memory,
                          pBindInfos[i].memoryOffset);
      }
   }

   return result;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyBuffer(VkDevice _device, VkBuffer buffer,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (device->bcn_active && buffer != VK_NULL_HANDLE) {
      simple_mtx_lock(&device->resource_mutex);
      struct wrapper_bcn_buffer *record =
         _mesa_hash_table_u64_search(device->bcn_buffer_table,
                                     (uint64_t)(uintptr_t)buffer);
      if (record)
         _mesa_hash_table_u64_remove(device->bcn_buffer_table,
                                     (uint64_t)(uintptr_t)buffer);
      simple_mtx_unlock(&device->resource_mutex);
      vk_free(&device->vk.alloc, record);
   }

   device->dispatch_table.DestroyBuffer(device->dispatch_handle, buffer, pAllocator);
}

/* ------------------------------------------------------------------------ */
/* Decompression                                                            */
/* ------------------------------------------------------------------------ */

struct bcn_decode_job {
   VkFormat format;
   const uint8_t *src;         /* first block of the slice */
   uint8_t *dst;               /* first texel of the slice */
   uint32_t width, height;     /* texels */
   uint32_t src_row_pitch;     /* bytes between block rows */
   uint32_t block_row_start, block_row_count;
};

static uint16_t
bcn_half_one(void)
{
   return 0x3c00;
}

/* Decodes one 4x4 block into a tightly packed RGBA8 / R8 / RG8 / RGBA16F
 * 4x4 tile. */
static void
bcn_decode_block(VkFormat format, const uint8_t *src, uint8_t *tile)
{
   switch (format) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      bcdec_bc1(src, tile, 4 * 4);
      /* No alpha in the RGB variants */
      for (int i = 0; i < 16; i++)
         tile[i * 4 + 3] = 0xff;
      break;
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      bcdec_bc1(src, tile, 4 * 4);
      break;
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
      bcdec_bc2(src, tile, 4 * 4);
      break;
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
      bcdec_bc3(src, tile, 4 * 4);
      break;
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
      bcdec_bc4(src, tile, 4, format == VK_FORMAT_BC4_SNORM_BLOCK);
      break;
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
      bcdec_bc5(src, tile, 4 * 2, format == VK_FORMAT_BC5_SNORM_BLOCK);
      break;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
      uint16_t rgb[4 * 4 * 3];
      uint16_t *rgba = (uint16_t *)tile;
      /* the pitch is in halfs here */
      bcdec_bc6h_half(src, rgb, 4 * 3, format == VK_FORMAT_BC6H_SFLOAT_BLOCK);
      for (int i = 0; i < 16; i++) {
         rgba[i * 4 + 0] = rgb[i * 3 + 0];
         rgba[i * 4 + 1] = rgb[i * 3 + 1];
         rgba[i * 4 + 2] = rgb[i * 3 + 2];
         rgba[i * 4 + 3] = bcn_half_one();
      }
      break;
   }
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      bcdec_bc7(src, tile, 4 * 4);
      break;
   default:
      memset(tile, 0, 4 * 4 * 8);
      break;
   }
}

static void *
bcn_decode_rows(void *data)
{
   const struct bcn_decode_job *job = data;
   const uint32_t texel = bcn_texel_size(job->format);
   const uint32_t block = bcn_block_size(job->format);
   const uint32_t blocks_x = DIV_ROUND_UP(job->width, 4);
   const uint32_t dst_pitch = job->width * texel;
   uint8_t tile[4 * 4 * 8];

   for (uint32_t by = job->block_row_start;
        by < job->block_row_start + job->block_row_count; by++) {
      const uint8_t *src = job->src + (size_t)by * job->src_row_pitch;
      const uint32_t rows = MIN2(4, job->height - by * 4);

      for (uint32_t bx = 0; bx < blocks_x; bx++, src += block) {
         const uint32_t cols = MIN2(4, job->width - bx * 4);
         bcn_decode_block(job->format, src, tile);

         /* Partial blocks at the right / bottom edge are clipped */
         for (uint32_t y = 0; y < rows; y++) {
            memcpy(job->dst + (size_t)(by * 4 + y) * dst_pitch + bx * 4 * texel,
                   tile + y * 4 * texel, cols * texel);
         }
      }
   }

   return NULL;
}

/* Debug aid (WRAPPER_MARK_BCN=1): fill emulated textures with a per format
 * color instead of decoding them. */
static void
bcn_mark(VkFormat format, uint8_t *dst, uint32_t width, uint32_t height)
{
   static const uint8_t colors[][4] = {
      { 0xff, 0xff, 0x00, 0xff }, /* BC1: yellow */
      { 0x00, 0x00, 0xff, 0xff }, /* BC2: blue */
      { 0x00, 0xff, 0xff, 0xff }, /* BC3: light blue */
      { 0xff, 0x00, 0x00, 0xff }, /* BC4: red */
      { 0x00, 0xff, 0x00, 0xff }, /* BC5: green */
      { 0x90, 0x40, 0xa0, 0xff }, /* BC6H: purple */
      { 0xff, 0x00, 0xff, 0xff }, /* BC7: magenta */
   };
   const uint32_t texel = bcn_texel_size(format);
   unsigned idx;

   if (format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
      idx = 0;
   else
      idx = 1 + (format - VK_FORMAT_BC2_UNORM_BLOCK) / 2;
   idx = MIN2(idx, ARRAY_SIZE(colors) - 1);

   for (uint32_t i = 0; i < width * height; i++) {
      uint8_t *p = dst + (size_t)i * texel;
      if (texel == 8) {
         uint16_t *h = (uint16_t *)p;
         h[0] = colors[idx][0] ? 0x3c00 : 0;
         h[1] = colors[idx][1] ? 0x3c00 : 0;
         h[2] = colors[idx][2] ? 0x3c00 : 0;
         h[3] = 0x3c00;
      } else {
         memcpy(p, colors[idx], texel);
      }
   }
}

struct bcn_options {
   bool mark;
   bool no_threads;
   bool use_cache;
   const char *cache_path;
};

static struct bcn_options bcn_options;
static once_flag bcn_options_once = ONCE_FLAG_INIT;

static void
bcn_init_options(void)
{
   bcn_options.mark = debug_get_bool_option("WRAPPER_MARK_BCN", false);
   bcn_options.no_threads = debug_get_bool_option("WRAPPER_NO_BCN_THREAD", false);
   bcn_options.use_cache = debug_get_bool_option("WRAPPER_USE_BCN_CACHE", false);
   bcn_options.cache_path = getenv("WRAPPER_CACHE_PATH");
   if (bcn_options.use_cache &&
       (!bcn_options.cache_path || !*bcn_options.cache_path)) {
      WRAPPER_LOG(info, "BCn: WRAPPER_CACHE_PATH not set, cache disabled");
      bcn_options.use_cache = false;
   }
   if (bcn_options.use_cache) {
      /* The old code created a hardcoded com.winlator.cmod path instead */
      mode_t old_mask = umask(0);
      mkdir(bcn_options.cache_path, 0770);
      umask(old_mask);
   }
}

static const struct bcn_options *
bcn_get_options(void)
{
   call_once(&bcn_options_once, bcn_init_options);
   return &bcn_options;
}

static void
bcn_cache_name(char *buf, size_t size, const struct bcn_options *options,
               uint64_t hash, VkFormat format, uint32_t w, uint32_t h,
               uint32_t slices)
{
   snprintf(buf, size, "%s/%s_%016llx_%d_%ux%ux%u.bcn", options->cache_path,
            get_executable_name(), (unsigned long long)hash, format, w, h,
            slices);
}

static bool
bcn_cache_load(const char *name, uint8_t *dst, size_t size)
{
   FILE *fp = fopen(name, "rb");
   if (!fp)
      return false;
   size_t read = fread(dst, 1, size, fp);
   fclose(fp);
   if (read == size)
      return true;
   unlink(name);
   return false;
}

static void
bcn_cache_store(const char *name, const uint8_t *src, size_t size)
{
   char tmp[PATH_MAX];
   snprintf(tmp, sizeof(tmp), "%s.XXXXXX", name);
   int fd = mkstemp(tmp);
   if (fd < 0)
      return;
   FILE *fp = fdopen(fd, "wb");
   if (!fp) {
      close(fd);
      unlink(tmp);
      return;
   }
   size_t written = fwrite(src, 1, size, fp);
   fclose(fp);
   if (written != size || rename(tmp, name) != 0)
      unlink(tmp);
}

static void
bcn_decode_slice(const struct bcn_options *options, VkFormat format,
                 const uint8_t *src, uint32_t src_row_pitch,
                 uint8_t *dst, uint32_t width, uint32_t height)
{
   const uint32_t blocks_y = DIV_ROUND_UP(height, 4);
   const uint32_t blocks = blocks_y * DIV_ROUND_UP(width, 4);

   struct bcn_decode_job job = {
      .format = format,
      .src = src,
      .dst = dst,
      .width = width,
      .height = height,
      .src_row_pitch = src_row_pitch,
      .block_row_start = 0,
      .block_row_count = blocks_y,
   };

   /* Threads only pay off for big textures (>= 256x256) */
   long cores = sysconf(_SC_NPROCESSORS_ONLN);
   uint32_t num_threads = 1;
   if (!options->no_threads && blocks >= 4096 && cores > 1)
      num_threads = MIN3((uint32_t)cores, 8, blocks_y);

   if (num_threads <= 1) {
      bcn_decode_rows(&job);
      return;
   }

   pthread_t threads[8];
   struct bcn_decode_job jobs[8];
   uint32_t row = 0, started = 0;
   for (uint32_t i = 0; i < num_threads; i++) {
      const uint32_t rows = blocks_y / num_threads + (i < blocks_y % num_threads);
      jobs[i] = job;
      jobs[i].block_row_start = row;
      jobs[i].block_row_count = rows;
      row += rows;
      if (pthread_create(&threads[i], NULL, bcn_decode_rows, &jobs[i]) != 0)
         bcn_decode_rows(&jobs[i]);
      else
         started |= 1u << i;
   }
   for (uint32_t i = 0; i < num_threads; i++) {
      if (started & (1u << i))
         pthread_join(threads[i], NULL);
   }
}

struct bcn_region {
   uint32_t width, height, slices;
   uint32_t src_row_pitch, src_slice_pitch;
   VkDeviceSize src_offset;     /* in the source buffer */
   VkDeviceSize dst_offset;     /* in the staging buffer */
   VkDeviceSize dst_size;
};

static uint32_t
bcn_region_layers(const struct wrapper_bcn_image *image,
                  const VkImageSubresourceLayers *subresource)
{
   if (subresource->layerCount == VK_REMAINING_ARRAY_LAYERS)
      return image->array_layers > subresource->baseArrayLayer ?
         image->array_layers - subresource->baseArrayLayer : 1;
   return subresource->layerCount;
}

static void
bcn_setup_region(const struct wrapper_bcn_image *image,
                 VkDeviceSize buffer_offset, uint32_t row_length,
                 uint32_t image_height, const VkImageSubresourceLayers *subresource,
                 VkExtent3D extent, VkDeviceSize *staging_size,
                 struct bcn_region *region)
{
   const VkFormat format = image->format;
   const uint32_t row_texels = row_length ? row_length : extent.width;
   const uint32_t height_texels = image_height ? image_height : extent.height;

   region->width = extent.width;
   region->height = extent.height;
   region->slices = MAX2(extent.depth, 1) * bcn_region_layers(image, subresource);
   region->src_row_pitch = DIV_ROUND_UP(row_texels, 4) * bcn_block_size(format);
   region->src_slice_pitch = DIV_ROUND_UP(height_texels, 4) * region->src_row_pitch;
   region->src_offset = buffer_offset;
   region->dst_size = (VkDeviceSize)extent.width * extent.height *
                      bcn_texel_size(format) * region->slices;
   /* bufferOffset has to be a multiple of the texel size (and 4) */
   region->dst_offset = align64(*staging_size, 16);
   *staging_size = region->dst_offset + region->dst_size;
}

static void
bcn_decode_region(const struct bcn_options *options,
                  const struct wrapper_bcn_image *image,
                  const uint8_t *src_base, uint8_t *dst_base,
                  const struct bcn_region *region)
{
   const VkFormat format = image->format;
   const size_t dst_slice = (size_t)region->width * region->height *
                            bcn_texel_size(format);
   const uint8_t *src = src_base + region->src_offset;
   uint8_t *dst = dst_base + region->dst_offset;

   if (options->mark) {
      for (uint32_t s = 0; s < region->slices; s++)
         bcn_mark(format, dst + s * dst_slice, region->width, region->height);
      return;
   }

   /* The cache needs the compressed data in one piece */
   const size_t packed_slice = (size_t)DIV_ROUND_UP(region->height, 4) *
                               region->src_row_pitch;
   const bool contiguous =
      region->src_row_pitch == DIV_ROUND_UP(region->width, 4) * bcn_block_size(format) &&
      (region->slices == 1 || region->src_slice_pitch == packed_slice);
   char cache_name[PATH_MAX];
   bool use_cache = options->use_cache && contiguous;

   if (use_cache) {
      const size_t src_size = packed_slice * region->slices;
      bcn_cache_name(cache_name, sizeof(cache_name), options,
                     XXH64(src, src_size, 0), format, region->width,
                     region->height, region->slices);
      if (bcn_cache_load(cache_name, dst, region->dst_size)) {
         WRAPPER_LOG(bcn, "Restored %ux%u BCn %d texture from %s",
                     region->width, region->height, format, cache_name);
         return;
      }
   }

   WRAPPER_LOG(bcn, "Decompressing %ux%ux%u BCn %d texture",
               region->width, region->height, region->slices, format);

   for (uint32_t s = 0; s < region->slices; s++) {
      bcn_decode_slice(options, format, src + (size_t)s * region->src_slice_pitch,
                       region->src_row_pitch, dst + s * dst_slice,
                       region->width, region->height);
   }

   if (use_cache)
      bcn_cache_store(cache_name, dst, region->dst_size);
}

/* ------------------------------------------------------------------------ */
/* Copies                                                                   */
/* ------------------------------------------------------------------------ */

static uint32_t
bcn_staging_memory_type(struct wrapper_device *device, uint32_t type_bits,
                        bool *coherent)
{
   const VkPhysicalDeviceMemoryProperties *props =
      &device->physical->memory_properties;
   const VkMemoryPropertyFlags wanted[] = {
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
   };

   for (unsigned w = 0; w < ARRAY_SIZE(wanted); w++) {
      for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
         if ((type_bits & (1u << i)) &&
             (props->memoryTypes[i].propertyFlags & wanted[w]) == wanted[w]) {
            *coherent = (props->memoryTypes[i].propertyFlags &
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return i;
         }
      }
   }

   return UINT32_MAX;
}

static VkResult
bcn_create_staging(struct wrapper_device *device, VkDeviceSize size,
                   struct wrapper_bcn_staging **out, void **map, bool *coherent)
{
   VkDevice dev = device->dispatch_handle;
   VkResult result;

   struct wrapper_bcn_staging *staging =
      vk_zalloc(&device->vk.alloc, sizeof(*staging), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!staging)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = device->dispatch_table.CreateBuffer(dev, &buffer_info, NULL,
                                                &staging->buffer);
   if (result != VK_SUCCESS)
      goto fail;

   VkMemoryRequirements reqs;
   device->dispatch_table.GetBufferMemoryRequirements(dev, staging->buffer, &reqs);

   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = bcn_staging_memory_type(device, reqs.memoryTypeBits,
                                                 coherent),
   };
   if (alloc_info.memoryTypeIndex == UINT32_MAX) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }

   result = device->dispatch_table.AllocateMemory(dev, &alloc_info, NULL,
                                                  &staging->memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = device->dispatch_table.BindBufferMemory(dev, staging->buffer,
                                                    staging->memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   result = device->dispatch_table.MapMemory(dev, staging->memory, 0,
                                             VK_WHOLE_SIZE, 0, map);
   if (result != VK_SUCCESS)
      goto fail;

   *out = staging;
   return VK_SUCCESS;

fail:
   device->dispatch_table.DestroyBuffer(dev, staging->buffer, NULL);
   device->dispatch_table.FreeMemory(dev, staging->memory, NULL);
   vk_free(&device->vk.alloc, staging);
   return result;
}

static void
bcn_attach_staging(struct wrapper_command_buffer *wcb,
                   struct wrapper_bcn_staging *staging)
{
   struct wrapper_device *device = wcb->device;

   simple_mtx_lock(&device->resource_mutex);
   list_addtail(&staging->link, &wcb->staging_list);
   if (!wcb->on_staging_list) {
      list_addtail(&wcb->staging_link, &device->bcn_staging_cmd_list);
      wcb->on_staging_list = true;
   }
   simple_mtx_unlock(&device->resource_mutex);
}

/* Host address of the source buffer's data.  *temp_memory is set when the
 * memory had to be mapped here (unmap after use). */
static const uint8_t *
bcn_map_source(struct wrapper_device *device, VkBuffer buffer,
               VkDeviceMemory *temp_memory)
{
   *temp_memory = VK_NULL_HANDLE;

   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_bcn_buffer *record =
      _mesa_hash_table_u64_search(device->bcn_buffer_table,
                                  (uint64_t)(uintptr_t)buffer);
   VkDeviceMemory memory = record ? record->memory : VK_NULL_HANDLE;
   VkDeviceSize offset = record ? record->offset : 0;
   uint8_t *base = memory ?
      _mesa_hash_table_u64_search(device->map_table, (uint64_t)(uintptr_t)memory) :
      NULL;
   simple_mtx_unlock(&device->resource_mutex);

   if (!record) {
      WRAPPER_LOG(error, "BCn: source buffer %p has no memory bound",
                  (void *)buffer);
      return NULL;
   }

   if (base)
      return base + offset;

   /* Not mapped by the application, the memory must be host visible */
   void *ptr = NULL;
   if (device->dispatch_table.MapMemory(device->dispatch_handle, memory, 0,
                                        VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) {
      WRAPPER_LOG(error, "BCn: can't map the source buffer memory");
      return NULL;
   }
   *temp_memory = memory;
   return (const uint8_t *)ptr + offset;
}

/* Common part of vkCmdCopyBufferToImage(2).  Returns false when the copy has
 * to go to the driver unchanged. */
static bool
bcn_copy_buffer_to_image(struct wrapper_command_buffer *wcb,
                         VkBuffer src_buffer, VkImage dst_image,
                         uint32_t region_count,
                         const VkDeviceSize *buffer_offsets,
                         const uint32_t *row_lengths,
                         const uint32_t *image_heights,
                         const VkImageSubresourceLayers *subresources,
                         const VkExtent3D *extents,
                         VkDeviceSize *staging_offsets,
                         VkBuffer *staging_buffer)
{
   struct wrapper_device *device = wcb->device;

   if (!device->bcn_active || region_count == 0)
      return false;

   struct wrapper_bcn_image image_record;
   if (!bcn_lookup_image(device, dst_image, &image_record))
      return false;
   const struct wrapper_bcn_image *image = &image_record;

   const struct bcn_options *options = bcn_get_options();

   struct bcn_region stack_regions[8];
   struct bcn_region *regions = stack_regions;
   if (region_count > ARRAY_SIZE(stack_regions)) {
      regions = malloc(sizeof(*regions) * region_count);
      if (!regions)
         return false;
   }

   VkDeviceSize staging_size = 0;
   for (uint32_t i = 0; i < region_count; i++) {
      bcn_setup_region(image, buffer_offsets[i], row_lengths[i],
                       image_heights[i], &subresources[i], extents[i],
                       &staging_size, &regions[i]);
      staging_offsets[i] = regions[i].dst_offset;
   }

   bool handled = false;
   VkDeviceMemory temp_memory;
   const uint8_t *src = bcn_map_source(device, src_buffer, &temp_memory);
   if (!src)
      goto out;

   struct wrapper_bcn_staging *staging;
   void *map;
   bool coherent = true;
   VkResult result = bcn_create_staging(device, MAX2(staging_size, 16),
                                        &staging, &map, &coherent);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "BCn: failed to create a %llu bytes staging buffer, res %d",
                  (unsigned long long)staging_size, result);
      goto out_unmap;
   }

   for (uint32_t i = 0; i < region_count; i++)
      bcn_decode_region(options, image, src, map, &regions[i]);

   if (!coherent) {
      const VkMappedMemoryRange range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = staging->memory,
         .offset = 0,
         .size = VK_WHOLE_SIZE,
      };
      device->dispatch_table.FlushMappedMemoryRanges(device->dispatch_handle,
                                                     1, &range);
   }
   device->dispatch_table.UnmapMemory(device->dispatch_handle, staging->memory);

   bcn_attach_staging(wcb, staging);
   *staging_buffer = staging->buffer;
   handled = true;

out_unmap:
   if (temp_memory != VK_NULL_HANDLE)
      device->dispatch_table.UnmapMemory(device->dispatch_handle, temp_memory);
out:
   if (regions != stack_regions)
      free(regions);
   return handled;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                             VkBuffer srcBuffer,
                             VkImage dstImage,
                             VkImageLayout dstImageLayout,
                             uint32_t regionCount,
                             const VkBufferImageCopy *pRegions)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   struct wrapper_device *device = wcb->device;

   if (!device->bcn_active || regionCount == 0)
      goto passthrough;

   VkDeviceSize *offsets = malloc(sizeof(*offsets) * regionCount * 2);
   uint32_t *pitches = malloc(sizeof(*pitches) * regionCount * 2);
   VkImageSubresourceLayers *subresources = malloc(sizeof(*subresources) * regionCount);
   VkExtent3D *extents = malloc(sizeof(*extents) * regionCount);
   VkBufferImageCopy *regions = malloc(sizeof(*regions) * regionCount);
   VkBuffer staging = VK_NULL_HANDLE;
   bool handled = false;

   if (offsets && pitches && subresources && extents && regions) {
      for (uint32_t i = 0; i < regionCount; i++) {
         offsets[i] = pRegions[i].bufferOffset;
         pitches[i] = pRegions[i].bufferRowLength;
         pitches[regionCount + i] = pRegions[i].bufferImageHeight;
         subresources[i] = pRegions[i].imageSubresource;
         extents[i] = pRegions[i].imageExtent;
      }

      handled = bcn_copy_buffer_to_image(wcb, srcBuffer, dstImage, regionCount,
                                         offsets, pitches, pitches + regionCount,
                                         subresources, extents,
                                         offsets + regionCount, &staging);
      if (handled) {
         for (uint32_t i = 0; i < regionCount; i++) {
            regions[i] = pRegions[i];
            regions[i].bufferOffset = offsets[regionCount + i];
            regions[i].bufferRowLength = 0;
            regions[i].bufferImageHeight = 0;
         }
         device->dispatch_table.CmdCopyBufferToImage(wcb->dispatch_handle,
                                                     staging, dstImage,
                                                     dstImageLayout,
                                                     regionCount, regions);
      }
   }

   free(offsets);
   free(pitches);
   free(subresources);
   free(extents);
   free(regions);

   if (handled)
      return;

passthrough:
   device->dispatch_table.CmdCopyBufferToImage(wcb->dispatch_handle, srcBuffer,
                                               dstImage, dstImageLayout,
                                               regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_CmdCopyBufferToImage2(VkCommandBuffer commandBuffer,
                              const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   struct wrapper_device *device = wcb->device;
   const VkCopyBufferToImageInfo2 *info = pCopyBufferToImageInfo;
   const uint32_t count = info->regionCount;

   if (!device->bcn_active || count == 0)
      goto passthrough;

   VkDeviceSize *offsets = malloc(sizeof(*offsets) * count * 2);
   uint32_t *pitches = malloc(sizeof(*pitches) * count * 2);
   VkImageSubresourceLayers *subresources = malloc(sizeof(*subresources) * count);
   VkExtent3D *extents = malloc(sizeof(*extents) * count);
   VkBufferImageCopy2 *regions = malloc(sizeof(*regions) * count);
   VkBuffer staging = VK_NULL_HANDLE;
   bool handled = false;

   if (offsets && pitches && subresources && extents && regions) {
      for (uint32_t i = 0; i < count; i++) {
         offsets[i] = info->pRegions[i].bufferOffset;
         pitches[i] = info->pRegions[i].bufferRowLength;
         pitches[count + i] = info->pRegions[i].bufferImageHeight;
         subresources[i] = info->pRegions[i].imageSubresource;
         extents[i] = info->pRegions[i].imageExtent;
      }

      handled = bcn_copy_buffer_to_image(wcb, info->srcBuffer, info->dstImage,
                                         count, offsets, pitches,
                                         pitches + count, subresources,
                                         extents, offsets + count, &staging);
      if (handled) {
         for (uint32_t i = 0; i < count; i++) {
            regions[i] = info->pRegions[i];
            /* VkCopyCommandTransformInfoQCOM etc. don't apply anymore */
            regions[i].pNext = NULL;
            regions[i].bufferOffset = offsets[count + i];
            regions[i].bufferRowLength = 0;
            regions[i].bufferImageHeight = 0;
         }
         VkCopyBufferToImageInfo2 copy = *info;
         copy.srcBuffer = staging;
         copy.pRegions = regions;
         if (device->dispatch_table.CmdCopyBufferToImage2) {
            device->dispatch_table.CmdCopyBufferToImage2(wcb->dispatch_handle,
                                                         &copy);
         } else {
            /* Can't happen (the application used it), but be safe */
            for (uint32_t i = 0; i < count; i++) {
               const VkBufferImageCopy region = {
                  .bufferOffset = regions[i].bufferOffset,
                  .imageSubresource = regions[i].imageSubresource,
                  .imageOffset = regions[i].imageOffset,
                  .imageExtent = regions[i].imageExtent,
               };
               device->dispatch_table.CmdCopyBufferToImage(
                  wcb->dispatch_handle, staging, info->dstImage,
                  info->dstImageLayout, 1, &region);
            }
         }
      }
   }

   free(offsets);
   free(pitches);
   free(subresources);
   free(extents);
   free(regions);

   if (handled)
      return;

passthrough:
   device->dispatch_table.CmdCopyBufferToImage2(wcb->dispatch_handle,
                                                pCopyBufferToImageInfo);
}

/* ------------------------------------------------------------------------ */
/* Command buffer lifetime of the staging buffers                           */
/* ------------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                           const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   struct wrapper_device *device = wcb->device;

   /* Beginning resets the command buffer, nothing of it is pending */
   if (wcb->on_staging_list) {
      simple_mtx_lock(&device->resource_mutex);
      wrapper_bcn_release_staging(wcb);
      simple_mtx_unlock(&device->resource_mutex);
   }

   return device->dispatch_table.BeginCommandBuffer(wcb->dispatch_handle,
                                                    pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                           VkCommandBufferResetFlags flags)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   struct wrapper_device *device = wcb->device;

   if (wcb->on_staging_list) {
      simple_mtx_lock(&device->resource_mutex);
      wrapper_bcn_release_staging(wcb);
      simple_mtx_unlock(&device->resource_mutex);
   }

   return device->dispatch_table.ResetCommandBuffer(wcb->dispatch_handle, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_ResetCommandPool(VkDevice _device, VkCommandPool commandPool,
                         VkCommandPoolResetFlags flags)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (device->bcn_active) {
      simple_mtx_lock(&device->resource_mutex);
      list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                               &device->bcn_staging_cmd_list, staging_link) {
         if (wcb->pool == commandPool)
            wrapper_bcn_release_staging(wcb);
      }
      simple_mtx_unlock(&device->resource_mutex);
   }

   return device->dispatch_table.ResetCommandPool(device->dispatch_handle,
                                                  commandPool, flags);
}
