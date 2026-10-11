#!/usr/bin/env bash
# Build libvulkan_wrapper.so (aarch64, Termux sysroot) and package it as a
# Winlator/WinLite graphics driver archive (wrapper.tzst).
#
# Works both in GitHub Actions and locally on any x86_64 Linux host.
#
# Environment:
#   ANDROID_NDK_HOME  NDK root (default: download r27c into $WORK_DIR)
#   BUILD_TYPE        meson buildtype (default: release)
#   BUILD_DIR         meson build dir (default: build-wrapper)
#   OUT_DIR           where the results go (default: out)
#   WORK_DIR          downloads / caches (default: .wrapper-cache)
#   TERMUX_PREFIX     Termux sysroot (default: /data/data/com.termux/files/usr)
#   SKIP_DEPS=1       don't download the NDK / Termux packages

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$SRC_DIR"

BUILD_TYPE="${BUILD_TYPE:-release}"
BUILD_DIR="${BUILD_DIR:-build-wrapper}"
OUT_DIR="${OUT_DIR:-out}"
WORK_DIR="${WORK_DIR:-$SRC_DIR/.wrapper-cache}"
TERMUX_PREFIX="${TERMUX_PREFIX:-/data/data/com.termux/files/usr}"
NDK_VERSION="${NDK_VERSION:-r27c}"
API_LEVEL="${API_LEVEL:-30}"
TERMUX_REPO="${TERMUX_REPO:-https://packages-cf.termux.dev/apt/termux-main}"

TERMUX_PACKAGES=(
  libandroid-shmem libdrm
  libxcb libx11 libxau libxdmcp libxext libxfixes libxrandr libxrender
  libxshmfence libxxf86vm xorgproto libexpat zlib
)

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

mkdir -p "$WORK_DIR" "$OUT_DIR"

log() { echo "::group::$*" 2>/dev/null || true; echo "==> $*"; }
endlog() { echo "::endgroup::" 2>/dev/null || true; }

# --------------------------------------------------------------------- NDK
if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "${ANDROID_NDK_HOME}" ]; then
  ANDROID_NDK_HOME="$WORK_DIR/android-ndk-$NDK_VERSION"
  if [ ! -d "$ANDROID_NDK_HOME" ]; then
    [ "${SKIP_DEPS:-0}" = 1 ] && { echo "NDK not found"; exit 1; }
    log "Downloading NDK $NDK_VERSION"
    curl -fL --retry 3 -o "$WORK_DIR/ndk.zip" \
      "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
    unzip -q "$WORK_DIR/ndk.zip" -d "$WORK_DIR"
    rm -f "$WORK_DIR/ndk.zip"
    endlog
  fi
fi
NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -x "$NDK_BIN/aarch64-linux-android$API_LEVEL-clang" ] || {
  echo "Bad NDK: $ANDROID_NDK_HOME"; exit 1; }

# ----------------------------------------------------------- Termux sysroot
if [ "${SKIP_DEPS:-0}" != 1 ] && [ ! -f "$TERMUX_PREFIX/lib/pkgconfig/xcb-dri3.pc" ]; then
  log "Fetching Termux packages"
  DEB_DIR="$WORK_DIR/debs"
  mkdir -p "$DEB_DIR"
  curl -fsSL --retry 3 "$TERMUX_REPO/dists/stable/main/binary-aarch64/Packages" \
    -o "$DEB_DIR/Packages"
  for pkg in "${TERMUX_PACKAGES[@]}"; do
    path=$(awk -v p="Package: $pkg" '$0==p{f=1} f&&/^Filename:/{print $2; exit}' \
      "$DEB_DIR/Packages")
    [ -n "$path" ] || { echo "Termux package not found: $pkg"; exit 1; }
    deb="$DEB_DIR/$(basename "$path")"
    [ -f "$deb" ] || curl -fsSL --retry 3 -o "$deb" "$TERMUX_REPO/$path"
    $SUDO dpkg-deb -x "$deb" /
  done
  # Termux packages ship everything as 0700/0600 (files belong to the app
  # uid on the device); make the sysroot readable for a non-root build user.
  $SUDO chmod -R u+rwX,go+rX /data/data/com.termux
  [ -n "$SUDO" ] && $SUDO chown -R "$(id -u):$(id -g)" /data/data/com.termux
  endlog
fi
[ -f "$TERMUX_PREFIX/lib/pkgconfig/xcb-dri3.pc" ] || {
  echo "Termux sysroot is missing at $TERMUX_PREFIX"; exit 1; }

# -------------------------------------------------------------- cross file
CROSS="$WORK_DIR/cross_termux.txt"
cat > "$CROSS" <<EOF
[binaries]
c = '$NDK_BIN/aarch64-linux-android$API_LEVEL-clang'
cpp = '$NDK_BIN/aarch64-linux-android$API_LEVEL-clang++'
ar = '$NDK_BIN/llvm-ar'
strip = '$NDK_BIN/llvm-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$TERMUX_PREFIX/lib/pkgconfig:$TERMUX_PREFIX/share/pkgconfig'
needs_exe_wrapper = true

[built-in options]
c_args = ['-D__TERMUX__', '-D__USE_GNU', '-U__ANDROID__', '-I$TERMUX_PREFIX/include', '-include', 'fcntl.h', '-include', 'unistd.h']
cpp_args = ['-D__TERMUX__', '-D__USE_GNU', '-U__ANDROID__', '-I$TERMUX_PREFIX/include', '-include', 'fcntl.h', '-include', 'unistd.h']
c_link_args = ['-L$TERMUX_PREFIX/lib', '-landroid-shmem']
cpp_link_args = ['-L$TERMUX_PREFIX/lib', '-landroid-shmem']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

# ------------------------------------------------------------------- build
log "Configuring ($BUILD_TYPE)"
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  meson setup "$BUILD_DIR" --cross-file "$CROSS" \
    -Dbuildtype="$BUILD_TYPE" \
    -Db_ndebug=true \
    -Dcpp_rtti=false \
    -Dgbm=disabled \
    -Dopengl=false \
    -Dllvm=disabled \
    -Dshared-llvm=disabled \
    -Dplatforms=x11 \
    -Dgallium-drivers= \
    -Dxmlconfig=disabled \
    -Dvulkan-drivers=wrapper
fi
endlog

log "Building"
ninja -C "$BUILD_DIR" \
  src/vulkan/wrapper/libvulkan_wrapper.so \
  subprojects/libadrenotools/libadrenotools.so \
  subprojects/libadrenotools/src/hook/libmain_hook.so \
  subprojects/libadrenotools/src/hook/libfile_redirect_hook.so \
  subprojects/libadrenotools/src/hook/libgsl_alloc_hook.so \
  subprojects/libadrenotools/src/hook/libhook_impl.so \
  src/vulkan/wrapper/wrapper_icd.armv8-a.json
endlog

# ----------------------------------------------------------------- package
log "Packaging"
PKG="$WORK_DIR/pkg"
rm -rf "$PKG"
mkdir -p "$PKG/usr/lib" "$PKG/usr/share/vulkan/icd.d"

WRAPPER_SO="$BUILD_DIR/src/vulkan/wrapper/libvulkan_wrapper.so"
cp "$WRAPPER_SO" "$OUT_DIR/libvulkan_wrapper.so.unstripped"

for so in \
  "$WRAPPER_SO" \
  "$BUILD_DIR/subprojects/libadrenotools/libadrenotools.so" \
  "$BUILD_DIR"/subprojects/libadrenotools/src/hook/lib*_hook.so \
  "$BUILD_DIR/subprojects/libadrenotools/src/hook/libhook_impl.so"; do
  "$NDK_BIN/llvm-strip" --strip-unneeded -o "$PKG/usr/lib/$(basename "$so")" "$so"
done

# The emulator loads the ICD from imagefs/usr/share/vulkan/icd.d/
# wrapper_icd.aarch64.json and resolves the library by name through
# LD_LIBRARY_PATH, so write a manifest with a bare library_path.
API_VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ICD"]["api_version"])' \
  "$BUILD_DIR/src/vulkan/wrapper/wrapper_icd.armv8-a.json")
cat > "$PKG/usr/share/vulkan/icd.d/wrapper_icd.aarch64.json" <<EOF
{
    "ICD": {
        "api_version": "$API_VERSION",
        "library_path": "libvulkan_wrapper.so"
    },
    "file_format_version": "1.0.0"
}
EOF

tar -C "$PKG" -cf - usr | zstd -19 -T0 -q -f -o "$OUT_DIR/wrapper.tzst"
cp "$PKG/usr/lib/libvulkan_wrapper.so" "$OUT_DIR/libvulkan_wrapper.so"
endlog

# ------------------------------------------------------------------ checks
log "Checks"
echo "NEEDED:"
"$NDK_BIN/llvm-readelf" -d "$OUT_DIR/libvulkan_wrapper.so" | awk '/NEEDED/{print "  " $NF}'
for sym in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion; do
  "$NDK_BIN/llvm-nm" -D --defined-only "$OUT_DIR/libvulkan_wrapper.so" | grep -q " $sym$" \
    || { echo "missing export: $sym"; exit 1; }
done
echo "Exports OK"
ls -l "$OUT_DIR"
endlog
