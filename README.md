# 0xygen-Linksys

Custom OpenWrt firmware builder for routers that host an eSIM-capable LTE
modem on the internal mini-PCIe / M.2 / USB bus. The default and only
preset target is the **Linksys EA6350 v3** (`ipq40xx/generic`,
`arm_cortex-a7_neon-vfpv4`), but the build is structured around an
OpenWrt-style **device / version selector** so other targets can be
slotted in by overriding three workflow inputs.

The same firmware supports **two modems out of the box**:

| Modem | Bus | eSIM | SIM-slot switch |
| --- | --- | --- | --- |
| **Fibocom L850-GL** | mPCIe / M.2 internal | Removable eUICC card | n/a (single slot) |
| **Foxconn T99W175** | USB (`05c6:90d5` / `05c6:9025`) | Internal eUICC | `AT^SWITCH_SLOT=<0\|1>` (0 = physical SIM, 1 = internal eSIM) |

QModem (with the user's T99W175 fixes patch on top of upstream
[FUjr/QModem](https://github.com/FUjr/QModem)) is the **primary modem
manager**. ModemManager is intentionally **not** installed: it owns
`/dev/cdc-wdm0` and races QModem / lpac for ownership of the modem,
which makes MBIM dialing unreliable and breaks the LPA when both stacks
are present.

## Build selection

The firmware workflow is **multi-device**. From the GitHub Actions UI,
**Actions → "Build OpenWrt firmware (multi-device)" → Run workflow**, you
get five inputs:

| Input | Purpose |
| --- | --- |
| `device` | Preset that fills `target_path` + `profile` + `sdk_arch`. Currently `linksys_ea6350v3` (default) or `custom`. |
| `openwrt_version` | OpenWrt release, e.g. `25.12.3`, `25.12.4`. `latest` auto-resolves the newest stable release from `downloads.openwrt.org/.versions.json`. |
| `target_path` | Only used when `device=custom`. Example: `ipq40xx/generic`, `ramips/mt7621`, `mediatek/filogic`. |
| `profile` | Only used when `device=custom`. Must match `make info` of the ImageBuilder for the chosen target. Example: `linksys_ea6350v3`. |
| `sdk_arch` | Only used when `device=custom`. SDK architecture tag for the `openwrt/sdk:<arch>-<version>` docker image that cross-compiles the Telegram bot helper. Example: `arm_cortex-a7_neon-vfpv4`. |

Picking the `linksys_ea6350v3` preset is equivalent to:

```
device=linksys_ea6350v3
target_path=ipq40xx/generic
profile=linksys_ea6350v3
sdk_arch=arm_cortex-a7_neon-vfpv4
```

A short-circuit before the build fails fast if the
`https://downloads.openwrt.org/releases/<version>/targets/<target_path>/openwrt-imagebuilder-<version>-<target_slug>.Linux-x86_64.tar.zst`
URL is not reachable, so typos / unpublished versions surface in seconds
instead of after a 5-minute crash.

The repo is split into two GitHub Actions workflows so day-to-day
firmware rebuilds are fast:

| Workflow | Trigger | Time | Output |
| --- | --- | --- | --- |
| **[Build OpenWrt firmware (multi-device)](.github/workflows/build-firmware.yml)** | push / PR / manual | ~5 min | `firmware-<profile>-<version>` artifact with `*-sysupgrade.bin`, `*-factory.bin`, `sha256sums`, `profiles.json` |
| **[Build custom packages (SDK)](.github/workflows/build-packages.yml)** | manual only | ~25 min | `custom-packages-arm_cortex-a7_neon-vfpv4-<version>` artifact with every custom `.apk` file |

The fast firmware workflow uses pre-built `.apk` files committed under
[`packages/prebuilt/`](packages/prebuilt). The slow SDK workflow only
needs to be re-run when an upstream source (`lpac`, `0xygen-aio`,
`QModem`, `luci-app-modemband`, …) changes; after running it you
download the artifact, drop the relevant `.apk` files into
`packages/prebuilt/`, and commit them.

## Custom packages baked in

All `.apk` files in [`packages/prebuilt/`](packages/prebuilt) are built
for `arm_cortex-a7_neon-vfpv4` against the configured OpenWrt release
(default 25.12.3).

| Package(s) | Source | Notes |
| --- | --- | --- |
| `qmodem`, `qmodem_monitor`, `luci-app-qmodem`, `luci-app-qmodem-next`, `luci-app-qmodem-monitor` | [`FUjr/QModem`](https://github.com/FUjr/QModem) + patches | Primary modem manager. Built with the T99W175 (Foxconn) compatibility patch under [`patches/qmodem/001-t99w175-foxconn-fixes.patch`](patches/qmodem/001-t99w175-foxconn-fixes.patch) applied by [`scripts/build-packages.sh`](scripts/build-packages.sh). |
| `lpac` | User-supplied L850-GL-patched source (`packages/lpac-overlay/lpac-l850gl-source.tar.gz`, `2.3.0-r2`) | eSIM LPA carrying the four MBIM patches (`mbim-skip-slot-mapping-env`, `mbim-use-extended-class-byte`, `mbim-open-channel-select-p2-0c`, `mbim-append-le00-to-envelope`). Works for both the L850-GL removable eUICC card and the T99W175 internal eUICC. The old `tools/esim` CLI from the tarball is **ignored** — the firmware ships its own `esim` CLI. |
| `luci-app-lpac-manager` | [`makungyu/0xygen-AIO`](https://github.com/makungyu/0xygen-AIO) (`luci-app-lpac-manager/`) | LuCI eSIM management UI + Telegram bot service. ModemManager dep stripped at build time. |
| `0xygen-aio` | `makungyu/0xygen-AIO` (`0xygen-aio/`) | Unified `esim` CLI + HYFE free-eSIM claim helpers (`hyfetrial`, `hyfe-telegram-claim`). |
| `atinout`, `luci-app-atinout` | [`4IceG/luci-app-atinout`](https://github.com/4IceG/luci-app-atinout) | AT-command shell. |
| `modemband`, `luci-app-modemband` | [`4IceG/luci-app-modemband`](https://github.com/4IceG/luci-app-modemband) | LTE/5G band selector. Overlay under [`files/usr/share/modemband/`](files/usr/share/modemband) adds extra L850-GL / L860-GL profiles. |
| `modemdata` | [`obsy/modemdata`](https://github.com/obsy/modemdata) | Required by `luci-app-modemdata`. |
| `luci-app-modemdata` | [`4IceG/luci-app-modemdata`](https://github.com/4IceG/luci-app-modemdata) | Detailed connection info. |
| `luci-theme-material` | OpenWrt `luci` feed (prebuilt) | Default LuCI theme. |
| `libcurl` + `curl` (rebuilt) | OpenWrt `feeds/packages/net/curl` | Rebuilt with `LIBCURL_IMAP=y`, `LIBCURL_POP3=y`, `LIBCURL_SMTP=y` so `0xygen-aio`'s HYFE OTP poller can talk IMAP. `PKG_RELEASE` bumped to `999` so this APK outranks the upstream prebuilt. |

Plus the Ookla Speedtest CLI binary (downloaded fresh during the
firmware build, see below).

Standard OpenWrt packages added on top: `libmbim`, `mbim-utils`,
`libqmi`, `uqmi`, `usb-modeswitch`, `comgt`, `comgt-ncm`, `picocom`,
the relevant `kmod-usb-*` drivers (`cdc-mbim`, `cdc-ncm`, `qmi-wwan`,
`serial-option`, `serial-qualcomm`, `wdm`, …), `jq`, `bc`, `terminfo`,
`rdisc6`, `coreutils-{timeout,stat}`, `ca-bundle`, `ca-certificates`,
`luci`, `luci-mod-*`, `luci-app-firewall`, `luci-app-opkg`,
**`luci-app-filebrowser`**, `nano`, `htop`. Full list in
[`config/packages.list`](config/packages.list).

`modemmanager` / `luci-proto-modemmanager` and the stock `sms-tool` are
**intentionally not installed** — QModem ships its own Q-flavoured
sms-tool and owns the modem.

## First-boot configuration

The image ships with a default
[`/etc/config/wireless`](files/etc/config/wireless) so both radios come
up enabled at first boot with the right SSID/password (no detection or
runtime regeneration involved):

- `radio0` (2.4 GHz): SSID **`0x`**, WPA2, password **`1sampai10`**, country `ID`
- `radio1` (5 GHz): SSID **`0x⁵`**, WPA2, password **`1sampai10`**, country `ID`

Three sysupgrade-safe `uci-defaults` scripts run once after the firmware
is flashed:

- [`97-multi-modem-defaults`](files/etc/uci-defaults/97-multi-modem-defaults)
  — seeds `/etc/config/lpac-esim` with the L850-GL / T99W175 MBIM
  defaults (`apdu_backend=mbim`, `mbim_proxy=1`,
  `mbim_skip_slot_mapping=1`,
  `custom_isd_r_aid=A0000005591010FFFFFFFF8900000100`,
  `at_device=/dev/ttyUSB2`, `modem_iface=1_1`, `reboot_method=script`).
- [`99-0xygen-firstboot`](files/etc/uci-defaults/99-0xygen-firstboot) —
  re-asserts both WiFi radios, switches LuCI to the Material theme,
  drops stale `mediaurlbase=/luci-static/argon` values, mounts
  `debugfs`, drops the LuCI index / module caches, restarts `rpcd`, and
  reloads `wifi` / `network` / `firewall`. ModemManager-era `modem`
  interface creation has been removed — QModem owns the modem now, and
  any legacy `network.modem` left over from older firmware is deleted.
- [`/etc/hotplug.d/usb/10-t99w175-serial`](files/etc/hotplug.d/usb/10-t99w175-serial)
  — binds the T99W175 USB serial endpoints (`05c6:90d5`, `05c6:9025`)
  so `/dev/ttyUSB*` and `/dev/cdc-wdm0` show up reliably for QModem and
  lpac.

Every step logs via `logger -t 0xygen-firstboot`. Tail with
`logread | grep -e 0xygen-firstboot -e t99w175 -e multi-modem` after
first boot to verify.

> Important: when flashing, **uncheck "Keep settings"** in LuCI (or use
> `sysupgrade -n`). The `uci-defaults` scripts only run once — if the
> system thinks settings were preserved it will skip first-boot setup
> entirely.

## eSIM (lpac) quick start

After boot, the lpac UCI defaults (committed by
`97-multi-modem-defaults`) already drive both modems through MBIM proxy
mode with the patched lpac:

```sh
uci show lpac-esim
# lpac-esim.config=settings
# lpac-esim.config.apdu_backend='mbim'
# lpac-esim.config.http_backend='curl'
# lpac-esim.config.mbim_device='/dev/cdc-wdm0'
# lpac-esim.config.mbim_proxy='1'
# lpac-esim.config.mbim_skip_slot_mapping='1'
# lpac-esim.config.custom_isd_r_aid='A0000005591010FFFFFFFF8900000100'
# lpac-esim.config.at_device='/dev/ttyUSB2'
# lpac-esim.config.modem_iface='1_1'
# lpac-esim.config.reboot_method='script'

esim                      # interactive CLI (menu)
esim --chip-info          # one-shot eUICC info
esim --profile-list       # list installed eSIM profiles
esim --sim-slot get       # T99W175 only: read current slot
esim --sim-slot set 1     # T99W175 only: switch to internal eSIM
esim --sim-slot caps      # show AT^SWITCH_SLOT capabilities
```

The unified `esim` CLI from `0xygen-aio` exposes:

- **Menu 2 → Profile list** now includes a sub-action for **switching
  the T99W175 SIM slot** (`AT^SWITCH_SLOT=0` for physical SIM,
  `AT^SWITCH_SLOT=1` for internal eSIM). The slot menu detects whether
  the connected modem actually supports the switch and skips it
  gracefully otherwise.
- **HYFE free-eSIM claim flow** (`esim hyfetrial`) with `imap` OTP
  polling powered by the rebuilt IMAP-enabled `libcurl`.
- A non-interactive `--telegram-hyfe-claim` mode used by the Telegram
  bot when running the HYFE wizard.

The old **menu 8 (Telegram bot settings)** has been removed — the
Telegram bot is configured straight from LuCI under
*Cellular → LPAC eSIM Manager → Telegram*.

### HYFE trial via Telegram

The Telegram bot (binary `esim-telegram-bot`, cross-compiled by
[`scripts/build-firmware.sh`](scripts/build-firmware.sh) using the
OpenWrt SDK docker image and shipped at `/usr/bin/esim-telegram-bot`)
now drives the full HYFE claim flow as a 3-step wizard:

1. `Klaim Free eSIM HYFE` button → bot asks for MSISDN.
2. After MSISDN, bot calls `hyfe-telegram-claim start-manual` and asks
   for the 6-digit OTP delivered to the configured email / SMS.
3. After OTP:
   - If `HYFE_CAPTCHA_MODE` is `auto` (the default), the bot calls
     `hyfe-telegram-claim finish` immediately.
   - If `HYFE_CAPTCHA_MODE=manual`, the bot asks for a
     `g-recaptcha-response` token and calls `hyfe-telegram-claim
     finish-manual`.

On success the bot replies with the **LPA: string in a copyable
MarkdownV2 code block** *and* a **QR code as a photo** (rendered via
`api.qrserver.com`, payload URL-encoded). The pending-state machinery
survives a bot restart, and `/cancel` aborts the wizard at any point.

## Ookla Speedtest CLI

Following the
[radenku.com tutorial](https://radenku.com/cara-install-speedtest-cli-openwrt/),
the official Ookla Speedtest CLI binary for `armhf` is downloaded during
the firmware build (URL + sha256 pinned in
[`scripts/build-firmware.sh`](scripts/build-firmware.sh)) and baked into
the image at `/usr/libexec/ookla-speedtest`. A wrapper at
`/usr/bin/speedtest` auto-accepts the EULA/GDPR so the very first run
needs no prompts:

```sh
speedtest                        # quick test, picks the best server
speedtest --servers              # list nearby servers
speedtest -s 1234                # use a specific server ID
speedtest -f json-pretty         # machine-readable output
```

## Repo layout

```
.
├── .github/workflows/
│   ├── build-firmware.yml              # multi-device firmware build
│   └── build-packages.yml              # manual SDK build of custom .apks
├── scripts/
│   ├── build-firmware.sh               # downloads ImageBuilder + builds image
│   └── build-packages.sh               # runs inside openwrt/sdk:<arch>-<version>
├── config/
│   └── packages.list                   # packages baked into the firmware
├── patches/
│   └── qmodem/
│       └── 001-t99w175-foxconn-fixes.patch
├── packages/
│   ├── lpac-overlay/
│   │   └── lpac-l850gl-source.tar.gz   # user-supplied patched lpac source
│   └── prebuilt/                       # .apk files committed for the image build
├── src/
│   └── esim-telegram-bot.c             # Telegram bridge (cross-compiled by build-firmware.sh)
├── files/
│   ├── etc/config/lpac-esim            # L850-GL / T99W175 MBIM defaults
│   ├── etc/config/wireless             # default wifi overlay (SSIDs/keys pre-set)
│   ├── etc/hotplug.d/usb/
│   │   └── 10-t99w175-serial           # T99W175 USB bind
│   ├── etc/uci-defaults/
│   │   ├── 97-multi-modem-defaults     # lpac-esim UCI seeding
│   │   └── 99-0xygen-firstboot         # wifi / theme / cache flush first-boot
│   ├── usr/bin/esim                    # interactive eSIM CLI
│   └── usr/bin/speedtest               # wrapper for Ookla CLI
└── README.md
```

## Local build

You need Docker and ~10 GB of free disk.

Fast path (rebuild only the firmware image with the defaults):

```bash
GITHUB_WORKSPACE="$PWD" bash scripts/build-firmware.sh
# → dist/firmware/openwrt-25.12.3-ipq40xx-generic-linksys_ea6350v3-squashfs-sysupgrade.bin
```

Override any of the three selectors to target a different device /
release:

```bash
GITHUB_WORKSPACE="$PWD" \
OPENWRT_VERSION=25.12.4 \
TARGET_PATH=ipq40xx/generic \
PROFILE=linksys_ea6350v3 \
    bash scripts/build-firmware.sh
```

Slow path (rebuild custom `.apk` packages from source):

```bash
mkdir -p dist
docker run --rm --user root \
    -e WORK=/work \
    -v "$PWD:/work" \
    -w /builder \
    openwrt/sdk:arm_cortex-a7_neon-vfpv4-25.12.3 \
    /bin/bash /work/scripts/build-packages.sh
# → dist/packages/*.apk
# Pick the custom ones (qmodem, luci-app-qmodem*, lpac, 0xygen-aio,
# luci-app-lpac-manager, modemband, modemdata, atinout, curl, …),
# drop into packages/prebuilt/, and commit. Re-run the fast path to
# roll them into a new firmware image.
```

## Flashing the EA6350 v3

- First time from stock Linksys firmware: flash
  `openwrt-25.12.3-ipq40xx-generic-linksys_ea6350v3-squashfs-factory.bin`
  via the stock GUI.
- Upgrading an already-OpenWrt EA6350v3: flash
  `openwrt-25.12.3-ipq40xx-generic-linksys_ea6350v3-squashfs-sysupgrade.bin`
  via LuCI → *System → Backup / Flash Firmware*, or `sysupgrade -n`
  from SSH. **Use `-n` / uncheck "Keep settings"** so the first-boot
  uci-defaults scripts run and apply the WiFi + lpac-esim + theme
  setup.

> The EA6350v3 has two firmware banks. After a successful first boot
> OpenWrt marks the booted partition as good; if it cannot, the
> bootloader will fall back to the other bank.
