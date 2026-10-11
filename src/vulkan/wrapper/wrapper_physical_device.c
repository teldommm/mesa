#include <math.h>
#include <fcntl.h>

#include "wrapper_private.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common.h"
#include "util/os_misc.h"
#include "vk_printers.h"

/* The runtime (and therefore every struct the wrapper knows how to patch)
 * is built against this header. Newer drivers (Adreno 8xx, v800+) report
 * Vulkan 1.4, whose structs this runtime does not understand, so never
 * advertise more than the header we were compiled with. */
#define WRAPPER_MAX_API_VERSION VK_HEADER_VERSION_COMPLETE

static uint32_t
parse_vk_version_from_env(void)
{
   const char *env = getenv("WRAPPER_VK_VERSION");
   uint32_t major = 0, minor = 0, patch = 0;

   if (!env || !*env)
      return 0;

   if (sscanf(env, "%u.%u.%u", &major, &minor, &patch) < 2)
      return 0;

   return VK_MAKE_API_VERSION(0, major, minor, patch);
}

static uint32_t
wrapper_clamp_api_version(const struct wrapper_physical_device *pdevice,
                          uint32_t driver_version)
{
   uint32_t version = driver_version;

   if (pdevice->api_version_override)
      version = pdevice->api_version_override;

   if (version > WRAPPER_MAX_API_VERSION)
      version = WRAPPER_MAX_API_VERSION;

   return version;
}

static VkResult
wrapper_setup_device_extensions(struct wrapper_physical_device *pdevice) {
   struct vk_device_extension_table *exts = &pdevice->vk.supported_extensions;
   uint32_t pdevice_extension_count = 0;
   VkExtensionProperties *pdevice_extensions;
   VkResult result;

   /* Do not assume the driver exposes fewer extensions than this runtime
    * knows about: recent Adreno drivers expose extensions newer than our
    * header and a fixed-size array returned VK_INCOMPLETE, which left the
    * whole extension table empty. */
   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, NULL);
   if (result != VK_SUCCESS)
      return result;

   pdevice_extensions = calloc(pdevice_extension_count ? pdevice_extension_count : 1,
                               sizeof(*pdevice_extensions));
   if (!pdevice_extensions)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, pdevice_extensions);
   if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      free(pdevice_extensions);
      return result;
   }

   WLOGD("EnumerateDeviceExtensionProperties:");
   for (int i = 0; i < pdevice_extension_count; i++) {
      LOG_STRUCT(VkExtensionProperties, &pdevice_extensions[i]);
   }

   *exts = wrapper_device_extensions;

   for (int i = 0; i < pdevice_extension_count; i++) {
      int idx;
      for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_device_extensions[idx].extensionName,
                     pdevice_extensions[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_DEVICE_EXTENSION_COUNT)
         continue;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      pdevice->base_supported_extensions.extensions[idx] =
         exts->extensions[idx] = true;
   }
   free(pdevice_extensions);

   exts->KHR_present_wait = exts->KHR_timeline_semaphore;
   if (CHECK_FLAG("NO_PRESENT_WAIT") || ENV_INT("WRAPPER_DISABLE_PRESENT_WAIT", 0)) {
      if (exts->KHR_present_wait) {
         WLOG("Disabling KHR_present_wait, some drivers misreport KHR_timeline_semaphore support (e.g. Adreno 6XX)");
         exts->KHR_present_wait = false;
      }
   }

   // Needed by dxvk (faked when missing, see wrapper_device.c for the stubs)
   exts->EXT_transform_feedback = true;
   exts->EXT_host_query_reset = true;
   exts->EXT_custom_border_color = true;

   return VK_SUCCESS;
}

static void
wrapper_apply_device_extension_blacklist(struct wrapper_physical_device *physical_device) {
   const char *env = getenv("WRAPPER_EXTENSION_BLACKLIST");
   if (!env || !*env)
      return;

   /* strtok() modifies its input: never tokenize the environment itself. */
   char *blacklist = strdup(env);
   if (!blacklist)
      return;

   char *saveptr = NULL;
   char *extension = strtok_r(blacklist, ", ", &saveptr);
   while (extension != NULL) {
      for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
         if (strcmp(extension, vk_device_extensions[i].extensionName) == 0) {
            WLOG("Blacklisting extension %s", extension);
            physical_device->vk.supported_extensions.extensions[i] = false;
         }
      }
      extension = strtok_r(NULL, ", ", &saveptr);
   }
   free(blacklist);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->instance, pName);
}

static int
wrapper_open_dma_heap(void)
{
   /* Uncached heap by default: the memory is imported as HOST_COHERENT and
    * Adreno does not snoop CPU caches for dma-buf imports, so the cached
    * system heap produced stale data / flickering textures. */
   int fd = -1;
   if (ENV_INT("WRAPPER_DMAHEAP_CACHED", 0))
      fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
   else
      fd = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);

   if (fd < 0)
      fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
   return fd;
}

VkResult enumerate_physical_device(struct vk_instance *_instance)
{
   struct wrapper_instance *instance = (struct wrapper_instance *)_instance;
   VkPhysicalDevice *physical_devices;
   uint32_t physical_device_count = 0;
   VkResult result;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, NULL);
   if (result != VK_SUCCESS)
      return result;

   if (physical_device_count == 0)
      return VK_SUCCESS;

   physical_devices = calloc(physical_device_count, sizeof(*physical_devices));
   if (!physical_devices)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, physical_devices);
   if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      free(physical_devices);
      return result;
   }
   result = VK_SUCCESS;

   for (int i = 0; i < physical_device_count; i++) {
      PFN_vkGetInstanceProcAddr get_instance_proc_addr;
      struct wrapper_physical_device *pdevice;

      pdevice = vk_zalloc(&_instance->alloc, sizeof(*pdevice), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (!pdevice) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         break;
      }

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_entrypoints, true);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wsi_physical_device_entrypoints, false);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_trampolines, false);

      result = vk_physical_device_init(&pdevice->vk,
                                       &instance->vk,
                                       NULL, NULL, NULL,
                                       &dispatch_table);
      if (result != VK_SUCCESS) {
         vk_free(&_instance->alloc, pdevice);
         break;
      }

      pdevice->instance = instance;
      pdevice->dispatch_handle = physical_devices[i];
      pdevice->dma_heap_fd = -1;
      get_instance_proc_addr = instance->dispatch_table.GetInstanceProcAddr;

      vk_physical_device_dispatch_table_load(&pdevice->dispatch_table,
                                             get_instance_proc_addr,
                                             instance->dispatch_handle);

      /* Driver identity first: several workarounds below depend on it. */
      pdevice->driver_properties = (VkPhysicalDeviceDriverProperties) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      pdevice->properties2 = (VkPhysicalDeviceProperties2) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &pdevice->driver_properties,
      };
      WPDEVICE.GetPhysicalDeviceProperties2(
         (VkPhysicalDevice) pdevice, &pdevice->properties2);
      WPDEVICE.GetPhysicalDeviceMemoryProperties(
         (VkPhysicalDevice) pdevice, &pdevice->memory_properties);

      WLOGD("GetPhysicalDeviceProperties2:");
      LOG_STRUCT(VkPhysicalDeviceProperties2, &pdevice->properties2);
      WLOGD("GetPhysicalDeviceMemoryProperties:");
      LOG_STRUCT(VkPhysicalDeviceMemoryProperties, &pdevice->memory_properties);

      const uint32_t driver_id = pdevice->driver_properties.driverID;
      const uint32_t driver_version = pdevice->properties2.properties.driverVersion;
      pdevice->is_qcom = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
      pdevice->api_version_override = parse_vk_version_from_env();
      pdevice->disable_placed = ENV_INT("WRAPPER_DISABLE_PLACED", 0) != 0;
      pdevice->resource_type = getenv("WRAPPER_RESOURCE_TYPE");
      if (!pdevice->resource_type || !*pdevice->resource_type)
         pdevice->resource_type = "auto";

      result = wrapper_setup_device_extensions(pdevice);
      if (result != VK_SUCCESS) {
         WLOGE("Failed to enumerate device extensions: %d", result);
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         break;
      }
      wrapper_setup_device_features(pdevice);

      struct vk_features *supported_features = &pdevice->vk.supported_features;
      pdevice->base_supported_features = *supported_features;

      supported_features->geometryShader = true;
      supported_features->presentId = true;
      supported_features->multiViewport = true;
      supported_features->depthClamp = true;
      supported_features->depthBiasClamp = true;
      supported_features->textureCompressionBC = true;
      supported_features->fillModeNonSolid = true;
      supported_features->shaderClipDistance = true;
      supported_features->shaderCullDistance = true;
      supported_features->presentWait = supported_features->timelineSemaphore &&
                                        pdevice->vk.supported_extensions.KHR_present_wait;
      supported_features->swapchainMaintenance1 = true;
      supported_features->imageCompressionControlSwapchain = false;

      /* VK_EXT_map_memory_placed is emulated by the wrapper. It must be
       * switchable: box64 with BOX64_MMAP32=1 (non-arm64ec) manages the
       * 32-bit address space itself and the emulator then asks us to turn
       * it off with WRAPPER_DISABLE_PLACED=1. */
      if (pdevice->disable_placed) {
         WLOG("Disabling VK_EXT_map_memory_placed (WRAPPER_DISABLE_PLACED)");
         pdevice->vk.supported_extensions.EXT_map_memory_placed = false;
         supported_features->memoryMapPlaced = false;
         supported_features->memoryMapRangePlaced = false;
         supported_features->memoryUnmapReserve = false;
      } else {
         supported_features->memoryMapPlaced = true;
         supported_features->memoryUnmapReserve = true;
      }

      // dxvk extension features support
      supported_features->geometryStreams = true;
      supported_features->transformFeedback = true;
      supported_features->hostQueryReset = true;
      supported_features->customBorderColors = true;
      supported_features->customBorderColorWithoutFormat = true;
      supported_features->dualSrcBlend = true; // Missing on G715 r38p1
      supported_features->multiDrawIndirect = true; // Missing on G57 r32p1
      supported_features->vertexPipelineStoresAndAtomics = true; // Missing on G57 r32p1

      const char *app_name = instance->vk.app_info.app_name
         ? instance->vk.app_info.app_name : "wrapper";
      const char *engine_name = instance->vk.app_info.engine_name
         ? instance->vk.app_info.engine_name : "wrapper";
      const uint32_t engine_version = instance->vk.app_info.engine_version;

      pdevice->is_dxvk = strstr(engine_name, "DXVK") != NULL;
      bool is_dxvk_2_plus = pdevice->is_dxvk && engine_version >= VK_MAKE_VERSION(2, 0, 0);

      WLOGD("AppInfo: app_name = %s, engine_name = %s, engine_version = %x",
         app_name, engine_name, engine_version);
      WLOGD("DriverInfo: driver_id = %d, driver_name = %s, driver_info = %s",
         driver_id,
         pdevice->driver_properties.driverName,
         pdevice->driver_properties.driverInfo);

      /* --- Qualcomm proprietary (Adreno) workarounds --- */
      if (pdevice->is_qcom) {
         if (driver_version > VK_MAKE_VERSION(512, 744, 0) && strstr(app_name, "clvk")) {
            /* HACK: Fixed clvk not working on qualcomm proprietary driver. */
            supported_features->globalPriorityQuery = false;
         }

         WLOG("Disabling VK_KHR_shader_float_controls on Qualcomm proprietary drivers");
         pdevice->vk.supported_extensions.KHR_shader_float_controls = false;

         if (pdevice->is_dxvk) {
            /* Adreno's line rasterization implementation hangs/crashes DXVK
             * line-heavy titles; DXVK falls back gracefully without it. */
            if (!ENV_INT("WRAPPER_QCOM_KEEP_LINE_RASTERIZATION", 0)) {
               WLOG("Disabling VK_EXT_line_rasterization for DXVK on Adreno");
               pdevice->vk.supported_extensions.EXT_line_rasterization = false;
            }
            /* DXVK >= 2.7 refuses to create a device without it; it only
             * carries pNext structs, so advertising it is harmless. */
            if (engine_version >= VK_MAKE_VERSION(2, 7, 0) &&
                !pdevice->vk.supported_extensions.KHR_pipeline_library) {
               WLOG("Faking VK_KHR_pipeline_library for DXVK >= 2.7");
               pdevice->vk.supported_extensions.KHR_pipeline_library = true;
            }
         }
      }

      if (driver_id == VK_DRIVER_ID_ARM_PROPRIETARY
            && pdevice->vk.supported_extensions.EXT_extended_dynamic_state
            && !is_dxvk_2_plus) {
         WLOG("Disabling VK_EXT_extended_dynamic_state on Mali proprietary drivers");
         pdevice->vk.supported_extensions.EXT_extended_dynamic_state = false;
         pdevice->vk.supported_extensions.EXT_extended_dynamic_state2 = false;
         pdevice->vk.supported_extensions.EXT_extended_dynamic_state3 = false;
      }

      /* Blacklist last so the user can also remove extensions we fake. */
      wrapper_apply_device_extension_blacklist(pdevice);

      result = wsi_device_init(&pdevice->wsi_device,
                               wrapper_physical_device_to_handle(pdevice),
                               wrapper_wsi_proc_addr, &_instance->alloc, -1,
                               NULL, &(struct wsi_device_options){});
      if (result != VK_SUCCESS) {
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         break;
      }
      pdevice->vk.wsi_device = &pdevice->wsi_device;
#ifdef __TERMUX__
      pdevice->wsi_device.wants_ahardware_buffer = true;
#endif

      /* Swapchain format order: the emulator passes WRAPPER_SURFACE_FORMAT
       * matching its X server drawable (rgba8 for HAL_PIXEL_FORMAT_RGBA_8888),
       * otherwise red/blue end up swapped. */
      if (driver_id == VK_DRIVER_ID_ARM_PROPRIETARY)
         pdevice->wsi_device.force_rgba8_unorm_first = true;
      else
         pdevice->wsi_device.force_bgra8_unorm_first = true;

      const char *surface_format = getenv("WRAPPER_SURFACE_FORMAT");
      if (surface_format) {
         if (!strcmp(surface_format, "rgba8")) {
            pdevice->wsi_device.force_rgba8_unorm_first = true;
            pdevice->wsi_device.force_bgra8_unorm_first = false;
         } else if (!strcmp(surface_format, "bgra8")) {
            pdevice->wsi_device.force_rgba8_unorm_first = false;
            pdevice->wsi_device.force_bgra8_unorm_first = true;
         }
      }

      pdevice->dma_heap_fd = wrapper_open_dma_heap();

      // BCn support detection (Xclipse/Mali/old Adreno drivers)
      WPDEVICE.GetPhysicalDeviceFormatProperties((VkPhysicalDevice) pdevice, VK_FORMAT_BC1_RGB_UNORM_BLOCK, &pdevice->bc1_format_properties);
      WLOGD("bc1 support:");
      LOG_STRUCT(VkFormatProperties, &pdevice->bc1_format_properties);
      WPDEVICE.GetPhysicalDeviceFormatProperties((VkPhysicalDevice) pdevice, VK_FORMAT_BC4_UNORM_BLOCK, &pdevice->bc4_format_properties);
      WLOGD("bc4 support:");
      LOG_STRUCT(VkFormatProperties, &pdevice->bc4_format_properties);

      pdevice->needs_bc1_emulation = !pdevice->base_supported_features.textureCompressionBC && !has_bc1_support(pdevice);
      pdevice->needs_bc4_emulation = !pdevice->base_supported_features.textureCompressionBC && !has_bc4_support(pdevice);

      /* WRAPPER_EMULATE_BCN as set by the emulator UI:
       *   0 = never, 1/3 = auto (only what the GPU lacks), 2 = force everything */
      int emulate_bcn = ENV_INT("WRAPPER_EMULATE_BCN", 3);
      bool force_bcn = CHECK_FLAG("FORCE_BCN_EMULATION") || emulate_bcn == 2;
      bool no_bcn = CHECK_FLAG("NO_BCN_EMULATION") || emulate_bcn == 0;

      if (force_bcn) {
         if (pdevice->base_supported_features.textureCompressionBC) {
            WLOG("Forcing BCn emulation");
            pdevice->base_supported_features.textureCompressionBC = false;
         }
         pdevice->needs_bc1_emulation = true;
         pdevice->needs_bc4_emulation = true;
      }

      if (no_bcn) {
         WLOG("Disabling BCn emulation");
         pdevice->needs_bc1_emulation = false;
         pdevice->needs_bc4_emulation = false;
      }

      if (CHECK_FLAG("NO_BC123_EMULATION")) {
         WLOG("Disabling BC123 emulation (disable by setting NO_BC123_EMULATION=0)");
         pdevice->needs_bc1_emulation = false;
      }

      list_addtail(&pdevice->vk.link, &_instance->physical_devices.list);
   }

   free(physical_devices);
   return result;
}

void destroy_physical_device(struct vk_physical_device *pdevice) {
   VK_FROM_HANDLE(wrapper_physical_device, wpdevice,
                  vk_physical_device_to_handle(pdevice));
   if (wpdevice->dma_heap_fd >= 0)
      close(wpdevice->dma_heap_fd);
   wsi_device_finish(pdevice->wsi_device, &pdevice->instance->alloc);
   vk_physical_device_finish(pdevice);
   vk_free(&pdevice->instance->alloc, pdevice);
}

WRAPPER_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                           const char* pLayerName,
                                           uint32_t* pPropertyCount,
                                           VkExtensionProperties* pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(physicalDevice,
                                                       pLayerName,
                                                       pPropertyCount,
                                                       pProperties);
}

WRAPPER_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures* pFeatures)
{
   return vk_common_GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

WRAPPER_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures2* pFeatures) {
   vk_common_GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
   // Fake select dxvk 1.10.3 mandatory features
   vk_foreach_struct(pnext, pFeatures->pNext) {
      switch (pnext->sType) {
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
         {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *extTransformFeedback =
                (VkPhysicalDeviceTransformFeedbackFeaturesEXT*) pnext;
            extTransformFeedback->transformFeedback = true;
            extTransformFeedback->geometryStreams = true;
            break;
         }
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
         {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT *extCustomBorderColor =
                (VkPhysicalDeviceCustomBorderColorFeaturesEXT*) pnext;
            extCustomBorderColor->customBorderColors = VK_TRUE;
            extCustomBorderColor->customBorderColorWithoutFormat = VK_TRUE;
            break;
         }
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT:
         {
            VkPhysicalDeviceHostQueryResetFeaturesEXT *extHostQueryReset =
                (VkPhysicalDeviceHostQueryResetFeaturesEXT*) pnext;
            extHostQueryReset->hostQueryReset = VK_TRUE;
            break;
         }
         default:
            break;
      }
   }
}

static void
wrapper_patch_device_identity(struct wrapper_physical_device *pdevice,
                              VkPhysicalDeviceProperties *props)
{
   props->apiVersion = wrapper_clamp_api_version(pdevice, props->apiVersion);

   const char *device_name = getenv("WRAPPER_DEVICE_NAME");
   if (device_name && *device_name)
      snprintf(props->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, "%s", device_name);

   int device_id = ENV_INT("WRAPPER_DEVICE_ID", 0);
   int vendor_id = ENV_INT("WRAPPER_VENDOR_ID", 0);
   if (device_id > 0)
      props->deviceID = device_id;
   if (vendor_id > 0)
      props->vendorID = vendor_id;
}

WRAPPER_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                    VkPhysicalDeviceProperties *pProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties(
      pdevice->dispatch_handle, pProperties);
   wrapper_patch_device_identity(pdevice, pProperties);
}

static void
wrapper_patch_float_controls(VkShaderFloatControlsIndependence *denorm,
                             VkShaderFloatControlsIndependence *rounding,
                             VkBool32 *ftz16, VkBool32 *ftz32,
                             VkBool32 *rte16, VkBool32 *rte32,
                             VkBool32 *szinp16, VkBool32 *szinp32)
{
   *denorm = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   *rounding = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   *ftz16 = *ftz32 = false;
   *rte16 = *rte32 = false;
   *szinp16 = *szinp32 = false;
}

WRAPPER_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties2* pProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties2(
      pdevice->dispatch_handle, pProperties);

   wrapper_patch_device_identity(pdevice, &pProperties->properties);

   /* Subgroup operations are hidden by default (buggy on several mobile
    * drivers). WRAPPER_EXPOSE_SUBGROUPS=1 keeps the driver values, which
    * vkd3d-proton needs for SM 6.x wave intrinsics. */
   const bool expose_subgroups = ENV_INT("WRAPPER_EXPOSE_SUBGROUPS", 0) != 0;
   const bool patch_float_controls = pdevice->is_qcom ||
                                     ENV_INT("WRAPPER_NO_FLOAT_CONTROLS", 0);
   const int driver_id_override = ENV_INT("WRAPPER_DRIVER_ID", 0);

   vk_foreach_struct(prop, pProperties->pNext) {
      switch (prop->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT:
      {
         VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *placed_prop =
               (VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *)prop;
         uint64_t os_page_size;
         os_get_page_size(&os_page_size);
         placed_prop->minPlacedMemoryMapAlignment = os_page_size;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT:
      {
      	 VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *texel_prop =
      	      (VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *)prop;
      	 texel_prop->storageTexelBufferOffsetAlignmentBytes = 1;
      	 texel_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
      	 break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR:
      {
         if (!patch_float_controls)
            break;
         VkPhysicalDeviceFloatControlsPropertiesKHR *p =
              (VkPhysicalDeviceFloatControlsPropertiesKHR *)prop;
         wrapper_patch_float_controls(&p->denormBehaviorIndependence,
                                      &p->roundingModeIndependence,
                                      &p->shaderDenormFlushToZeroFloat16,
                                      &p->shaderDenormFlushToZeroFloat32,
                                      &p->shaderRoundingModeRTEFloat16,
                                      &p->shaderRoundingModeRTEFloat32,
                                      &p->shaderSignedZeroInfNanPreserveFloat16,
                                      &p->shaderSignedZeroInfNanPreserveFloat32);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT:
      {
         if (pdevice->base_supported_extensions.EXT_transform_feedback)
            break;
         VkPhysicalDeviceTransformFeedbackPropertiesEXT *feedback_prop =
              (VkPhysicalDeviceTransformFeedbackPropertiesEXT *)prop;
         feedback_prop->maxTransformFeedbackStreams = 4;
         feedback_prop->maxTransformFeedbackBuffers = 4;
         feedback_prop->maxTransformFeedbackBufferSize = 0xffffffff;
         feedback_prop->maxTransformFeedbackStreamDataSize = 512;
         feedback_prop->maxTransformFeedbackBufferDataSize = 512;
         feedback_prop->maxTransformFeedbackBufferDataStride = 512;
         feedback_prop->transformFeedbackQueries = true;
         feedback_prop->transformFeedbackStreamsLinesTriangles = true;
         feedback_prop->transformFeedbackRasterizationStreamSelect = false;
         feedback_prop->transformFeedbackDraw = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
      {
         if (expose_subgroups)
            break;
         VkPhysicalDeviceVulkan11Properties *vk11_prop =
              (VkPhysicalDeviceVulkan11Properties *)prop;
         vk11_prop->subgroupSupportedOperations = 0;
         vk11_prop->subgroupSupportedStages = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
      {
         VkPhysicalDeviceVulkan12Properties *p =
              (VkPhysicalDeviceVulkan12Properties *)prop;
         if (patch_float_controls) {
            wrapper_patch_float_controls(&p->denormBehaviorIndependence,
                                         &p->roundingModeIndependence,
                                         &p->shaderDenormFlushToZeroFloat16,
                                         &p->shaderDenormFlushToZeroFloat32,
                                         &p->shaderRoundingModeRTEFloat16,
                                         &p->shaderRoundingModeRTEFloat32,
                                         &p->shaderSignedZeroInfNanPreserveFloat16,
                                         &p->shaderSignedZeroInfNanPreserveFloat32);
         }
         if (driver_id_override > 0)
            p->driverID = driver_id_override;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
      {
         if (driver_id_override > 0)
            ((VkPhysicalDeviceDriverProperties *)prop)->driverID = driver_id_override;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
      {
         VkPhysicalDeviceVulkan13Properties *vk13_prop =
              (VkPhysicalDeviceVulkan13Properties *)prop;
         vk13_prop->storageTexelBufferOffsetAlignmentBytes = 1;
         vk13_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
      {
         if (expose_subgroups)
            break;
         VkPhysicalDeviceSubgroupProperties *subgroup_prop =
              (VkPhysicalDeviceSubgroupProperties *)prop;
         subgroup_prop->supportedOperations = 0;
         subgroup_prop->supportedStages = 0;
         break;
      }
      default:
         break;
      }
   }
}

static void
wrapper_fill_emulated_bc_image_properties(struct wrapper_physical_device *pdevice,
                                          VkImageType type,
                                          VkImageTiling tiling,
                                          VkImageCreateFlags flags,
                                          VkImageFormatProperties *props)
{
   const VkPhysicalDeviceLimits *limits = &pdevice->properties2.properties.limits;

   switch (type) {
   case VK_IMAGE_TYPE_1D:
      props->maxExtent = (VkExtent3D) { limits->maxImageDimension1D, 1, 1 };
      break;
   case VK_IMAGE_TYPE_3D:
      props->maxExtent = (VkExtent3D) { limits->maxImageDimension3D,
                                        limits->maxImageDimension3D,
                                        limits->maxImageDimension3D };
      break;
   case VK_IMAGE_TYPE_2D:
   default:
      if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
         props->maxExtent = (VkExtent3D) { limits->maxImageDimensionCube,
                                           limits->maxImageDimensionCube, 1 };
      else
         props->maxExtent = (VkExtent3D) { limits->maxImageDimension2D,
                                           limits->maxImageDimension2D, 1 };
      break;
   }

   if (tiling == VK_IMAGE_TILING_LINEAR ||
       tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
       (flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)) {
      props->maxMipLevels = 1;
   } else {
      uint32_t max_dim = MAX2(props->maxExtent.width, props->maxExtent.height);
      max_dim = MAX2(max_dim, props->maxExtent.depth);
      props->maxMipLevels = (uint32_t) floor(log2((double) max_dim)) + 1;
   }

   if (tiling == VK_IMAGE_TILING_LINEAR || type == VK_IMAGE_TYPE_3D)
      props->maxArrayLayers = 1;
   else
      props->maxArrayLayers = limits->maxImageArrayLayers;

   props->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->maxResourceSize = 562949953421312ull;
}

WRAPPER_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
	                                           VkFormat format,
	                                           VkImageType type,
	                                           VkImageTiling tiling,
	                                           VkImageUsageFlags usage,
	                                           VkImageCreateFlags flags,
	                                           VkImageFormatProperties *pImageFormatProperties)
{
   VkResult result;
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   result = wrapper_physical_device_trampolines.GetPhysicalDeviceImageFormatProperties(
      physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);

   if (result == VK_ERROR_FORMAT_NOT_SUPPORTED && is_bc_image_format(format) &&
       unwrap_vk_format_physical_device(pdevice, format) != format) {
      wrapper_fill_emulated_bc_image_properties(pdevice, type, tiling, flags,
                                                pImageFormatProperties);
      return VK_SUCCESS;
   }

   return result;
}

WRAPPER_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                                const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
                                                VkImageFormatProperties2* pImageFormatProperties)
{
   VkResult result;
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   result = wrapper_physical_device_trampolines.GetPhysicalDeviceImageFormatProperties2(
         physicalDevice, pImageFormatInfo, pImageFormatProperties);
   if (result == VK_ERROR_FORMAT_NOT_SUPPORTED && is_bc_image_format(pImageFormatInfo->format) &&
       unwrap_vk_format_physical_device(pdevice, pImageFormatInfo->format) != pImageFormatInfo->format) {
      wrapper_fill_emulated_bc_image_properties(pdevice, pImageFormatInfo->type,
                                                pImageFormatInfo->tiling,
                                                pImageFormatInfo->flags,
                                                &pImageFormatProperties->imageFormatProperties);
      return VK_SUCCESS;
   }

   if (CHECK_FLAG("DISABLE_EXTERNAL_FD")) {
      vk_foreach_struct(pnext, pImageFormatProperties->pNext) {
         if (pnext->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) {
            WLOGD("Unsetting VkExternalImageFormatProperties::externalMemoryFeatures");
            VkExternalImageFormatProperties* obj = (VkExternalImageFormatProperties*) pnext;
            obj->externalMemoryProperties.externalMemoryFeatures = 0;
         }
      }
   }

   return result;
}

#define WRAPPER_EMULATED_BC_FEATURES \
   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)

WRAPPER_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                            VkFormat format,
                                            VkFormatProperties* pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   PCHECKV(GetPhysicalDeviceFormatProperties(physicalDevice, format, pFormatProperties));

   if (is_bc_image_format(format) &&
       unwrap_vk_format_physical_device(pdevice, format) != format) {
      /* Emulated BCn: advertise what we can actually do with the decoded
       * RGBA/RGBA16F copy (no storage / buffer features). */
      pFormatProperties->optimalTilingFeatures |= WRAPPER_EMULATED_BC_FEATURES;
   }
}

WRAPPER_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                           VkFormat format,
                                           VkFormatProperties2* pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   PCHECKV(GetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties));

   if (!is_bc_image_format(format) ||
       unwrap_vk_format_physical_device(pdevice, format) == format)
      return;

   /* DXVK queries format support through the *2 variant only, so without
    * this override emulated BCn formats were reported as unsupported. */
   pFormatProperties->formatProperties.optimalTilingFeatures |= WRAPPER_EMULATED_BC_FEATURES;
   vk_foreach_struct(pnext, pFormatProperties->pNext) {
      if (pnext->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
         VkFormatProperties3 *p3 = (VkFormatProperties3 *) pnext;
         p3->optimalTilingFeatures |= (VkFormatFeatureFlags2) WRAPPER_EMULATED_BC_FEATURES;
      }
   }
}

static void
wrapper_patch_memory_heaps(VkPhysicalDeviceMemoryProperties *props,
                           VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget)
{
   int vmem_mb = ENV_INT("WRAPPER_VMEM_MAX_SIZE", 0);
   if (vmem_mb <= 0)
      return;

   VkDeviceSize size = (VkDeviceSize) vmem_mb * 1024ull * 1024ull;
   for (uint32_t i = 0; i < props->memoryHeapCount; i++) {
      if (!(props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
         continue;
      props->memoryHeaps[i].size = size;
      if (budget && budget->heapBudget[i] > size)
         budget->heapBudget[i] = size;
   }
}

WRAPPER_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, pMemoryProperties);
   wrapper_patch_memory_heaps(pMemoryProperties, NULL);
}

WRAPPER_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties2(
      pdevice->dispatch_handle, pMemoryProperties);
   VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget =
      vk_find_struct(pMemoryProperties->pNext, PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
   wrapper_patch_memory_heaps(&pMemoryProperties->memoryProperties, budget);
}
