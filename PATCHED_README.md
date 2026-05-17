# 0xygen-Linksys (patched)

Branch: `devin/1779009517-merge-upstream-dual-modem`
Base: `devin/1778457778-build-ea6350v3` (default branch repo lo)
Commits di atas base (urut commit):
- `2bfc125` — Merge upstream dual-modem (L850-GL + T99W175) work, fix CI build
- `3fca903` — scripts: restore executable bit on build-packages.sh

## Apa yang udah ada di zip ini
- Working tree udah dalam state final, patched.
- `.git/` history lengkap, branch lokal `devin/1779009517-merge-upstream-dual-modem`
  udah ke-check out, remote `origin` masih ngarah ke
  `https://github.com/sevoxxson/0xygen-Linksys.git`.

## Cara push + bikin PR dari laptop lo
```bash
unzip 0xygen-Linksys-patched.zip
cd 0xygen-Linksys
git push -u origin devin/1779009517-merge-upstream-dual-modem
```
Habis push, buka link compare-nya (otomatis muncul di output git push), atau:
https://github.com/sevoxxson/0xygen-Linksys/compare/devin/1778457778-build-ea6350v3...devin/1779009517-merge-upstream-dual-modem?expand=1

Title yang gua saranin:
**Merge upstream dual-modem (L850-GL + T99W175) + fix CI build**

Base branch buat PR-nya: `devin/1778457778-build-ea6350v3` (sama kayak PR #3).

## Verifikasi lokal sebelum di-zip
- `bash -n` semua shell script: lulus
- `gcc -fsyntax-only src/esim-telegram-bot.c`: lulus
- Build full lokal dengan Docker (OpenWrt 25.12.3 SDK + ImageBuilder
  arm_cortex-a7_neon-vfpv4) menghasilkan:
  - openwrt-25.12.3-ipq40xx-generic-linksys_ea6350v3-squashfs-factory.bin (~18 MB)
  - openwrt-25.12.3-ipq40xx-generic-linksys_ea6350v3-squashfs-sysupgrade.bin (~15 MB)

## Highlight fix di patch
1. `config/packages.list`: drop `rdisc6` — gak ada di feed OpenWrt 25.12.3
   ipq40xx. Ini root cause CI build gagal di PR upstream.
2. `scripts/build-firmware.sh:94`: `SDK_IMAGE` pakai `${SDK_ARCH:-arm_cortex-a7_neon-vfpv4}`
   alih-alih hardcode — bikin profile `device=custom` di workflow akhirnya
   nge-cross-compile telegram-bot dengan SDK yang bener.
3. `src/esim-telegram-bot.c:787`: `send_photo()` pakai `--data-urlencode` buat
   `photo_arg` (sebelumnya `-d`, jadi URL QR yang punya `&` ke-truncate sebelum
   sampai Telegram).

## Highlight merge dari upstream `0xlineage-maker/0xygen-Linksys` PR #4
- Dual-modem Fibocom L850-GL + Foxconn T99W175 lewat QModem
  (ModemManager dicopot biar gak berebut /dev/cdc-wdm0 dengan lpac).
- patched lpac (overlay tar di `packages/lpac-overlay/`) dengan 4 MBIM patch
  buat T99W175.
- `patches/qmodem/001-t99w175-foxconn-fixes.patch` — QModem T99W175 Foxconn fix.
- `files/etc/uci-defaults/97-multi-modem-defaults` — first-boot seed
  `lpac-esim` config untuk MBIM/curl backend.
- `files/etc/hotplug.d/usb/10-t99w175-serial` — USB hotplug bind buat
  T99W175 (05c6:90d5 / 05c6:9025).
- Workflow `device=linksys_ea6350v3 | custom` selector + `target_path`,
  `profile`, `sdk_arch` input.
- HYFE 3-step wizard (MSISDN -> OTP -> [captcha]) di Telegram bot,
  balas LPA string MarkdownV2 + QR foto.
- `esim --sim-slot` toggle (AT^SWITCH_SLOT) buat T99W175.
