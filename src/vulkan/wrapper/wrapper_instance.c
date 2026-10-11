#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "graphicsenv_hook.hpp"
#include "wrapper_entrypoints.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "adrenotools/driver.h"

const struct vk_instance_extension_table wrapper_instance_extensions = {
   .KHR_get_surface_capabilities2 = true,
   .EXT_surface_maintenance1 = true,
   .KHR_surface_maintenance1 = true,
   .KHR_surface_protected_capabilities = true,
   .KHR_surface = true,
   .EXT_swapchain_colorspace = true,
#ifdef VK_USE_PLATFORM_ANDROID_KHR
   .KHR_android_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .KHR_display = true,
   .KHR_get_display_properties2 = true,
   .EXT_display_surface_counter = true,
   .EXT_acquire_drm_display = true,
   .EXT_direct_mode_display = true,
#endif
   .EXT_headless_surface = true,
};

#define WRAPPER_VALIDATION_LAYER "VK_LAYER_KHRONOS_validation"
#define WRAPPER_API_DUMP_LAYER   "VK_LAYER_LUNARG_api_dump"

static void *vulkan_library_handle;
static PFN_vkCreateInstance create_instance;
static PFN_vkGetInstanceProcAddr get_instance_proc_addr;
static PFN_vkEnumerateInstanceVersion enumerate_instance_version;
static PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extension_properties;
static PFN_vkEnumerateInstanceLayerProperties enumerate_instance_layer_properties;
static PFN_vkCreateDebugUtilsMessengerEXT create_debug_utils_messenger;
static PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_utils_messenger;
static VkDebugUtilsMessengerEXT debug_utils_messenger;
static struct vk_instance_extension_table *supported_instance_extensions;
static bool has_intercepted_layer_paths = false;

#ifdef __LP64__
#define DEFAULT_VULKAN_PATH "/system/lib64/libvulkan.so"
#else
#define DEFAULT_VULKAN_PATH "/system/lib/libvulkan.so"
#endif

static void
init_debug_messenger(VkInstance instance)
{
   create_debug_utils_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)
      get_instance_proc_addr(instance, "vkCreateDebugUtilsMessengerEXT");
   destroy_debug_utils_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
      get_instance_proc_addr(instance, "vkDestroyDebugUtilsMessengerEXT");

   if (!create_debug_utils_messenger || !destroy_debug_utils_messenger)
      return;

   WRAPPER_LOG(info, "Creating debug messenger");

   const VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = &wrapper_debug_utils_messenger,
   };

   if (create_debug_utils_messenger(instance, &messenger_info, NULL,
                                    &debug_utils_messenger) != VK_SUCCESS)
      debug_utils_messenger = VK_NULL_HANDLE;
}

/* Loads the real Vulkan driver: a custom one through libadrenotools when the
 * emulator selected one (ADRENOTOOLS_* variables), the system one otherwise.
 */
static void *
get_vulkan_handle(void)
{
   const char *path = getenv("ADRENOTOOLS_DRIVER_PATH");
   const char *redirect_dir = getenv("ADRENOTOOLS_REDIRECT_DIR");
   const char *name = getenv("ADRENOTOOLS_DRIVER_NAME");
   const char *hooks = getenv("ADRENOTOOLS_HOOKS_PATH");
   char hooks_buf[PATH_MAX];
   struct stat sb;

   init_wrapper_logging();

#ifdef __TERMUX__
   if (!hooks && getenv("PREFIX")) {
      snprintf(hooks_buf, sizeof(hooks_buf), "%s/lib", getenv("PREFIX"));
      hooks = hooks_buf;
   }
#else
   (void)hooks_buf;
#endif

   if (WRAPPER_LOG_LEVEL(validation) || WRAPPER_LOG_LEVEL(apidump))
      has_intercepted_layer_paths = set_layer_paths();

   if (hooks && path && name && stat(path, &sb) == 0) {
      char temp[PATH_MAX];
      snprintf(temp, sizeof(temp), "%stemp", path);
      mkdir(temp, S_IRWXU | S_IRWXG);

      int flags = ADRENOTOOLS_DRIVER_CUSTOM;
      if (redirect_dir)
         flags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;

      WRAPPER_LOG(info, "Loading custom driver %s%s", path, name);
      void *handle = adrenotools_open_libvulkan(RTLD_NOW, flags, temp, hooks,
                                                path, name, redirect_dir,
                                                NULL);
      if (handle)
         return handle;

      WRAPPER_LOG(error, "adrenotools failed to load %s%s, using the system driver",
                  path, name);
   }

   return dlopen(DEFAULT_VULKAN_PATH, RTLD_NOW | RTLD_LOCAL);
}

static bool
vulkan_library_init(void)
{
   if (vulkan_library_handle)
      return true;

   vulkan_library_handle = get_vulkan_handle();
   if (!vulkan_library_handle) {
      fprintf(stderr, "wrapper: failed to load the Vulkan driver: %s\n", dlerror());
      return false;
   }

   create_instance = dlsym(vulkan_library_handle, "vkCreateInstance");
   get_instance_proc_addr = dlsym(vulkan_library_handle, "vkGetInstanceProcAddr");
   enumerate_instance_version = dlsym(vulkan_library_handle,
                                      "vkEnumerateInstanceVersion");
   enumerate_instance_extension_properties =
      dlsym(vulkan_library_handle, "vkEnumerateInstanceExtensionProperties");
   enumerate_instance_layer_properties =
      dlsym(vulkan_library_handle, "vkEnumerateInstanceLayerProperties");

   if (!create_instance || !get_instance_proc_addr ||
       !enumerate_instance_extension_properties) {
      fprintf(stderr, "wrapper: the Vulkan driver lacks required entrypoints\n");
      return false;
   }

   return true;
}

static VkResult
wrapper_vulkan_init(void)
{
   VkExtensionProperties props[VK_INSTANCE_EXTENSION_COUNT];
   uint32_t prop_count = VK_INSTANCE_EXTENSION_COUNT;
   VkResult result;

   if (supported_instance_extensions)
      return VK_SUCCESS;

   if (!vulkan_library_init())
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   result = enumerate_instance_extension_properties(NULL, &prop_count, props);
   if (result != VK_SUCCESS && result != VK_INCOMPLETE)
      return result;

   struct vk_instance_extension_table *exts = malloc(sizeof(*exts));
   if (!exts)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *exts = wrapper_instance_extensions;

   for (uint32_t i = 0; i < prop_count; i++) {
      int idx;
      for (idx = 0; idx < VK_INSTANCE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_instance_extensions[idx].extensionName,
                    props[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_INSTANCE_EXTENSION_COUNT)
         continue;

      exts->extensions[idx] = true;
   }

   /* The wrapper's command buffers / queues are not the driver's, so debug
    * utils can't simply be forwarded. */
   exts->EXT_debug_utils = false;

   supported_instance_extensions = exts;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateInstanceVersion(uint32_t* pApiVersion)
{
   if (!vulkan_library_init())
      return vk_error(NULL, VK_ERROR_INCOMPATIBLE_DRIVER);

   if (!enumerate_instance_version) {
      *pApiVersion = VK_API_VERSION_1_0;
      return VK_SUCCESS;
   }

   return enumerate_instance_version(pApiVersion);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateInstanceExtensionProperties(const char* pLayerName,
                                             uint32_t* pPropertyCount,
                                             VkExtensionProperties* pProperties)
{
   VkResult result = wrapper_vulkan_init();
   if (result != VK_SUCCESS)
      return vk_error(NULL, result);

   return vk_enumerate_instance_extension_properties(supported_instance_extensions,
                                                     pPropertyCount,
                                                     pProperties);
}

static void
add_extension(const char **exts, uint32_t *count, const char *name)
{
   for (uint32_t i = 0; i < *count; i++) {
      if (!strcmp(exts[i], name))
         return;
   }
   assert(*count < VK_INSTANCE_EXTENSION_COUNT);
   exts[(*count)++] = name;
}

static void
set_wrapper_required_extensions(uint32_t *enable_extension_count,
                                const char **enable_extensions)
{
#define REQUIRED_EXTENSION(name) \
   if (supported_instance_extensions->name) \
      add_extension(enable_extensions, enable_extension_count, "VK_" #name);
   REQUIRED_EXTENSION(KHR_get_physical_device_properties2);
   REQUIRED_EXTENSION(KHR_surface);
   REQUIRED_EXTENSION(KHR_external_fence_capabilities);
   REQUIRED_EXTENSION(KHR_external_memory_capabilities);
   REQUIRED_EXTENSION(KHR_external_semaphore_capabilities);
#undef REQUIRED_EXTENSION
}

/* Enables the debug layers requested through WRAPPER_LOG_LEVEL.  A layer
 * that can't be found is skipped: a game running without validation beats
 * one that doesn't start. */
static void
setup_debug_layers(VkInstanceCreateInfo *create_info,
                   VkLayerSettingsCreateInfoEXT *layer_settings_info)
{
   static const char *enabled_layers[2];
   static const VkBool32 enable = VK_TRUE;
   static const VkLayerSettingEXT layer_settings[] = {
      { WRAPPER_VALIDATION_LAYER, "validate_best_practices", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enable },
      { WRAPPER_VALIDATION_LAYER, "validate_best_practices_arm", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enable },
   };
   bool have_validation = false, have_api_dump = false;
   uint32_t enabled_layer_count = 0;

   if (has_intercepted_layer_paths && enumerate_instance_layer_properties) {
      uint32_t layer_count = 0;
      enumerate_instance_layer_properties(&layer_count, NULL);
      if (layer_count) {
         VkLayerProperties *layer_props = calloc(layer_count, sizeof(*layer_props));
         if (layer_props) {
            enumerate_instance_layer_properties(&layer_count, layer_props);
            for (uint32_t i = 0; i < layer_count; i++) {
               if (!strcmp(layer_props[i].layerName, WRAPPER_VALIDATION_LAYER))
                  have_validation = true;
               if (!strcmp(layer_props[i].layerName, WRAPPER_API_DUMP_LAYER))
                  have_api_dump = true;
            }
            free(layer_props);
         }
      }
   }

   if (WRAPPER_LOG_LEVEL(validation)) {
      if (have_validation)
         enabled_layers[enabled_layer_count++] = WRAPPER_VALIDATION_LAYER;
      else
         fprintf(stderr, "wrapper: validation layer requested but not found "
                 "(layer paths %s), continuing without it\n",
                 has_intercepted_layer_paths ? "set" : "NOT set");
   }

   if (WRAPPER_LOG_LEVEL(apidump)) {
      if (have_api_dump)
         enabled_layers[enabled_layer_count++] = WRAPPER_API_DUMP_LAYER;
      else
         fprintf(stderr, "wrapper: api_dump layer requested but not found, "
                 "continuing without it\n");
   }

   if (enabled_layer_count == 0)
      return;

   create_info->enabledLayerCount = enabled_layer_count;
   create_info->ppEnabledLayerNames = enabled_layers;

   if (have_validation && WRAPPER_LOG_LEVEL(validation)) {
      *layer_settings_info = (VkLayerSettingsCreateInfoEXT) {
         .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
         .pNext = create_info->pNext,
         .settingCount = ARRAY_SIZE(layer_settings),
         .pSettings = layer_settings,
      };
      create_info->pNext = layer_settings_info;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkInstance *pInstance)
{
   const char *wrapper_enable_extensions[VK_INSTANCE_EXTENSION_COUNT];
   uint32_t wrapper_enable_extension_count = 0;
   VkApplicationInfo wrapper_application_info = { 0 };
   VkInstanceCreateInfo wrapper_create_info = *pCreateInfo;
   VkLayerSettingsCreateInfoEXT layer_settings_info;
   struct vk_instance_dispatch_table dispatch_table;
   struct wrapper_instance *instance;
   VkResult result;

   result = wrapper_vulkan_init();
   if (result != VK_SUCCESS)
      return vk_error(NULL, result);

   instance = vk_zalloc2(vk_default_allocator(), pAllocator, sizeof(*instance),
                         8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk, supported_instance_extensions,
                             &dispatch_table, pCreateInfo,
                             pAllocator ? pAllocator : vk_default_allocator());
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to init instance, res %d", result);
      vk_free2(vk_default_allocator(), pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->vk.physical_devices.enumerate = enumerate_physical_device;
   instance->vk.physical_devices.destroy = destroy_physical_device;

   /* Forward the extensions the application enabled, except the ones the
    * wrapper implements on its own (surfaces, handled by the common WSI). */
   for (int idx = 0; idx < VK_INSTANCE_EXTENSION_COUNT; idx++) {
      if (!instance->vk.enabled_extensions.extensions[idx])
         continue;

      if (wrapper_instance_extensions.extensions[idx])
         continue;

      add_extension(wrapper_enable_extensions, &wrapper_enable_extension_count,
                    vk_instance_extensions[idx].extensionName);
   }

   set_wrapper_required_extensions(&wrapper_enable_extension_count,
                                   wrapper_enable_extensions);

   if (WRAPPER_LOG_LEVEL(validation) &&
       supported_instance_extensions->EXT_layer_settings)
      add_extension(wrapper_enable_extensions, &wrapper_enable_extension_count,
                    "VK_EXT_layer_settings");

   if (wrapper_create_info.pApplicationInfo) {
      wrapper_application_info = *wrapper_create_info.pApplicationInfo;
   } else {
      wrapper_application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
      wrapper_application_info.pApplicationName = "wrapper";
      wrapper_application_info.pEngineName = "wrapper";
      wrapper_application_info.apiVersion = VK_API_VERSION_1_0;
      if (enumerate_instance_version)
         enumerate_instance_version(&wrapper_application_info.apiVersion);
   }
   wrapper_create_info.pApplicationInfo = &wrapper_application_info;

   if (WRAPPER_LOG_LEVEL(validation) || WRAPPER_LOG_LEVEL(apidump))
      setup_debug_layers(&wrapper_create_info, &layer_settings_info);

   /* The validation layer provides VK_EXT_debug_utils for the messenger. */
   for (uint32_t i = 0; i < wrapper_create_info.enabledLayerCount; i++) {
      if (!strcmp(wrapper_create_info.ppEnabledLayerNames[i], WRAPPER_VALIDATION_LAYER))
         add_extension(wrapper_enable_extensions, &wrapper_enable_extension_count,
                       "VK_EXT_debug_utils");
   }

   wrapper_create_info.enabledExtensionCount = wrapper_enable_extension_count;
   wrapper_create_info.ppEnabledExtensionNames = wrapper_enable_extensions;

   result = create_instance(&wrapper_create_info, pAllocator,
                            &instance->dispatch_handle);
   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed driver createInstance, res %d", result);
      vk_instance_finish(&instance->vk);
      vk_free2(vk_default_allocator(), pAllocator, instance);
      return vk_error(NULL, result);
   }

   if (WRAPPER_LOG_LEVEL(validation))
      init_debug_messenger(instance->dispatch_handle);

   vk_instance_dispatch_table_load(&instance->dispatch_table,
                                   get_instance_proc_addr,
                                   instance->dispatch_handle);

   *pInstance = wrapper_instance_to_handle(instance);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyInstance(VkInstance _instance,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_instance, instance, _instance);

   if (!instance)
      return;

   VkInstance dispatch_handle = instance->dispatch_handle;
   PFN_vkDestroyInstance destroy_instance = instance->dispatch_table.DestroyInstance;

   if (destroy_debug_utils_messenger && debug_utils_messenger != VK_NULL_HANDLE) {
      destroy_debug_utils_messenger(dispatch_handle, debug_utils_messenger, NULL);
      debug_utils_messenger = VK_NULL_HANDLE;
   }

   /* Destroys the physical devices too (destroy_physical_device()) */
   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);

   if (destroy_instance)
      destroy_instance(dispatch_handle, pAllocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_GetInstanceProcAddr(VkInstance _instance,
                            const char *pName)
{
   VK_FROM_HANDLE(wrapper_instance, instance, _instance);
   return vk_instance_get_proc_addr(instance ? &instance->vk : NULL,
                                    &wrapper_instance_entrypoints,
                                    pName);
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance,
                          const char *pName);

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance,
                          const char *pName)
{
   return wrapper_GetInstanceProcAddr(instance, pName);
}
