# Vulkan wrapper

A layered Vulkan ICD for Android: it loads the real Vulkan driver (the
system one, or a custom one through libadrenotools) and adds the common Mesa
WSI on top, so X11 surfaces work in the emulator's X server.

Ported from the 25.0-based mesa-wrapper / GameNative wrapper to Mesa 26.2.4,
keeping only the generic parts and the Adreno (Qualcomm proprietary driver)
workarounds.

## Build

CI: `.github/workflows/wrapper.yml` (GitHub Actions, manual run, push or a
`wrapper-*` tag for a release).  It calls `.github/wrapper/build.sh`, which
also works locally on x86_64 Linux:

```
./.github/wrapper/build.sh            # downloads NDK r27c + Termux packages
ANDROID_NDK_HOME=/path/to/ndk BUILD_TYPE=debugoptimized ./.github/wrapper/build.sh
```

Host packages: ninja, pkg-config, bison, flex, glslang, zstd, unzip, python3
with meson, mako, pyyaml, packaging.  The Termux sysroot is unpacked to
`/data/data/com.termux/files/usr` (needs sudo).

Results in `out/`:

* `wrapper.tzst` - drop-in driver package (`usr/lib/*.so` +
  `usr/share/vulkan/icd.d/wrapper_icd.aarch64.json`), same layout as the
  emulator's `graphics_driver/wrapper.tzst`;
* `libvulkan_wrapper.so` (stripped) and `libvulkan_wrapper.so.unstripped`.

Manual meson invocation:

```
meson setup build --cross-file <android cross file, with -D__TERMUX__> \
    -Dcpp_rtti=false -Dgbm=disabled -Dopengl=false -Dllvm=disabled \
    -Dshared-llvm=disabled -Dplatforms=x11 -Dgallium-drivers= \
    -Dxmlconfig=disabled -Dvulkan-drivers=wrapper
ninja -C build src/vulkan/wrapper/libvulkan_wrapper.so
```

The ICD manifest meson generates has an absolute `library_path`; the
emulator needs `wrapper_icd.aarch64.json` with a bare `libvulkan_wrapper.so`,
which `build.sh` writes.

`-Dvulkan-drivers=wrapper` turns on the Android stubs, the
`VK_USE_PLATFORM_ANDROID_KHR` and `VK_WSI_ANDROID_AHB` defines and pulls
libadrenotools as a subproject.  The adrenotools hook libraries
(`libmain_hook.so`, `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`,
`libhook_impl.so`) and `libadrenotools.so` must be shipped next to
`libvulkan_wrapper.so`.

## WSI changes (VK_WSI_ANDROID_AHB)

The emulator's X server has no XFIXES and no xshmfence, and only imports
AHardwareBuffers:

* swapchain images are AHardwareBuffers (`WSI_IMAGE_TYPE_ANDROID`,
  `wsi_common_android.c`), passed to DRI3 PixmapFromBuffers with modifier
  1255 through a socketpair (`AHardwareBuffer_sendHandleToUnixSocket`, the
  server acks with one byte);
* idle fences are plain XSync fences;
* XFIXES is optional (damage regions are skipped without it);
* only one channel order is exposed (`WRAPPER_SURFACE_FORMAT`);
* `_MESA_DRV*` window properties instead of `_VARIABLE_REFRESH`.

## Environment variables

| Variable | Meaning |
|---|---|
| `ADRENOTOOLS_DRIVER_PATH`, `ADRENOTOOLS_DRIVER_NAME`, `ADRENOTOOLS_HOOKS_PATH`, `ADRENOTOOLS_REDIRECT_DIR` | custom driver loading |
| `WRAPPER_SURFACE_FORMAT` | `bgra8` (default) or `rgba8` |
| `WRAPPER_BLIT` / `MESA_VK_WSI_DEBUG=blit` | render to an intermediate image and blit |
| `MESA_VK_WSI_DEBUG=forcesync` | wait for the image fence before each present |
| `WRAPPER_MAX_IMAGE_COUNT` | pin the swapchain length |
| `WRAPPER_DISABLE_PLACED` | don't emulate VK_EXT_map_memory_placed |
| `WRAPPER_RESOURCE_TYPE` | `auto`, `dmabuf`, `ahb`, `opaque`: backing of placed allocations |
| `WRAPPER_DMAHEAP_CACHED` | use the cached dma-buf heap |
| `WRAPPER_DISABLE_PRESENT_WAIT` | hide VK_KHR_present_wait(2) |
| `WRAPPER_EXTENSION_BLACKLIST` | comma separated device extensions to hide |
| `WRAPPER_VK_VERSION`, `WRAPPER_DEVICE_NAME`, `WRAPPER_DEVICE_ID`, `WRAPPER_VENDOR_ID`, `WRAPPER_DRIVER_ID` | spoof device properties |
| `WRAPPER_VMEM_MAX_SIZE` | override the size of memory heap 0 (MiB) |
| `WRAPPER_MEMORY_BUDGET` | emulate `VK_EXT_memory_budget` when the driver lacks it (default on) |
| `WRAPPER_MEMORY_BUDGET_RESERVE` | memory (MiB) kept out of the emulated budget for the system, default 1024 |
| `WRAPPER_SUBGROUP_OPS` | `1`/`0`: report or hide the driver's subgroup operations (default: reported on Adreno — Turnip and the Qualcomm driver — hidden elsewhere) |
| `WRAPPER_SAFE_CREATE_DEVICE` | retry vkCreateDevice with an empty pNext (default on) |
| `WRAPPER_DEVICE_FAULT` | enable VK_EXT_device_fault reports on device loss |
| `WRAPPER_LOG_LEVEL` | `info,error,shader,validation,apidump` |
| `WRAPPER_LOG_FILE` | log file path, or `stdout` / `stderr` |
| `WRAPPER_LAYER_PATH` | where the validation / api_dump layers are |
