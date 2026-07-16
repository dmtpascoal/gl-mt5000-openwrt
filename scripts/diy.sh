#!/usr/bin/env bash
# Graft the GL-MT5000 device support (OpenWrt PR #24237) onto official
# openwrt-25.12, then convert the RTL8366UB (RTL8371C) switch from GL's
# swconfig driver to a DSA driver ported to the kernel 6.12 API.
# Run with CWD = OpenWrt source root.
set -eu

WORKSPACE="${GITHUB_WORKSPACE:-$(cd "$(dirname "$0")/.." && pwd)}"
RTLPKG=package/kernel/rtl8366ub
BOARDD=target/linux/mediatek/filogic/base-files/etc/board.d/02_network
FILOGIC_MK=target/linux/mediatek/image/filogic.mk
KCFG=target/linux/mediatek/filogic/config-6.12

# --- 1. Graft the single GL device-support commit (PR #24237 head) ---------
echo ">> Grafting GL-MT5000 device support (PR #24237) onto openwrt-25.12"
git config user.email build@local
git config user.name mt5000-build
git remote add glinet "${GL_DEVICE_COMMIT:-https://github.com/GLiNet-Tech/openwrt.git}" 2>/dev/null || true
# depth>=2 so cherry-pick has the commit's PARENT as a merge base; with depth 1
# git lacks the base and treats the whole tree as add/add conflicts.
git fetch --depth 3 glinet mt5000
git cherry-pick -n FETCH_HEAD || { echo ">> ERROR: graft cherry-pick failed (openwrt-25.12 drift?)"; git cherry-pick --abort 2>/dev/null || true; exit 1; }
test -f target/linux/mediatek/dts/mt7987a-gl-mt5000.dts || { echo ">> ERROR: DTS missing after graft"; exit 1; }
grep -q "glinet_gl-mt5000" "$FILOGIC_MK" || { echo ">> ERROR: device recipe missing after graft"; exit 1; }
echo ">> graft OK"

# --- 2. swconfig -> DSA ----------------------------------------------------
echo ">> Converting RTL8366UB swconfig driver -> DSA (kernel 6.12 API)"

# Ported DSA driver + build the DSA object instead of the swconfig one
cp "$WORKSPACE/files/dsa/rtl8366ub_dsa.c" "$RTLPKG/src/rtl8366ub_dsa.c"
sed -i 's#^rtl8366ub-y += rtl8366ub_mdio.o#rtl8366ub-y += rtl8366ub_dsa.o#' "$RTLPKG/src/Makefile"
sed -i '/^rtl8366ub-y += rtl8366ub_dsa.o/a rtl8366ub-y += l2.o' "$RTLPKG/src/Makefile"

# Drop the swconfig package dependency (DSA core + tagger are in-kernel)
sed -i 's#DEPENDS:=@TARGET_mediatek +kmod-swconfig#DEPENDS:=@TARGET_mediatek#' "$RTLPKG/Makefile"

# Scrub swconfig leftovers ONLY from the gl-mt5000 recipe (do NOT touch other
# devices' recipes that legitimately use swconfig, e.g. mercusys_mr85x)
sed -i '/^define Device\/glinet_gl-mt5000$/,/^endef$/{s/ kmod-rtl8366ub-mdio//g; s/ swconfig\b//g}' "$FILOGIC_MK"

# DSA device tree (switch as an mdio child, per-port user netdevs + cpu@17)
cp "$WORKSPACE/files/dsa/mt7987a-gl-mt5000.dts" target/linux/mediatek/dts/mt7987a-gl-mt5000.dts

# GL's grafted board.d hunk inserts the gl-mt5000 case WITHOUT terminating the
# preceding case (missing ';;') = shell syntax error. Repair the terminator AND
# set the DSA default network (lan1/lan2 user ports, WAN on the SoC PHY eth1).
perl -0pi -e 's/(ucidef_set_interfaces_lan_wan eth0 eth1\n)\s*glinet,gl-mt5000\)\s*\n.*?;;/$1\t\t;;\n\tglinet,gl-mt5000)\n\t\tucidef_set_interfaces_lan_wan "lan1 lan2" "eth1"\n\t\t;;/s' "$BOARDD"

# Enable the RTL8_4 DSA tagger in the kernel config
grep -q "CONFIG_NET_DSA_TAG_RTL8_4=y" "$KCFG" || echo "CONFIG_NET_DSA_TAG_RTL8_4=y" >> "$KCFG"

# --- 3. Sanity gates (each can actually fail) ------------------------------
grep -q "rtl8366ub_dsa.o" "$RTLPKG/src/Makefile" || { echo ">> ERROR: DSA object not wired into src/Makefile"; exit 1; }
  grep -q "^rtl8366ub-y += l2.o" "$RTLPKG/src/Makefile" || { echo ">> ERROR: l2.o not wired (F7 fdb/learning/fast-age need rtksw_l2_*)"; exit 1; }
grep -q "switch@0" target/linux/mediatek/dts/mt7987a-gl-mt5000.dts || { echo ">> ERROR: DSA DTS not applied"; exit 1; }
grep -qF 'ucidef_set_interfaces_lan_wan "lan1 lan2" "eth1"' "$BOARDD" || { echo ">> ERROR: gl-mt5000 DSA board.d line not injected"; exit 1; }
grep -q 'glinet,gl-mt5000)' "$BOARDD" || { echo ">> ERROR: gl-mt5000 case missing from board.d"; exit 1; }
if grep -q '17@eth0' "$BOARDD"; then echo ">> ERROR: stale swconfig ucidef_add_switch survived"; exit 1; fi
sh -n "$BOARDD" || { echo ">> ERROR: board.d/02_network has a shell syntax error"; exit 1; }
if grep -q 'kmod-rtl8366ub-mdio' "$FILOGIC_MK"; then echo ">> ERROR: nonexistent kmod-rtl8366ub-mdio still in DEVICE_PACKAGES"; exit 1; fi

# --- 4. First-boot defaults ------------------------------------------------
mkdir -p files/etc/uci-defaults
cat > files/etc/uci-defaults/99-gl-mt5000 <<'UCI'
#!/bin/sh
uci -q batch <<-EOF
	set system.@system[0].hostname='GL-MT5000'
	set system.@system[0].timezone='WET0WEST,M3.5.0/1,M10.5.0'
	set system.@system[0].zonename='Europe/Lisbon'
	commit system
EOF
exit 0
UCI

echo ">> DSA conversion applied"


