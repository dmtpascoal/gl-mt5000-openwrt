# gl-mt5000-openwrt

A **clean, minimal** build of **official OpenWrt 25.12** (kernel 6.12) for the
**GL.iNet GL-MT5000 (Brume 3)** — with the RTL8371C ("RTL8366UB") switch driven
as a proper **DSA** switch instead of the legacy swconfig driver.

This is *vanilla OpenWrt + the not-yet-merged device support + a ported DSA
switch driver*. It is **not** a GL.iNet firmware fork, and it carries no proxy
stack, themes, or external feeds.

## What the build does

1. Clones official `openwrt/openwrt` at branch `openwrt-25.12` (kernel 6.12).
2. Grafts the GL-MT5000 device-support commit from OpenWrt PR
   [#24237](https://github.com/openwrt/openwrt/pull/24237) (DTS, image recipe,
   board files, and GL's GPL-released RTL8371C SDK package).
3. Converts the switch package from GL's **swconfig** driver to a **DSA** driver
   (`files/dsa/rtl8366ub_dsa.c`, ported from GL's kernel-5.4 code to the 6.12 DSA
   API) and installs a matching DSA device tree (`files/dsa/mt7987a-gl-mt5000.dts`).
4. Builds a minimal image: LuCI + `ethtool`/`ip-full`/`tcpdump` for validation.

All of that is driven by `scripts/diy.sh` and `config/mt5000.config`.

## Build it

GitHub → **Actions** → **Build GL-MT5000 OpenWrt (DSA)** → **Run workflow**.
When it finishes, download the **`gl-mt5000-firmware`** artifact (kept 14 days).
It contains:
- `openwrt-mediatek-filogic-glinet_gl-mt5000-squashfs-sysupgrade.bin` — flash image
- `...-initramfs-kernel.bin` — RAM-boot kernel for safe serial/U-Boot testing
- `resolved.config`, `.manifest` — what was actually built

## Install / recovery

- **Flash:** upload the `sysupgrade.bin` on the GL stock firmware upgrade page
  (uncheck "keep settings"), or `sysupgrade -n <image>` over SSH.
- **Recovery:** GL U-Boot failsafe — power on holding reset until the LED
  flashes, browse to `192.168.1.1`, upload stock GL firmware.
  See <https://docs.gl-inet.com/router/en/4/faq/debrick/>.

## Status / caveats

- ⚠️ **The DSA driver is not yet hardware-validated.** It compiles, but LAN↔LAN,
  the tagged VLAN trunk, and 2.5G link speed must be verified on a bench unit
  before flashing a production router.
- This driver is a **fork-only** solution. Mainline OpenWrt wants the RTL8371C
  added to the existing `rtl8365mb` driver instead of vendoring GL's SDK
  (see the discussion on PR #24237). This repo prioritizes a *working image now*.
- MediaTek PPE cannot hardware-offload DSA-tagged (`rtl8_4`) LAN flows, so
  LAN↔WAN NAT is CPU-bound (~1.5–2.3 Gbps), not 2.5G line rate.

## Layout

```
.github/workflows/build.yml   cloud build (GitHub Actions)
scripts/diy.sh                graft + swconfig->DSA conversion
config/mt5000.config          lean seed .config
files/dsa/rtl8366ub_dsa.c     DSA driver (ported 5.4 -> 6.12)
files/dsa/mt7987a-gl-mt5000.dts  DSA device tree
```
