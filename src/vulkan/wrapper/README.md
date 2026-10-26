# Vulkan wrapper

A layered Vulkan ICD for Android: it loads the real Vulkan driver (the
system one, or a custom one through libadrenotools) and adds the common Mesa
WSI on top, so X11 surfaces work in the emulator's X server.

Ported from the 25.0-based mesa-wrapper / GameNative wrapper to Mesa 26.2.4,
with all its vendor workarounds:

* Adreno (Qualcomm proprietary driver): line rasterization hidden and
  VK_KHR_pipeline_library faked for DXVK, shader float controls hidden,
  globalPriorityQuery off for clvk on 512.744+;
* Mali / MediaTek (ARM proprietary driver): VK_EXT_robustness2,
  dualSrcBlend and multiDrawIndirect faked for DXVK, calibrated timestamps
  and extended dynamic state 1/2 hidden, SPIR-V patches
  (`spirv_patcher.cpp`), RGBA8 surfaces by default;
* BCn textures for drivers without them (`wrapper_bcn.c`): old Adreno
  drivers (< 512.530), Mali, PowerVR, and Xclipse (Exynos) for the formats its
  driver lacks;
* PowerVR: swapchain AHardwareBuffers with CPU_READ_RARELY.

## BCn emulation

`WRAPPER_EMULATE_BCN` selects the mode:

| Value | Mode |
|---|---|
| `0` | off |
| `1` | report the BCn formats as supported, the driver decodes them (hardware that can, driver that doesn't say so) |
| `2` | decompress every BCn format on the CPU |
| `3` | auto (default): per format, native when the driver samples it, decompressed on the CPU otherwise; off on Turnip; mode 1 on the Qualcomm driver >= 512.530 when it hides textureCompressionBC, off when it has it |

Emulated images are created with an uncompressed format (RGBA8 for BC1-3/7,
R8 / RG8 for BC4 / BC5, RGBA16F for BC6H), their format properties are the
driver's for that format.  vkCmdCopyBufferToImage(2) decompresses the source
data into a staging buffer owned by the command buffer (released when it is
begun again, reset or freed).  The data is read when the copy is recorded, so
it must already be in the buffer then.  VK_EXT_host_image_copy is hidden while
formats are emulated.

## SPIR-V patches

On Mali: OpConstantComposite of spec-constant bool vectors becomes
OpSpecConstantComposite (`WRAPPER_NO_PATCH_OPCONSTCOMP=1` disables) and the
ClipDistance / CullDistance builtins are removed
(`WRAPPER_NO_REMOVE_CLIP_DISTANCE=1` disables).  On every driver the builtins
are also removed when the driver lacks shaderClipDistance / shaderCullDistance,
which the wrapper advertises regardless.  Only modules created with
vkCreateShaderModule are patched (not ones chained into pipeline stages).

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
* only one channel order is exposed (`WRAPPER_SURFACE_FORMAT`, R8G8B8A8 by default);
* `_MESA_DRV*` window properties instead of `_VARIABLE_REFRESH`.

## Environment variables

| Variable | Meaning |
|---|---|
| `ADRENOTOOLS_DRIVER_PATH`, `ADRENOTOOLS_DRIVER_NAME`, `ADRENOTOOLS_HOOKS_PATH`, `ADRENOTOOLS_REDIRECT_DIR` | custom driver loading |
| `WRAPPER_SURFACE_FORMAT` | `rgba8` (default) or `bgra8`: channel order of the swapchain images, must match the emulator's buffers |
| `WRAPPER_AHB_UBWC` | `1`: allocate the R8G8B8A8 swapchain images UBWC compressed (Qualcomm gralloc). Only with the proprietary Adreno driver and `rgba8`; ignored otherwise (default: off) |
| `WRAPPER_BLIT` / `MESA_VK_WSI_DEBUG=blit` | render to an intermediate image and blit |
| `MESA_VK_WSI_NO_RENDER_WAIT=1` | X11 WSI: don't wait for the render fence in the present thread before PresentPixmap (upstream behaviour; frames may reach the compositor unfinished) |
| `WRAPPER_MAX_IMAGE_COUNT` | pin the swapchain length |
| `WRAPPER_DISABLE_PLACED` | don't emulate VK_EXT_map_memory_placed |
| `WRAPPER_RESOURCE_TYPE` | `auto`, `dmabuf`, `ahb`, `opaque`: backing of placed allocations |
| `WRAPPER_DMAHEAP_CACHED` | use the cached dma-buf heap |
| `WRAPPER_DISABLE_PRESENT_WAIT` | hide VK_KHR_present_wait(2) |
| `WRAPPER_EXTENSION_BLACKLIST` | comma separated device extensions to hide |
| `WRAPPER_VENDOR_ID`, `WRAPPER_DEVICE_ID` | spoof vendorID / deviceID (decimal or `0x` hex; unset/0 = real value) |
| `WRAPPER_DEVICE_NAME` | spoof deviceName as is (default: `Wrapper(<real name>)`) |
| `WRAPPER_DRIVER_ID` | spoof `VkDriverId` (e.g. `4` = NVIDIA_PROPRIETARY) in VkPhysicalDeviceDriverProperties and VkPhysicalDeviceVulkan12Properties. Note: DXVK / vkd3d-proton select driver specific paths from it |
| `WRAPPER_DRIVER_NAME`, `WRAPPER_DRIVER_INFO` | spoof driverName / driverInfo in both structs (default: `Wrapper driver` / the driver version) |
| `WRAPPER_DRIVER_VERSION` | spoof driverVersion: a raw number, or a dotted version encoded for the reported vendor (NVIDIA 10.8.8.6 bits, `VK_MAKE_VERSION` layout otherwise); a dotted value is also the default driverInfo |
| `WRAPPER_VK_VERSION` | spoof apiVersion (`1.3.280`) |
| `WRAPPER_VMEM_MAX_SIZE` | size (MiB) reported for the first device-local heap; lowering always applies. With Turnip it can't raise the heap (Turnip refuses allocations past its own one), with other drivers it can, up to the larger of the driver's heap and Mesa's GPU heap share of the physical memory (75% above 4 GiB). `heapBudget` and `heapUsage` are clamped to the heap size |
| `WRAPPER_MEMORY_BUDGET` | emulate `VK_EXT_memory_budget` when the driver lacks it, and in place of the Qualcomm driver's own one (default on; `0` keeps the driver's) |
| `WRAPPER_MEMORY_BUDGET_RESERVE` | memory (MiB) kept out of the emulated budget for the system, default 1024 |
| `WRAPPER_SUBGROUP_OPS` | `1`/`0`: report or hide the driver's subgroup operations (default: reported on Adreno — Turnip and the Qualcomm driver — hidden elsewhere) |
| `WRAPPER_SAFE_CREATE_DEVICE` | retry vkCreateDevice with an empty pNext (default on) |
| `WRAPPER_DEVICE_FAULT` | enable VK_EXT_device_fault reports on device loss |
| `WRAPPER_EMULATE_BCN` | BCn mode, see above (default 3) |
| `WRAPPER_USE_BCN_CACHE` | keep decompressed textures in `WRAPPER_CACHE_PATH` |
| `WRAPPER_CACHE_PATH` | directory of the BCn cache |
| `WRAPPER_NO_BCN_THREAD` | decompress on the calling thread only |
| `WRAPPER_MARK_BCN` | fill emulated textures with a per format color (debug) |
| `WRAPPER_NO_PATCH_OPCONSTCOMP` | Mali: skip the OpSpecConstantComposite patch |
| `WRAPPER_NO_REMOVE_CLIP_DISTANCE` | Mali: keep ClipDistance / CullDistance |
| `WRAPPER_LOG_LEVEL` | `info,error,shader,validation,bcn,apidump` |
| `WRAPPER_LOG_FILE` | log file path, or `stdout` / `stderr` |
| `WRAPPER_LAYER_PATH` | where the validation / api_dump layers are |
