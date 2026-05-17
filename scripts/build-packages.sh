#!/bin/bash
# Build custom OpenWrt packages for Linksys EA6350v3 (ipq40xx,
# arm_cortex-a7_neon-vfpv4) using the OpenWrt SDK.
#
# Expects to run inside the
#     openwrt/sdk:arm_cortex-a7_neon-vfpv4-${OPENWRT_VERSION}
# docker image with /builder as the SDK root and /work as a bind mount of the
# checked-out 0xygen-Linksys repository.
#
# What this script produces (under /work/dist/packages):
#   - QModem core + LuCI apps, with the T99W175 (Foxconn) fixes patch applied
#     on top of upstream FUjr/QModem.git. QModem is the primary modem manager
#     for this firmware; ModemManager is intentionally not installed.
#   - lpac (eUICC/eSIM LPA) built from the user-supplied L850-GL-patched
#     source tarball (packages/lpac-overlay/lpac-l850gl-source.tar.gz)
#     instead of upstream estkme-group/lpac. This version carries:
#         100-lpac-mbim-skip-slot-mapping-env.patch
#         110-lpac-mbim-use-extended-class-byte.patch
#         120-lpac-mbim-open-channel-select-p2-0c.patch
#         130-lpac-mbim-append-le00-to-envelope.patch
#     and ships the L850-GL/T99W175 mbim_proxy / custom_isd_r_aid /
#     mbim_skip_slot_mapping defaults that the rest of the firmware expects.
#   - luci-app-lpac-manager + 0xygen-aio (HYFE trial claim, telegram bot)
#   - luci-app-modemband + modemband, luci-app-modemdata + modemdata,
#     luci-app-atinout + atinout
#   - libcurl + curl rebuilt with IMAP/IMAPS/POP3/SMTP support so the HYFE
#     OTP helper (hyfetrial/otp.sh) can poll mail in IMAP mode.
#
# ModemManager is intentionally NOT installed: it owns /dev/cdc-wdm0 and
# stops QModem and lpac from running cleanly when both are present, so we
# drop the dependency at the lpac-manager / 0xygen-aio level.
set -euxo pipefail

cd /builder

WORK="${WORK:-/work}"
SRC="${WORK}/sources"
FEED="${WORK}/custom-feed"
DIST="${WORK}/dist/packages"

mkdir -p "${SRC}" "${FEED}" "${DIST}"

clone_shallow() {
    local url="$1" dest="$2"
    if [ ! -d "${dest}/.git" ]; then
        rm -rf "${dest}"
        git clone --depth 1 "${url}" "${dest}"
    fi
}

# ---------------------------------------------------------------------------
# 1. Clone source repositories
# ---------------------------------------------------------------------------
clone_shallow https://github.com/makungyu/0xygen-AIO.git              "${SRC}/0xygen-AIO"
clone_shallow https://github.com/4IceG/luci-app-modemband.git         "${SRC}/luci-app-modemband"
clone_shallow https://github.com/4IceG/luci-app-modemdata.git         "${SRC}/luci-app-modemdata"
clone_shallow https://github.com/obsy/modemdata.git                   "${SRC}/modemdata-src"
clone_shallow https://github.com/4IceG/luci-app-atinout.git           "${SRC}/luci-app-atinout"
clone_shallow https://github.com/FUjr/QModem.git                      "${SRC}/QModem"

# ---------------------------------------------------------------------------
# 1a. Apply the T99W175 (Foxconn) fixes patch on top of upstream QModem.
# ---------------------------------------------------------------------------
# The patch is committed at patches/qmodem/001-t99w175-foxconn-fixes.patch.
# We mark the source tree with a sentinel file so re-runs of this script
# (e.g. cached docker layer + cached SRC dir) do not try to re-apply it and
# fail with "previously applied".
QMODEM_PATCH="${WORK}/patches/qmodem/001-t99w175-foxconn-fixes.patch"
QMODEM_SENTINEL="${SRC}/QModem/.qmodem_t99w175_applied"
if [ -f "${QMODEM_PATCH}" ] && [ ! -f "${QMODEM_SENTINEL}" ]; then
    echo "==== Applying T99W175 fixes to QModem ===="
    ( cd "${SRC}/QModem" && patch -p1 -N -r /tmp/qmodem-reject.log < "${QMODEM_PATCH}" )
    touch "${QMODEM_SENTINEL}"
fi

# ---------------------------------------------------------------------------
# 2. Stage the user-provided lpac (L850-GL patched) source as a feed package
# ---------------------------------------------------------------------------
# Tarball layout (committed at packages/lpac-overlay/lpac-l850gl-source.tar.gz):
#   lpac-l850gl-source/lpac-l850gl-2.3.0/      <- patched lpac library source
#   lpac-l850gl-source/openwrt-package/        <- Makefile + MBIM patches + files
#   lpac-l850gl-source/tools/                  <- old esim CLI (IGNORED on purpose)
LPAC_OVERLAY_TGZ="${WORK}/packages/lpac-overlay/lpac-l850gl-source.tar.gz"
LPAC_STAGE="${SRC}/lpac-overlay"
if [ ! -f "${LPAC_OVERLAY_TGZ}" ]; then
    echo "ERROR: lpac overlay tarball missing at ${LPAC_OVERLAY_TGZ}" >&2
    exit 1
fi
rm -rf "${LPAC_STAGE}"
mkdir -p "${LPAC_STAGE}"
tar -xzf "${LPAC_OVERLAY_TGZ}" -C "${LPAC_STAGE}"

LPAC_OVERLAY_ROOT="${LPAC_STAGE}/lpac-l850gl-source"
if [ ! -d "${LPAC_OVERLAY_ROOT}/lpac-l850gl-2.3.0" ] || \
   [ ! -d "${LPAC_OVERLAY_ROOT}/openwrt-package" ]; then
    echo "ERROR: unexpected lpac overlay layout under ${LPAC_OVERLAY_ROOT}" >&2
    ls -R "${LPAC_OVERLAY_ROOT}" >&2 || true
    exit 1
fi

# Repackage the patched library source as lpac-2.3.0.tar.gz so the
# openwrt-package Makefile (which pins PKG_SOURCE=lpac-2.3.0.tar.gz +
# extracts ./lpac-2.3.0/...) picks it up via dl/. We rename the top-level
# directory so OpenWrt's standard package.mk extract step lands files in
# the expected $(PKG_BUILD_DIR).
LPAC_REPACK_DIR="${SRC}/lpac-repack"
rm -rf "${LPAC_REPACK_DIR}"
mkdir -p "${LPAC_REPACK_DIR}/lpac-2.3.0"
cp -a "${LPAC_OVERLAY_ROOT}/lpac-l850gl-2.3.0/." "${LPAC_REPACK_DIR}/lpac-2.3.0/"
( cd "${LPAC_REPACK_DIR}" && tar -czf lpac-2.3.0.tar.gz lpac-2.3.0 )
LPAC_HASH="$(sha256sum "${LPAC_REPACK_DIR}/lpac-2.3.0.tar.gz" | awk '{print $1}')"
mkdir -p /builder/dl
cp -f "${LPAC_REPACK_DIR}/lpac-2.3.0.tar.gz" /builder/dl/lpac-2.3.0.tar.gz
echo "lpac-2.3.0.tar.gz (patched) sha256=${LPAC_HASH}"

# Drop openwrt-package/ into our feed as a brand-new lpac package — this
# replaces any upstream lpac that the standard OpenWrt packages feed would
# otherwise provide.
rm -rf "${FEED}/lpac"
mkdir -p "${FEED}/lpac"
cp -a "${LPAC_OVERLAY_ROOT}/openwrt-package/." "${FEED}/lpac/"

# Patch PKG_HASH so OpenWrt's source verification matches our repackaged
# tarball (the upstream tarball would have a different hash). Also point
# PKG_SOURCE_URL at the local dl/ directory so the build never tries to
# reach codeload.github.com — guarantees reproducibility on hosted runners.
sed -i -E "s|^PKG_HASH:=.*|PKG_HASH:=${LPAC_HASH}|" "${FEED}/lpac/Makefile"
sed -i -E 's|^PKG_SOURCE_URL:=.*|PKG_SOURCE_URL:=file:///builder/dl|' "${FEED}/lpac/Makefile"

# ---------------------------------------------------------------------------
# 3. Lay out a flat custom feed of package source directories
# ---------------------------------------------------------------------------
ln -sfn "${SRC}/luci-app-modemband/luci-app-modemband"  "${FEED}/luci-app-modemband"
ln -sfn "${SRC}/luci-app-modemband/modemband/modemband" "${FEED}/modemband"
ln -sfn "${SRC}/luci-app-modemdata/luci-app-modemdata"  "${FEED}/luci-app-modemdata"
ln -sfn "${SRC}/modemdata-src/modemdata"                "${FEED}/modemdata"
ln -sfn "${SRC}/luci-app-atinout/luci-app-atinout"      "${FEED}/luci-app-atinout"
ln -sfn "${SRC}/luci-app-atinout/atinout"               "${FEED}/atinout"

# QModem ships every package under application/<name>/ and luci/<name>/.
# Flat-symlink each subdir that carries a Makefile into our feed so
# `scripts/feeds install` resolves them.
for d in "${SRC}/QModem/application"/* "${SRC}/QModem/luci"/*; do
    [ -f "${d}/Makefile" ] || continue
    ln -sfn "${d}" "${FEED}/$(basename "${d}")"
done

# ---------------------------------------------------------------------------
# 4. Register feeds and update/install them
# ---------------------------------------------------------------------------
# Drop any prior `src-link 0xygen_aio` entry that pointed at the unpatched
# 0xygen-AIO lpac feed — we now ship lpac (and lpac-manager) via the
# `extras` src-link below, built from the patched source we just staged.
sed -i -E '/^src-link[[:space:]]+0xygen_aio[[:space:]]/d' feeds.conf.default || true

if ! grep -q '^src-link 0xygen_aio_app ' feeds.conf.default; then
    echo "src-link 0xygen_aio_app ${SRC}/0xygen-AIO" >> feeds.conf.default
fi
if ! grep -q '^src-link extras ' feeds.conf.default; then
    echo "src-link extras ${FEED}" >> feeds.conf.default
fi

./scripts/feeds update -a

# Our `extras` feed wins over upstream packages because feeds.conf.default
# is read top-to-bottom and our `lpac` directory under custom-feed/ takes
# precedence over the lpac in feeds/packages (which we don't even install
# in the first place).
./scripts/feeds install -p extras \
    lpac \
    luci-app-modemband modemband \
    luci-app-modemdata modemdata \
    luci-app-atinout atinout \
    qmodem qmodem_monitor \
    luci-app-qmodem luci-app-qmodem-next luci-app-qmodem-monitor

# Pull in 0xygen-aio + luci-app-lpac-manager. The upstream 0xygen-AIO repo
# also ships an lpac Makefile, but we deliberately do NOT install its lpac
# here — it would shadow the patched lpac from our extras feed.
./scripts/feeds install -p 0xygen_aio_app luci-app-lpac-manager 0xygen-aio

# Standard feed deps used by the firmware image at install time.
./scripts/feeds install \
    libmbim mbim-utils \
    libcurl curl ca-certificates ca-bundle \
    luci-base luci-compat luci-lua-runtime \
    sms-tool comgt jsonfilter \
    jq wget-ssl libubus-lua

# ---------------------------------------------------------------------------
# 5. Strip ModemManager from package dependency trees
# ---------------------------------------------------------------------------
# `luci-app-lpac-manager`, `0xygen-aio` and a handful of QModem LuCI apps
# declare `modemmanager` as a soft dep. We don't ship ModemManager (QModem
# owns /dev/cdc-wdm*; ModemManager fights it for ownership of the modem),
# so prune the literal "modemmanager" token from each affected Makefile.
strip_mm_dep() {
    local mk="$1"
    [ -f "${mk}" ] || return 0
    if grep -qE '(\+|\b)modemmanager\b' "${mk}"; then
        echo "Stripping modemmanager dep from ${mk}"
        sed -i -E 's/\+modemmanager//g; s/[[:space:]]+modemmanager\b//g' "${mk}"
    fi
}
strip_mm_dep "${SRC}/0xygen-AIO/0xygen-aio/Makefile"
strip_mm_dep "${SRC}/0xygen-AIO/luci-app-lpac-manager/Makefile"
for mk in "${FEED}"/luci-app-qmodem*/Makefile "${FEED}"/qmodem*/Makefile; do
    strip_mm_dep "${mk}"
done

# ---------------------------------------------------------------------------
# 6. Rebuild libcurl with IMAP/IMAPS/POP3/SMTP support
# ---------------------------------------------------------------------------
# The HYFE OTP helper in 0xygen-aio (src/usr/lib/hyfetrial/otp.sh, imap mode)
# polls mailboxes via curl. The upstream OpenWrt libcurl APK is built without
# these protocols by default; we bump PKG_RELEASE so our rebuild outranks the
# prebuilt version when the ImageBuilder resolves packages.
CURL_MAKEFILE="$(find feeds/packages -maxdepth 4 -path '*/curl/Makefile' -print -quit || true)"
if [ -z "${CURL_MAKEFILE}" ]; then
    echo "ERROR: cannot locate curl Makefile in feeds/packages" >&2
    find feeds/packages -name Makefile | grep -i curl || true
    exit 1
fi
echo "Patching ${CURL_MAKEFILE} to bump PKG_RELEASE"
sed -i -E '/^PKG_RELEASE[[:space:]]*:?=/c\PKG_RELEASE:=999' "${CURL_MAKEFILE}"
grep '^PKG_RELEASE' "${CURL_MAKEFILE}"

# ---------------------------------------------------------------------------
# 7. Configure and build
# ---------------------------------------------------------------------------
cat >> .config <<'CFG'
CONFIG_PACKAGE_lpac=y
CONFIG_LPAC_WITH_AT=y
CONFIG_LPAC_WITH_MBIM=y
CONFIG_LPAC_WITH_PCSC=n
CONFIG_LPAC_WITH_UQMI=n
CONFIG_PACKAGE_luci-app-lpac-manager=y
CONFIG_PACKAGE_0xygen-aio=y
CONFIG_PACKAGE_atinout=y
CONFIG_PACKAGE_luci-app-atinout=y
CONFIG_PACKAGE_modemband=y
CONFIG_PACKAGE_luci-app-modemband=y
CONFIG_PACKAGE_modemdata=y
CONFIG_PACKAGE_luci-app-modemdata=y

# QModem core + LuCI apps. T99W175 (Foxconn) support is provided by the
# patch under patches/qmodem/ that we applied to the QModem source above.
CONFIG_PACKAGE_qmodem=y
CONFIG_PACKAGE_qmodem_monitor=y
CONFIG_PACKAGE_luci-app-qmodem=y
CONFIG_PACKAGE_luci-app-qmodem-next=y
CONFIG_PACKAGE_luci-app-qmodem-monitor=y

# ModemManager intentionally left disabled (QModem owns the modem device).
# CONFIG_PACKAGE_modemmanager is not set

# libcurl rebuild with IMAP / IMAPS / POP3 / SMTP for HYFE OTP automation.
CONFIG_PACKAGE_libcurl=y
CONFIG_PACKAGE_curl=y
CONFIG_LIBCURL_OPENSSL=y
# Keep the standard protocol set OpenWrt ships by default ...
CONFIG_LIBCURL_FILE=y
CONFIG_LIBCURL_FTP=y
CONFIG_LIBCURL_HTTP=y
CONFIG_LIBCURL_COOKIES=y
CONFIG_LIBCURL_PROXY=y
CONFIG_LIBCURL_TLS_SRP=y
CONFIG_LIBCURL_THREADED_RESOLVER=y
CONFIG_LIBCURL_UNIX_SOCKETS=y
CONFIG_LIBCURL_VERBOSE=y
CONFIG_LIBCURL_NTLM=y
CONFIG_LIBCURL_NGHTTP2=y
# ... and explicitly enable the mail protocols that hyfetrial/otp.sh needs.
CONFIG_LIBCURL_IMAP=y
CONFIG_LIBCURL_POP3=y
CONFIG_LIBCURL_SMTP=y
CFG

make defconfig

JOBS="$(nproc)"
build_pkg() {
    local target="$1"
    echo "==== Building ${target} ===="
    make "${target}/compile" V=s -j"${JOBS}" || \
        make "${target}/compile" V=s -j1
}

# Build standalone (no LuCI dependency tree first) modem-related binaries.
build_pkg package/feeds/extras/atinout
build_pkg package/feeds/extras/modemband
build_pkg package/feeds/extras/modemdata

# QModem + LuCI apps. Build qmodem first so its files are staged before the
# LuCI apps reference them.
build_pkg package/feeds/extras/qmodem
build_pkg package/feeds/extras/qmodem_monitor
build_pkg package/feeds/extras/luci-app-qmodem
build_pkg package/feeds/extras/luci-app-qmodem-next
build_pkg package/feeds/extras/luci-app-qmodem-monitor

# Build LuCI apps (PKGARCH=all). luci-theme-material is provided by the
# standard OpenWrt luci feed so it is consumed at firmware-image time by
# the ImageBuilder; no need to compile it from source here.
build_pkg package/feeds/extras/luci-app-atinout
build_pkg package/feeds/extras/luci-app-modemband
build_pkg package/feeds/extras/luci-app-modemdata

# Build patched lpac (L850-GL/T99W175 MBIM fixes), the LuCI manager and the
# 0xygen-aio CLI/HYFE bundle. The local lpac in our `extras` feed is the
# one we want — never reach for feeds/packages/lpac (it would be the
# unpatched upstream).
build_pkg package/feeds/extras/lpac
build_pkg package/feeds/0xygen_aio_app/luci-app-lpac-manager
build_pkg package/feeds/0xygen_aio_app/0xygen-aio

# Rebuild libcurl + curl with IMAP/SMTP support (PKG_RELEASE bumped above).
build_pkg package/feeds/packages/curl

# ---------------------------------------------------------------------------
# 8. Collect outputs
# ---------------------------------------------------------------------------
find bin -type f \( -name '*.apk' -o -name '*.ipk' \) -print -exec cp -f {} "${DIST}/" \;

ls -la "${DIST}/"
echo "Built $(find "${DIST}" -type f | wc -l) package files."
