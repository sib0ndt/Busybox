#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <busybox-version> [output-dir]" >&2
  exit 1
fi

BUSYBOX_VERSION="$1"
OUTPUT_DIR="${2:-$PWD/release}"
TARGET_KERNEL_ARCH="${TARGET_KERNEL_ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-}"
UBOOT_ARCH="${UBOOT_ARCH:-arm64}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
ROOTFS_DIR="${WORK_DIR}/rootfs"
BUSYBOX_ARCHIVE="${WORK_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"
MAKE_ARGS=("ARCH=${TARGET_KERNEL_ARCH}")

if [[ -n "${CROSS_COMPILE}" ]]; then
  MAKE_ARGS+=("CROSS_COMPILE=${CROSS_COMPILE}")
fi

cleanup() {
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

mkdir -p "${OUTPUT_DIR}" "${ROOTFS_DIR}"

echo "[1/7] Downloading BusyBox ${BUSYBOX_VERSION}"
curl -fsSL "${BUSYBOX_URL}" -o "${BUSYBOX_ARCHIVE}"

echo "[2/7] Extracting source"
tar -xjf "${BUSYBOX_ARCHIVE}" -C "${WORK_DIR}"
BUSYBOX_SRC="${WORK_DIR}/busybox-${BUSYBOX_VERSION}"

echo "[3/7] Copying ramdisk template from repository"
rsync -a \
  --exclude='.git' \
  --exclude='.github' \
  --exclude='release' \
  --exclude='scripts/ci' \
  "${REPO_ROOT}/" "${ROOTFS_DIR}/"

if [[ ! -f "${ROOTFS_DIR}/init" ]]; then
  echo "Template init file was not found at ${ROOTFS_DIR}/init" >&2
  exit 1
fi

# Keep the repository's init and scripts, but refresh BusyBox applet trees.
rm -rf "${ROOTFS_DIR}/bin" "${ROOTFS_DIR}/sbin" "${ROOTFS_DIR}/usr/bin" "${ROOTFS_DIR}/usr/sbin"
mkdir -p "${ROOTFS_DIR}/bin" "${ROOTFS_DIR}/sbin" "${ROOTFS_DIR}/usr/bin" "${ROOTFS_DIR}/usr/sbin"

echo "[4/7] Building BusyBox (static)"
make -C "${BUSYBOX_SRC}" "${MAKE_ARGS[@]}" defconfig
if grep -q '^# CONFIG_STATIC is not set' "${BUSYBOX_SRC}/.config"; then
  sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "${BUSYBOX_SRC}/.config"
elif ! grep -q '^CONFIG_STATIC=y' "${BUSYBOX_SRC}/.config"; then
  echo 'CONFIG_STATIC=y' >> "${BUSYBOX_SRC}/.config"
fi
if make -C "${BUSYBOX_SRC}" "${MAKE_ARGS[@]}" olddefconfig >/dev/null 2>&1; then
  echo "Applied BusyBox olddefconfig"
else
  echo "olddefconfig is unavailable; using non-interactive oldconfig"
  set +o pipefail
  yes "" | make -C "${BUSYBOX_SRC}" "${MAKE_ARGS[@]}" oldconfig
  set -o pipefail
fi
make -C "${BUSYBOX_SRC}" "${MAKE_ARGS[@]}" -j"$(nproc)"

echo "[5/7] Installing BusyBox applets into rootfs"
make -C "${BUSYBOX_SRC}" "${MAKE_ARGS[@]}" CONFIG_PREFIX="${ROOTFS_DIR}" install
chmod 0755 "${ROOTFS_DIR}/init"

echo "[6/7] Creating initramfs and uInitrd"
(
  cd "${ROOTFS_DIR}"
  find . -print0 | LC_ALL=C sort -z | cpio --null --create --format=newc --owner=0:0 > "${OUTPUT_DIR}/initramfs"
)
mkimage -n 'Linux' -A "${UBOOT_ARCH}" -O linux -T ramdisk -C none -d "${OUTPUT_DIR}/initramfs" "${OUTPUT_DIR}/uInitrd"

echo "[7/7] Compressing release artifact"
tar -C "${OUTPUT_DIR}" -czf "${OUTPUT_DIR}/uInitrd.tar.gz" uInitrd
sha256sum "${OUTPUT_DIR}/initramfs" "${OUTPUT_DIR}/uInitrd" "${OUTPUT_DIR}/uInitrd.tar.gz" > "${OUTPUT_DIR}/SHA256SUMS"

echo "Build complete: ${OUTPUT_DIR}"
