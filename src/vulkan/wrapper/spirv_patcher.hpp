#ifndef __SPIRV_PATCHER_HPP
#define __SPIRV_PATCHER_HPP

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPIR-V workarounds for the Mali (ARM proprietary) driver, ported from the
 * Mesa 25 based wrapper.  Both work in place on a copy of the module;
 * word_count / *word_count are in 32-bit words. */

/* Drops the ClipDistance / CullDistance capabilities and BuiltIn decorations
 * (the wrapper advertises shaderClipDistance / shaderCullDistance even when
 * the driver lacks them).  Returns true when the module was changed. */
bool
spirv_remove_clip_cull_distance(uint32_t *words, size_t *word_count,
                                bool clip, bool cull);

/* Turns OpConstantComposite of a bool vector whose constituents are all
 * specialization constants into OpSpecConstantComposite (unbound texture
 * checks in DXVK shaders).  Returns true when the module was changed. */
bool
spirv_patch_spec_constant_composites(uint32_t *words, size_t word_count);

/* Whether the module uses ClipDistance / CullDistance at all */
void
spirv_scan_clip_cull_distance(const uint32_t *words, size_t word_count,
                              bool *has_clip, bool *has_cull);

#ifdef __cplusplus
}
#endif

#endif
