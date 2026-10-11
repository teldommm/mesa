/*
 * VK_EXT_map_memory_placed emulation.
 *
 * Host visible allocations are backed by a dma-buf (dma-heap or ION), an
 * AHardwareBuffer or an opaque fd exported by the driver, so that they can
 * be mmap()ed at the address the application asks for (Wine needs this for
 * 32-bit processes).
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>

#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "vk_common_entrypoints.h"
#include "util/os_file.h"
#include "vk_util.h"

static int
safe_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

static int
dma_heap_alloc(int heap_fd, size_t size)
{
   struct dma_heap_allocation_data alloc_data = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   if (safe_ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0)
      return -1;

   return alloc_data.fd;
}

/* Legacy ION (kernels < 4.12), see bo_init_new_ion_legacy() in turnip's
 * tu_knl_kgsl.cc */
struct ion_allocation_data_legacy {
   size_t len;
   size_t align;
   unsigned int heap_id_mask;
   unsigned int flags;
   __u32 handle;
};

struct ion_fd_data_legacy {
   __u32 handle;
   int fd;
};

struct ion_handle_data_legacy {
   __u32 handle;
};

/* ION from kernel 4.12 on */
struct ion_allocation_data_modern {
   __u64 len;
   __u32 heap_id_mask;
   __u32 flags;
   __u32 fd;
   __u32 unused;
};

#define ION_IOC_MAGIC            'I'
#define ION_IOC_ALLOC_LEGACY     _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_legacy)
#define ION_IOC_FREE_LEGACY      _IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data_legacy)
#define ION_IOC_SHARE_LEGACY     _IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data_legacy)
#define ION_IOC_ALLOC_MODERN     _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_modern)

/* ION_HEAP_SYSTEM | ION_SYSTEM_HEAP_ID (Qualcomm) */
#define ION_HEAP_MASK_QCOM ((1U << 0) | (1U << 25))

static int
ion_heap_alloc_modern(int heap_fd, size_t size)
{
   struct ion_allocation_data_modern alloc_data = {
      .len = size,
      .heap_id_mask = ION_HEAP_MASK_QCOM,
      .flags = 0, /* uncached */
   };

   if (safe_ioctl(heap_fd, ION_IOC_ALLOC_MODERN, &alloc_data) < 0) {
      alloc_data.heap_id_mask = 1U;
      if (safe_ioctl(heap_fd, ION_IOC_ALLOC_MODERN, &alloc_data) < 0)
         return -1;
   }

   return alloc_data.fd;
}

static int
ion_heap_alloc_legacy(int heap_fd, size_t size)
{
   struct ion_allocation_data_legacy alloc_data = {
      .len = size,
      .align = 0,
      .heap_id_mask = ION_HEAP_MASK_QCOM,
      .flags = 0,
   };

   if (safe_ioctl(heap_fd, ION_IOC_ALLOC_LEGACY, &alloc_data) < 0) {
      alloc_data.align = 4096;
      alloc_data.heap_id_mask = 1U;
      if (safe_ioctl(heap_fd, ION_IOC_ALLOC_LEGACY, &alloc_data) < 0)
         return -1;
   }

   struct ion_fd_data_legacy fd_data = {
      .handle = alloc_data.handle,
      .fd = -1,
   };
   int ret = safe_ioctl(heap_fd, ION_IOC_SHARE_LEGACY, &fd_data);

   /* The shared fd keeps the buffer alive, drop the handle either way. */
   struct ion_handle_data_legacy free_data = { .handle = alloc_data.handle };
   safe_ioctl(heap_fd, ION_IOC_FREE_LEGACY, &free_data);

   if (ret < 0) {
      WRAPPER_LOG(error, "Failed to share the ION handle, errno %d", errno);
      return -1;
   }

   return fd_data.fd;
}

static int
ion_heap_alloc(int heap_fd, size_t size)
{
   static int ion_iface = 0;

   if (!ion_iface) {
      /* The legacy interface has ION_IOC_FREE, the modern one returns
       * ENOTTY for it (same probe as turnip). */
      struct ion_handle_data_legacy probe = { .handle = 0 };
      if (safe_ioctl(heap_fd, ION_IOC_FREE_LEGACY, &probe) >= 0 || errno != ENOTTY)
         ion_iface = 1;
      else
         ion_iface = 2;
      WRAPPER_LOG(info, "Using the %s ION interface",
                  ion_iface == 1 ? "legacy" : "modern");
   }

   return ion_iface == 2 ? ion_heap_alloc_modern(heap_fd, size)
                         : ion_heap_alloc_legacy(heap_fd, size);
}

static int
wrapper_dmabuf_alloc(struct wrapper_device *device, size_t size)
{
   int heap_fd = device->physical->dma_heap_fd;
   if (heap_fd < 0)
      return -1;

   int fd = dma_heap_alloc(heap_fd, size);
   if (fd < 0)
      fd = ion_heap_alloc(heap_fd, size);

   return fd;
}

/* First memory type allowed by type_bits that has any of the given flags,
 * falling back to any allowed type. */
static uint32_t
wrapper_select_allowed_memory_type(struct wrapper_device *device,
                                   uint32_t type_bits,
                                   VkMemoryPropertyFlags flags)
{
   const VkPhysicalDeviceMemoryProperties *props =
      &device->physical->memory_properties;

   for (uint32_t idx = 0; idx < props->memoryTypeCount; idx++) {
      if ((type_bits & (1u << idx)) &&
          (props->memoryTypes[idx].propertyFlags & flags) == flags)
         return idx;
   }

   for (uint32_t idx = 0; idx < props->memoryTypeCount; idx++) {
      if (type_bits & (1u << idx))
         return idx;
   }

   return UINT32_MAX;
}

static VkResult
wrapper_allocate_memory_dmaheap(struct wrapper_device *device,
                                const VkMemoryAllocateInfo* pAllocateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                VkDeviceMemory* pMemory,
                                int *out_fd)
{
   VkResult result;

   *out_fd = wrapper_dmabuf_alloc(device, pAllocateInfo->allocationSize);
   if (*out_fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkMemoryFdPropertiesKHR memory_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   result = device->dispatch_table.GetMemoryFdPropertiesKHR(
      device->dispatch_handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      *out_fd, &memory_fd_props);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to get memory fd properties, res %d", result);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   /* Keep the memory type the application asked for when the dma-buf can
    * be imported into it. */
   uint32_t memory_type_index = pAllocateInfo->memoryTypeIndex;
   if (!(memory_fd_props.memoryTypeBits & (1u << memory_type_index))) {
      memory_type_index = wrapper_select_allowed_memory_type(device,
         memory_fd_props.memoryTypeBits,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   }
   if (memory_type_index == UINT32_MAX) {
      WRAPPER_LOG(error, "No compatible memory type for dma-buf %d", *out_fd);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   VkImportMemoryFdInfoKHR import_fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = pAllocateInfo->pNext,
      .fd = os_dupfd_cloexec(*out_fd),
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   if (import_fd_info.fd < 0)
      return VK_ERROR_TOO_MANY_OBJECTS;

   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   allocate_info.pNext = &import_fd_info;
   allocate_info.memoryTypeIndex = memory_type_index;

   result = device->dispatch_table.AllocateMemory(
      device->dispatch_handle, &allocate_info, pAllocator, pMemory);

   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to import dma-heap memory, res %d", result);
      /* The fd is only consumed on success */
      close(import_fd_info.fd);
   }

   return result;
}

static VkResult
wrapper_allocate_memory_opaque_fd(struct wrapper_device *device,
                                  const VkMemoryAllocateInfo *pAllocateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkDeviceMemory *pMemory,
                                  int *out_fd)
{
   VkResult result;

   VkExportMemoryAllocateInfo export_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = pAllocateInfo->pNext,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };

   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   allocate_info.pNext = &export_memory_info;

   result = device->dispatch_table.AllocateMemory(device->dispatch_handle,
                                                  &allocate_info,
                                                  pAllocator,
                                                  pMemory);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to allocate opaque fd memory, res %d", result);
      return result;
   }

   const VkMemoryGetFdInfoKHR get_memory_fd = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = *pMemory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };

   result = device->dispatch_table.GetMemoryFdKHR(device->dispatch_handle,
                                                  &get_memory_fd, out_fd);
   if (result != VK_SUCCESS || *out_fd < 0) {
      WRAPPER_LOG(error, "Failed to get opaque fd");
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   return VK_SUCCESS;
}

static VkResult
wrapper_allocate_memory_ahardware_buffer(struct wrapper_device *device,
                                         const VkMemoryAllocateInfo* pAllocateInfo,
                                         const VkAllocationCallbacks* pAllocator,
                                         VkDeviceMemory* pMemory,
                                         AHardwareBuffer **pAHardwareBuffer)
{
   VkResult result;

   if (device->dispatch_table.GetMemoryAndroidHardwareBufferANDROID == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   VkExportMemoryAllocateInfo export_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = pAllocateInfo->pNext,
      .handleTypes =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   allocate_info.pNext = &export_memory_info;

   result = device->dispatch_table.AllocateMemory(device->dispatch_handle,
                                                  &allocate_info,
                                                  pAllocator,
                                                  pMemory);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to allocate AHardwareBuffer memory, res %d", result);
      return result;
   }

   result = device->dispatch_table.GetMemoryAndroidHardwareBufferANDROID(
      device->dispatch_handle,
      &(VkMemoryGetAndroidHardwareBufferInfoANDROID) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
         .memory = *pMemory,
      },
      pAHardwareBuffer);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to export the AHardwareBuffer, res %d", result);
      return result;
   }

   if (AHardwareBuffer_getNativeHandle(*pAHardwareBuffer) == NULL) {
      WRAPPER_LOG(error, "Invalid AHardwareBuffer native handle");
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   return VK_SUCCESS;
}

static void
wrapper_device_memory_reset(struct wrapper_device_memory *mem)
{
   struct wrapper_device *device = mem->device;

   if (mem->map_address && mem->map_size) {
      munmap(mem->map_address, mem->map_size);
      mem->map_address = NULL;
      mem->map_size = 0;
   }
   if (mem->dispatch_handle != VK_NULL_HANDLE) {
      device->dispatch_table.FreeMemory(device->dispatch_handle,
                                        mem->dispatch_handle, mem->alloc);
      mem->dispatch_handle = VK_NULL_HANDLE;
   }
   if (mem->ahardware_buffer) {
      AHardwareBuffer_release(mem->ahardware_buffer);
      mem->ahardware_buffer = NULL;
   }
   if (mem->fd != -1) {
      close(mem->fd);
      mem->fd = -1;
   }
}

/* Called with resource_mutex held */
static VkResult
wrapper_device_memory_create(struct wrapper_device *device,
                             const VkAllocationCallbacks *alloc,
                             struct wrapper_device_memory **out_mem)
{
   struct wrapper_device_memory *mem =
      vk_zalloc2(&device->vk.alloc, alloc, sizeof(*mem), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   mem->fd = -1;
   mem->device = device;
   mem->alloc = alloc;
   list_add(&mem->link, &device->device_memory_list);

   *out_mem = mem;
   return VK_SUCCESS;
}

/* Called with resource_mutex held */
void
wrapper_device_memory_destroy(struct wrapper_device_memory *mem)
{
   struct wrapper_device *device = mem->device;

   if (mem->dispatch_handle != VK_NULL_HANDLE)
      _mesa_hash_table_u64_remove(device->memory_table,
                                  (uint64_t)(uintptr_t)mem->dispatch_handle);

   wrapper_device_memory_reset(mem);
   list_del(&mem->link);
   vk_free2(&device->vk.alloc, mem->alloc, mem);
}

static struct wrapper_device_memory *
wrapper_device_memory_lookup_locked(struct wrapper_device *device,
                                    VkDeviceMemory handle)
{
   if (handle == VK_NULL_HANDLE)
      return NULL;

   return _mesa_hash_table_u64_search(device->memory_table,
                                      (uint64_t)(uintptr_t)handle);
}

static struct wrapper_device_memory *
wrapper_device_memory_lookup(struct wrapper_device *device,
                             VkDeviceMemory handle)
{
   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_device_memory *mem =
      wrapper_device_memory_lookup_locked(device, handle);
   simple_mtx_unlock(&device->resource_mutex);
   return mem;
}

static void
unlink_struct(VkMemoryAllocateInfo *alloc_info, VkStructureType sType)
{
   VkBaseOutStructure *prev = (VkBaseOutStructure *)alloc_info;
   vk_foreach_struct(s, (void *)alloc_info->pNext) {
      if (s->sType == sType) {
         /* Note: may modify a struct owned by the application */
         prev->pNext = s->pNext;
         return;
      }
      prev = s;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AllocateMemory(VkDevice _device,
                       const VkMemoryAllocateInfo* pAllocateInfo,
                       const VkAllocationCallbacks* pAllocator,
                       VkDeviceMemory* pMemory)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_device_memory *mem = NULL;
   VkResult result;

   if (pAllocateInfo->memoryTypeIndex >= device->physical->memory_properties.memoryTypeCount)
      goto fallback;

   VkMemoryPropertyFlags property_flags =
      device->physical->memory_properties.memoryTypes[
         pAllocateInfo->memoryTypeIndex].propertyFlags;

   if (!(property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      goto fallback;

   if (!device->vk.enabled_features.memoryMapPlaced ||
       !device->vk.enabled_extensions.EXT_map_memory_placed)
      goto fallback;

   if (vk_find_struct_const(pAllocateInfo, IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID) ||
       vk_find_struct_const(pAllocateInfo, IMPORT_MEMORY_FD_INFO_KHR) ||
       vk_find_struct_const(pAllocateInfo, IMPORT_MEMORY_HOST_POINTER_INFO_EXT) ||
       vk_find_struct_const(pAllocateInfo, EXPORT_MEMORY_ALLOCATE_INFO))
      goto fallback;

   WRAPPER_LOG(info, "Emulating vkAllocateMemory");

   /* The dedicated resource was not created with external memory: some
    * mobile drivers reject (or crash on) a dedicated allocation combined with
    * an import/export, and it's only a hint for host visible memory. */
   VkMemoryAllocateInfo memory_allocate_info = *pAllocateInfo;
   unlink_struct(&memory_allocate_info, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);

   simple_mtx_lock(&device->resource_mutex);

   result = wrapper_device_memory_create(device, pAllocator, &mem);
   if (result != VK_SUCCESS) {
      simple_mtx_unlock(&device->resource_mutex);
      return vk_error(device, result);
   }

   mem->alloc_size = pAllocateInfo->allocationSize;

   const char *resource_type = device->physical->resource_type;
   if (strstr(resource_type, "ahb")) {
      WRAPPER_LOG(info, "Using AHardwareBuffer memory backend");
      result = wrapper_allocate_memory_ahardware_buffer(device,
         &memory_allocate_info, pAllocator, &mem->dispatch_handle, &mem->ahardware_buffer);
   } else if (strstr(resource_type, "dmabuf")) {
      WRAPPER_LOG(info, "Using DMABUF memory backend");
      result = wrapper_allocate_memory_dmaheap(device,
         &memory_allocate_info, pAllocator, &mem->dispatch_handle, &mem->fd);
   } else if (strstr(resource_type, "opaque")) {
      WRAPPER_LOG(info, "Using opaque fd memory backend");
      result = wrapper_allocate_memory_opaque_fd(device,
         &memory_allocate_info, pAllocator, &mem->dispatch_handle, &mem->fd);
   } else {
      WRAPPER_LOG(info, "Using auto memory backend");
      result = wrapper_allocate_memory_dmaheap(device,
         &memory_allocate_info, pAllocator, &mem->dispatch_handle, &mem->fd);

      if (result != VK_SUCCESS) {
         wrapper_device_memory_reset(mem);
         result = wrapper_allocate_memory_ahardware_buffer(device,
            &memory_allocate_info, pAllocator, &mem->dispatch_handle, &mem->ahardware_buffer);
      }

      if (result != VK_SUCCESS) {
         wrapper_device_memory_reset(mem);
         result = wrapper_allocate_memory_opaque_fd(device,
            &memory_allocate_info, pAllocator, &mem->dispatch_handle, &mem->fd);
      }
   }

   if (result != VK_SUCCESS) {
      /* E.g. an image that requires a dedicated allocation: hand out a regular
       * allocation, it just can't be placed-mapped. */
      WRAPPER_LOG(error, "Placed-map emulation failed (res %d), using a regular allocation",
                  result);
      wrapper_device_memory_destroy(mem);
      simple_mtx_unlock(&device->resource_mutex);
      goto fallback;
   }

   _mesa_hash_table_u64_insert(device->memory_table,
                               (uint64_t)(uintptr_t)mem->dispatch_handle, mem);
   *pMemory = mem->dispatch_handle;

   simple_mtx_unlock(&device->resource_mutex);
   return VK_SUCCESS;

fallback:
   return device->dispatch_table.AllocateMemory(device->dispatch_handle,
      pAllocateInfo, pAllocator, pMemory);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_FreeMemory(VkDevice _device, VkDeviceMemory _memory,
                   const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (_memory == VK_NULL_HANDLE)
      return;

   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_device_memory *mem =
      wrapper_device_memory_lookup_locked(device, _memory);
   if (mem) {
      mem->alloc = pAllocator;
      wrapper_device_memory_destroy(mem);
      simple_mtx_unlock(&device->resource_mutex);
      return;
   }
   simple_mtx_unlock(&device->resource_mutex);

   device->dispatch_table.FreeMemory(device->dispatch_handle, _memory, pAllocator);
}

static int
wrapper_device_memory_get_fd(struct wrapper_device_memory *mem)
{
   if (!mem->ahardware_buffer)
      return mem->fd;

   const native_handle_t *handle =
      AHardwareBuffer_getNativeHandle(mem->ahardware_buffer);
   if (!handle)
      return -1;

   /* The native handle may carry several fds (e.g. metadata alongside the
    * memory); use the first one that is large enough to back the
    * allocation. */
   for (int idx = 0; idx < handle->numFds; idx++) {
      off_t size = lseek(handle->data[idx], 0, SEEK_END);
      if (size >= 0 && (size_t)size >= mem->alloc_size)
         return handle->data[idx];
   }

   return -1;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_MapMemory2KHR(VkDevice _device,
                      const VkMemoryMapInfoKHR* pMemoryMapInfo,
                      void** ppData)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   const VkMemoryMapPlacedInfoEXT *placed_info = NULL;
   VkResult result;

   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT)
      placed_info = vk_find_struct_const(pMemoryMapInfo->pNext,
                                         MEMORY_MAP_PLACED_INFO_EXT);

   struct wrapper_device_memory *mem =
      wrapper_device_memory_lookup(device, pMemoryMapInfo->memory);
   if (!placed_info || !mem) {
      return device->dispatch_table.MapMemory(device->dispatch_handle,
         pMemoryMapInfo->memory, pMemoryMapInfo->offset, pMemoryMapInfo->size,
         0, ppData);
   }

   WRAPPER_LOG(info, "Emulating vkMapMemory2KHR");

   simple_mtx_lock(&device->resource_mutex);

   if (mem->map_address) {
      if (placed_info->pPlacedAddress != mem->map_address) {
         WRAPPER_LOG(error, "Placed address/mapped address mismatch");
         result = VK_ERROR_MEMORY_MAP_FAILED;
         goto fail;
      }
      goto out;
   }

   int fd = wrapper_device_memory_get_fd(mem);
   if (fd < 0) {
      WRAPPER_LOG(error, "No mappable fd for memory %p", (void *)pMemoryMapInfo->memory);
      result = VK_ERROR_MEMORY_MAP_FAILED;
      goto fail;
   }

   /* A placed map puts [offset, offset + size) at pPlacedAddress. */
   if (pMemoryMapInfo->offset >= mem->alloc_size) {
      result = VK_ERROR_MEMORY_MAP_FAILED;
      goto fail;
   }
   mem->map_size = pMemoryMapInfo->size == VK_WHOLE_SIZE ?
      mem->alloc_size - pMemoryMapInfo->offset : pMemoryMapInfo->size;

   WRAPPER_LOG(info, "Mapping memory %p (offset %llu) at %p, size %zu",
               (void *)pMemoryMapInfo->memory,
               (unsigned long long)pMemoryMapInfo->offset,
               placed_info->pPlacedAddress, mem->map_size);

   mem->map_address = mmap(placed_info->pPlacedAddress, mem->map_size,
                           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                           fd, pMemoryMapInfo->offset);
   if (mem->map_address == MAP_FAILED) {
      WRAPPER_LOG(error, "mmap failed: errno %d", errno);
      mem->map_address = NULL;
      mem->map_size = 0;
      result = VK_ERROR_MEMORY_MAP_FAILED;
      goto fail;
   }

out:
   *ppData = mem->map_address;
   simple_mtx_unlock(&device->resource_mutex);
   return VK_SUCCESS;

fail:
   simple_mtx_unlock(&device->resource_mutex);
   return vk_error(device, result);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_UnmapMemory(VkDevice _device, VkDeviceMemory _memory)
{
   vk_common_UnmapMemory(_device, _memory);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_UnmapMemory2KHR(VkDevice _device,
                        const VkMemoryUnmapInfoKHR* pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   struct wrapper_device_memory *mem =
      wrapper_device_memory_lookup(device, pMemoryUnmapInfo->memory);
   if (!mem || !mem->map_address) {
      device->dispatch_table.UnmapMemory(device->dispatch_handle,
                                         pMemoryUnmapInfo->memory);
      return VK_SUCCESS;
   }

   WRAPPER_LOG(info, "Emulating vkUnmapMemory2KHR");

   simple_mtx_lock(&device->resource_mutex);

   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      void *reserved = mmap(mem->map_address, mem->map_size, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      if (reserved == MAP_FAILED) {
         WRAPPER_LOG(error, "Failed to replace the mapping with a reservation");
         simple_mtx_unlock(&device->resource_mutex);
         return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
      }
   } else {
      munmap(mem->map_address, mem->map_size);
   }

   mem->map_size = 0;
   mem->map_address = NULL;

   simple_mtx_unlock(&device->resource_mutex);
   return VK_SUCCESS;
}
