#include <string.h>

#include <unordered_map>
#include <unordered_set>

#include "spirv_patcher.hpp"

namespace {

const uint32_t SpvMagic = 0x07230203;
const uint32_t HeaderWords = 5;

const uint32_t OpCapability = 17;
const uint32_t OpTypeBool = 20;
const uint32_t OpTypeVector = 23;
const uint32_t OpConstantComposite = 44;
const uint32_t OpSpecConstantTrue = 48;
const uint32_t OpSpecConstantFalse = 49;
const uint32_t OpSpecConstant = 50;
const uint32_t OpSpecConstantComposite = 51;
const uint32_t OpDecorate = 71;

const uint32_t CapabilityClipDistance = 32;
const uint32_t CapabilityCullDistance = 33;

const uint32_t DecorationBuiltIn = 11;
const uint32_t BuiltInClipDistance = 3;
const uint32_t BuiltInCullDistance = 4;

bool
valid_module(const uint32_t *words, size_t word_count)
{
   return words && word_count > HeaderWords && words[0] == SpvMagic;
}

/* Calls fn(offset, opcode, length) for each instruction, stops on a
 * malformed one. */
template <typename Fn>
void
for_each_instruction(const uint32_t *words, size_t word_count, Fn fn)
{
   size_t offset = HeaderWords;
   while (offset < word_count) {
      const uint32_t length = words[offset] >> 16;
      const uint32_t opcode = words[offset] & 0xffffu;
      if (length == 0 || offset + length > word_count)
         break;
      fn(offset, opcode, length);
      offset += length;
   }
}

int
clip_cull_kind(const uint32_t *words, size_t offset, uint32_t opcode,
               uint32_t length)
{
   if (opcode == OpCapability && length >= 2) {
      if (words[offset + 1] == CapabilityClipDistance)
         return 1;
      if (words[offset + 1] == CapabilityCullDistance)
         return 2;
   }
   if (opcode == OpDecorate && length >= 4 &&
       words[offset + 2] == DecorationBuiltIn) {
      if (words[offset + 3] == BuiltInClipDistance)
         return 1;
      if (words[offset + 3] == BuiltInCullDistance)
         return 2;
   }
   return 0;
}

} /* namespace */

void
spirv_scan_clip_cull_distance(const uint32_t *words, size_t word_count,
                              bool *has_clip, bool *has_cull)
{
   *has_clip = false;
   *has_cull = false;
   if (!valid_module(words, word_count))
      return;

   for_each_instruction(words, word_count,
                        [&](size_t offset, uint32_t opcode, uint32_t length) {
      const int kind = clip_cull_kind(words, offset, opcode, length);
      *has_clip |= kind == 1;
      *has_cull |= kind == 2;
   });
}

bool
spirv_remove_clip_cull_distance(uint32_t *words, size_t *word_count,
                                bool clip, bool cull)
{
   if (!valid_module(words, *word_count) || (!clip && !cull))
      return false;

   /* Compact the instruction stream in place */
   size_t read = HeaderWords, write = HeaderWords;
   bool changed = false;

   while (read < *word_count) {
      const uint32_t length = words[read] >> 16;
      const uint32_t opcode = words[read] & 0xffffu;
      if (length == 0 || read + length > *word_count) {
         /* Malformed, keep the rest as is */
         memmove(&words[write], &words[read],
                 (*word_count - read) * sizeof(uint32_t));
         write += *word_count - read;
         break;
      }

      const int kind = clip_cull_kind(words, read, opcode, length);
      if ((kind == 1 && clip) || (kind == 2 && cull)) {
         changed = true;
      } else {
         if (write != read)
            memmove(&words[write], &words[read], length * sizeof(uint32_t));
         write += length;
      }
      read += length;
   }

   *word_count = write;
   return changed;
}

bool
spirv_patch_spec_constant_composites(uint32_t *words, size_t word_count)
{
   if (!valid_module(words, word_count))
      return false;

   std::unordered_set<uint32_t> bool_types;
   std::unordered_map<uint32_t, uint32_t> bool_vector_types; /* id -> size */
   std::unordered_set<uint32_t> spec_constants;
   bool changed = false;

   for_each_instruction(words, word_count,
                        [&](size_t offset, uint32_t opcode, uint32_t length) {
      switch (opcode) {
      case OpTypeBool:
         if (length >= 2)
            bool_types.insert(words[offset + 1]);
         break;
      case OpTypeVector:
         if (length >= 4 && bool_types.count(words[offset + 2]))
            bool_vector_types[words[offset + 1]] = words[offset + 3];
         break;
      case OpSpecConstantTrue:
      case OpSpecConstantFalse:
      case OpSpecConstant:
         if (length >= 3)
            spec_constants.insert(words[offset + 2]);
         break;
      case OpConstantComposite: {
         if (length < 3)
            break;
         auto it = bool_vector_types.find(words[offset + 1]);
         if (it == bool_vector_types.end())
            break;
         const uint32_t components = it->second;
         if (length < 3 + components)
            break;
         uint32_t matches = 0;
         for (uint32_t i = 0; i < components; i++)
            matches += spec_constants.count(words[offset + 3 + i]);
         if (matches == components) {
            words[offset] = (words[offset] & ~0xffffu) | OpSpecConstantComposite;
            /* It is a spec constant now, later composites may use it */
            spec_constants.insert(words[offset + 2]);
            changed = true;
         }
         break;
      }
      default:
         break;
      }
   });

   return changed;
}
