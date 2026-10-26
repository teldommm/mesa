#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
#include "util/os_time.h"
#include "util/u_atomic.h"
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
 *   (WRAPPER_VK_VERSION     apiVersion, see wrapper_clamp_api_version())
 *
 * Numbers are decimal or 0x hexadecimal; a leading 0 is not octal.  Unset,
 * empty, 0 or malformed (logged) values are not spoofed and the driver's own
 * value is reported.  The ID / name / info of the driver are applied to both
 * VkPhysicalDeviceDriverProperties and VkPhysicalDeviceVulkan12Properties.
 *
 * WRAPPER_DRIVER_VERSION given as a dotted version is encoded the way the
 * reported vendor does it: NVIDIA packs major.minor.sub.patch as 10.8.8.6
 * bits, everything else uses the VK_MAKE_VERSION layout.  When
 * WRAPPER_DRIVER_INFO is unset, the spoofed version also becomes driverInfo.
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
      WLOGE("Ignoring %s=%s: not a number", name, str);
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
         WLOGE("Ignoring WRAPPER_DRIVER_VERSION=%s", driver_version);
      }
   }

   const char *device_name = wrapper_get_string_env("WRAPPER_DEVICE_NAME");
   if (device_name)
      snprintf(id->device_name, sizeof(id->device_name), "%s", device_name);

   const char *driver_name = wrapper_get_string_env("WRAPPER_DRIVER_NAME");
   if (driver_name)
      snprintf(id->driver_name, sizeof(id->driver_name), "%s", driver_name);

   const char *driver_info = wrapper_get_string_env("WRAPPER_DRIVER_INFO");
   if (driver_info)
      snprintf(id->driver_info, sizeof(id->driver_info), "%s", driver_info);
   else if (driver_version_info)
      snprintf(id->driver_info, sizeof(id->driver_info), "%s",
               driver_version_info);
   else if (id->driver_version)
      wrapper_format_driver_version(vendor_id, id->driver_version,
                                    id->driver_info, sizeof(id->driver_info));

   if (id->vendor_id || id->device_id || id->driver_id || id->driver_version ||
       id->device_name[0] || id->driver_name[0] || id->driver_info[0]) {
      WLOG("Spoofing identity: vendor 0x%04x device 0x%04x name \"%s\" "
           "driverID %d driver \"%s\" info \"%s\" driverVersion 0x%08x",
           id->vendor_id ? id->vendor_id : real->vendorID,
           id->device_id ? id->device_id : real->deviceID,
           id->device_name[0] ? id->device_name : real->deviceName,
           id->driver_id ? id->driver_id : pdevice->driver_properties.driverID,
           id->driver_name[0] ? id->driver_name
                              : pdevice->driver_properties.driverName,
           id->driver_info[0] ? id->driver_info
                              : pdevice->driver_properties.driverInfo,
           id->driver_version ? id->driver_version : real->driverVersion);
   }
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

   if (id->driver_name[0]) {
      memset(driver_name, 0, VK_MAX_DRIVER_NAME_SIZE);
      snprintf(driver_name, VK_MAX_DRIVER_NAME_SIZE, "%s", id->driver_name);
   }

   if (id->driver_info[0]) {
      memset(driver_info, 0, VK_MAX_DRIVER_INFO_SIZE);
      snprintf(driver_info, VK_MAX_DRIVER_INFO_SIZE, "%s", id->driver_info);
   }
}

/* The Qualcomm driver's own budget is replaced by the emulated one, other
 * drivers that have the extension keep theirs (Turnip already derives it
 * from the system memory). */
static void
wrapper_setup_memory_budget(struct wrapper_physical_device *pdevice)
{
   if (pdevice->base_supported_extensions.EXT_memory_budget && !pdevice->is_qcom)
      return;

   if (!ENV_INT("WRAPPER_MEMORY_BUDGET", 1))
      return;

   const int reserve_mb = ENV_INT("WRAPPER_MEMORY_BUDGET_RESERVE", 1024);
   pdevice->emulate_memory_budget = true;
   pdevice->memory_budget_reserve = (uint64_t) MAX2(reserve_mb, 0) * 1048576;
   pdevice->vk.supported_extensions.EXT_memory_budget = true;
   WLOG("Emulating VK_EXT_memory_budget (reserve %d MiB)", MAX2(reserve_mb, 0));
}

/* Mesa's GPU heap share of the physical memory (os_gpu_heap_size_calculate()
 * in newer Mesa): 25% up to 1 GiB, 50% up to 4 GiB, 75% above. */
static uint64_t
wrapper_gpu_heap_share(uint64_t memory)
{
   uint64_t share;
   if (memory <= 1ull << 30)
      share = memory / 4;
   else if (memory <= 4ull << 30)
      share = memory / 2;
   else
      share = memory / 4 * 3;
   return share & ~((1ull << 20) - 1);
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
   const int mib = ENV_INT("WRAPPER_VMEM_MAX_SIZE", 0);

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
   uint64_t size = (uint64_t) mib * 1048576;
   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_MESA_TURNIP) {
      size = MIN2(size, driver_size);
   } else {
      uint64_t total;
      if (os_get_total_physical_memory(&total))
         size = MIN2(size, MAX2(driver_size, wrapper_gpu_heap_share(total)));
   }

   pdevice->vmem_size = size;
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
      wrapper_setup_identity(pdevice);
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

      wrapper_setup_memory_budget(pdevice);
      wrapper_setup_vmem_override(pdevice);

      /* Blacklist last so the user can also remove extensions we fake. */
      wrapper_apply_device_extension_blacklist(pdevice);
      if (!pdevice->vk.supported_extensions.EXT_memory_budget)
         pdevice->emulate_memory_budget = false;

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

      /* Channel order of the swapchain images, which must be the order of the
       * emulator's own buffers, see get_sorted_vk_formats() in wsi_common_x11.c.
       * RGBA8 on every driver unless WRAPPER_SURFACE_FORMAT=bgra8. */
      const char *surface_format = getenv("WRAPPER_SURFACE_FORMAT");
      const bool bgra = surface_format && !strcmp(surface_format, "bgra8");
      pdevice->wsi_device.force_rgba8_unorm_first = !bgra;
      pdevice->wsi_device.force_bgra8_unorm_first = bgra;

      /* UBWC compressed swapchain images (WRAPPER_AHB_UBWC=1).  Only RGBA8888: a
       * compressed BGRA buffer shows black in the emulator.  Only the proprietary
       * Adreno driver: Turnip does not recognise UBWC in an imported
       * AHardwareBuffer and draws artefacts.  is_qcom comes from the real driver,
       * not from the identity the wrapper may present. */
      pdevice->wsi_device.android_ahb_ubwc =
         ENV_INT("WRAPPER_AHB_UBWC", 0) != 0 && !bgra && pdevice->is_qcom;

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
   const struct wrapper_identity *id = &pdevice->identity;

   props->apiVersion = wrapper_clamp_api_version(pdevice, props->apiVersion);

   if (id->vendor_id)
      props->vendorID = id->vendor_id;
   if (id->device_id)
      props->deviceID = id->device_id;
   if (id->driver_version)
      props->driverVersion = id->driver_version;

   if (id->device_name[0]) {
      memset(props->deviceName, 0, sizeof(props->deviceName));
      snprintf(props->deviceName, sizeof(props->deviceName), "%s",
               id->device_name);
   }
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
         wrapper_apply_driver_identity(pdevice, &p->driverID,
                                       p->driverName, p->driverInfo);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
      {
         VkPhysicalDeviceDriverProperties *driver_prop =
              (VkPhysicalDeviceDriverProperties *)prop;
         wrapper_apply_driver_identity(pdevice, &driver_prop->driverID,
                                       driver_prop->driverName,
                                       driver_prop->driverInfo);
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
wrapper_patch_memory_heap_size(const struct wrapper_physical_device *pdevice,
                               VkPhysicalDeviceMemoryProperties *props)
{
   if (pdevice->vmem_size && pdevice->vmem_heap < props->memoryHeapCount)
      props->memoryHeaps[pdevice->vmem_heap].size = pdevice->vmem_size;
}

/* Bytes the kernel considers available, cached for 100 ms. */
static uint64_t
wrapper_get_mem_available(void)
{
   static uint64_t cached_value;
   static int64_t cached_time;

   const int64_t now = os_time_get_nano();
   if (p_atomic_read(&cached_time) && now - p_atomic_read(&cached_time) < 100000000ll)
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

WRAPPER_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, pMemoryProperties);
   wrapper_patch_memory_heap_size(pdevice, pMemoryProperties);
}

WRAPPER_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget = NULL;
   VkBaseOutStructure *budget_prev = NULL;

   /* The driver doesn't know the emulated budget struct, hide it. */
   if (pdevice->emulate_memory_budget) {
      VkBaseOutStructure *prev = (VkBaseOutStructure *) pMemoryProperties;
      vk_foreach_struct(ext, pMemoryProperties->pNext) {
         if (ext->sType ==
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            budget = (void *) ext;
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
      budget_prev->pNext = (VkBaseOutStructure *) budget;
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
