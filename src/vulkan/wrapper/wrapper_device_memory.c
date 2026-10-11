// #define native_handle_t __native_handle_t
// #define buffer_handle_t __buffer_handle_t
#include "wrapper_private.h"
#include "wrapper_entrypoints.h"
#include "vk_common_entrypoints.h"
// #undef native_handle_t
// #undef buffer_handle_t
#include "util/os_file.h"
#include "vk_util.h"
#include "vk_printers.h"
#include "wrapper_checks.h"

#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>

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
dma_heap_alloc(int heap_fd, size_t size) {
   struct dma_heap_allocation_data alloc_data = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   if (safe_ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0)
      return -1;

   return alloc_data.fd;
}

static int
ion_heap_alloc(int heap_fd, size_t size) {
   struct ion_allocation_data {
      __u64 len;
      __u32 heap_id_mask;
      __u32 flags;
      __u32 fd;
      __u32 unused;
   } alloc_data = {
      .len = size,
      /* ION_HEAP_SYSTEM | ION_SYSTEM_HEAP_ID */
      .heap_id_mask = (1U << 0) | (1U << 25),
      .flags = 0, /* uncached */
   };

   if (safe_ioctl(heap_fd, _IOWR('I', 0, struct ion_allocation_data),
                  &alloc_data) < 0)
      return -1;

   return alloc_data.fd;
}

static int
wrapper_dmabuf_alloc(struct wrapper_device *device, size_t size)
{
   int fd;

   fd = dma_heap_alloc(device->physical->dma_heap_fd, size);

   if (fd < 0)
      fd = ion_heap_alloc(device->physical->dma_heap_fd, size);

   return fd;
}



uint32_t
wrapper_select_device_memory_type(struct wrapper_device *device,
                                  VkMemoryPropertyFlags flags) {
   VkPhysicalDeviceMemoryProperties *props =
      &device->physical->memory_properties;

   /* Exact match of all requested flags first, then any overlap. */
   for (uint32_t idx = 0; idx < props->memoryTypeCount; idx++) {
      if ((props->memoryTypes[idx].propertyFlags & flags) == flags)
         return idx;
   }
   for (uint32_t idx = 0; idx < props->memoryTypeCount; idx++) {
      if (props->memoryTypes[idx].propertyFlags & flags)
         return idx;
   }
   return UINT32_MAX;
}

/* Pick a memory type allowed by type_bits, keeping the application's choice
 * (and therefore its HOST_CACHED/HOST_COHERENT semantics) whenever possible. */
static uint32_t
wrapper_pick_import_memory_type(struct wrapper_device *device,
                                uint32_t app_index, uint32_t type_bits)
{
   VkPhysicalDeviceMemoryProperties *props = &device->physical->memory_properties;

   if (type_bits == 0 || (type_bits & (1u << app_index)))
      return app_index;

   VkMemoryPropertyFlags wanted = props->memoryTypes[app_index].propertyFlags;
   for (uint32_t idx = 0; idx < props->memoryTypeCount; idx++) {
      if ((type_bits & (1u << idx)) &&
          (props->memoryTypes[idx].propertyFlags & wanted) == wanted)
         return idx;
   }
   for (uint32_t idx = 0; idx < props->memoryTypeCount; idx++) {
      if ((type_bits & (1u << idx)) &&
          (props->memoryTypes[idx].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         return idx;
   }
   return app_index;
}

static VkResult
wrapper_allocate_memory_dmaheap(struct wrapper_device *device,
                                const VkMemoryAllocateInfo* pAllocateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                VkDeviceMemory* pMemory,
                                int *out_fd) {
   VkImportMemoryFdInfoKHR import_fd_info;
   VkMemoryAllocateInfo allocate_info;
   VkResult result;

   if (device->physical->dma_heap_fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   *out_fd = wrapper_dmabuf_alloc(device, pAllocateInfo->allocationSize);
   if (*out_fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkMemoryFdPropertiesKHR memory_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
   };
   result = wrapper_device_trampolines.GetMemoryFdPropertiesKHR(
      (VkDevice) device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         *out_fd, &memory_fd_props);

   if (result != VK_SUCCESS) {
      WLOGD("GetMemoryFdPropertiesKHR failed: %d", result);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   import_fd_info = (VkImportMemoryFdInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = pAllocateInfo->pNext,
      .fd = os_dupfd_cloexec(*out_fd),
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   allocate_info = *pAllocateInfo;
   allocate_info.pNext = &import_fd_info;
   allocate_info.memoryTypeIndex =
      wrapper_pick_import_memory_type(device, pAllocateInfo->memoryTypeIndex,
                                      memory_fd_props.memoryTypeBits);

   result = wrapper_device_trampolines.AllocateMemory((VkDevice) device, &allocate_info, pAllocator, pMemory);

   if (result != VK_SUCCESS && import_fd_info.fd != -1)
      close(import_fd_info.fd);

   return result;
}

static VkResult
wrapper_allocate_memory_dmabuf(struct wrapper_device *device,
                               const VkMemoryAllocateInfo* pAllocateInfo,
                               const VkAllocationCallbacks* pAllocator,
                               VkDeviceMemory* pMemory,
                               int *out_fd) {
   VkExportMemoryAllocateInfo export_memory_info;
   VkMemoryAllocateInfo allocate_info;
   VkResult result;

   export_memory_info = (VkExportMemoryAllocateInfo) {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = pAllocateInfo->pNext,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   allocate_info = *pAllocateInfo;
   allocate_info.pNext = &export_memory_info;

   result = wrapper_device_trampolines.AllocateMemory((VkDevice) device, &allocate_info, pAllocator, pMemory);
   if (result != VK_SUCCESS)
      return result;

   result = wrapper_device_trampolines.GetMemoryFdKHR(
      (VkDevice) device,
      &(VkMemoryGetFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = *pMemory,
         .handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      },
      out_fd);

   if (result != VK_SUCCESS)
      return result;

   if (lseek(*out_fd, 0, SEEK_SET) ||
       lseek(*out_fd, 0, SEEK_END) < pAllocateInfo->allocationSize)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   lseek(*out_fd, 0, SEEK_SET);
   return VK_SUCCESS;
}

static VkResult
wrapper_allocate_memory_opaque_fd(struct wrapper_device *device,
                                  const VkMemoryAllocateInfo *pAllocateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkDeviceMemory *pMemory,
                                  int *out_fd)
{
   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   VkExportMemoryAllocateInfo export_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = pAllocateInfo->pNext,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   allocate_info.pNext = &export_memory_info;

   VkResult result = wrapper_device_trampolines.AllocateMemory(
      (VkDevice) device, &allocate_info, pAllocator, pMemory);
   if (result != VK_SUCCESS)
      return result;

   result = wrapper_device_trampolines.GetMemoryFdKHR(
      (VkDevice) device,
      &(VkMemoryGetFdInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = *pMemory,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      },
      out_fd);

   if (result != VK_SUCCESS || *out_fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   /* An opaque fd is only usable here if it is mmap-able (Adreno exports a
    * dma-buf); reject anything we cannot size. */
   if (lseek(*out_fd, 0, SEEK_END) < (off_t) pAllocateInfo->allocationSize)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   lseek(*out_fd, 0, SEEK_SET);

   return VK_SUCCESS;
}

static VkResult
wrapper_allocate_memory_ahardware_buffer(struct wrapper_device *device,
                                         const VkMemoryAllocateInfo* pAllocateInfo,
                                         const VkAllocationCallbacks* pAllocator,
                                         VkDeviceMemory* pMemory,
                                         AHardwareBuffer **pAHardwareBuffer) {
   VkExportMemoryAllocateInfo export_memory_info;
   VkMemoryAllocateInfo allocate_info;
   VkResult result;

   export_memory_info = (VkExportMemoryAllocateInfo) {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = pAllocateInfo->pNext,
      .handleTypes =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   allocate_info = *pAllocateInfo;
   allocate_info.pNext = &export_memory_info;

   /* VUID-VkMemoryAllocateInfo-pNext-01874: AHB exports dedicated to an
    * image must use allocationSize 0 (the Qualcomm driver rejects others). */
   const VkMemoryDedicatedAllocateInfo *dedicated =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (dedicated && dedicated->image != VK_NULL_HANDLE)
      allocate_info.allocationSize = 0;

   result = wrapper_device_trampolines.AllocateMemory((VkDevice) device,
                  &allocate_info,
                  pAllocator,
                  pMemory);
   if (result != VK_SUCCESS)
      return result;

   result = wrapper_device_trampolines.GetMemoryAndroidHardwareBufferANDROID(
      (VkDevice) device,
      &(VkMemoryGetAndroidHardwareBufferInfoANDROID) {
         .sType =
            VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
         .memory = *pMemory,
      },
      pAHardwareBuffer);

   if (result != VK_SUCCESS)
      return result;
   
   if (AHardwareBuffer_getNativeHandle(*pAHardwareBuffer) == NULL)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   return VK_SUCCESS;
}

static void
wrapper_device_memory_reset(struct wrapper_device_memory *mem) {
   struct wrapper_device *device = mem->device;
   if (mem->ahardware_buffer) {
      AHardwareBuffer_release(mem->ahardware_buffer);
      mem->ahardware_buffer = NULL;
   }
   if (mem->dmabuf_fd != -1) {
      close(mem->dmabuf_fd);
      mem->dmabuf_fd = -1;
   }
   if (mem->map_address && mem->map_size) {
      munmap(mem->map_address, mem->map_size);
      mem->map_address = NULL;
      mem->map_size = 0;
   }
   if (mem->dispatch_handle != VK_NULL_HANDLE) {
      CHECKV(FreeMemory((VkDevice) device, mem->dispatch_handle, mem->alloc));
      mem->dispatch_handle = VK_NULL_HANDLE;
   }
}

/* Must be called with device->resource_mutex held. */
VkResult
wrapper_device_memory_create(struct wrapper_device *device,
                             const VkAllocationCallbacks *alloc,
                             struct wrapper_device_memory **out_mem)
{
   *out_mem = vk_zalloc2(&device->vk.alloc, alloc,
                         sizeof(struct wrapper_device_memory),
                         8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (*out_mem == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   (*out_mem)->dmabuf_fd = -1;
   (*out_mem)->device = device;
   (*out_mem)->alloc = alloc ? alloc : &device->vk.alloc;
   list_add(&(*out_mem)->link, &device->device_memory_list);
   return VK_SUCCESS;
}

/* Must be called with device->resource_mutex held. */
void
wrapper_device_memory_destroy(struct wrapper_device_memory *mem) {
   struct wrapper_device *device = mem->device;
   if (mem->dispatch_handle != VK_NULL_HANDLE && device->memory_map)
      _mesa_hash_table_u64_remove(device->memory_map, (uint64_t) mem->dispatch_handle);
   wrapper_device_memory_reset(mem);
   list_del(&mem->link);
   vk_free2(&device->vk.alloc, mem->alloc, mem);
}

/* Must be called with device->resource_mutex held. */
static struct wrapper_device_memory *
wrapper_device_memory_lookup_locked(struct wrapper_device *device,
                                    VkDeviceMemory handle) {
   if (handle == VK_NULL_HANDLE || !device->memory_map)
      return NULL;
   return _mesa_hash_table_u64_search(device->memory_map, (uint64_t) handle);
}

static struct wrapper_device_memory *
wrapper_device_memory_from_handle(struct wrapper_device *device,
                                  VkDeviceMemory handle) {
   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_device_memory *mem = wrapper_device_memory_lookup_locked(device, handle);
   simple_mtx_unlock(&device->resource_mutex);
   return mem;
}

enum wrapper_memory_backend {
   BACKEND_DMABUF,   /* driver allocation exported as dma-buf */
   BACKEND_DMAHEAP,  /* dma-heap/ion allocation imported into the driver */
   BACKEND_AHB,
   BACKEND_OPAQUE,
};

static VkResult
wrapper_allocate_with_backend(struct wrapper_device *device,
                              enum wrapper_memory_backend backend,
                              const VkMemoryAllocateInfo *info,
                              const VkAllocationCallbacks *pAllocator,
                              struct wrapper_device_memory *mem)
{
   switch (backend) {
   case BACKEND_DMABUF:
      return wrapper_allocate_memory_dmabuf(device, info, pAllocator,
                                            &mem->dispatch_handle, &mem->dmabuf_fd);
   case BACKEND_DMAHEAP:
      return wrapper_allocate_memory_dmaheap(device, info, pAllocator,
                                             &mem->dispatch_handle, &mem->dmabuf_fd);
   case BACKEND_AHB:
      return wrapper_allocate_memory_ahardware_buffer(device, info, pAllocator,
                                                      &mem->dispatch_handle, &mem->ahardware_buffer);
   case BACKEND_OPAQUE:
      return wrapper_allocate_memory_opaque_fd(device, info, pAllocator,
                                               &mem->dispatch_handle, &mem->dmabuf_fd);
   }
   return VK_ERROR_INITIALIZATION_FAILED;
}

/* WRAPPER_RESOURCE_TYPE (set by the emulator):
 *   auto (default) : dmabuf export -> dma-heap -> AHB -> opaque fd
 *   dmabuf         : dma-heap import (same meaning as the mesa wrapper)
 *   export         : driver dma-buf export only
 *   ahb / opaque   : that backend only */
static uint32_t
wrapper_memory_backend_order(const char *type, enum wrapper_memory_backend *order)
{
   if (type && strstr(type, "ahb")) {
      order[0] = BACKEND_AHB;
      return 1;
   }
   if (type && strstr(type, "opaque")) {
      order[0] = BACKEND_OPAQUE;
      return 1;
   }
   if (type && strstr(type, "export")) {
      order[0] = BACKEND_DMABUF;
      return 1;
   }
   if (type && (strstr(type, "dmabuf") || strstr(type, "dmaheap"))) {
      order[0] = BACKEND_DMAHEAP;
      order[1] = BACKEND_DMABUF;
      return 2;
   }
   order[0] = BACKEND_DMABUF;
   order[1] = BACKEND_DMAHEAP;
   order[2] = BACKEND_AHB;
   order[3] = BACKEND_OPAQUE;
   return 4;
}

static const char *backend_names[] = { "dmabuf-export", "dma-heap", "ahb", "opaque-fd" };

_Atomic static uint64_t allocations;

WRAPPER_AllocateMemory(VkDevice _device,
                       const VkMemoryAllocateInfo* pAllocateInfo,
                       const VkAllocationCallbacks* pAllocator,
                       VkDeviceMemory* pMemory) {
    VK_FROM_HANDLE(wrapper_device, device, _device);
    struct wrapper_device_memory *mem = NULL;
    VkResult result;
    const VkMemoryAllocateInfo allocate_info = *pAllocateInfo;

    bool debug = should_log_memory_debug();
    _Atomic static uint64_t allocated_memory[VK_MAX_MEMORY_TYPES];

    if (debug) {
        WLOGD("WRAPPER_AllocateMemory, pAllocateInfo:");
        LOG_STRUCT(VkMemoryAllocateInfo, pAllocateInfo);
    }

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

    if (debug) WLOGD("Emulating AllocateMemory");
    simple_mtx_lock(&device->resource_mutex);

    result = wrapper_device_memory_create(device, pAllocator, &mem);
    if (result != VK_SUCCESS) {
        simple_mtx_unlock(&device->resource_mutex);
        return vk_error(device, result);
    }
    mem->alloc_size = pAllocateInfo->allocationSize;

    enum wrapper_memory_backend order[4];
    uint32_t order_count = wrapper_memory_backend_order(device->physical->resource_type, order);
    result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t i = 0; i < order_count; i++) {
        if (debug) WLOGD("Trying %s", backend_names[order[i]]);
        result = wrapper_allocate_with_backend(device, order[i], pAllocateInfo, pAllocator, mem);
        if (result == VK_SUCCESS)
            break;
        wrapper_device_memory_reset(mem);
    }

    if (result != VK_SUCCESS) {
        WLOGE("Emulated AllocateMemory failed (size=%llu, type=%u): %d",
              (unsigned long long) pAllocateInfo->allocationSize,
              pAllocateInfo->memoryTypeIndex, result);
        wrapper_device_memory_destroy(mem);
        simple_mtx_unlock(&device->resource_mutex);
        return vk_error(device, result);
    }

    _mesa_hash_table_u64_insert(device->memory_map, (uint64_t) mem->dispatch_handle, mem);
    *pMemory = mem->dispatch_handle;
    simple_mtx_unlock(&device->resource_mutex);
    if (debug) WLOGD("AllocateMemory out VkDeviceMemory: %p", *pMemory);
    goto tracking;

fallback:
    if (debug) WLOGD("Dispatching to vkAllocateMemory (not emulating AllocateMemory)");
    result = CHECK(AllocateMemory(_device, &allocate_info, pAllocator, pMemory));
    if (debug) WLOGD("vkAllocateMemory returned %d", result);

tracking:
    if (result == VK_SUCCESS) {
        allocations++;
        if (allocate_info.memoryTypeIndex < VK_MAX_MEMORY_TYPES)
            allocated_memory[allocate_info.memoryTypeIndex] += allocate_info.allocationSize;
    }
    if (debug || result != VK_SUCCESS) {
        WLOGD("%s %llu bytes in memory type %u", result == VK_SUCCESS ? "Allocated" : "Tried to allocate",
              (unsigned long long) allocate_info.allocationSize, allocate_info.memoryTypeIndex);
        WLOGD("Total allocations: %llu / %u", (unsigned long long) allocations,
              device->physical->properties2.properties.limits.maxMemoryAllocationCount);
        for (int i = 0; i < VK_MAX_MEMORY_TYPES; i++) {
            if (allocated_memory[i]) {
                WLOGD("allocated_memory[%d] = %llu", i, (unsigned long long) allocated_memory[i]);
            }
        }
    }
    return result;
}

WRAPPER_FreeMemory(VkDevice _device, VkDeviceMemory _memory,
                   const VkAllocationCallbacks* pAllocator)
{
    VK_FROM_HANDLE(wrapper_device, device, _device);

    if (_memory == VK_NULL_HANDLE)
        return;

    simple_mtx_lock(&device->resource_mutex);
    struct wrapper_device_memory *mem = wrapper_device_memory_lookup_locked(device, _memory);
    if (mem) {
        mem->alloc = pAllocator ? pAllocator : &device->vk.alloc;
        allocations--;
        wrapper_device_memory_destroy(mem);
        simple_mtx_unlock(&device->resource_mutex);
        return;
    }
    simple_mtx_unlock(&device->resource_mutex);

    allocations--;
    CHECKV(FreeMemory((VkDevice) device, _memory, pAllocator));
}

static int
wrapper_device_memory_get_fd(struct wrapper_device_memory *mem, bool debug)
{
   if (!mem->ahardware_buffer)
      return mem->dmabuf_fd;

   /* Some gralloc implementations do not put the buffer in data[0]: pick the
    * first fd that is large enough to back the allocation. */
   const native_handle_t *handle = AHardwareBuffer_getNativeHandle(mem->ahardware_buffer);
   if (!handle)
      return -1;

   for (int idx = 0; idx < handle->numFds; idx++) {
      off_t size = lseek(handle->data[idx], 0, SEEK_END);
      if (size < 0) {
         WLOG("lseek failed on AHB fd (idx=%d, fd=%d), errno = %d", idx, handle->data[idx], errno);
         continue;
      }
      if ((size_t) size >= mem->alloc_size) {
         if (debug) WLOGD("AHB fd idx=%d size=0x%llx", idx, (unsigned long long) size);
         return handle->data[idx];
      }
   }
   WLOGE("Failed to find an AHB fd >= alloc_size of 0x%zx", mem->alloc_size);
   return -1;
}

WRAPPER_MapMemory2KHR(VkDevice _device,
                      const VkMemoryMapInfoKHR* pMemoryMapInfo,
                      void** ppData)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   bool debug = should_log_memory_debug();
   const VkMemoryMapPlacedInfoEXT *placed_info = NULL;
   struct wrapper_device_memory *mem;
   VkResult result = VK_SUCCESS;

   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      placed_info = vk_find_struct_const(pMemoryMapInfo->pNext,
         MEMORY_MAP_PLACED_INFO_EXT);
      if (debug && placed_info) {
         WLOGD("Using VK_MEMORY_MAP_PLACED_BIT_EXT:")
         LOG_STRUCT(VkMemoryMapPlacedInfoEXT, placed_info);
      }
   }

   mem = placed_info ? wrapper_device_memory_from_handle(device, pMemoryMapInfo->memory) : NULL;
   if (!mem) {
      if (debug) WLOGD("Not emulating MapMemory2KHR");
      return CHECK(MapMemory(_device,
         pMemoryMapInfo->memory, pMemoryMapInfo->offset, pMemoryMapInfo->size,
            0, ppData));
   }

   simple_mtx_lock(&device->resource_mutex);

   if (mem->map_address) {
      if (placed_info->pPlacedAddress != mem->map_address ||
          pMemoryMapInfo->offset != mem->map_offset) {
         WLOGE("mem %p is already mapped at %p (offset %zu), remap to %p requested",
               (void *) pMemoryMapInfo->memory, mem->map_address, mem->map_offset,
               placed_info->pPlacedAddress);
         result = VK_ERROR_MEMORY_MAP_FAILED;
         goto out;
      }
      *ppData = mem->map_address;
      goto out;
   }

   int fd = wrapper_device_memory_get_fd(mem, debug);
   if (fd < 0) {
      result = VK_ERROR_MEMORY_MAP_FAILED;
      goto out;
   }

   size_t map_size;
   if (pMemoryMapInfo->size == VK_WHOLE_SIZE) {
      size_t total = mem->alloc_size;
      if (!total) {
         off_t end = lseek(fd, 0, SEEK_END);
         if (end < 0) {
            WLOGE("Failed to lseek(fd=%d), errno = %d", fd, errno);
            result = VK_ERROR_MEMORY_MAP_FAILED;
            goto out;
         }
         total = end;
      }
      map_size = total - pMemoryMapInfo->offset;
   } else {
      map_size = pMemoryMapInfo->size;
   }

   /* VK_EXT_map_memory_placed: the byte at `offset` lands on pPlacedAddress.
    * offset is a multiple of minPlacedMemoryMapAlignment (the page size). */
   void *addr = mmap(placed_info->pPlacedAddress, map_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                     fd, (off_t) pMemoryMapInfo->offset);
   if (addr == MAP_FAILED) {
      WLOGE("mmap failed emulating MapMemory2KHR: errno = %d", errno);
      result = VK_ERROR_MEMORY_MAP_FAILED;
      goto out;
   }

   mem->map_address = addr;
   mem->map_size = map_size;
   mem->map_offset = pMemoryMapInfo->offset;
   *ppData = addr;
   if (debug) WLOGD("mem %p mapped to %p (size 0x%zx)", (void *) pMemoryMapInfo->memory, addr, map_size);

out:
   simple_mtx_unlock(&device->resource_mutex);
   if (result != VK_SUCCESS)
      return vk_error(device, result);
   return VK_SUCCESS;
}

WRAPPER_UnmapMemory(VkDevice _device, VkDeviceMemory _memory) {
   vk_common_UnmapMemory(_device, _memory);
}

WRAPPER_UnmapMemory2KHR(VkDevice _device,
                        const VkMemoryUnmapInfoKHR* pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   bool debug = should_log_memory_debug();

   simple_mtx_lock(&device->resource_mutex);
   struct wrapper_device_memory *mem =
      wrapper_device_memory_lookup_locked(device, pMemoryUnmapInfo->memory);

   if (!mem || !mem->map_address) {
      simple_mtx_unlock(&device->resource_mutex);
      if (debug) WLOGD("Unmapping mem %p without emulation", (void *) pMemoryUnmapInfo->memory);
      CHECKV(UnmapMemory(_device, pMemoryUnmapInfo->memory));
      return VK_SUCCESS;
   }

   if (debug) WLOGD("Unmapping mem %p (mapped at %p)", (void *) pMemoryUnmapInfo->memory, mem->map_address);
   VkResult result = VK_SUCCESS;
   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      /* Keep the address range reserved for the application. */
      void *reserved = mmap(mem->map_address, mem->map_size,
         PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
      if (reserved == MAP_FAILED) {
         WLOGE("Failed to replace mapping with reserved memory");
         result = VK_ERROR_MEMORY_MAP_FAILED;
      }
   } else {
      munmap(mem->map_address, mem->map_size);
   }

   mem->map_size = 0;
   mem->map_offset = 0;
   mem->map_address = NULL;
   simple_mtx_unlock(&device->resource_mutex);
   return result == VK_SUCCESS ? VK_SUCCESS : vk_error(device, result);
}
