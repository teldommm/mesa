#ifndef GRAPHICSENV_HOOK_HPP
#define GRAPHICSENV_HOOK_HPP

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Points libgraphicsenv's layer search path at WRAPPER_LAYER_PATH so the
 * system Vulkan loader can find the validation / api_dump layers. */
bool set_layer_paths(void);

#ifdef __cplusplus
}
#endif

#endif
