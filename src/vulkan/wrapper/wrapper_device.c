#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_queue.h"
#include "vk_util.h"
#include "util/list.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"

/* Stack storage for the per-frame submits, bigger batches use the heap */
#define WRAPPER_STACK_SUBMITS 8
#define WRAPPER_STACK_COMMAND_BUFFERS 64

const struct vk_device_extension_table wrapper_device_extensions =
{
   .KHR_swapchain = true,
   .EXT_swapchain_maintenance1 = true,
   .KHR_swapchain_maintenance1 = true,
   .KHR_swapchain_mutable_format = true,
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .EXT_display_control = true,
#endif
   .KHR_present_id = true,
   .KHR_present_id2 = true,
   .KHR_present_wait = true,
   .KHR_present_wait2 = true,
   .KHR_incremental_present = true,
};

const struct vk_device_extension_table wrapper_filter_extensions =
{
   .EXT_hdr_metadata = true,
   .GOOGLE_display_timing = true,
   .KHR_shared_presentable_image = true,
   .EXT_image_compression_control_swapchain = true,
   /* Needs RandR CRTC queries the emulator's X server doesn't implement */
   .EXT_present_timing = true,
};

static void
add_extension(const char **exts, uint32_t *count, const char *name)
{
   for (uint32_t i = 0; i < *count; i++) {
      if (!strcmp(exts[i], name))
         return;
   }
   assert(*count < VK_DEVICE_EXTENSION_COUNT);
   exts[(*count)++] = name;
}

static void
wrapper_filter_enabled_extensions(const struct wrapper_device *device,
                                  uint32_t *enable_extension_count,
                                  const char **enable_extensions)
{
   for (int idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
      if (!device->vk.enabled_extensions.extensions[idx])
         continue;

      if (!device->physical->base_supported_extensions.extensions[idx])
         continue;

      if (wrapper_device_extensions.extensions[idx])
         continue;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      add_extension(enable_extensions, enable_extension_count,
                    vk_device_extensions[idx].extensionName);
   }
}

/* Extensions the wrapper and the common WSI code need on the driver's
 * device, whether the application enabled them or not. */
static void
wrapper_append_required_extensions(const struct wrapper_device *device,
                                   uint32_t *count,
                                   const char **exts)
{
   const struct vk_device_extension_table *base =
      &device->physical->base_supported_extensions;

#define REQUIRED_EXTENSION(name) \
   if (base->name) \
      add_extension(exts, count, "VK_" #name);

   REQUIRED_EXTENSION(KHR_external_fence);
   REQUIRED_EXTENSION(KHR_external_semaphore);
   REQUIRED_EXTENSION(KHR_external_memory);
   REQUIRED_EXTENSION(KHR_external_fence_fd);
   REQUIRED_EXTENSION(KHR_external_semaphore_fd);
   REQUIRED_EXTENSION(KHR_external_memory_fd);
   REQUIRED_EXTENSION(KHR_dedicated_allocation);
   REQUIRED_EXTENSION(EXT_queue_family_foreign);
   REQUIRED_EXTENSION(KHR_maintenance1);
   REQUIRED_EXTENSION(KHR_maintenance2);
   REQUIRED_EXTENSION(KHR_image_format_list);
   REQUIRED_EXTENSION(KHR_swapchain);
   REQUIRED_EXTENSION(KHR_timeline_semaphore);
   REQUIRED_EXTENSION(EXT_external_memory_host);
   REQUIRED_EXTENSION(EXT_external_memory_dma_buf);
   REQUIRED_EXTENSION(EXT_image_drm_format_modifier);
   REQUIRED_EXTENSION(ANDROID_external_memory_android_hardware_buffer);
#undef REQUIRED_EXTENSION
}

static void
unlink_vk_struct(VkBaseInStructure *create_info,
                 const VkBaseInStructure **current,
                 VkBaseInStructure **prev)
{
   if (!*prev)
      create_info->pNext = (*current)->pNext;
   else
      (*prev)->pNext = (*current)->pNext;

   *current = (*current)->pNext;
}

/* Drops the feature structs of extensions the wrapper implements or fakes
 * and that the driver doesn't know about. */
static void
process_device_pnext_chain(VkBaseInStructure *create_info,
                           struct wrapper_physical_device *pdevice)
{
   const uint32_t api_version = pdevice->properties2.properties.apiVersion;
   const struct vk_device_extension_table *base =
      &pdevice->base_supported_extensions;
   const VkBaseInStructure *current = (VkBaseInStructure *)create_info->pNext;
   VkBaseInStructure *prev = NULL;

#define UNLINK_UNLESS(cond, what) \
   if (cond) \
      break; \
   WRAPPER_LOG(info, "Unlinking " what " from the device pNext chain"); \
   unlink_vk_struct(create_info, &current, &prev); \
   continue;

   while (current != NULL) {
      switch (current->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
         VkPhysicalDeviceTransformFeedbackFeaturesEXT *transform_features =
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)current;
         transform_features->geometryStreams &=
            pdevice->base_supported_features.geometryStreams;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
         UNLINK_UNLESS(base->EXT_robustness2 || base->KHR_robustness2,
                       "VkPhysicalDeviceRobustness2Features");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
         UNLINK_UNLESS(base->KHR_present_id, "VkPhysicalDevicePresentIdFeaturesKHR");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR:
         UNLINK_UNLESS(base->KHR_present_id2, "VkPhysicalDevicePresentId2FeaturesKHR");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
         UNLINK_UNLESS(base->KHR_present_wait, "VkPhysicalDevicePresentWaitFeaturesKHR");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR:
         UNLINK_UNLESS(base->KHR_present_wait2, "VkPhysicalDevicePresentWait2FeaturesKHR");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR:
         UNLINK_UNLESS(base->KHR_swapchain_maintenance1 ||
                       base->EXT_swapchain_maintenance1,
                       "VkPhysicalDeviceSwapchainMaintenance1Features");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT:
         UNLINK_UNLESS(base->EXT_map_memory_placed,
                       "VkPhysicalDeviceMapMemoryPlacedFeaturesEXT");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
         UNLINK_UNLESS(api_version >= VK_API_VERSION_1_1,
                       "VkPhysicalDeviceVulkan11Features");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
         UNLINK_UNLESS(api_version >= VK_API_VERSION_1_2,
                       "VkPhysicalDeviceVulkan12Features");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
         UNLINK_UNLESS(api_version >= VK_API_VERSION_1_3,
                       "VkPhysicalDeviceVulkan13Features");
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
         UNLINK_UNLESS(api_version >= VK_API_VERSION_1_4,
                       "VkPhysicalDeviceVulkan14Features");
      default:
         break;
      }
      prev = (VkBaseInStructure *)current;
      current = current->pNext;
   }
#undef UNLINK_UNLESS
}

static VkResult
wrapper_create_device_queue(struct wrapper_device *device,
                            const VkDeviceCreateInfo* pCreateInfo)
{
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *create_info = &pCreateInfo->pQueueCreateInfos[i];
      for (uint32_t j = 0; j < create_info->queueCount; j++) {
         struct wrapper_queue *queue =
            vk_zalloc(&device->vk.alloc, sizeof(*queue), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (!queue)
            return VK_ERROR_OUT_OF_HOST_MEMORY;

         if (create_info->flags) {
            device->dispatch_table.GetDeviceQueue2(
               device->dispatch_handle,
               &(VkDeviceQueueInfo2) {
                  .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                  .flags = create_info->flags,
                  .queueFamilyIndex = create_info->queueFamilyIndex,
                  .queueIndex = j,
               },
               &queue->dispatch_handle);
         } else {
            device->dispatch_table.GetDeviceQueue(
               device->dispatch_handle, create_info->queueFamilyIndex,
               j, &queue->dispatch_handle);
         }
         queue->device = device;

         VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info, j);
         if (result != VK_SUCCESS) {
            vk_free(&device->vk.alloc, queue);
            return result;
         }
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateDevice(VkPhysicalDevice physicalDevice,
                     const VkDeviceCreateInfo* pCreateInfo,
                     const VkAllocationCallbacks* pAllocator,
                     VkDevice* pDevice)
{
   VK_FROM_HANDLE(wrapper_physical_device, physical_device, physicalDevice);
   const char *wrapper_enable_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t wrapper_enable_extension_count = 0;
   VkDeviceCreateInfo wrapper_create_info = *pCreateInfo;
   struct vk_device_dispatch_table dispatch_table;
   struct wrapper_device *device;
   VkPhysicalDeviceFeatures2 *pdf2;
   VkPhysicalDeviceFeatures *pdf = NULL;
   VkPhysicalDeviceFeatures enabled_features;
   VkResult result;
   static int wrapper_safe_create_device = -1;
   static int wrapper_device_fault = -1;
   VkPhysicalDeviceFaultFeaturesEXT fault_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
   };
   bool used_fallback_create = false;

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   list_inithead(&device->command_buffer_list);
   list_inithead(&device->device_memory_list);
   device->memory_table = _mesa_hash_table_u64_create(NULL);
   if (!device->memory_table) {
      vk_free2(&physical_device->instance->vk.alloc, pAllocator, device);
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (physical_device->emulate_memory_budget) {
      device->budget_table = _mesa_hash_table_u64_create(NULL);
      if (!device->budget_table) {
         _mesa_hash_table_u64_destroy(device->memory_table);
         vk_free2(&physical_device->instance->vk.alloc, pAllocator, device);
         return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   simple_mtx_init(&device->resource_mutex, mtx_plain);
   device->physical = physical_device;

   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_device_trampolines, false);

   result = vk_device_init(&device->vk, &physical_device->vk,
                           &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to init Vulkan device, res %d", result);
      _mesa_hash_table_u64_destroy(device->memory_table);
      if (device->budget_table)
         _mesa_hash_table_u64_destroy(device->budget_table);
      simple_mtx_destroy(&device->resource_mutex);
      vk_free2(&physical_device->instance->vk.alloc, pAllocator, device);
      return vk_error(physical_device, result);
   }

   wrapper_filter_enabled_extensions(device,
      &wrapper_enable_extension_count, wrapper_enable_extensions);
   wrapper_append_required_extensions(device,
      &wrapper_enable_extension_count, wrapper_enable_extensions);

   /* VK_EXT_device_fault turns a bare VK_ERROR_DEVICE_LOST into a report of
    * the faulting address and vendor fault codes (WRAPPER_DEVICE_FAULT=1). */
   if (wrapper_device_fault == -1)
      wrapper_device_fault = debug_get_bool_option("WRAPPER_DEVICE_FAULT", false);

   const bool enable_device_fault = wrapper_device_fault &&
      physical_device->base_supported_extensions.EXT_device_fault &&
      physical_device->base_supported_features.deviceFault;
   if (enable_device_fault)
      add_extension(wrapper_enable_extensions, &wrapper_enable_extension_count,
                    "VK_EXT_device_fault");

   wrapper_create_info.enabledExtensionCount = wrapper_enable_extension_count;
   wrapper_create_info.ppEnabledExtensionNames = wrapper_enable_extensions;

   /* Mask the faked features with what the driver really supports.  The
    * VkPhysicalDeviceFeatures2 in the chain belongs to the application, only
    * write to it when something actually has to be cleared. */
   if (pCreateInfo->pEnabledFeatures) {
      enabled_features = *pCreateInfo->pEnabledFeatures;
      pdf = &enabled_features;
      wrapper_create_info.pEnabledFeatures = pdf;
   }
   pdf2 = __vk_find_struct((void *)pCreateInfo->pNext,
                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);

#define DISABLE_FEATURE(f) \
   if (!physical_device->base_supported_features.f) { \
      if (pdf && pdf->f) \
         pdf->f = VK_FALSE; \
      if (pdf2 && pdf2->features.f) \
         pdf2->features.f = VK_FALSE; \
   }

   DISABLE_FEATURE(multiViewport);
   DISABLE_FEATURE(depthClamp);
   DISABLE_FEATURE(depthBiasClamp);
   DISABLE_FEATURE(fillModeNonSolid);
   DISABLE_FEATURE(shaderClipDistance);
   DISABLE_FEATURE(shaderCullDistance);
#undef DISABLE_FEATURE

   process_device_pnext_chain((VkBaseInStructure *)&wrapper_create_info,
                              device->physical);

   if (enable_device_fault &&
       !vk_find_struct_const(wrapper_create_info.pNext, PHYSICAL_DEVICE_FAULT_FEATURES_EXT)) {
      fault_features.deviceFault = VK_TRUE;
      fault_features.deviceFaultVendorBinary =
         physical_device->base_supported_features.deviceFaultVendorBinary;
      fault_features.pNext = (void *)wrapper_create_info.pNext;
      wrapper_create_info.pNext = &fault_features;
   }

   if (WRAPPER_LOG_LEVEL(info)) {
      for (uint32_t i = 0; i < wrapper_enable_extension_count; i++)
         WRAPPER_LOG(info, "Enabling device extension %s", wrapper_enable_extensions[i]);
   }

   if (wrapper_safe_create_device == -1)
      wrapper_safe_create_device = debug_get_bool_option("WRAPPER_SAFE_CREATE_DEVICE", true);

   result = physical_device->dispatch_table.CreateDevice(
      physical_device->dispatch_handle, &wrapper_create_info,
      pAllocator, &device->dispatch_handle);

   if (result != VK_SUCCESS && wrapper_safe_create_device) {
      WRAPPER_LOG(info, "Forcing device creation with a NULL pNext chain");
      wrapper_create_info.pNext = NULL;
      used_fallback_create = true;
      result = physical_device->dispatch_table.CreateDevice(
         physical_device->dispatch_handle, &wrapper_create_info,
         pAllocator, &device->dispatch_handle);
   }

   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed driver createDevice, res %d", result);
      device->dispatch_handle = VK_NULL_HANDLE;
      wrapper_DestroyDevice(wrapper_device_to_handle(device), pAllocator);
      return vk_error(physical_device, result);
   }

   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)
      physical_device->instance->dispatch_table.GetInstanceProcAddr(
         physical_device->instance->dispatch_handle, "vkGetDeviceProcAddr");
   vk_device_dispatch_table_load(&device->dispatch_table, gdpa,
                                 device->dispatch_handle);

   device->device_fault_enabled = enable_device_fault && !used_fallback_create;

   /* The common WSI code submits with vkQueueSubmit2, which is only valid on
    * the driver's device with synchronization2 enabled. */
   device->has_queue_submit2 =
      device->dispatch_table.QueueSubmit2 != NULL &&
      device->vk.enabled_features.synchronization2 &&
      physical_device->base_supported_features.synchronization2 &&
      !used_fallback_create;

   result = wrapper_create_device_queue(device, pCreateInfo);
   if (result != VK_SUCCESS) {
      wrapper_DestroyDevice(wrapper_device_to_handle(device), pAllocator);
      return vk_error(physical_device, result);
   }

   /* Without the placed-map emulation the memory entrypoints go straight to
    * the driver. */
   if (!physical_device->vk.supported_features.memoryMapPlaced) {
      device->vk.dispatch_table.MapMemory2 =
         wrapper_device_trampolines.MapMemory2;
      device->vk.dispatch_table.UnmapMemory =
         wrapper_device_trampolines.UnmapMemory;
      device->vk.dispatch_table.UnmapMemory2 =
         wrapper_device_trampolines.UnmapMemory2;

      /* The emulated memory budget still needs to see every allocation. */
      if (!device->budget_table) {
         device->vk.dispatch_table.AllocateMemory =
            wrapper_device_trampolines.AllocateMemory;
         device->vk.dispatch_table.FreeMemory =
            wrapper_device_trampolines.FreeMemory;
      }
   }

   *pDevice = wrapper_device_to_handle(device);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                       uint32_t queueIndex, VkQueue* pQueue)
{
   vk_common_GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceQueue2(VkDevice _device, const VkDeviceQueueInfo2* pQueueInfo,
                        VkQueue* pQueue)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   struct vk_queue *queue = NULL;
   vk_foreach_queue(iter, device) {
      if (iter->queue_family_index == pQueueInfo->queueFamilyIndex &&
          iter->index_in_family == pQueueInfo->queueIndex &&
          iter->flags == pQueueInfo->flags) {
         queue = iter;
         break;
      }
   }

   *pQueue = queue ? vk_queue_to_handle(queue) : VK_NULL_HANDLE;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_GetDeviceProcAddr(VkDevice _device, const char* pName)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   return vk_device_get_proc_addr(&device->vk, pName);
}

static const char *
wrapper_fault_addr_type_str(VkDeviceFaultAddressTypeEXT t)
{
   switch (t) {
   case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:                        return "none";
   case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:               return "read-invalid";
   case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:              return "write-invalid";
   case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:            return "execute-invalid";
   case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "ip-unknown";
   case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "ip-invalid";
   case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:  return "ip-fault";
   default:                                                          return "unknown";
   }
}

/* On VK_ERROR_DEVICE_LOST dump the VK_EXT_device_fault report to stderr
 * (and the wrapper log) so a GPU hang shows where it faulted. */
static void
wrapper_log_device_fault(struct wrapper_device *device)
{
   PFN_vkGetDeviceFaultInfoEXT get_fault_info =
      device->dispatch_table.GetDeviceFaultInfoEXT;

   if (!device->device_fault_enabled || !get_fault_info)
      return;

   VkDeviceFaultCountsEXT counts = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
   };
   if (get_fault_info(device->dispatch_handle, &counts, NULL) != VK_SUCCESS)
      return;

   VkDeviceFaultInfoEXT info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
   };
   if (counts.addressInfoCount)
      info.pAddressInfos =
         calloc(counts.addressInfoCount, sizeof(VkDeviceFaultAddressInfoEXT));
   if (counts.vendorInfoCount)
      info.pVendorInfos =
         calloc(counts.vendorInfoCount, sizeof(VkDeviceFaultVendorInfoEXT));
   /* The opaque vendor binary dump isn't consumed. */
   counts.vendorBinarySize = 0;

   if (get_fault_info(device->dispatch_handle, &counts, &info) == VK_SUCCESS) {
      fprintf(stderr, "[wrapper] VK_ERROR_DEVICE_LOST: %s\n", info.description);
      WRAPPER_LOG(error, "VK_ERROR_DEVICE_LOST: %s", info.description);

      for (uint32_t i = 0; info.pAddressInfos && i < counts.addressInfoCount; i++) {
         const VkDeviceFaultAddressInfoEXT *a = &info.pAddressInfos[i];
         fprintf(stderr, "[wrapper]   address fault[%u]: type=%s address=0x%llx precision=0x%llx\n",
                 i, wrapper_fault_addr_type_str(a->addressType),
                 (unsigned long long)a->reportedAddress,
                 (unsigned long long)a->addressPrecision);
      }
      for (uint32_t i = 0; info.pVendorInfos && i < counts.vendorInfoCount; i++) {
         const VkDeviceFaultVendorInfoEXT *v = &info.pVendorInfos[i];
         fprintf(stderr, "[wrapper]   vendor fault[%u]: %s code=0x%llx data=0x%llx\n",
                 i, v->description,
                 (unsigned long long)v->vendorFaultCode,
                 (unsigned long long)v->vendorFaultData);
      }
   }

   free(info.pAddressInfos);
   free(info.pVendorInfos);
}

static inline VkCommandBuffer
wrapper_unwrap_command_buffer(VkCommandBuffer command_buffer)
{
   if (command_buffer == VK_NULL_HANDLE)
      return VK_NULL_HANDLE;
   return wrapper_command_buffer_from_handle(command_buffer)->dispatch_handle;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueueSubmit(VkQueue _queue, uint32_t submitCount,
                    const VkSubmitInfo* pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   VkSubmitInfo stack_submits[WRAPPER_STACK_SUBMITS];
   VkCommandBuffer stack_command_buffers[WRAPPER_STACK_COMMAND_BUFFERS];
   VkSubmitInfo *wrapper_submits = stack_submits;
   VkCommandBuffer *command_buffers = stack_command_buffers;
   uint32_t total_command_buffers = 0;
   VkResult result;

   for (uint32_t i = 0; i < submitCount; i++)
      total_command_buffers += pSubmits[i].commandBufferCount;

   /* Submits happen every frame, only go to the heap for big batches. */
   if (submitCount > ARRAY_SIZE(stack_submits))
      wrapper_submits = malloc(sizeof(*wrapper_submits) * submitCount);
   if (total_command_buffers > ARRAY_SIZE(stack_command_buffers))
      command_buffers = malloc(sizeof(*command_buffers) * total_command_buffers);
   if (!wrapper_submits || !command_buffers) {
      if (wrapper_submits != stack_submits)
         free(wrapper_submits);
      if (command_buffers != stack_command_buffers)
         free(command_buffers);
      return vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   uint32_t cb = 0;
   for (uint32_t i = 0; i < submitCount; i++) {
      wrapper_submits[i] = pSubmits[i];
      wrapper_submits[i].pCommandBuffers = &command_buffers[cb];
      for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++)
         command_buffers[cb++] =
            wrapper_unwrap_command_buffer(pSubmits[i].pCommandBuffers[j]);
   }

   result = queue->device->dispatch_table.QueueSubmit(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);

   if (result == VK_ERROR_DEVICE_LOST)
      wrapper_log_device_fault(queue->device);

   if (wrapper_submits != stack_submits)
      free(wrapper_submits);
   if (command_buffers != stack_command_buffers)
      free(command_buffers);

   return result;
}

static VkPipelineStageFlags
wrapper_stage_flags2_to_1(VkPipelineStageFlags2 stages)
{
   if (stages == VK_PIPELINE_STAGE_2_NONE)
      return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
   /* Stages without a legacy equivalent */
   if (stages >> 32)
      return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   return (VkPipelineStageFlags)stages;
}

/* vkQueueSubmit2 emulated with vkQueueSubmit, for drivers (or devices) that
 * don't have synchronization2.  The common WSI code relies on it. */
static VkResult
wrapper_queue_submit2_emulated(struct wrapper_queue *queue,
                               uint32_t submitCount,
                               const VkSubmitInfo2 *pSubmits,
                               VkFence fence)
{
   uint32_t total_waits = 0, total_signals = 0, total_cmds = 0;
   for (uint32_t i = 0; i < submitCount; i++) {
      total_waits += pSubmits[i].waitSemaphoreInfoCount;
      total_signals += pSubmits[i].signalSemaphoreInfoCount;
      total_cmds += pSubmits[i].commandBufferInfoCount;
   }

   VkSubmitInfo *submits = calloc(MAX2(submitCount, 1), sizeof(*submits));
   VkTimelineSemaphoreSubmitInfo *timelines =
      calloc(MAX2(submitCount, 1), sizeof(*timelines));
   VkSemaphore *wait_sems = calloc(MAX2(total_waits, 1), sizeof(*wait_sems));
   VkPipelineStageFlags *wait_stages = calloc(MAX2(total_waits, 1), sizeof(*wait_stages));
   uint64_t *wait_values = calloc(MAX2(total_waits, 1), sizeof(*wait_values));
   VkSemaphore *signal_sems = calloc(MAX2(total_signals, 1), sizeof(*signal_sems));
   uint64_t *signal_values = calloc(MAX2(total_signals, 1), sizeof(*signal_values));
   VkCommandBuffer *cmds = calloc(MAX2(total_cmds, 1), sizeof(*cmds));

   VkResult result = VK_SUCCESS;
   if (!submits || !timelines || !wait_sems || !wait_stages || !wait_values ||
       !signal_sems || !signal_values || !cmds) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out;
   }

   uint32_t w = 0, s = 0, c = 0;
   for (uint32_t i = 0; i < submitCount; i++) {
      const VkSubmitInfo2 *in = &pSubmits[i];
      bool has_values = false;

      submits[i] = (VkSubmitInfo) {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = in->waitSemaphoreInfoCount,
         .pWaitSemaphores = &wait_sems[w],
         .pWaitDstStageMask = &wait_stages[w],
         .commandBufferCount = in->commandBufferInfoCount,
         .pCommandBuffers = &cmds[c],
         .signalSemaphoreCount = in->signalSemaphoreInfoCount,
         .pSignalSemaphores = &signal_sems[s],
      };
      timelines[i] = (VkTimelineSemaphoreSubmitInfo) {
         .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .waitSemaphoreValueCount = in->waitSemaphoreInfoCount,
         .pWaitSemaphoreValues = &wait_values[w],
         .signalSemaphoreValueCount = in->signalSemaphoreInfoCount,
         .pSignalSemaphoreValues = &signal_values[s],
      };

      for (uint32_t j = 0; j < in->waitSemaphoreInfoCount; j++, w++) {
         wait_sems[w] = in->pWaitSemaphoreInfos[j].semaphore;
         wait_stages[w] = wrapper_stage_flags2_to_1(in->pWaitSemaphoreInfos[j].stageMask);
         wait_values[w] = in->pWaitSemaphoreInfos[j].value;
         has_values |= wait_values[w] != 0;
      }
      for (uint32_t j = 0; j < in->commandBufferInfoCount; j++, c++)
         cmds[c] = wrapper_unwrap_command_buffer(in->pCommandBufferInfos[j].commandBuffer);
      for (uint32_t j = 0; j < in->signalSemaphoreInfoCount; j++, s++) {
         signal_sems[s] = in->pSignalSemaphoreInfos[j].semaphore;
         signal_values[s] = in->pSignalSemaphoreInfos[j].value;
         has_values |= signal_values[s] != 0;
      }

      /* Values are ignored for binary semaphores, only chain the struct
       * when timeline semaphores are involved. */
      if (has_values)
         submits[i].pNext = &timelines[i];
   }

   result = queue->device->dispatch_table.QueueSubmit(queue->dispatch_handle,
                                                      submitCount, submits,
                                                      fence);

out:
   free(submits);
   free(timelines);
   free(wait_sems);
   free(wait_stages);
   free(wait_values);
   free(signal_sems);
   free(signal_values);
   free(cmds);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueueSubmit2(VkQueue _queue, uint32_t submitCount,
                     const VkSubmitInfo2* pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   VkResult result;

   if (!queue->device->has_queue_submit2) {
      result = wrapper_queue_submit2_emulated(queue, submitCount, pSubmits, fence);
      if (result == VK_ERROR_DEVICE_LOST)
         wrapper_log_device_fault(queue->device);
      return result;
   }

   uint32_t total_command_buffers = 0;
   for (uint32_t i = 0; i < submitCount; i++)
      total_command_buffers += pSubmits[i].commandBufferInfoCount;

   VkSubmitInfo2 stack_submits[WRAPPER_STACK_SUBMITS];
   VkCommandBufferSubmitInfo stack_command_buffers[WRAPPER_STACK_COMMAND_BUFFERS];
   VkSubmitInfo2 *wrapper_submits = stack_submits;
   VkCommandBufferSubmitInfo *command_buffers = stack_command_buffers;

   if (submitCount > ARRAY_SIZE(stack_submits))
      wrapper_submits = malloc(sizeof(*wrapper_submits) * submitCount);
   if (total_command_buffers > ARRAY_SIZE(stack_command_buffers))
      command_buffers = malloc(sizeof(*command_buffers) * total_command_buffers);
   if (!wrapper_submits || !command_buffers) {
      if (wrapper_submits != stack_submits)
         free(wrapper_submits);
      if (command_buffers != stack_command_buffers)
         free(command_buffers);
      return vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   uint32_t cb = 0;
   for (uint32_t i = 0; i < submitCount; i++) {
      wrapper_submits[i] = pSubmits[i];
      wrapper_submits[i].pCommandBufferInfos = &command_buffers[cb];
      for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; j++) {
         command_buffers[cb] = pSubmits[i].pCommandBufferInfos[j];
         command_buffers[cb].commandBuffer =
            wrapper_unwrap_command_buffer(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
         cb++;
      }
   }

   result = queue->device->dispatch_table.QueueSubmit2(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);

   if (result == VK_ERROR_DEVICE_LOST)
      wrapper_log_device_fault(queue->device);

   if (wrapper_submits != stack_submits)
      free(wrapper_submits);
   if (command_buffers != stack_command_buffers)
      free(command_buffers);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                           uint32_t commandBufferCount,
                           const VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   VkCommandBuffer stack_buffers[32];
   VkCommandBuffer *command_buffers = stack_buffers;

   if (commandBufferCount > ARRAY_SIZE(stack_buffers)) {
      command_buffers = malloc(sizeof(*command_buffers) * commandBufferCount);
      if (!command_buffers)
         return;
   }

   for (uint32_t i = 0; i < commandBufferCount; i++)
      command_buffers[i] = wrapper_unwrap_command_buffer(pCommandBuffers[i]);

   wcb->device->dispatch_table.CmdExecuteCommands(
      wcb->dispatch_handle, commandBufferCount, command_buffers);

   if (command_buffers != stack_buffers)
      free(command_buffers);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateShaderModule(VkDevice _device,
                           const VkShaderModuleCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkShaderModule *pShaderModule)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (WRAPPER_LOG_LEVEL(shader))
      dump_shader_code(pCreateInfo->pCode, pCreateInfo->codeSize);

   return device->dispatch_table.CreateShaderModule(
      device->dispatch_handle, pCreateInfo, pAllocator, pShaderModule);
}

static VkResult
wrapper_command_buffer_create(struct wrapper_device *device,
                              VkCommandPool pool,
                              VkCommandBuffer dispatch_handle,
                              VkCommandBuffer *pCommandBuffer)
{
   struct wrapper_command_buffer *wcb =
      vk_object_zalloc(&device->vk, &device->vk.alloc,
                       sizeof(struct wrapper_command_buffer),
                       VK_OBJECT_TYPE_COMMAND_BUFFER);
   if (!wcb)
      return vk_error(&device->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   wcb->device = device;
   wcb->pool = pool;
   wcb->dispatch_handle = dispatch_handle;
   list_add(&wcb->link, &device->command_buffer_list);

   *pCommandBuffer = wrapper_command_buffer_to_handle(wcb);

   return VK_SUCCESS;
}

/* Called with resource_mutex held */
static void
wrapper_command_buffer_destroy(struct wrapper_device *device,
                               struct wrapper_command_buffer *wcb,
                               bool free_dispatch)
{
   if (wcb == NULL)
      return;

   if (free_dispatch) {
      device->dispatch_table.FreeCommandBuffers(
         device->dispatch_handle, wcb->pool, 1, &wcb->dispatch_handle);
   }

   list_del(&wcb->link);
   vk_object_free(&device->vk, &device->vk.alloc, wcb);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AllocateCommandBuffers(VkDevice _device,
                               const VkCommandBufferAllocateInfo* pAllocateInfo,
                               VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult result;
   uint32_t i;

   result = device->dispatch_table.AllocateCommandBuffers(
      device->dispatch_handle, pAllocateInfo, pCommandBuffers);
   if (result != VK_SUCCESS)
      return result;

   simple_mtx_lock(&device->resource_mutex);

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = wrapper_command_buffer_create(
         device, pAllocateInfo->commandPool, pCommandBuffers[i],
         pCommandBuffers + i);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      /* pCommandBuffers[0..i-1] are wrapped, the rest are driver handles */
      for (uint32_t q = 0; q < i; q++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb, pCommandBuffers[q]);
         wrapper_command_buffer_destroy(device, wcb, true);
      }

      device->dispatch_table.FreeCommandBuffers(
         device->dispatch_handle, pAllocateInfo->commandPool,
         pAllocateInfo->commandBufferCount - i, pCommandBuffers + i);

      for (i = 0; i < pAllocateInfo->commandBufferCount; i++)
         pCommandBuffers[i] = VK_NULL_HANDLE;
   }

   simple_mtx_unlock(&device->resource_mutex);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_FreeCommandBuffers(VkDevice _device,
                           VkCommandPool commandPool,
                           uint32_t commandBufferCount,
                           const VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   simple_mtx_lock(&device->resource_mutex);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(wrapper_command_buffer, wcb, pCommandBuffers[i]);
      wrapper_command_buffer_destroy(device, wcb, true);
   }

   simple_mtx_unlock(&device->resource_mutex);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyCommandPool(VkDevice _device, VkCommandPool commandPool,
                           const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   simple_mtx_lock(&device->resource_mutex);

   /* Destroying the pool frees its command buffers in the driver */
   list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                            &device->command_buffer_list, link) {
      if (wcb->pool == commandPool)
         wrapper_command_buffer_destroy(device, wcb, false);
   }

   simple_mtx_unlock(&device->resource_mutex);

   device->dispatch_table.DestroyCommandPool(device->dispatch_handle,
                                             commandPool, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyDevice(VkDevice _device, const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (!device)
      return;

   simple_mtx_lock(&device->resource_mutex);

   /* The driver frees the command buffers with the device */
   list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                            &device->command_buffer_list, link) {
      wrapper_command_buffer_destroy(device, wcb, false);
   }
   list_for_each_entry_safe(struct wrapper_device_memory, mem,
                            &device->device_memory_list, link) {
      wrapper_device_memory_destroy(mem);
   }

   simple_mtx_unlock(&device->resource_mutex);

   list_for_each_entry_safe(struct vk_queue, queue, &device->vk.queues, link) {
      vk_queue_finish(queue);
      vk_free(&device->vk.alloc, queue);
   }

   if (device->dispatch_handle != VK_NULL_HANDLE) {
      device->dispatch_table.DestroyDevice(device->dispatch_handle,
                                           pAllocator);
   }

   /* Allocations the application leaked go away with the device */
   if (device->budget_table) {
      hash_table_u64_foreach(device->budget_table, entry) {
         const uintptr_t packed = (uintptr_t)entry.data;
         p_atomic_add(&device->physical->heap_usage[packed & 0xf],
                      -(int64_t)(packed >> 4));
      }
      _mesa_hash_table_u64_destroy(device->budget_table);
   }

   _mesa_hash_table_u64_destroy(device->memory_table);
   simple_mtx_destroy(&device->resource_mutex);
   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
}

static uint64_t
unwrap_device_object(VkObjectType objectType,
                     uint64_t objectHandle)
{
   if (objectHandle == 0)
      return 0;

   switch (objectType) {
   case VK_OBJECT_TYPE_DEVICE:
      return (uint64_t)(uintptr_t)wrapper_device_from_handle((VkDevice)(uintptr_t)objectHandle)->dispatch_handle;
   case VK_OBJECT_TYPE_QUEUE:
      return (uint64_t)(uintptr_t)wrapper_queue_from_handle((VkQueue)(uintptr_t)objectHandle)->dispatch_handle;
   case VK_OBJECT_TYPE_COMMAND_BUFFER:
      return (uint64_t)(uintptr_t)wrapper_command_buffer_from_handle((VkCommandBuffer)(uintptr_t)objectHandle)->dispatch_handle;
   default:
      return objectHandle;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_SetPrivateData(VkDevice _device, VkObjectType objectType,
                       uint64_t objectHandle,
                       VkPrivateDataSlot privateDataSlot,
                       uint64_t data)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   uint64_t object_handle = unwrap_device_object(objectType, objectHandle);
   return device->dispatch_table.SetPrivateData(device->dispatch_handle,
      objectType, object_handle, privateDataSlot, data);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPrivateData(VkDevice _device, VkObjectType objectType,
                       uint64_t objectHandle,
                       VkPrivateDataSlot privateDataSlot,
                       uint64_t* pData)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   uint64_t object_handle = unwrap_device_object(objectType, objectHandle);
   device->dispatch_table.GetPrivateData(device->dispatch_handle,
      objectType, object_handle, privateDataSlot, pData);
}

/* VK_EXT_debug_utils is hidden from the application, but the common WSI code
 * labels its own command buffers.  Forward when the driver has it. */
VKAPI_ATTR VkResult VKAPI_CALL
wrapper_SetDebugUtilsObjectNameEXT(VkDevice _device,
                                   const VkDebugUtilsObjectNameInfoEXT *pNameInfo)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (device->dispatch_table.SetDebugUtilsObjectNameEXT == NULL)
      return VK_SUCCESS;

   VkDebugUtilsObjectNameInfoEXT name_info = *pNameInfo;
   name_info.objectHandle = unwrap_device_object(pNameInfo->objectType,
                                                 pNameInfo->objectHandle);
   return device->dispatch_table.SetDebugUtilsObjectNameEXT(device->dispatch_handle,
                                                            &name_info);
}
