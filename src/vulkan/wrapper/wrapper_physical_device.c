#include <errno.h>
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

/*
 * Device identity spoofing.
 *
 *   WRAPPER_VENDOR_ID       vendorID                    0x10de / 4318
 *   WRAPPER_DEVICE_ID       deviceID                    0x2684
 *   WRAPPER_DEVICE_NAME     deviceName, as is           NVIDIA GeForce RTX 4090
 *   WRAPPER_DRIVER_ID       VkDriverId                  4 (NVIDIA_PROPRIETARY)
 *   WRAPPER_DRIVER_NAME     driverName                  NVIDIA
 *   WRAPPER_DRIVER_INFO     driverInfo                  560.94
 *   WRAPPER_DRIVER_VERSION  driverVersion               560.94 or 0x8c178000
 *   WRAPPER_VK_VERSION      apiVersion                  1.3.280
 *
 * Numbers are decimal or 0x hexadecimal; a leading 0 is not octal.  Unset,
 * empty, 0 or malformed (logged) values are not spoofed.  The ID / name /
 * info of the driver are applied to both VkPhysicalDeviceDriverProperties
 * and VkPhysicalDeviceVulkan12Properties.
 *
 * WRAPPER_DRIVER_VERSION given as a dotted version is encoded the way the
 * reported vendor does it: NVIDIA packs major.minor.sub.patch as 10.8.8.6
 * bits, everything else uses the VK_MAKE_VERSION layout.  The dotted string also
 * becomes the default driverInfo.
 *
 * Without the variables the wrapper keeps reporting "Wrapper(<name>)",
 * "Wrapper driver" and the driver's version as driverInfo.
 */

#define WRAPPER_VENDOR_ID_NVIDIA 0x10de

static bool
wrapper_parse_u32(const char *str, uint32_t *out)
{
   while (*str == ' ' || *str == '\t')
      str++;

   int base = 10;
   if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
      base = 16;
      str += 2;
   }
   if (!*str)
      return false;

   char *end;
   errno = 0;
   const unsigned long long value = strtoull(str, &end, base);
   while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
      end++;

   if (errno || end == str || *end || value > UINT32_MAX)
      return false;

   *out = value;
   return true;
}

static uint32_t
wrapper_get_u32_env(const char *name)
{
   const char *str = getenv(name);
   uint32_t value = 0;

   if (!str || !*str)
      return 0;

   if (!wrapper_parse_u32(str, &value)) {
      WRAPPER_LOG(error, "Ignoring %s=%s: not a number", name, str);
      return 0;
   }

   return value;
}

static const char *
wrapper_get_string_env(const char *name)
{
   const char *str = getenv(name);
   return str && *str ? str : NULL;
}

/* "a.b[.c[.d]]" -> parts, returns the number of parts (0 if not dotted) */
static unsigned
wrapper_parse_dotted_version(const char *str, unsigned parts[4])
{
   char tail;

   parts[0] = parts[1] = parts[2] = parts[3] = 0;
   if (!strchr(str, '.'))
      return 0;

   const int n = sscanf(str, "%u.%u.%u.%u%c",
                        &parts[0], &parts[1], &parts[2], &parts[3], &tail);
   return n >= 2 && n <= 4 ? n : 0;
}

static uint32_t
wrapper_encode_driver_version(uint32_t vendor_id, const unsigned parts[4])
{
   if (vendor_id == WRAPPER_VENDOR_ID_NVIDIA) {
      return (parts[0] & 0x3ff) << 22 | (parts[1] & 0xff) << 14 |
             (parts[2] & 0xff) << 6 | (parts[3] & 0x3f);
   }

   /* VK_MAKE_VERSION layout (10.10.12 bits): unlike VK_MAKE_API_VERSION it
    * keeps majors >= 128, e.g. the Qualcomm driver's 512.x */
   return (parts[0] & 0x3ff) << 22 | (parts[1] & 0x3ff) << 12 |
          (parts[2] & 0xfff);
}

static void
wrapper_format_driver_version(uint32_t vendor_id, uint32_t version,
                              char *buf, size_t size)
{
   if (vendor_id == WRAPPER_VENDOR_ID_NVIDIA) {
      const unsigned sub = (version >> 6) & 0xff, patch = version & 0x3f;

      if (sub || patch)
         snprintf(buf, size, "%u.%u.%u.%u", version >> 22,
                  (version >> 14) & 0xff, sub, patch);
      else
         snprintf(buf, size, "%u.%02u", version >> 22, (version >> 14) & 0xff);
      return;
   }

   snprintf(buf, size, "%u.%u.%u",
            version >> 22, (version >> 12) & 0x3ff, version & 0xfff);
}

static void
wrapper_setup_identity(struct wrapper_physical_device *pdevice)
{
   struct wrapper_identity *id = &pdevice->identity;
   const VkPhysicalDeviceProperties *real = &pdevice->properties2.properties;

   memset(id, 0, sizeof(*id));

   id->vendor_id = wrapper_get_u32_env("WRAPPER_VENDOR_ID");
   id->device_id = wrapper_get_u32_env("WRAPPER_DEVICE_ID");
   id->driver_id = (VkDriverId)wrapper_get_u32_env("WRAPPER_DRIVER_ID");

   const char *vk_version = wrapper_get_string_env("WRAPPER_VK_VERSION");
   if (vk_version) {
      unsigned major = 0, minor = 0, patch = 0;
      if (sscanf(vk_version, "%u.%u.%u", &major, &minor, &patch) >= 2)
         id->api_version = VK_MAKE_API_VERSION(0, major, minor, patch);
      else
         WRAPPER_LOG(error, "Ignoring WRAPPER_VK_VERSION=%s", vk_version);
   }

   /* The vendor the application will see decides the version encoding */
   const uint32_t vendor_id = id->vendor_id ? id->vendor_id : real->vendorID;

   const char *driver_version = wrapper_get_string_env("WRAPPER_DRIVER_VERSION");
   const char *driver_version_info = NULL;
   if (driver_version) {
      unsigned parts[4];
      uint32_t value;

      if (wrapper_parse_dotted_version(driver_version, parts)) {
         id->driver_version = wrapper_encode_driver_version(vendor_id, parts);
         driver_version_info = driver_version;
      } else if (wrapper_parse_u32(driver_version, &value)) {
         id->driver_version = value;
      } else {
         WRAPPER_LOG(error, "Ignoring WRAPPER_DRIVER_VERSION=%s", driver_version);
      }
   }

   const char *device_name = wrapper_get_string_env("WRAPPER_DEVICE_NAME");
   if (device_name)
      snprintf(id->device_name, sizeof(id->device_name), "%s", device_name);
   else
      snprintf(id->device_name, sizeof(id->device_name), "Wrapper(%s)",
               real->deviceName);

   const char *driver_name = wrapper_get_string_env("WRAPPER_DRIVER_NAME");
   snprintf(id->driver_name, sizeof(id->driver_name), "%s",
            driver_name ? driver_name : "Wrapper driver");

   const char *driver_info = wrapper_get_string_env("WRAPPER_DRIVER_INFO");
   if (driver_info)
      snprintf(id->driver_info, sizeof(id->driver_info), "%s", driver_info);
   else if (driver_version_info)
      snprintf(id->driver_info, sizeof(id->driver_info), "%s",
               driver_version_info);
   else if (id->driver_version)
      wrapper_format_driver_version(vendor_id, id->driver_version,
                                    id->driver_info, sizeof(id->driver_info));
   else
      wrapper_format_driver_version(0, real->driverVersion,
                                    id->driver_info, sizeof(id->driver_info));

   if (id->vendor_id || id->device_id || id->driver_id || id->driver_version ||
       device_name || driver_name || driver_info) {
      WRAPPER_LOG(info, "Spoofing identity: vendor 0x%04x device 0x%04x "
                  "name \"%s\" driverID %d driver \"%s\" info \"%s\" "
                  "driverVersion 0x%08x",
                  id->vendor_id ? id->vendor_id : real->vendorID,
                  id->device_id ? id->device_id : real->deviceID,
                  id->device_name,
                  id->driver_id ? id->driver_id
                                : pdevice->driver_properties.driverID,
                  id->driver_name, id->driver_info,
                  id->driver_version ? id->driver_version
                                     : real->driverVersion);
   }
}

static void
wrapper_apply_device_identity(const struct wrapper_physical_device *pdevice,
                              VkPhysicalDeviceProperties *props)
{
   const struct wrapper_identity *id = &pdevice->identity;

   if (id->vendor_id)
      props->vendorID = id->vendor_id;
   if (id->device_id)
      props->deviceID = id->device_id;
   if (id->driver_version)
      props->driverVersion = id->driver_version;
   if (id->api_version)
      props->apiVersion = id->api_version;

   memset(props->deviceName, 0, sizeof(props->deviceName));
   snprintf(props->deviceName, sizeof(props->deviceName), "%s",
            id->device_name);
}

/* VkPhysicalDeviceDriverProperties and VkPhysicalDeviceVulkan12Properties */
static void
wrapper_apply_driver_identity(const struct wrapper_physical_device *pdevice,
                              VkDriverId *driver_id,
                              char driver_name[VK_MAX_DRIVER_NAME_SIZE],
                              char driver_info[VK_MAX_DRIVER_INFO_SIZE])
{
   const struct wrapper_identity *id = &pdevice->identity;

   if (id->driver_id)
      *driver_id = id->driver_id;

   memset(driver_name, 0, VK_MAX_DRIVER_NAME_SIZE);
   snprintf(driver_name, VK_MAX_DRIVER_NAME_SIZE, "%s", id->driver_name);
   memset(driver_info, 0, VK_MAX_DRIVER_INFO_SIZE);
   snprintf(driver_info, VK_MAX_DRIVER_INFO_SIZE, "%s", id->driver_info);
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

/* Workarounds for the proprietary ARM (Mali, MediaTek Dimensity, ...) driver,
 * ported from the Mesa 25 based wrapper.  The faked features are masked with
 * what the driver supports in wrapper_CreateDevice(), the SPIR-V side is in
 * wrapper_CreateShaderModule(). */
static void
wrapper_apply_arm_workarounds(struct wrapper_physical_device *pdevice,
                              const char *engine_name)
{
   struct vk_features *supported_features = &pdevice->vk.supported_features;

   if (engine_is(engine_name, "DXVK")) {
      /* DXVK 2.x requires VK_EXT_robustness2.  Only the extension is
       * advertised (as in the Mesa 25 wrapper), the features stay the
       * driver's; the feature struct is dropped from vkCreateDevice when the
       * driver lacks the extension. */
      if (!pdevice->vk.supported_extensions.EXT_robustness2 &&
          !pdevice->vk.supported_extensions.KHR_robustness2) {
         WRAPPER_LOG(info, "Faking VK_EXT_robustness2");
         pdevice->vk.supported_extensions.EXT_robustness2 = true;
      }

      /* Needed for D3D11 feature level 11.0 */
      WRAPPER_LOG(info, "Faking dualSrcBlend feature");
      supported_features->dualSrcBlend = true;
      WRAPPER_LOG(info, "Faking multiDrawIndirect feature");
      supported_features->multiDrawIndirect = true;
   }

   WRAPPER_LOG(info, "Disabling VK_EXT/KHR_calibrated_timestamps");
   pdevice->vk.supported_extensions.EXT_calibrated_timestamps = false;
   pdevice->vk.supported_extensions.KHR_calibrated_timestamps = false;

   WRAPPER_LOG(info, "Disabling VK_EXT_extended_dynamic_state and VK_EXT_extended_dynamic_state2");
   pdevice->vk.supported_extensions.EXT_extended_dynamic_state = false;
   pdevice->vk.supported_extensions.EXT_extended_dynamic_state2 = false;
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

/* The Qualcomm driver's own budget is replaced by the emulated one, other
 * drivers that have the extension keep theirs (Turnip already derives it
 * from the system memory). */
static void
wrapper_setup_memory_budget(struct wrapper_physical_device *pdevice)
{
   const bool qualcomm =
      pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;

   if (pdevice->base_supported_extensions.EXT_memory_budget && !qualcomm)
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

/* WRAPPER_VMEM_MAX_SIZE (MiB): size reported for the first device-local
 * heap (DXGI DedicatedVideoMemory, the 80% heuristics of DXVK and zink).
 * Lowering always applies.  Turnip refuses allocations past its own heap, so
 * there it can't be raised; other drivers may report a larger heap, up to
 * the share of the physical memory Mesa uses for a GPU heap (75% above
 * 4 GiB, like Turnip) or the driver's own heap if that is larger. */
static void
wrapper_setup_vmem_override(struct wrapper_physical_device *pdevice)
{
   const VkPhysicalDeviceMemoryProperties *props = &pdevice->memory_properties;
   const int64_t mib = debug_get_num_option("WRAPPER_VMEM_MAX_SIZE", 0);

   pdevice->vmem_heap = 0;
   pdevice->vmem_size = 0;
   if (mib <= 0 || props->memoryHeapCount == 0)
      return;

   for (uint32_t i = 0; i < props->memoryHeapCount; i++) {
      if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
         pdevice->vmem_heap = i;
         break;
      }
   }

   const uint64_t driver_size = props->memoryHeaps[pdevice->vmem_heap].size;
   uint64_t size = MIN2((uint64_t)mib, UINT64_MAX >> 20) * 1048576;
   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_MESA_TURNIP) {
      size = MIN2(size, driver_size);
   } else {
      uint64_t total;
      if (os_get_total_physical_memory(&total)) {
         const uint64_t limit =
            os_gpu_heap_size_calculate(total, OS_GPU_HEAP_SIZE_HEURISTIC, NULL);
         size = MIN2(size, MAX2(driver_size, limit));
      }
   }

   pdevice->vmem_size = size;
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

      /* Usage past a lowered heap is reported as the heap size: still over
       * budget, but "size - usage" can't wrap around (zink's meminfo). */
      budget->heapUsage[i] = MIN2(usage, size);
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

   wrapper_setup_identity(pdevice);

   const char *app_name = instance->vk.app_info.app_name
      ? instance->vk.app_info.app_name : "wrapper";
   const char *engine_name = instance->vk.app_info.engine_name
      ? instance->vk.app_info.engine_name : "wrapper";
   const uint32_t engine_version = instance->vk.app_info.engine_version;

   pdevice->wsi_device.engine_name = engine_name;

   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
      wrapper_apply_qualcomm_workarounds(pdevice, app_name, engine_name,
                                         engine_version);
   else if (pdevice->driver_properties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY)
      wrapper_apply_arm_workarounds(pdevice, engine_name);

   /* BCn formats (WRAPPER_EMULATE_BCN, see wrapper_bcn.c) */
   wrapper_bcn_setup_physical_device(pdevice);

   wrapper_setup_subgroup_ops(pdevice);
   wrapper_setup_memory_budget(pdevice);
   wrapper_setup_vmem_override(pdevice);

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

   /* Channel order of the swapchain images, which must be the order of the emulator's own
    * buffers, see get_sorted_vk_formats() in wsi_common_x11.c.  RGBA8 on every driver unless
    * WRAPPER_SURFACE_FORMAT=bgra8. */
   const char *wrapper_surface_format = getenv("WRAPPER_SURFACE_FORMAT");
   const bool wrapper_bgra = wrapper_surface_format && !strcmp(wrapper_surface_format, "bgra8");
   pdevice->wsi_device.force_rgba8_unorm_first = !wrapper_bgra;
   pdevice->wsi_device.force_bgra8_unorm_first = wrapper_bgra;

   /* UBWC compressed swapchain images (WRAPPER_AHB_UBWC=1).  Only RGBA8888: a
    * compressed BGRA buffer shows black in the emulator.  Only the proprietary
    * Adreno driver: Turnip does not recognise UBWC in an imported AHardwareBuffer
    * and draws artefacts.  Decided from pdevice->driver_properties, the real
    * driver: what the WSI sees through the wrapper may be a spoofed identity. */
   pdevice->wsi_device.android_ahb_ubwc =
      debug_get_bool_option("WRAPPER_AHB_UBWC", false) && !wrapper_bgra &&
      pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;

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

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                    VkPhysicalDeviceProperties *pProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties(
      pdevice->dispatch_handle, pProperties);

   wrapper_apply_device_identity(pdevice, pProperties);
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

   wrapper_apply_device_identity(pdevice, &pProperties->properties);

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

         wrapper_apply_driver_identity(pdevice, &vk12_prop->driverID,
                                       vk12_prop->driverName,
                                       vk12_prop->driverInfo);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
         VkPhysicalDeviceDriverProperties *driver_prop = (void *)prop;
         wrapper_apply_driver_identity(pdevice, &driver_prop->driverID,
                                       driver_prop->driverName,
                                       driver_prop->driverInfo);
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
wrapper_patch_memory_heap_size(const struct wrapper_physical_device *pdevice,
                               VkPhysicalDeviceMemoryProperties *props)
{
   if (pdevice->vmem_size && pdevice->vmem_heap < props->memoryHeapCount)
      props->memoryHeaps[pdevice->vmem_heap].size = pdevice->vmem_size;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, pMemoryProperties);

   wrapper_patch_memory_heap_size(pdevice, pMemoryProperties);
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

   const VkPhysicalDeviceMemoryProperties *props =
      &pMemoryProperties->memoryProperties;
   wrapper_patch_memory_heap_size(pdevice, &pMemoryProperties->memoryProperties);

   if (budget) {
      budget_prev->pNext = (VkBaseOutStructure *)budget;
      wrapper_fill_memory_budget(pdevice, props, budget);
   } else {
      /* The driver's own budget: heapBudget must not exceed the heap size,
       * which WRAPPER_VMEM_MAX_SIZE may have lowered; usage past it is
       * reported as the heap size, as in wrapper_fill_memory_budget(). */
      VkPhysicalDeviceMemoryBudgetPropertiesEXT *native =
         vk_find_struct(pMemoryProperties->pNext,
                        PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
      for (uint32_t i = 0; native && i < props->memoryHeapCount; i++) {
         const VkDeviceSize size = props->memoryHeaps[i].size;
         native->heapBudget[i] = MIN2(native->heapBudget[i], size);
         native->heapUsage[i] = MIN2(native->heapUsage[i], size);
      }
   }
}
