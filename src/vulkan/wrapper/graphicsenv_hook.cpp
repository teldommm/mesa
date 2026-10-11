#include <stdlib.h>
#include <dlfcn.h>
#include <string>

#include "graphicsenv_hook.hpp"
#include "wrapper_log.h"

#define LIBNAME "/system/lib64/libgraphicsenv.so"

static void *graphicsenv_handle = nullptr;
static std::string env_layers_path;

static void *(*getInstance)() = nullptr;
static const std::string& (*getLayerPaths)(void *) = nullptr;
static void *(*getAppNamespace)(void *) = nullptr;
static void (*setLayerPaths)(void *, void *, const std::string) = nullptr;

template <typename T>
static void get_function_pointer(T& pointer, const char *name) {
   if (!pointer)
      pointer = reinterpret_cast<T>(dlsym(graphicsenv_handle, name));
}

extern "C"
bool set_layer_paths(void) {
   if (!graphicsenv_handle)
      graphicsenv_handle = dlopen(LIBNAME, RTLD_NOW);

   if (!graphicsenv_handle) {
      WRAPPER_LOG(error, "Failed to open %s", LIBNAME);
      return false;
   }

   get_function_pointer(getInstance, "_ZN7android11GraphicsEnv11getInstanceEv");
   get_function_pointer(getLayerPaths, "_ZN7android11GraphicsEnv13getLayerPathsEv");
   get_function_pointer(getAppNamespace, "_ZN7android11GraphicsEnv15getAppNamespaceEv");
   get_function_pointer(setLayerPaths, "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");
   if (!setLayerPaths)
      get_function_pointer(setLayerPaths, "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceENSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");

   if (!getInstance || !getLayerPaths || !getAppNamespace || !setLayerPaths) {
      WRAPPER_LOG(error, "Failed to get function pointers from libgraphicsenv.so");
      return false;
   }

   void *instance = getInstance();
   if (!instance) {
      WRAPPER_LOG(error, "Failed to obtain GraphicsEnv instance");
      return false;
   }

   void *app_namespace = getAppNamespace(instance);

   if (env_layers_path.empty()) {
      const char *env = getenv("WRAPPER_LAYER_PATH");
      if (env)
         env_layers_path = env;
   }

   if (env_layers_path.empty()) {
      WRAPPER_LOG(error, "WRAPPER_LAYER_PATH is not set, can't load layers");
      return false;
   }

   setLayerPaths(instance, app_namespace, env_layers_path);

   if (getLayerPaths(instance) != env_layers_path) {
      WRAPPER_LOG(error, "Failed to change layer paths");
      return false;
   }

   return true;
}
