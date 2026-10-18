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

/* BCn texture support for drivers that lack it (WRAPPER_EMULATE_BCN):
 *
 *  0  off
 *  1  report the BCn formats as supported, the driver decodes them itself
 *     (hardware that can, behind a driver that doesn't say so)
 *  2  decompress every BCn format on the CPU
 *  3  auto (default): per format, native when the driver samples it,
 *     decompressed otherwise; off on Turnip; Qualcomm >= 512.530 reports
 *     the formats when the driver hides textureCompressionBC.
 */
enum wrapper_bcn_mode {
   WRAPPER_BCN_OFF = 0,
   WRAPPER_BCN_FAKE = 1,
   WRAPPER_BCN_FULL = 2,
   WRAPPER_BCN_AUTO = 3,
};

/* VK_FORMAT_BC1_RGB_UNORM_BLOCK .. VK_FORMAT_BC7_SRGB_BLOCK are contiguous */
#define WRAPPER_BCN_FIRST_FORMAT VK_FORMAT_BC1_RGB_UNORM_BLOCK
#define WRAPPER_BCN_LAST_FORMAT  VK_FORMAT_BC7_SRGB_BLOCK

static inline bool
wrapper_is_bcn_format(VkFormat format)
{
   return format >= WRAPPER_BCN_FIRST_FORMAT &&
          format <= WRAPPER_BCN_LAST_FORMAT;
}

static inline uint32_t
wrapper_bcn_format_bit(VkFormat format)
{
   return wrapper_is_bcn_format(format) ?
      1u << (format - WRAPPER_BCN_FIRST_FORMAT) : 0;
}

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

   /* Resolved WRAPPER_EMULATE_BCN mode and, per BCn format
    * (wrapper_bcn_format_bit()), the formats whose properties the wrapper
    * reports and the ones it decompresses on the CPU. */
   enum wrapper_bcn_mode bcn_mode;
   uint32_t bcn_fake_mask;
   uint32_t bcn_emulate_mask;
};

static inline bool
wrapper_bcn_is_emulated(const struct wrapper_physical_device *pdevice,
                        VkFormat format)
{
   return (pdevice->bcn_emulate_mask & wrapper_bcn_format_bit(format)) != 0;
}

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

   /* CPU BCn decompression (physical->bcn_emulate_mask != 0), see
    * wrapper_bcn.c.  All tables are protected by resource_mutex. */
   bool bcn_active;
   /* VkImage -> struct wrapper_bcn_image (emulated BCn images only) */
   struct hash_table_u64 *bcn_image_table;
   /* VkBuffer -> struct wrapper_bcn_buffer (memory binding) */
   struct hash_table_u64 *bcn_buffer_table;
   /* VkDeviceMemory -> host address of offset 0, for memory the
    * application currently has mapped */
   struct hash_table_u64 *map_table;
   /* Command buffers holding staging buffers (wrapper_command_buffer::
    * staging_link) */
   struct list_head bcn_staging_cmd_list;
};

VK_DEFINE_HANDLE_CASTS(wrapper_device, vk.base, VkDevice,
                       VK_OBJECT_TYPE_DEVICE)

struct wrapper_command_buffer {
   struct vk_command_buffer vk;

   struct wrapper_device *device;
   struct list_head link;
   VkCommandPool pool;
   VkCommandBuffer dispatch_handle;

   /* Staging buffers with decompressed BCn data recorded into this command
    * buffer (struct wrapper_bcn_staging); released when the command buffer
    * is reset, begun again or freed. */
   struct list_head staging_list;
   struct list_head staging_link;
   bool on_staging_list;
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

/* wrapper_bcn.c */
void
wrapper_bcn_setup_physical_device(struct wrapper_physical_device *pdevice);

VkResult
wrapper_bcn_device_init(struct wrapper_device *device);

void
wrapper_bcn_device_finish(struct wrapper_device *device);

/* Called with resource_mutex held */
void
wrapper_bcn_release_staging(struct wrapper_command_buffer *wcb);

void
wrapper_bcn_track_map(struct wrapper_device *device, VkDeviceMemory memory,
                      VkDeviceSize offset, void *ptr);

void
wrapper_bcn_untrack_map(struct wrapper_device *device, VkDeviceMemory memory);

#endif
