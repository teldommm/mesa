#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"

static uint32_t
parse_vk_version_from_env(void)
{
   static int parsed = 0;
   static uint32_t api_version = 0;

   if (parsed)
      return api_version;
   parsed = 1;

   const char *wrapper_vk_version = getenv("WRAPPER_VK_VERSION");
   if (wrapper_vk_version) {
      unsigned major = 0, minor = 0, patch = 0;
      if (sscanf(wrapper_vk_version, "%u.%u.%u", &major, &minor, &patch) >= 2)
         api_version = VK_MAKE_API_VERSION(0, major, minor, patch);
   }

   return api_version;
}

static VkResult
wrapper_setup_device_extensions(struct wrapper_physical_device *pdevice)
{
   struct vk_device_extension_table *exts = &pdevice->vk.supported_extensions;
   VkExtensionProperties *pdevice_extensions;
   uint32_t pdevice_extension_count = 0;
   VkResult result;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, NULL);
   if (result != VK_SUCCESS)
      return result;

   pdevice_extensions = calloc(MAX2(pdevice_extension_count, 1),
                               sizeof(*pdevice_extensions));
   if (!pdevice_extensions)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count,
      pdevice_extensions);
   if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      free(pdevice_extensions);
      return result;
   }

   *exts = wrapper_device_extensions;

   for (uint32_t i = 0; i < pdevice_extension_count; i++) {
      int idx;
      for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_device_extensions[idx].extensionName,
                    pdevice_extensions[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_DEVICE_EXTENSION_COUNT)
         continue;

      pdevice->base_supported_extensions.extensions[idx] = true;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      exts->extensions[idx] = true;
   }

   free(pdevice_extensions);

   /* Present wait is implemented by the common WSI on top of timeline
    * semaphores. */
   exts->KHR_present_wait = exts->KHR_timeline_semaphore;
   exts->KHR_present_wait2 = exts->KHR_timeline_semaphore;

   return VK_SUCCESS;
}

static void
wrapper_apply_device_extension_blacklist(struct wrapper_physical_device *physical_device)
{
   const char *env = getenv("WRAPPER_EXTENSION_BLACKLIST");
   if (!env || !*env)
      return;

   char *blacklist = strdup(env);
   if (!blacklist)
      return;

   char *saveptr = NULL;
   for (char *extension = strtok_r(blacklist, ",", &saveptr); extension != NULL;
        extension = strtok_r(NULL, ",", &saveptr)) {
      while (*extension == ' ')
         extension++;
      for (char *end = extension + strlen(extension);
           end > extension && (end[-1] == ' ' || end[-1] == '\n'); end--)
         end[-1] = '\0';
      for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
         if (!strcmp(extension, vk_device_extensions[i].extensionName)) {
            WRAPPER_LOG(info, "Blacklisting extension %s", extension);
            physical_device->vk.supported_extensions.extensions[i] = false;
         }
      }
   }

   free(blacklist);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->instance, pName);
}

static bool
engine_is(const char *engine_name, const char *name)
{
   return engine_name && strstr(engine_name, name) != NULL;
}

/* Workarounds for the proprietary Qualcomm (Adreno) driver */
static void
wrapper_apply_qualcomm_workarounds(struct wrapper_physical_device *pdevice,
                                   const char *app_name,
                                   const char *engine_name,
                                   uint32_t engine_version)
{
   struct vk_features *supported_features = &pdevice->vk.supported_features;
   const uint32_t driver_version = pdevice->properties2.properties.driverVersion;

   if (engine_is(engine_name, "DXVK")) {
      WRAPPER_LOG(info, "Disabling VK_EXT/KHR_line_rasterization");
      pdevice->vk.supported_extensions.EXT_line_rasterization = false;
      pdevice->vk.supported_extensions.KHR_line_rasterization = false;

      if (engine_version >= VK_MAKE_VERSION(2, 7, 0)) {
         WRAPPER_LOG(info, "Faking VK_KHR_pipeline_library");
         pdevice->vk.supported_extensions.KHR_pipeline_library = true;
      }
   }

   if (driver_version > VK_MAKE_VERSION(512, 744, 0) &&
       app_name && strstr(app_name, "clvk")) {
      WRAPPER_LOG(info, "Disabling globalPriorityQuery feature");
      supported_features->globalPriorityQuery = false;
   }

   WRAPPER_LOG(info, "Disabling VK_KHR_shader_float_controls");
   pdevice->vk.supported_extensions.KHR_shader_float_controls = false;
}

/* Subgroup operations used to be hidden for every driver (that caps
 * vkd3d-proton at shader model 5.1).  They are reported on Adreno, both with
 * Turnip and the Qualcomm driver, and still hidden for other GPUs.
 * WRAPPER_SUBGROUP_OPS=0/1 forces either way. */
static void
wrapper_setup_subgroup_ops(struct wrapper_physical_device *pdevice)
{
   const VkDriverId driver_id = pdevice->driver_properties.driverID;
   bool expose = driver_id == VK_DRIVER_ID_MESA_TURNIP ||
                 driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;

   const char *env = getenv("WRAPPER_SUBGROUP_OPS");
   if (env && *env)
      expose = debug_get_bool_option("WRAPPER_SUBGROUP_OPS", expose);

   pdevice->expose_subgroup_ops = expose;
   WRAPPER_LOG(info, "%s subgroup operations",
               expose ? "Exposing" : "Hiding");
}

static void
wrapper_setup_memory_budget(struct wrapper_physical_device *pdevice)
{
   if (pdevice->base_supported_extensions.EXT_memory_budget)
      return;

   if (!debug_get_bool_option("WRAPPER_MEMORY_BUDGET", true))
      return;

   pdevice->emulate_memory_budget = true;
   pdevice->memory_budget_reserve =
      (uint64_t)debug_get_num_option("WRAPPER_MEMORY_BUDGET_RESERVE", 1024) *
      1048576;
   pdevice->vk.supported_extensions.EXT_memory_budget = true;
   WRAPPER_LOG(info, "Emulating VK_EXT_memory_budget (reserve %" PRIu64 " MiB)",
               pdevice->memory_budget_reserve / 1048576);
}

/* Bytes the kernel considers available, cached for 100 ms. */
static uint64_t
wrapper_get_mem_available(void)
{
   static uint64_t cached_value;
   static int64_t cached_time;

   const int64_t now = os_time_get_nano();
   if (cached_time && now - p_atomic_read(&cached_time) < 100000000ll)
      return p_atomic_read(&cached_value);

   uint64_t value = 0;
   int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
   if (fd >= 0) {
      char buf[1024];
      ssize_t len = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (len > 0) {
         buf[len] = '\0';
         const char *line = strstr(buf, "MemAvailable:");
         if (line)
            value = strtoull(line + strlen("MemAvailable:"), NULL, 10) * 1024;
      }
   }

   p_atomic_set(&cached_value, value);
   p_atomic_set(&cached_time, now);
   return value;
}

static void
wrapper_fill_memory_budget(struct wrapper_physical_device *pdevice,
                           const VkPhysicalDeviceMemoryProperties *props,
                           VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget)
{
   const uint64_t available = wrapper_get_mem_available();
   const uint64_t spare = available > pdevice->memory_budget_reserve ?
      available - pdevice->memory_budget_reserve : 0;

   for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; i++) {
      if (i >= props->memoryHeapCount) {
         budget->heapUsage[i] = 0;
         budget->heapBudget[i] = 0;
         continue;
      }

      const uint64_t size = props->memoryHeaps[i].size;
      const uint64_t usage = p_atomic_read(&pdevice->heap_usage[i]);

      /* Without /proc/meminfo, report the whole heap like the driver would
       * implicitly do. */
      uint64_t heap_budget = available ? usage + spare : size;
      heap_budget = MIN2(heap_budget, size);
      if (heap_budget == 0)
         heap_budget = MIN2(size, 64ull * 1048576);

      budget->heapUsage[i] = usage;
      budget->heapBudget[i] = heap_budget;
   }
}

static VkResult
wrapper_physical_device_init(struct wrapper_instance *instance,
                             VkPhysicalDevice dispatch_handle,
                             struct wrapper_physical_device **out)
{
   struct vk_instance *_instance = &instance->vk;
   struct wrapper_physical_device *pdevice;
   VkResult result;

   static int wrapper_disable_placed = -1;
   static int wrapper_dmaheap_cached = -1;
   static int wrapper_disable_present_wait = -1;

   pdevice = vk_zalloc(&_instance->alloc, sizeof(*pdevice), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!pdevice)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   pdevice->dma_heap_fd = -1;

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_physical_device_trampolines, false);

   result = vk_physical_device_init(&pdevice->vk, &instance->vk,
                                    NULL, NULL, NULL, &dispatch_table);
   if (result != VK_SUCCESS) {
      vk_free(&_instance->alloc, pdevice);
      return result;
   }

   /* Features are faked/filtered by the wrapper itself (see
    * wrapper_CreateDevice), don't let vk_device_init() reject them. */
   pdevice->vk.skip_device_feature_check = true;

   pdevice->instance = instance;
   pdevice->dispatch_handle = dispatch_handle;

   vk_physical_device_dispatch_table_load(&pdevice->dispatch_table,
                                          instance->dispatch_table.GetInstanceProcAddr,
                                          instance->dispatch_handle);

   if (wrapper_disable_placed == -1)
      wrapper_disable_placed = debug_get_bool_option("WRAPPER_DISABLE_PLACED", false);

   if (wrapper_disable_present_wait == -1)
      wrapper_disable_present_wait = debug_get_bool_option("WRAPPER_DISABLE_PRESENT_WAIT", false);

   pdevice->driver_properties = (VkPhysicalDeviceDriverProperties) {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
   };
   pdevice->properties2 = (VkPhysicalDeviceProperties2) {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &pdevice->driver_properties,
   };
   pdevice->dispatch_table.GetPhysicalDeviceProperties2(
      pdevice->dispatch_handle, &pdevice->properties2);
   pdevice->properties2.pNext = NULL;

   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, &pdevice->memory_properties);

   result = wrapper_setup_device_extensions(pdevice);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&pdevice->vk);
      vk_free(&_instance->alloc, pdevice);
      return result;
   }
   wrapper_setup_device_features(pdevice);

   struct vk_features *supported_features = &pdevice->vk.supported_features;
   pdevice->base_supported_features = *supported_features;

   /* Implemented by the common WSI */
   supported_features->presentId = true;
   supported_features->presentId2 = true;
   supported_features->swapchainMaintenance1 = true;
   if (wrapper_disable_present_wait) {
      WRAPPER_LOG(info, "Disabling present wait");
      supported_features->presentWait = false;
      supported_features->presentWait2 = false;
      pdevice->vk.supported_extensions.KHR_present_wait = false;
      pdevice->vk.supported_extensions.KHR_present_wait2 = false;
   } else {
      supported_features->presentWait = supported_features->timelineSemaphore;
      supported_features->presentWait2 = supported_features->timelineSemaphore;
   }
   supported_features->imageCompressionControlSwapchain = false;

   /* Faked: the application may enable them, they are masked with what the
    * driver really supports before vkCreateDevice is forwarded. */
   supported_features->multiViewport = true;
   supported_features->depthClamp = true;
   supported_features->depthBiasClamp = true;
   supported_features->fillModeNonSolid = true;
   supported_features->shaderClipDistance = true;
   supported_features->shaderCullDistance = true;

   /* VK_EXT_map_memory_placed is emulated (see wrapper_device_memory.c) */
   if (!wrapper_disable_placed) {
      pdevice->vk.supported_extensions.EXT_map_memory_placed = true;
      pdevice->vk.supported_extensions.KHR_map_memory2 = true;
      supported_features->memoryMapPlaced = true;
      supported_features->memoryUnmapReserve = true;
   } else {
      WRAPPER_LOG(info, "Disabling VK_EXT_map_memory_placed");
      pdevice->vk.supported_extensions.EXT_map_memory_placed = false;
      supported_features->memoryMapPlaced = false;
      supported_features->memoryUnmapReserve = false;
   }

   result = wsi_device_init(&pdevice->wsi_device,
                            wrapper_physical_device_to_handle(pdevice),
                            wrapper_wsi_proc_addr, &_instance->alloc, -1,
                            NULL, &(struct wsi_device_options){});
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&pdevice->vk);
      vk_free(&_instance->alloc, pdevice);
      return result;
   }

   pdevice->vk.wsi_device = &pdevice->wsi_device;

   WRAPPER_LOG(info, "GPU Name: %s", pdevice->properties2.properties.deviceName);
   WRAPPER_LOG(info, "Driver Version: %u.%u.%u",
               VK_API_VERSION_MAJOR(pdevice->properties2.properties.driverVersion),
               VK_API_VERSION_MINOR(pdevice->properties2.properties.driverVersion),
               VK_API_VERSION_PATCH(pdevice->properties2.properties.driverVersion));

   const char *app_name = instance->vk.app_info.app_name
      ? instance->vk.app_info.app_name : "wrapper";
   const char *engine_name = instance->vk.app_info.engine_name
      ? instance->vk.app_info.engine_name : "wrapper";
   const uint32_t engine_version = instance->vk.app_info.engine_version;

   pdevice->wsi_device.engine_name = engine_name;

   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
      wrapper_apply_qualcomm_workarounds(pdevice, app_name, engine_name,
                                         engine_version);

   wrapper_setup_subgroup_ops(pdevice);
   wrapper_setup_memory_budget(pdevice);

   if (wrapper_dmaheap_cached == -1)
      wrapper_dmaheap_cached = debug_get_bool_option("WRAPPER_DMAHEAP_CACHED", false);

   pdevice->dma_heap_fd = open(wrapper_dmaheap_cached ? "/dev/dma_heap/system"
                                                      : "/dev/dma_heap/system-uncached",
                               O_RDONLY | O_CLOEXEC);
   if (pdevice->dma_heap_fd < 0) {
      WRAPPER_LOG(info, "dma-buf heap not found, falling back to /dev/ion");
      pdevice->dma_heap_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
   }

   const char *wrapper_resource_type = getenv("WRAPPER_RESOURCE_TYPE");
   pdevice->resource_type = wrapper_resource_type ? wrapper_resource_type : "auto";

   /* Channel order the emulator's X server consumes, see
    * get_sorted_vk_formats() in wsi_common_x11.c. */
   pdevice->wsi_device.force_bgra8_unorm_first = true;
   const char *wrapper_surface_format = getenv("WRAPPER_SURFACE_FORMAT");
   if (wrapper_surface_format && !strcmp(wrapper_surface_format, "rgba8")) {
      pdevice->wsi_device.force_rgba8_unorm_first = true;
      pdevice->wsi_device.force_bgra8_unorm_first = false;
   }

   /* Last, so the extensions the wrapper adds or emulates above (placed
    * memory, map_memory2, memory_budget, pipeline_library, ...) can be
    * blacklisted too. */
   wrapper_apply_device_extension_blacklist(pdevice);
   if (!pdevice->vk.supported_extensions.EXT_memory_budget)
      pdevice->emulate_memory_budget = false;

   *out = pdevice;
   return VK_SUCCESS;
}

VkResult
enumerate_physical_device(struct vk_instance *_instance)
{
   struct wrapper_instance *instance = (struct wrapper_instance *)_instance;
   VkPhysicalDevice *physical_devices;
   uint32_t physical_device_count = 0;
   VkResult result;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, NULL);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to retrieve physical devices count, res %d", result);
      return result;
   }

   if (physical_device_count == 0)
      return VK_SUCCESS;

   physical_devices = malloc(physical_device_count * sizeof(VkPhysicalDevice));
   if (!physical_devices)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, physical_devices);
   if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      WRAPPER_LOG(error, "Failed to enumerate physical devices, res %d", result);
      free(physical_devices);
      return result;
   }

   result = VK_SUCCESS;
   for (uint32_t i = 0; i < physical_device_count; i++) {
      struct wrapper_physical_device *pdevice;

      result = wrapper_physical_device_init(instance, physical_devices[i], &pdevice);
      if (result != VK_SUCCESS)
         break;

      list_addtail(&pdevice->vk.link, &_instance->physical_devices.list);
   }

   free(physical_devices);
   return result;
}

void
destroy_physical_device(struct vk_physical_device *pdevice)
{
   VK_FROM_HANDLE(wrapper_physical_device, wpdevice,
                  vk_physical_device_to_handle(pdevice));
   if (wpdevice->dma_heap_fd >= 0)
      close(wpdevice->dma_heap_fd);
   wsi_device_finish(pdevice->wsi_device, &pdevice->instance->alloc);
   vk_physical_device_finish(pdevice);
   vk_free(&pdevice->instance->alloc, pdevice);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                           const char* pLayerName,
                                           uint32_t* pPropertyCount,
                                           VkExtensionProperties* pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(physicalDevice,
                                                       pLayerName,
                                                       pPropertyCount,
                                                       pProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures* pFeatures)
{
   vk_common_GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures2* pFeatures)
{
   vk_common_GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

static void
wrapper_patch_device_identity(VkPhysicalDeviceProperties *props)
{
   const char *device_name_env = getenv("WRAPPER_DEVICE_NAME");
   char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

   snprintf(device_name, sizeof(device_name), "Wrapper(%s)",
            device_name_env ? device_name_env : props->deviceName);
   memcpy(props->deviceName, device_name, sizeof(device_name));

   const uint32_t device_id = debug_get_num_option("WRAPPER_DEVICE_ID", 0);
   const uint32_t vendor_id = debug_get_num_option("WRAPPER_VENDOR_ID", 0);
   const uint32_t api_version = parse_vk_version_from_env();

   if (device_id > 0)
      props->deviceID = device_id;

   if (vendor_id > 0)
      props->vendorID = vendor_id;

   if (api_version > 0)
      props->apiVersion = api_version;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                    VkPhysicalDeviceProperties *pProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties(
      pdevice->dispatch_handle, pProperties);

   wrapper_patch_device_identity(pProperties);
}

static void
wrapper_disable_float_controls(VkShaderFloatControlsIndependence *denorm,
                               VkShaderFloatControlsIndependence *rounding,
                               VkBool32 *flush_to_zero_16, VkBool32 *flush_to_zero_32,
                               VkBool32 *rte_16, VkBool32 *rte_32,
                               VkBool32 *preserve_16, VkBool32 *preserve_32)
{
   *denorm = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   *rounding = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   *flush_to_zero_16 = VK_FALSE;
   *flush_to_zero_32 = VK_FALSE;
   *rte_16 = VK_FALSE;
   *rte_32 = VK_FALSE;
   *preserve_16 = VK_FALSE;
   *preserve_32 = VK_FALSE;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties2* pProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   const bool is_qualcomm =
      pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;

   pdevice->dispatch_table.GetPhysicalDeviceProperties2(
      pdevice->dispatch_handle, pProperties);

   wrapper_patch_device_identity(&pProperties->properties);

   vk_foreach_struct(prop, pProperties->pNext) {
      switch (prop->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT: {
         VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *placed_prop = (void *)prop;
         uint64_t os_page_size;
         os_get_page_size(&os_page_size);
         placed_prop->minPlacedMemoryMapAlignment = os_page_size;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT: {
         VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *texel_prop = (void *)prop;
         texel_prop->storageTexelBufferOffsetAlignmentBytes = 1;
         texel_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES: {
         if (!is_qualcomm)
            break;

         VkPhysicalDeviceFloatControlsProperties *p = (void *)prop;
         wrapper_disable_float_controls(&p->denormBehaviorIndependence,
                                        &p->roundingModeIndependence,
                                        &p->shaderDenormFlushToZeroFloat16,
                                        &p->shaderDenormFlushToZeroFloat32,
                                        &p->shaderRoundingModeRTEFloat16,
                                        &p->shaderRoundingModeRTEFloat32,
                                        &p->shaderSignedZeroInfNanPreserveFloat16,
                                        &p->shaderSignedZeroInfNanPreserveFloat32);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
         if (pdevice->expose_subgroup_ops)
            break;

         VkPhysicalDeviceVulkan11Properties *vk11_prop = (void *)prop;
         vk11_prop->subgroupSupportedOperations = 0;
         vk11_prop->subgroupSupportedStages = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
         VkPhysicalDeviceVulkan12Properties *vk12_prop = (void *)prop;

         if (is_qualcomm) {
            wrapper_disable_float_controls(&vk12_prop->denormBehaviorIndependence,
                                           &vk12_prop->roundingModeIndependence,
                                           &vk12_prop->shaderDenormFlushToZeroFloat16,
                                           &vk12_prop->shaderDenormFlushToZeroFloat32,
                                           &vk12_prop->shaderRoundingModeRTEFloat16,
                                           &vk12_prop->shaderRoundingModeRTEFloat32,
                                           &vk12_prop->shaderSignedZeroInfNanPreserveFloat16,
                                           &vk12_prop->shaderSignedZeroInfNanPreserveFloat32);
         }

         const uint32_t driver_id = debug_get_num_option("WRAPPER_DRIVER_ID", 0);
         if (driver_id > 0)
            vk12_prop->driverID = driver_id;

         snprintf(vk12_prop->driverInfo, sizeof(vk12_prop->driverInfo),
                  "%u.%u.%u",
                  VK_API_VERSION_MAJOR(pProperties->properties.driverVersion),
                  VK_API_VERSION_MINOR(pProperties->properties.driverVersion),
                  VK_API_VERSION_PATCH(pProperties->properties.driverVersion));
         snprintf(vk12_prop->driverName, sizeof(vk12_prop->driverName),
                  "Wrapper driver");
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
         VkPhysicalDeviceVulkan13Properties *vk13_prop = (void *)prop;
         vk13_prop->storageTexelBufferOffsetAlignmentBytes = 1;
         vk13_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
         if (pdevice->expose_subgroup_ops)
            break;

         VkPhysicalDeviceSubgroupProperties *subgroup_prop = (void *)prop;
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
wrapper_patch_memory_heap_size(VkPhysicalDeviceMemoryProperties *props)
{
   static int wrapper_vmem_max_size = -1;

   if (wrapper_vmem_max_size == -1)
      wrapper_vmem_max_size = debug_get_num_option("WRAPPER_VMEM_MAX_SIZE", 0);

   if (wrapper_vmem_max_size > 0 && props->memoryHeapCount > 0)
      props->memoryHeaps[0].size = (VkDeviceSize)wrapper_vmem_max_size * 1048576;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, pMemoryProperties);

   wrapper_patch_memory_heap_size(pMemoryProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget = NULL;
   VkBaseOutStructure *budget_prev = NULL;

   /* The driver doesn't know the emulated budget struct, hide it. */
   if (pdevice->emulate_memory_budget) {
      VkBaseOutStructure *prev = (VkBaseOutStructure *)pMemoryProperties;
      vk_foreach_struct(ext, pMemoryProperties->pNext) {
         if (ext->sType ==
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            budget = (void *)ext;
            budget_prev = prev;
            prev->pNext = ext->pNext;
            break;
         }
         prev = ext;
      }
   }

   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties2(
      pdevice->dispatch_handle, pMemoryProperties);

   wrapper_patch_memory_heap_size(&pMemoryProperties->memoryProperties);

   if (budget) {
      budget_prev->pNext = (VkBaseOutStructure *)budget;
      wrapper_fill_memory_budget(pdevice, &pMemoryProperties->memoryProperties,
                                 budget);
   }
}
