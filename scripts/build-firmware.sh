#!/bin/bash
# Build the OpenWrt sysupgrade + factory firmware image using the official
# OpenWrt ImageBuilder.
#
# Default target: Linksys EA6350v3 (ipq40xx/generic, arm_cortex-a7_neon-vfpv4).
# Other devices supported by overriding OPENWRT_VERSION / TARGET_PATH / PROFILE
# from the build-firmware.yml workflow inputs (see README "Build selection").
#
# Required inputs (all overridable from the env / workflow_dispatch):
#   OPENWRT_VERSION   e.g. 25.12.3 (https://downloads.openwrt.org/releases/)
#   TARGET_PATH       e.g. ipq40xx/generic, ramips/mt7621, mediatek/filogic
#   PROFILE           e.g. linksys_ea6350v3 (must match `make info` of the IB)
#
# The same image supports BOTH the Fibocom L850-GL (internal mPCIe/M.2) and
# the Foxconn T99W175 (USB 05c6:90d5 / 05c6:9025) eSIM modems by way of:
#   - QModem as the modem manager (ModemManager intentionally NOT installed)
#   - lpac built from the user-supplied L850-GL-patched source tarball
#     (packages/lpac-overlay/lpac-l850gl-source.tar.gz) so MBIM proxy /
#     custom_isd_r_aid / mbim_skip_slot_mapping defaults are correct for
#     both modems
#   - files/etc/uci-defaults/97-multi-modem-defaults seeding lpac-esim UCI
#   - files/etc/hotplug.d/usb/10-t99w175-serial binding T99W175 USB ports
#
# Custom packages (QModem core + LuCI apps, patched lpac, luci-app-lpac-
# manager, 0xygen-aio, libcurl with IMAP, etc.) are read from
# packages/prebuilt/ (.apk / .ipk files committed to the repository). To
# refresh them, run the build-packages.yml workflow manually, download the
# artifact, and commit the resulting files.
set -euxo pipefail

OPENWRT_VERSION="${OPENWRT_VERSION:-25.12.3}"
TARGET_PATH="${TARGET_PATH:-ipq40xx/generic}"
PROFILE="${PROFILE:-linksys_ea6350v3}"

# Convert TARGET_PATH (e.g. ipq40xx/generic) into the dash-joined form
# (ipq40xx-generic) used by the ImageBuilder tarball name.
TARGET_SLUG="${TARGET_PATH//\//-}"

REPO_ROOT="${GITHUB_WORKSPACE:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_ROOT="${BUILD_ROOT:-${REPO_ROOT}/build}"
DIST_ROOT="${REPO_ROOT}/dist"
PREBUILT_DIR="${PREBUILT_DIR:-${REPO_ROOT}/packages/prebuilt}"

# Ookla Speedtest CLI binary (32-bit ARM hardfloat = arm_cortex-a7_neon-vfpv4).
# Pinned URL + sha256 for reproducible builds. See:
#   https://www.speedtest.net/apps/cli
#   https://radenku.com/cara-install-speedtest-cli-openwrt/
OOKLA_SPEEDTEST_URL="${OOKLA_SPEEDTEST_URL:-https://install.speedtest.net/app/cli/ookla-speedtest-1.2.0-linux-armhf.tgz}"
OOKLA_SPEEDTEST_SHA256="${OOKLA_SPEEDTEST_SHA256:-e45fcdebbd8a185553535533dd032d6b10bc8c64eee4139b1147b9c09835d08d}"

mkdir -p "${BUILD_ROOT}" "${DIST_ROOT}/firmware"
cd "${BUILD_ROOT}"

IB_BASENAME="openwrt-imagebuilder-${OPENWRT_VERSION}-${TARGET_SLUG}.Linux-x86_64"
IB_TARBALL="${IB_BASENAME}.tar.zst"
IB_URL="https://downloads.openwrt.org/releases/${OPENWRT_VERSION}/targets/${TARGET_PATH}/${IB_TARBALL}"

if [ ! -d "${IB_BASENAME}" ]; then
    if [ ! -f "${IB_TARBALL}" ]; then
        curl -fSL --retry 5 --retry-delay 2 -o "${IB_TARBALL}" "${IB_URL}"
    fi
    tar --use-compress-program=unzstd -xf "${IB_TARBALL}"
fi

cd "${IB_BASENAME}"

# Add custom packages committed to packages/prebuilt/
mkdir -p packages
if compgen -G "${PREBUILT_DIR}/*.apk" > /dev/null; then
    cp -v "${PREBUILT_DIR}/"*.apk packages/
fi
if compgen -G "${PREBUILT_DIR}/*.ipk" > /dev/null; then
    cp -v "${PREBUILT_DIR}/"*.ipk packages/
fi

ls -la packages/ || true

# Stage uci-defaults / overlay files
rm -rf files
mkdir -p files
cp -av "${REPO_ROOT}/files/." files/
find files -type f -name '99-*' -exec chmod 0755 {} \;
find files/etc/uci-defaults -type f -exec chmod 0755 {} \; 2>/dev/null || true
find files/etc/init.d         -type f -exec chmod 0755 {} \; 2>/dev/null || true
[ -f files/usr/bin/speedtest ] && chmod 0755 files/usr/bin/speedtest
[ -f files/usr/bin/esim ] && chmod 0755 files/usr/bin/esim
[ -f files/etc/init.d/esim-telegram-bot ] && chmod 0755 files/etc/init.d/esim-telegram-bot

# Build the lightweight Telegram bridge as a native target binary with the SDK
# toolchain. The binary shells out to existing OpenWrt tools (curl/jq/lpac-esim)
# instead of embedding heavy dependencies.
if [ -f "${REPO_ROOT}/src/esim-telegram-bot.c" ]; then
    mkdir -p files/usr/bin
    SDK_IMAGE="openwrt/sdk:${SDK_ARCH:-arm_cortex-a7_neon-vfpv4}-${OPENWRT_VERSION}"
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "${REPO_ROOT}:/work:ro" \
        -v "${PWD}/files/usr/bin:/out" \
        "${SDK_IMAGE}" \
        /bin/sh -c 'export STAGING_DIR=/builder/staging_dir; CC="$(find "$STAGING_DIR" -type f -name "*-openwrt-linux-*-gcc" | head -n1)"; [ -n "$CC" ] && "$CC" -Wall -Wextra -Os -s -o /out/esim-telegram-bot /work/src/esim-telegram-bot.c'
    chmod 0755 files/usr/bin/esim-telegram-bot
    ls -lh files/usr/bin/esim-telegram-bot
fi

# Download Ookla Speedtest CLI armhf binary and stage it into the overlay
# at /usr/libexec/ookla-speedtest. The wrapper at /usr/bin/speedtest (also
# in the overlay) calls it with --accept-license --accept-gdpr.
SPEEDTEST_TGZ="${BUILD_ROOT}/ookla-speedtest-armhf.tgz"
if [ ! -f "${SPEEDTEST_TGZ}" ]; then
    curl -fSL --retry 5 --retry-delay 2 -o "${SPEEDTEST_TGZ}" "${OOKLA_SPEEDTEST_URL}"
fi
echo "${OOKLA_SPEEDTEST_SHA256}  ${SPEEDTEST_TGZ}" | sha256sum -c -

mkdir -p files/usr/libexec
tar -xzf "${SPEEDTEST_TGZ}" -C files/usr/libexec speedtest
mv -f files/usr/libexec/speedtest files/usr/libexec/ookla-speedtest
chmod 0755 files/usr/libexec/ookla-speedtest

# Convert the package list (ignoring comments / blank lines) into a flat list
PACKAGES="$(grep -v -E '^[[:space:]]*(#|$)' "${REPO_ROOT}/config/packages.list" | tr '\n' ' ')"

echo "==== Building image with packages: ===="
echo "${PACKAGES}"

# Build factory + sysupgrade images
make image \
    PROFILE="${PROFILE}" \
    PACKAGES="${PACKAGES}" \
    FILES="files/"

# Copy outputs into dist/firmware/
find bin/targets -type f \( \
        -name "*${PROFILE}*sysupgrade*" \
     -o -name "*${PROFILE}*factory*" \
     -o -name "sha256sums" \
     -o -name "profiles.json" \
    \) -print -exec cp -f {} "${DIST_ROOT}/firmware/" \;

ls -la "${DIST_ROOT}/firmware/"
echo "Firmware build complete for ${PROFILE} on OpenWrt ${OPENWRT_VERSION}."
