/*
 * Transform feedback emulation for drivers without VK_EXT_transform_feedback
 * (Qualcomm proprietary / Adreno, Mali, PowerVR).
 *
 * The wrapper advertises VK_EXT_transform_feedback so that DXVK accepts the
 * device. Before this file existed, the first game that actually used stream
 * output called a NULL function pointer, and shaders carrying Xfb decorations
 * were handed to a driver that does not understand them.
 *
 * What we do when the driver lacks the extension:
 *  - strip TransformFeedback / GeometryStreams from SPIR-V,
 *  - make the xfb commands no-ops (stream output is simply not captured),
 *  - back xfb query pools with a dummy pool and report zero primitives so
 *    that applications waiting on the results never hang.
 */
#include <stdlib.h>
#include <string.h>

#include "wrapper_private.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "wrapper_checks.h"
#include "vk_printers.h"
#include "vk_util.h"
#include "util/hash_table.h"

/* ---------------------------------------------------------------- SPIR-V */

#define SPV_OP_NOP                    0
#define SPV_OP_EXECUTION_MODE         16
#define SPV_OP_CAPABILITY             17
#define SPV_OP_CONSTANT               43
#define SPV_OP_DECORATE               71
#define SPV_OP_MEMBER_DECORATE        72
#define SPV_OP_EMIT_VERTEX            218
#define SPV_OP_END_PRIMITIVE          219
#define SPV_OP_EMIT_STREAM_VERTEX     220
#define SPV_OP_END_STREAM_PRIMITIVE   221

#define SPV_CAP_TRANSFORM_FEEDBACK    53
#define SPV_CAP_GEOMETRY_STREAMS      54
#define SPV_EXEC_MODE_XFB             11
#define SPV_DEC_STREAM                29
#define SPV_DEC_OFFSET                35
#define SPV_DEC_XFB_BUFFER            36
#define SPV_DEC_XFB_STRIDE            37

static bool
spirv_has_xfb(const uint32_t *code, size_t words)
{
   size_t offset = 5;
   while (offset < words) {
      uint32_t length = code[offset] >> 16;
      uint32_t opcode = code[offset] & 0xffff;
      if (length == 0 || offset + length > words)
         return false;
      if (opcode == SPV_OP_CAPABILITY && length >= 2 &&
          (code[offset + 1] == SPV_CAP_TRANSFORM_FEEDBACK ||
           code[offset + 1] == SPV_CAP_GEOMETRY_STREAMS))
         return true;
      /* Capabilities come first in a valid module. */
      if (opcode != SPV_OP_CAPABILITY && opcode != 11 /* OpExtInstImport */ &&
          opcode != 10 /* OpExtension */ && opcode != SPV_OP_NOP)
         return false;
      offset += length;
   }
   return false;
}

/* Returns a malloc'ed module in *out (caller frees) or false if untouched. */
bool
wrapper_spirv_strip_xfb(const uint32_t *code, size_t words,
                        uint32_t **out, size_t *out_words)
{
   if (words < 5 || code[0] != 0x07230203 || !spirv_has_xfb(code, words))
      return false;

   uint32_t *dst = malloc(words * sizeof(uint32_t));
   if (!dst)
      return false;

   /* id -> constant value, only for the stream operands we care about */
   uint32_t bound = code[3];
   uint8_t *is_nonzero = calloc(bound ? bound : 1, 1);
   uint8_t *is_const = calloc(bound ? bound : 1, 1);
   if (!is_nonzero || !is_const) {
      free(dst);
      free(is_nonzero);
      free(is_const);
      return false;
   }

   size_t offset = 5;
   while (offset < words) {
      uint32_t length = code[offset] >> 16;
      uint32_t opcode = code[offset] & 0xffff;
      if (length == 0 || offset + length > words)
         break;
      if (opcode == SPV_OP_CONSTANT && length >= 4) {
         uint32_t id = code[offset + 2];
         if (id < bound) {
            is_const[id] = 1;
            is_nonzero[id] = code[offset + 3] != 0;
         }
      }
      offset += length;
   }

   memcpy(dst, code, 5 * sizeof(uint32_t));
   size_t w = 5;
   unsigned removed = 0;
   offset = 5;
   while (offset < words) {
      uint32_t length = code[offset] >> 16;
      uint32_t opcode = code[offset] & 0xffff;
      if (length == 0 || offset + length > words) {
         /* malformed: copy the rest verbatim */
         memcpy(dst + w, code + offset, (words - offset) * sizeof(uint32_t));
         w += words - offset;
         break;
      }

      bool drop = false;
      switch (opcode) {
      case SPV_OP_CAPABILITY:
         drop = code[offset + 1] == SPV_CAP_TRANSFORM_FEEDBACK ||
                code[offset + 1] == SPV_CAP_GEOMETRY_STREAMS;
         break;
      case SPV_OP_EXECUTION_MODE:
         drop = length >= 3 && code[offset + 2] == SPV_EXEC_MODE_XFB;
         break;
      case SPV_OP_DECORATE:
         /* OpDecorate Offset on a variable is only valid for xfb outputs;
          * block member offsets use OpMemberDecorate and are kept. */
         drop = length >= 3 &&
                (code[offset + 2] == SPV_DEC_STREAM ||
                 code[offset + 2] == SPV_DEC_OFFSET ||
                 code[offset + 2] == SPV_DEC_XFB_BUFFER ||
                 code[offset + 2] == SPV_DEC_XFB_STRIDE);
         break;
      case SPV_OP_MEMBER_DECORATE:
         drop = length >= 4 &&
                (code[offset + 3] == SPV_DEC_STREAM ||
                 code[offset + 3] == SPV_DEC_XFB_BUFFER ||
                 code[offset + 3] == SPV_DEC_XFB_STRIDE);
         break;
      case SPV_OP_EMIT_STREAM_VERTEX:
      case SPV_OP_END_STREAM_PRIMITIVE: {
         uint32_t stream = length >= 2 ? code[offset + 1] : 0;
         bool nonzero = stream < bound && is_const[stream] && is_nonzero[stream];
         if (nonzero) {
            /* Only stream 0 is rasterized; other streams fed SO only. */
            drop = true;
         } else {
            dst[w++] = (1u << 16) | (opcode == SPV_OP_EMIT_STREAM_VERTEX ?
                                     SPV_OP_EMIT_VERTEX : SPV_OP_END_PRIMITIVE);
            removed++;
            offset += length;
            continue;
         }
         break;
      }
      default:
         break;
      }

      if (drop) {
         removed++;
      } else {
         memcpy(dst + w, code + offset, length * sizeof(uint32_t));
         w += length;
      }
      offset += length;
   }

   free(is_nonzero);
   free(is_const);

   if (!removed) {
      free(dst);
      return false;
   }

   *out = dst;
   *out_words = w;
   return true;
}

/* -------------------------------------------------------------- commands */

static inline bool
xfb_is_native(struct wrapper_device *device)
{
   return device->physical->base_supported_extensions.EXT_transform_feedback;
}

WRAPPER_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer,
                                           uint32_t firstBinding,
                                           uint32_t bindingCount,
                                           const VkBuffer* pBuffers,
                                           const VkDeviceSize* pOffsets,
                                           const VkDeviceSize* pSizes)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (xfb_is_native(wcb->device))
      CHECKV(CmdBindTransformFeedbackBuffersEXT(commandBuffer, firstBinding, bindingCount,
                                                pBuffers, pOffsets, pSizes));
}

WRAPPER_CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer,
                                     uint32_t firstCounterBuffer,
                                     uint32_t counterBufferCount,
                                     const VkBuffer* pCounterBuffers,
                                     const VkDeviceSize* pCounterBufferOffsets)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (xfb_is_native(wcb->device))
      CHECKV(CmdBeginTransformFeedbackEXT(commandBuffer, firstCounterBuffer, counterBufferCount,
                                          pCounterBuffers, pCounterBufferOffsets));
}

WRAPPER_CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer,
                                   uint32_t firstCounterBuffer,
                                   uint32_t counterBufferCount,
                                   const VkBuffer* pCounterBuffers,
                                   const VkDeviceSize* pCounterBufferOffsets)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (xfb_is_native(wcb->device))
      CHECKV(CmdEndTransformFeedbackEXT(commandBuffer, firstCounterBuffer, counterBufferCount,
                                        pCounterBuffers, pCounterBufferOffsets));
}

WRAPPER_CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer,
                                    uint32_t instanceCount,
                                    uint32_t firstInstance,
                                    VkBuffer counterBuffer,
                                    VkDeviceSize counterBufferOffset,
                                    uint32_t counterOffset,
                                    uint32_t vertexStride)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (xfb_is_native(wcb->device))
      CHECKV(CmdDrawIndirectByteCountEXT(commandBuffer, instanceCount, firstInstance,
                                         counterBuffer, counterBufferOffset,
                                         counterOffset, vertexStride));
   /* else: nothing was captured, so DrawAuto draws nothing. */
}

/* Number of live emulated pools in the process: keeps the hot query paths
 * (DXVK occlusion queries) free of locking when no game uses SO queries. */
static _Atomic uint32_t g_fake_xfb_pool_count;

static bool
is_fake_xfb_pool(struct wrapper_device *device, VkQueryPool pool)
{
   if (g_fake_xfb_pool_count == 0 || xfb_is_native(device) || pool == VK_NULL_HANDLE)
      return false;
   simple_mtx_lock(&device->resource_mutex);
   bool fake = device->fake_xfb_query_pools &&
               _mesa_hash_table_u64_search(device->fake_xfb_query_pools, (uint64_t) pool) != NULL;
   simple_mtx_unlock(&device->resource_mutex);
   return fake;
}

WRAPPER_CreateQueryPool(VkDevice _device,
                        const VkQueryPoolCreateInfo* pCreateInfo,
                        const VkAllocationCallbacks* pAllocator,
                        VkQueryPool* pQueryPool)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   if (pCreateInfo->queryType != VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT ||
       xfb_is_native(device))
      return CHECK(CreateQueryPool(_device, pCreateInfo, pAllocator, pQueryPool));

   /* Back it with a timestamp pool: never begun, results are synthesized. */
   VkQueryPoolCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = pCreateInfo->queryCount,
   };
   VkResult result = CHECK(CreateQueryPool(_device, &info, pAllocator, pQueryPool));
   if (result != VK_SUCCESS)
      return result;

   WLOG("Emulating a transform feedback query pool (%u queries)", pCreateInfo->queryCount);
   simple_mtx_lock(&device->resource_mutex);
   _mesa_hash_table_u64_insert(device->fake_xfb_query_pools, (uint64_t) *pQueryPool, (void *) 1);
   g_fake_xfb_pool_count++;
   simple_mtx_unlock(&device->resource_mutex);
   return VK_SUCCESS;
}

WRAPPER_DestroyQueryPool(VkDevice _device,
                         VkQueryPool queryPool,
                         const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   if (queryPool != VK_NULL_HANDLE && !xfb_is_native(device)) {
      simple_mtx_lock(&device->resource_mutex);
      if (device->fake_xfb_query_pools &&
          _mesa_hash_table_u64_search(device->fake_xfb_query_pools, (uint64_t) queryPool)) {
         _mesa_hash_table_u64_remove(device->fake_xfb_query_pools, (uint64_t) queryPool);
         g_fake_xfb_pool_count--;
      }
      simple_mtx_unlock(&device->resource_mutex);
   }
   CHECKV(DestroyQueryPool(_device, queryPool, pAllocator));
}

WRAPPER_CmdBeginQueryIndexedEXT(VkCommandBuffer commandBuffer,
                                VkQueryPool queryPool,
                                uint32_t query,
                                VkQueryControlFlags flags,
                                uint32_t index)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (xfb_is_native(wcb->device)) {
      CHECKV(CmdBeginQueryIndexedEXT(commandBuffer, queryPool, query, flags, index));
      return;
   }
   if (is_fake_xfb_pool(wcb->device, queryPool))
      return;
   /* Non-xfb query types only have index 0. */
   CHECKV(CmdBeginQuery(commandBuffer, queryPool, query, flags));
}

WRAPPER_CmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer,
                              VkQueryPool queryPool,
                              uint32_t query,
                              uint32_t index)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (xfb_is_native(wcb->device)) {
      CHECKV(CmdEndQueryIndexedEXT(commandBuffer, queryPool, query, index));
      return;
   }
   if (is_fake_xfb_pool(wcb->device, queryPool))
      return;
   CHECKV(CmdEndQuery(commandBuffer, queryPool, query));
}

WRAPPER_CmdBeginQuery(VkCommandBuffer commandBuffer,
                      VkQueryPool queryPool,
                      uint32_t query,
                      VkQueryControlFlags flags)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (is_fake_xfb_pool(wcb->device, queryPool))
      return;
   CHECKV(CmdBeginQuery(commandBuffer, queryPool, query, flags));
}

WRAPPER_CmdEndQuery(VkCommandBuffer commandBuffer,
                    VkQueryPool queryPool,
                    uint32_t query)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (is_fake_xfb_pool(wcb->device, queryPool))
      return;
   CHECKV(CmdEndQuery(commandBuffer, queryPool, query));
}

/* xfb stream queries return 2 values: primitives written / needed. */
#define XFB_QUERY_VALUES 2

WRAPPER_GetQueryPoolResults(VkDevice _device,
                            VkQueryPool queryPool,
                            uint32_t firstQuery,
                            uint32_t queryCount,
                            size_t dataSize,
                            void* pData,
                            VkDeviceSize stride,
                            VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   if (!is_fake_xfb_pool(device, queryPool))
      return CHECK(GetQueryPoolResults(_device, queryPool, firstQuery, queryCount,
                                       dataSize, pData, stride, flags));

   const size_t elem = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
   const bool availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   for (uint32_t i = 0; i < queryCount; i++) {
      uint8_t *entry = (uint8_t *) pData + i * stride;
      size_t size = elem * (XFB_QUERY_VALUES + (availability ? 1 : 0));
      if ((size_t)(entry - (uint8_t *) pData) + size > dataSize)
         break;
      memset(entry, 0, elem * XFB_QUERY_VALUES);
      if (availability) {
         if (elem == 8)
            *(uint64_t *)(entry + elem * XFB_QUERY_VALUES) = 1;
         else
            *(uint32_t *)(entry + elem * XFB_QUERY_VALUES) = 1;
      }
   }
   return VK_SUCCESS;
}

WRAPPER_CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                                VkQueryPool queryPool,
                                uint32_t firstQuery,
                                uint32_t queryCount,
                                VkBuffer dstBuffer,
                                VkDeviceSize dstOffset,
                                VkDeviceSize stride,
                                VkQueryResultFlags flags)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   if (!is_fake_xfb_pool(wcb->device, queryPool)) {
      CHECKV(CmdCopyQueryPoolResults(commandBuffer, queryPool, firstQuery, queryCount,
                                     dstBuffer, dstOffset, stride, flags));
      return;
   }
   if (queryCount == 0)
      return;

   const VkDeviceSize elem = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
   const bool availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
   const VkDeviceSize entry = elem * (XFB_QUERY_VALUES + (availability ? 1 : 0));
   const VkDeviceSize size = (queryCount - 1) * stride + entry;

   CHECKV(CmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, 0));
   if (availability) {
      const uint64_t one = 1;
      for (uint32_t i = 0; i < queryCount; i++) {
         CHECKV(CmdUpdateBuffer(commandBuffer, dstBuffer,
                                dstOffset + i * stride + elem * XFB_QUERY_VALUES,
                                elem, &one));
      }
   }
}
