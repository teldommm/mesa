#ifndef __WRAPPER_PRIVATE_H
#define __WRAPPER_PRIVATE_H

#include "vulkan/runtime/vk_instance.h"
#include "vulkan/runtime/vk_physical_device.h"
#include "vulkan/runtime/vk_device.h"
#include "vulkan/runtime/vk_queue.h"
#include "vulkan/runtime/vk_command_buffer.h"
#include "vulkan/runtime/vk_log.h"
#include "vulkan/util/vk_dispatch_table.h"
#include "vulkan/wsi/wsi_common.h"
#include "util/simple_mtx.h"
#include "util/hash_table.h"
#include "util/list.h"

extern const struct vk_instance_extension_table wrapper_instance_extensions;
/* Device extensions implemented by the wrapper itself (on top of the common
 * WSI code); never forwarded to the underlying driver. */
extern const struct vk_device_extension_table wrapper_device_extensions;
/* Device extensions of the underlying driver that are hidden from the
 * application. */
extern const struct vk_device_extension_table wrapper_filter_extensions;

struct wrapper_instance {
   struct vk_instance vk;

   VkInstance dispatch_handle;
   struct vk_instance_dispatch_table dispatch_table;
};

VK_DEFINE_HANDLE_CASTS(wrapper_instance, vk.base, VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)

struct wrapper_physical_device {
   struct vk_physical_device vk;

   int dma_heap_fd;
   const char *resource_type;
   VkPhysicalDevice dispatch_handle;
   VkPhysicalDeviceProperties2 properties2;
   VkPhysicalDeviceDriverProperties driver_properties;
   VkPhysicalDeviceMemoryProperties memory_properties;
   struct wsi_device wsi_device;
   struct wrapper_instance *instance;
   /* What the underlying driver really supports */
   struct vk_features base_supported_features;
   struct vk_device_extension_table base_supported_extensions;
   struct vk_physical_device_dispatch_table dispatch_table;

   /* Report the driver's subgroup operations (WRAPPER_SUBGROUP_OPS) */
   bool expose_subgroup_ops;

   /* VK_EXT_memory_budget emulated for drivers without it (Adreno): usage is
    * tracked per heap at vkAllocateMemory/vkFreeMemory time, the budget is
    * derived from MemAvailable minus a reserve (WRAPPER_MEMORY_BUDGET*). */
   bool emulate_memory_budget;
   uint64_t memory_budget_reserve;
   uint64_t heap_usage[VK_MAX_MEMORY_HEAPS];
};

VK_DEFINE_HANDLE_CASTS(wrapper_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

struct wrapper_queue {
   struct vk_queue vk;

   struct wrapper_device *device;
   VkQueue dispatch_handle;
};

VK_DEFINE_HANDLE_CASTS(wrapper_queue, vk.base, VkQueue,
                       VK_OBJECT_TYPE_QUEUE)

struct wrapper_device {
   struct vk_device vk;

   VkDevice dispatch_handle;
   simple_mtx_t resource_mutex;
   struct list_head command_buffer_list;
   struct list_head device_memory_list;
   /* VkDeviceMemory -> struct wrapper_device_memory (emulated placed maps) */
   struct hash_table_u64 *memory_table;
   /* VkDeviceMemory -> size << 4 | heap index, for the emulated
    * VK_EXT_memory_budget (NULL when not emulated) */
   struct hash_table_u64 *budget_table;
   struct wrapper_physical_device *physical;
   struct vk_device_dispatch_table dispatch_table;

   /* vkQueueSubmit2 can be forwarded to the driver (otherwise it's
    * translated to vkQueueSubmit, the common WSI code relies on it). */
   bool has_queue_submit2;
   /* VK_EXT_device_fault was enabled on the driver's device */
   bool device_fault_enabled;
};

VK_DEFINE_HANDLE_CASTS(wrapper_device, vk.base, VkDevice,
                       VK_OBJECT_TYPE_DEVICE)

struct wrapper_command_buffer {
   struct vk_command_buffer vk;

   struct wrapper_device *device;
   struct list_head link;
   VkCommandPool pool;
   VkCommandBuffer dispatch_handle;
};

VK_DEFINE_HANDLE_CASTS(wrapper_command_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

/* Host visible allocation backed by a dma-buf / AHardwareBuffer / opaque fd,
 * used to emulate VK_EXT_map_memory_placed. */
struct wrapper_device_memory {
   struct AHardwareBuffer *ahardware_buffer;
   struct wrapper_device *device;
   struct list_head link;
   int fd;
   void *map_address;
   size_t map_size;
   size_t alloc_size;
   VkDeviceMemory dispatch_handle;
   const VkAllocationCallbacks *alloc;
};

VkResult enumerate_physical_device(struct vk_instance *_instance);
void destroy_physical_device(struct vk_physical_device *pdevice);

void
wrapper_setup_device_features(struct wrapper_physical_device *physical_device);

void
wrapper_device_memory_destroy(struct wrapper_device_memory *mem);

#endif
