// SPDX-License-Identifier: GPL-2.0
/*
 * This is a sparsely documented chip, the only viable documentation seems
 * to be a patched up code drop from the vendor that appear in various
 * GPL source trees.
 *
 * Copyright (C) 2024 Jianhui Zhao <jianhui.zhao@gl-inet.com>
 *
 * Ported from the kernel 5.4 DSA API to 6.12 (phylink_get_caps, per-vid
 * port_vlan_add/del with extack, struct dsa_bridge, modern dsa_switch
 * allocation, of_get_phy_mode by-reference). VLAN membership is now
 * read-modify-write so bridged multi-port VLANs keep all members.
 */

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/debugfs.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/bitops.h>
#include <linux/regmap.h>
#include <linux/phylink.h>
#include <linux/ethtool.h>
#include <net/dsa.h>
#include <net/switchdev.h>
#include <linux/gpio/consumer.h>

#include "rtk_switch.h"
#include "port.h"
#include "vlan.h"
#include "cpu.h"
#include "l2.h"
#include "stat.h"
#include "rtl8366ub_dsa.h"
#include "dal/smi.h"

extern void rtk_set_mdc_mdio(struct mii_bus *bus, int id);

/* Length of the Realtek rtl8_4 CPU tag inserted on the CPU/extension port
 * (see net/dsa/tag_rtl8_4.c, RTL8_4_TAG_LEN).
 */
#define RTL8366UB_CPU_TAG_LEN	8

/* Hardware per-port max frame length ceiling: RTL8371C_MAX_PACKET_LENGTH
 * (0x3FEF) from the SDK DAL (dal/rtl8371c/dal_rtl8371c_port.h). The DAL
 * rejects any rtk_port_maxPacketLength_set() above this value.
 */
#define RTL8366UB_MAX_PKT_LEN	0x3FEF

static int rtl8366ub_find_cpu_port(struct dsa_switch *ds)
{
    /* Find the connected cpu port. Valid ext ports are EXT_PORT0 (16) or
     * EXT_PORT1 (17); the MT5000 wires the 2500base-x/HSGMII CPU link on
     * EXT_PORT1.
     */
    if (dsa_is_cpu_port(ds, EXT_PORT0))
        return EXT_PORT0;

    if (dsa_is_cpu_port(ds, EXT_PORT1))
        return EXT_PORT1;

    return -EINVAL;
}

static enum dsa_tag_protocol rtl8366ub_sw_get_tag_protocol(struct dsa_switch *ds, int port,
                                                           enum dsa_tag_protocol mp)
{
    struct rtl8366ub_priv *priv = ds->priv;

    if (port != EXT_PORT0 && port != EXT_PORT1) {
        dev_warn(priv->dev, "port not matched with tagging CPU port\n");
        return DSA_TAG_PROTO_NONE;
    }

    return DSA_TAG_PROTO_RTL8_4;
}

static int rtl8366ub_cpu_mac_config(struct dsa_switch *ds, rtksw_port_t port,
                                    phy_interface_t phy_mode)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_port_mac_ability_t mac_cfg;
    rtksw_mode_ext_t mode_ext;

    mode_ext = RTKSW_MODE_EXT_HSGMII;
    mac_cfg.forcemode = PORT_MAC_FORCE;
    mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
    mac_cfg.duplex = RTKSW_PORT_FULL_DUPLEX;
    mac_cfg.link = RTKSW_PORT_LINKUP;
    mac_cfg.nway = RTKSW_DISABLED;
    mac_cfg.txpause = RTKSW_ENABLED;
    mac_cfg.rxpause = RTKSW_ENABLED;

    switch (phy_mode) {
        case PHY_INTERFACE_MODE_2500BASEX:
            mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
            mode_ext = RTKSW_MODE_EXT_HSGMII;
            break;
        case PHY_INTERFACE_MODE_10GKR:
        case PHY_INTERFACE_MODE_USXGMII:
            mac_cfg.speed = RTKSW_PORT_SPEED_10G;
            mode_ext = RTKSW_MODE_EXT_USXGMII;
            break;
        default:
            dev_err(priv->dev, "phy mode %d not supported for CPU port\n", phy_mode);
            return -EINVAL;
    }
    /* Propagate the write result: this forces the CPU/ext MAC link and is a
     * load-bearing step - if it silently fails, setup() must not go on to
     * report success (see the caller, which now checks this return).
     */
    if (rtk_port_macForceLinkExt_set(port, mode_ext, &mac_cfg) != RT_ERR_OK) {
        dev_err(priv->dev, "failed to force CPU port ext MAC link\n");
        return -EIO;
    }
    return 0;
}

static int rtl8366ub_sw_setup(struct dsa_switch *ds)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_portmask_t portmask;
    rtksw_portmask_t cpu_portmask;
    struct dsa_port *cpu_dp;
    phy_interface_t interface;
    int ret, i;
    u32 val;

    /* Reset whole chip through gpio pin */
    gpiod_set_value_cansleep(priv->reset, 0);
    usleep_range(100000, 150000);
    gpiod_set_value_cansleep(priv->reset, 1);
    usleep_range(1000000, 1500000);

    /* Detect device */
    ret = reg_mdcmdio_read(0x4, &val);
    if (ret) {
        dev_err(priv->dev, "can't get chip ID (%d)\n", ret);
        return ret;
    }

    switch (val) {
        case 0x8366:
            dev_info(priv->dev, "found an RTL8366UB switch\n");
            break;
        default:
            dev_err(priv->dev, "found an Unknown Realtek switch (id=0x%04x)\n",
                    val);
            return -ENODEV;
    }

    priv->cpu_port = rtl8366ub_find_cpu_port(ds);
    if (priv->cpu_port < 0) {
        dev_err(priv->dev, "No cpu port configured in both ext port %d and %d\n",
                EXT_PORT0, EXT_PORT1);
        return -EINVAL;
    }

    cpu_dp = dsa_to_port(ds, priv->cpu_port);
    ret = of_get_phy_mode(cpu_dp->dn, &interface);
    if (ret) {
        dev_err(priv->dev, "Can't find phy-mode for cpu port\n");
        return -ENODEV;
    }

    if (rtk_switch_init() != RT_ERR_OK) {
        dev_err(priv->dev, "rtk_switch_init failed\n");
        return -EIO;
    }
    if (rtk_vlan_init() != RT_ERR_OK) {
        dev_err(priv->dev, "rtk_vlan_init failed\n");
        return -EIO;
    }
    /*
     * The L2 module has its own init that the switch/vlan inits do NOT run:
     * dal_rtl8371c_switch_init() touches none of the L2 config, and the
     * driver never called rtk_l2_init(). Yet it drives rtk_l2_addr_add/del,
     * rtk_l2_ucastAddr_flush and rtk_l2_limitLearningCnt_set - the SDK header
     * explicitly says "Initialize l2 module before calling any l2 APIs".
     * rtk_l2_init() sets a sane LUT aging time (300 s), enables the exact-
     * match CAM (BCAM_DISABLE=0) and selects MAC-based multicast lookup;
     * without it those stay at power-on register defaults, so dynamic FDB on
     * the bridged trunk ages at an undefined rate (stale entries or constant
     * unknown-unicast flooding on the LAN).
     */
    if (rtk_l2_init() != RT_ERR_OK) {
        dev_err(priv->dev, "rtk_l2_init failed\n");
        return -EIO;
    }

    RTKSW_PORTMASK_CLEAR(portmask);
    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        RTKSW_PORTMASK_CLEAR(cpu_portmask);
        RTKSW_PORTMASK_PORT_SET(cpu_portmask, priv->cpu_port);
        rtk_port_isolation_set(i, &cpu_portmask);

        RTKSW_PORTMASK_PORT_SET(portmask, i);

        /*
         * rtk_vlan_init() turns on per-port ingress VLAN filtering, which
         * would drop tagged frames whose VID has no 4k-table membership.
         * DSA expects a non-filtering switch by default (transparent to
         * VLAN tags); .port_vlan_filtering re-enables it per port when a
         * bridge requests vlan_filtering=1.
         */
        rtk_vlan_portIgrFilterEnable_set(i, RTKSW_DISABLED);

        /*
         * DSA model: a standalone (non-bridged) port must not learn source
         * MACs; learning is turned on per-port by .port_bridge_flags only
         * when the port joins a bridge with BR_LEARNING set (and turned off
         * again on leave). rtk_l2_init() above left every port's auto-learn
         * limit at the chip maximum (learning ON), so clear it here
         * (0 == learning disabled), mirroring the in-tree rtl8365mb setup
         * (rtl8365mb_port_set_learning(.., false) for every port).
         */
        rtk_l2_limitLearningCnt_set(i, 0);
    }
    rtk_port_isolation_set(EXT_PORT0, &portmask);
    rtk_port_isolation_set(priv->cpu_port, &portmask);

    /* rtk_vlan_init() turns ingress VLAN filtering on for ALL valid ports,
     * including the CPU/extension port. The setup loop above only clears it
     * for the user ports (0..RTL8366UB_NUM_PORTS-1), and DSA only ever
     * toggles .port_vlan_filtering on user ports - so the CPU port would stay
     * filtered forever. The CPU port is a trunk that must accept every bridge
     * VLAN (and CPU-injected frames); leave its ingress filter off. Egress is
     * still governed by the per-VID member set programmed in port_vlan_add.
     */
    rtk_vlan_portIgrFilterEnable_set(priv->cpu_port, RTKSW_DISABLED);

    ret = rtl8366ub_cpu_mac_config(ds, priv->cpu_port, interface);
    if (ret) {
        dev_err(priv->dev, "CPU port MAC config failed\n");
        return ret;
    }

    /* CPU tag insertion + CPU-port enable are load-bearing: if either write
     * fails, gmac0 never sees correctly rtl8_4-tagged frames and the whole
     * gateway goes dark. Fail registration (so DSA unwinds) instead of
     * booting a switch that answers to nothing. Mirrors rtl8365mb, which
     * checks every setup-time register write.
     */
    if (rtk_cpu_tagPort_set(priv->cpu_port, CPU_INSERT_TO_ALL) != RT_ERR_OK ||
        rtk_cpu_enable_set(RTKSW_ENABLED) != RT_ERR_OK) {
        dev_err(priv->dev, "failed to enable CPU tag port\n");
        return -EIO;
    }

    if (rtk_port_phyEnableAll_set(RTKSW_ENABLED) != RT_ERR_OK) {
        dev_err(priv->dev, "failed to enable PHYs\n");
        return -EIO;
    }

    /*
     * Start every user port administratively down (STP DISABLED) so it does
     * not forward frames to the CPU before it has been brought up. DSA moves
     * each port to BR_STATE_FORWARDING from dsa_port_enable() on .ndo_open
     * (and to DISABLED on close), so this is safe and does not strand a port.
     * Mirrors the in-tree rtl8365mb setup, whose comment notes ports would
     * otherwise still forward to the CPU despite being down by default. The
     * CPU/ext port is intentionally left untouched (stays forwarding).
     */
    for (i = 0; i < RTL8366UB_NUM_PORTS; i++)
        rtk_stp_mstpState_set(0, i, RTKSW_STP_STATE_DISABLED);

    return 0;
}

static int rtl8366ub_sw_phy_read(struct dsa_switch *ds, int port, int regnum)
{
    unsigned int val;

    /* Return the MDIO "no device" value on error rather than leaking stack
     * garbage onto the DSA user MII bus (would hallucinate PHY IDs on scan).
     */
    if (rtk_port_phyReg_get(port, regnum, &val) != RT_ERR_OK)
        return 0xffff;
    return val & 0xffff;
}

static int rtl8366ub_sw_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val)
{
    if (rtk_port_phyReg_set(port, regnum, val) != RT_ERR_OK)
        return -EIO;
    return 0;
}

static int rtl8366ub_sw_port_enable(struct dsa_switch *ds, int port,
                                    struct phy_device *phy)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_port_phy_ability_t ability;

    if (!dsa_is_user_port(ds, port))
        return 0;

    mutex_lock(&priv->reg_mutex);
    priv->ports[port].enable = true;

    /* Power the PHY up. 0xa610 is an OCP register, so it must go through the
     * OCP accessor; the Clause-22 rtk_port_phyReg_set rejects reg > 0x1f.
     */
    rtk_port_phyOCPReg_set(port, 0xa610, 0x2058);

    /* The integrated PHYs are 2.5G capable, but generic C22 phylib can only
     * advertise up to 1000BASE-T. Program the full ability (incl. 2.5G)
     * through the SDK so the port can actually negotiate 2.5G.
     */
    memset(&ability, 0, sizeof(ability));
    ability.AutoNegotiation = 1;
    ability.Half_10 = 1;
    ability.Full_10 = 1;
    ability.Half_100 = 1;
    ability.Full_100 = 1;
    ability.Full_1000 = 1;
    ability.Full_2P5G = 1;
    ability.FC = 1;
    ability.AsyFC = 1;
    rtk_port_phyAutoNegoAbility_set(port, &ability);

    mutex_unlock(&priv->reg_mutex);
    return 0;
}

static void rtl8366ub_sw_port_disable(struct dsa_switch *ds, int port)
{
    struct rtl8366ub_priv *priv = ds->priv;

    if (!dsa_is_user_port(ds, port))
        return;

    mutex_lock(&priv->reg_mutex);
    priv->ports[port].enable = false;
    /* Power the PHY down via the OCP accessor (0xa610, power-down bit set). */
    rtk_port_phyOCPReg_set(port, 0xa610, 0x2858);
    mutex_unlock(&priv->reg_mutex);
}

static void rtl8366ub_sw_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
    u32 stp_state;

    if (dsa_is_unused_port(ds, port))
        return;

    switch (state) {
        case BR_STATE_DISABLED:
            stp_state = RTKSW_STP_STATE_DISABLED;
            break;
        case BR_STATE_BLOCKING:
        case BR_STATE_LISTENING:
            stp_state = RTKSW_STP_STATE_BLOCKING;
            break;
        case BR_STATE_LEARNING:
            stp_state = RTKSW_STP_STATE_LEARNING;
            break;
        case BR_STATE_FORWARDING:
        default:
            stp_state = RTKSW_STP_STATE_FORWARDING;
            break;
    }

    /* All VLAN entries carry fid_msti=0, so the spanning-tree state must be
     * written to MSTI 0 — the instance that actually governs their traffic.
     * (The original wrote MSTI 1, an instance no VLAN references, making STP
     * a hardware no-op.)
     */
    rtk_stp_mstpState_set(0, port, stp_state);
}

static int rtl8366ub_sw_port_bridge_join(struct dsa_switch *ds, int port,
                                         struct dsa_bridge bridge,
                                         bool *tx_fwd_offload,
                                         struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_portmask_t portmask_tmp;
    rtksw_portmask_t portmask;
    int i;

    /* The isolation update is a read-modify-write spanning several SDK
     * calls (rtk_port_isolation_get + _set). Hold reg_mutex so it cannot
     * interleave with another port's join/leave or a vlan_add/del RMW,
     * matching the locking already used in port_vlan_add/del. (rtnl_lock
     * serializes these switchdev ops today, so this is robustness /
     * consistency rather than an active race, but the SDK-level lock only
     * makes each individual get/set atomic, not the get..set pair.)
     */
    mutex_lock(&priv->reg_mutex);

    rtk_port_isolation_get(port, &portmask);

    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        if (i == port)
            continue;

        if (!dsa_port_offloads_bridge(dsa_to_port(ds, i), &bridge))
            continue;

        /* Join this port to each other port on the bridge */
        rtk_port_isolation_get(i, &portmask_tmp);
        RTKSW_PORTMASK_PORT_SET(portmask_tmp, port);
        rtk_port_isolation_set(i, &portmask_tmp);

        RTKSW_PORTMASK_PORT_SET(portmask, i);
    }

    /* Join each other port on the bridge to this port */
    rtk_port_isolation_set(port, &portmask);

    mutex_unlock(&priv->reg_mutex);

    return 0;
}

static void rtl8366ub_sw_port_bridge_leave(struct dsa_switch *ds, int port,
                                           struct dsa_bridge bridge)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_portmask_t portmask_tmp;
    rtksw_portmask_t portmask;
    int i;

    /* See port_bridge_join: hold reg_mutex over the isolation get..set RMW. */
    mutex_lock(&priv->reg_mutex);

    rtk_port_isolation_get(port, &portmask);

    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        if (i == port)
            continue;

        if (!dsa_port_offloads_bridge(dsa_to_port(ds, i), &bridge))
            continue;

        /* Remove this port from any other port on the bridge */
        rtk_port_isolation_get(i, &portmask_tmp);
        RTKSW_PORTMASK_PORT_CLEAR(portmask_tmp, port);
        rtk_port_isolation_set(i, &portmask_tmp);

        RTKSW_PORTMASK_PORT_CLEAR(portmask, i);
    }

    rtk_port_isolation_set(port, &portmask);

    mutex_unlock(&priv->reg_mutex);
}

static int rtl8366ub_sw_port_vlan_add(struct dsa_switch *ds, int port,
                                      const struct switchdev_obj_port_vlan *vlan,
                                      struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;
    bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
    bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
    rtksw_vlan_cfg_t vlan_entry;
    u16 vid = vlan->vid;

    mutex_lock(&priv->reg_mutex);

    /* Read-modify-write: keep the existing members of this VLAN so that a
     * bridge spanning several ports does not lose members as each port is
     * added one at a time. A read failure must NOT fall back to a zeroed
     * entry — writing that back would erase every other member of the VID.
     */
    if (rtk_vlan_get(vid, &vlan_entry) != RT_ERR_OK) {
        mutex_unlock(&priv->reg_mutex);
        NL_SET_ERR_MSG_MOD(extack, "failed to read current VLAN entry");
        return -EIO;
    }

    RTK_PORTMASK_PORT_SET(vlan_entry.mbr, port);
    if (untagged)
        RTK_PORTMASK_PORT_SET(vlan_entry.untag, port);
    else
        RTK_PORTMASK_PORT_CLEAR(vlan_entry.untag, port);
    vlan_entry.ivl_en = 1;

    rtk_vlan_set(vid, &vlan_entry);

    if (pvid) {
        rtk_vlan_portPvid_set(port, vid, 0);
        if (port < RTL8366UB_NUM_PORTS)
            priv->ports[port].pvid = vid;
    }

    mutex_unlock(&priv->reg_mutex);
    return 0;
}

static int rtl8366ub_sw_port_vlan_del(struct dsa_switch *ds, int port,
                                      const struct switchdev_obj_port_vlan *vlan)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_vlan_cfg_t vlan_entry;
    u16 vid = vlan->vid;
    u16 pvid;

    mutex_lock(&priv->reg_mutex);

    /* A read failure must not fall back to a zeroed entry: writing that back
     * would wipe the other member ports of this VID. Skip the membership
     * write and only fix up PVID bookkeeping below.
     */
    if (rtk_vlan_get(vid, &vlan_entry) == RT_ERR_OK) {
        /* Only drop this port from the VLAN, keep any other members intact. */
        RTK_PORTMASK_PORT_CLEAR(vlan_entry.mbr, port);
        RTK_PORTMASK_PORT_CLEAR(vlan_entry.untag, port);
        rtk_vlan_set(vid, &vlan_entry);
    }

    if (port < RTL8366UB_NUM_PORTS) {
        pvid = priv->ports[port].pvid;
        if (pvid == vid) {
            rtk_vlan_portPvid_set(port, 1, 0);
            priv->ports[port].pvid = 1;
        }
    }

    mutex_unlock(&priv->reg_mutex);
    return 0;
}

static void rtl8366ub_sw_phylink_get_caps(struct dsa_switch *ds, int port,
                                          struct phylink_config *config)
{
    config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
                               MAC_10 | MAC_100 | MAC_1000FD;

    if (port == EXT_PORT0 || port == EXT_PORT1) {
        /* CPU / extension port: 2.5G HSGMII (or 10G USXGMII) SerDes link. */
        config->mac_capabilities |= MAC_2500FD | MAC_5000FD | MAC_10000FD;
        __set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
        __set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
        __set_bit(PHY_INTERFACE_MODE_10GKR, config->supported_interfaces);
    } else {
        /* User ports use the integrated copper PHYs, up to 2.5G. */
        config->mac_capabilities |= MAC_2500FD;
        __set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
        __set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
    }
}

static void rtl8366ub_sw_phylink_mac_config(struct dsa_switch *ds, int port,
                                            unsigned int mode,
                                            const struct phylink_link_state *state)
{
    struct rtl8366ub_priv *priv = ds->priv;

    /* The CPU/extension port is force-configured in setup(); user ports use
     * their integrated PHYs driven by phylib, with the MAC forced from
     * mac_link_up(). Nothing to do here for either conventional PHY or
     * fixed-link modes.
     */
    if (mode != MLO_AN_PHY && mode != MLO_AN_FIXED)
        dev_err(priv->dev,
                "port %d supports only conventional PHY or fixed-link\n", port);
}

static void rtl8366ub_sw_phylink_mac_link_down(struct dsa_switch *ds, int port,
                                               unsigned int mode,
                                               phy_interface_t interface)
{
    rtksw_port_mac_ability_t mac_cfg;

    if (port >= RTL8366UB_NUM_PORTS)
        return;

    /* Fully initialize: the DAL range-checks every field, so leaving 6 of 7
     * fields as stack garbage makes the force-link-down write get rejected
     * (or, worse, writes random speed/duplex/pause into the MAC register).
     */
    memset(&mac_cfg, 0, sizeof(mac_cfg));
    mac_cfg.forcemode = PORT_MAC_FORCE;
    mac_cfg.speed = RTKSW_PORT_SPEED_1000M;
    mac_cfg.duplex = RTKSW_PORT_FULL_DUPLEX;
    mac_cfg.link = RTKSW_PORT_LINKDOWN;
    mac_cfg.nway = RTKSW_DISABLED;
    mac_cfg.txpause = RTKSW_DISABLED;
    mac_cfg.rxpause = RTKSW_DISABLED;
    rtk_port_macForceLink_set(port, &mac_cfg);
}

static int rtl8366ub_sw_port_vlan_filtering(struct dsa_switch *ds, int port,
                                            bool vlan_filtering,
                                            struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;

    /* Per-port ingress VLAN filtering: only enforce membership when the
     * bridge actually asks for vlan_filtering=1. setup() leaves it off so a
     * plain (non-filtering) bridge is transparent to tags.
     */
    mutex_lock(&priv->reg_mutex);
    rtk_vlan_portIgrFilterEnable_set(port,
        vlan_filtering ? RTKSW_ENABLED : RTKSW_DISABLED);
    mutex_unlock(&priv->reg_mutex);
    return 0;
}

static void rtl8366ub_sw_port_fast_age(struct dsa_switch *ds, int port)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_l2_flushCfg_t cfg;

    /* Flush only the dynamically-learned unicast entries on this port. The
     * config is zero-initialised, so flushByVid/flushByFid/flushByMac stay
     * disabled and the SDK takes the flush-by-port path; flushStaticAddr=0
     * keeps static (bridge-offloaded) FDB entries intact. 'port' is the DSA
     * index == SDK logical port and is L2P-mapped inside the DAL.
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.flushByPort = RTKSW_ENABLED;
    cfg.port = port;
    cfg.flushStaticAddr = RTKSW_DISABLED;

    mutex_lock(&priv->reg_mutex);
    rtk_l2_ucastAddr_flush(&cfg);
    mutex_unlock(&priv->reg_mutex);
}

static int rtl8366ub_sw_port_pre_bridge_flags(struct dsa_switch *ds, int port,
                                              struct switchdev_brport_flags flags,
                                              struct netlink_ext_ack *extack)
{
    /* Only per-port MAC learning can be toggled in hardware. */
    if (flags.mask & ~(BR_LEARNING))
        return -EINVAL;

    return 0;
}

static int rtl8366ub_sw_port_bridge_flags(struct dsa_switch *ds, int port,
                                          struct switchdev_brport_flags flags,
                                          struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_api_ret_t ret = RT_ERR_OK;

    if (flags.mask & BR_LEARNING) {
        /* A per-port auto-learn limit of 0 disables SA learning; the chip's
         * max LUT count re-enables it (mirrors rtl8365mb's learn-limit
         * approach). maxLutAddrNumber_get() returns 0 before init, but this
         * op only runs after setup() has completed rtk_switch_init().
         */
        mutex_lock(&priv->reg_mutex);
        ret = rtk_l2_limitLearningCnt_set(port,
                (flags.val & BR_LEARNING) ?
                    rtksw_switch_maxLutAddrNumber_get(0) : 0);
        mutex_unlock(&priv->reg_mutex);
    }

    return (ret == RT_ERR_OK) ? 0 : -EIO;
}

static int rtl8366ub_sw_port_fdb_add(struct dsa_switch *ds, int port,
                                     const unsigned char *addr, u16 vid,
                                     struct dsa_db db)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_l2_ucastAddr_t l2;
    rtksw_mac_t mac;
    rtksw_api_ret_t ret;

    memcpy(mac.octet, addr, ETH_ALEN);

    memset(&l2, 0, sizeof(l2));
    l2.port = port;
    l2.is_static = 1;
    if (vid) {
        l2.ivl = 1;             /* IVL: entry keyed on (MAC, cvid) */
        l2.cvid = vid;
    } else {
        l2.ivl = 0;             /* SVL: entry keyed on (MAC, fid 0) */
        l2.fid = 0;
    }

    mutex_lock(&priv->reg_mutex);
    ret = rtk_l2_addr_add(&mac, &l2);
    mutex_unlock(&priv->reg_mutex);

    return (ret == RT_ERR_OK) ? 0 : -EIO;
}

static int rtl8366ub_sw_port_fdb_del(struct dsa_switch *ds, int port,
                                     const unsigned char *addr, u16 vid,
                                     struct dsa_db db)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_l2_ucastAddr_t l2;
    rtksw_mac_t mac;
    rtksw_api_ret_t ret;

    memcpy(mac.octet, addr, ETH_ALEN);

    memset(&l2, 0, sizeof(l2));
    if (vid) {
        l2.ivl = 1;
        l2.cvid = vid;
    } else {
        l2.ivl = 0;
        l2.fid = 0;
    }

    mutex_lock(&priv->reg_mutex);
    ret = rtk_l2_addr_del(&mac, &l2);
    mutex_unlock(&priv->reg_mutex);

    /* A missing entry is not an error for an idempotent delete. */
    if (ret == RT_ERR_OK || ret == RT_ERR_L2_ENTRY_NOTFOUND)
        return 0;

    return -EIO;
}

static void rtl8366ub_sw_phylink_mac_link_up(struct dsa_switch *ds, int port,
                                             unsigned int mode,
                                             phy_interface_t interface,
                                             struct phy_device *phydev,
                                             int speed, int duplex,
                                             bool tx_pause, bool rx_pause)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_port_mac_ability_t mac_cfg;
    rtksw_port_linkStatus_t link = RTKSW_PORT_LINKDOWN;
    rtksw_port_speed_t phy_speed = RTKSW_PORT_SPEED_1000M;
    rtksw_port_duplex_t phy_duplex = RTKSW_PORT_FULL_DUPLEX;
    bool resolved;

    if (port >= RTL8366UB_NUM_PORTS)
        return;

    memset(&mac_cfg, 0, sizeof(mac_cfg));

    /*
     * The user-port copper PHYs are driven by generic C22 phylib, which tops
     * out at 1000BASE-T: the 2500BASE-T advertisement (OCP 0xA5D4) and
     * resolution (OCP 0xA434) live in vendor OCP space that C22 genphy never
     * reads, so @speed caps at 1000 even when the link actually ran at 2.5G.
     * Read the chip's own resolved speed/duplex and force the switch MAC to
     * the real rate; only fall back to auto-sync if that read fails (never
     * force a stale 1000, which would silently cap the port).
     */
    mutex_lock(&priv->reg_mutex);
    resolved = (rtk_port_phyStatus_get(port, &link, &phy_speed,
                                       &phy_duplex) == RT_ERR_OK) &&
               (link == RTKSW_PORT_LINKUP);

    if (resolved) {
        mac_cfg.forcemode = PORT_MAC_FORCE;
        mac_cfg.speed = phy_speed;
        mac_cfg.duplex = phy_duplex;
    } else {
        mac_cfg.forcemode = PORT_MAC_NORMAL;
        switch (speed) {
            case SPEED_10:   mac_cfg.speed = RTKSW_PORT_SPEED_10M; break;
            case SPEED_100:  mac_cfg.speed = RTKSW_PORT_SPEED_100M; break;
            case SPEED_2500: mac_cfg.speed = RTKSW_PORT_SPEED_2500M; break;
            case SPEED_1000:
            default:         mac_cfg.speed = RTKSW_PORT_SPEED_1000M; break;
        }
        mac_cfg.duplex = (duplex == DUPLEX_FULL) ?
            RTKSW_PORT_FULL_DUPLEX : RTKSW_PORT_HALF_DUPLEX;
    }

    mac_cfg.link = RTKSW_PORT_LINKUP;
    mac_cfg.nway = RTKSW_DISABLED;
    mac_cfg.txpause = tx_pause;
    mac_cfg.rxpause = rx_pause;

    rtk_port_macForceLink_set(port, &mac_cfg);
    mutex_unlock(&priv->reg_mutex);
}

static int rtl8366ub_sw_port_change_mtu(struct dsa_switch *ds, int port,
                                        int new_mtu)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_api_ret_t ret;
    u32 frame_size;

    /* MTU is the L3 payload; the switch polices the whole L2 frame, so add
     * Ethernet + one VLAN tag + FCS overhead (1500 -> 1522).
     */
    frame_size = new_mtu + VLAN_ETH_HLEN + ETH_FCS_LEN;

    /* Frames crossing the CPU/extension port additionally carry the 8-byte
     * rtl8_4 DSA tag, so that port must accept the extra bytes. DSA drives
     * this op for each user port (dp->index) and, via the MTU notifier, for
     * the CPU port with cpu_mtu == largest user MTU (tag NOT included), so we
     * add it here.
     */
    if (dsa_is_cpu_port(ds, port))
        frame_size += RTL8366UB_CPU_TAG_LEN;

    mutex_lock(&priv->reg_mutex);
    ret = rtk_port_maxPacketLength_set(port, frame_size);
    mutex_unlock(&priv->reg_mutex);

    return (ret == RT_ERR_OK) ? 0 : -EIO;
}

static int rtl8366ub_sw_port_max_mtu(struct dsa_switch *ds, int port)
{
    /* Largest L3 MTU that still fits the hardware frame ceiling once the L2
     * overhead and the rtl8_4 CPU tag are accounted for. DSA clamps this
     * against the conduit's own max_mtu.
     */
    return RTL8366UB_MAX_PKT_LEN - VLAN_ETH_HLEN - ETH_FCS_LEN -
           RTL8366UB_CPU_TAG_LEN;
}

/*
 * Per-port hardware MIB counters exposed through ethtool -S, mirroring the
 * in-tree Realtek DSA drivers (rtl8365mb/rtl8366rb). The SDK's stat module is
 * compiled into the module (stat.o + dal/rtl8371c/dal_rtl8371c_stat.o), and
 * rtk_stat_port_get() L2P-maps the port internally. These ops run in process
 * (rtnl) context, where the SDK's sleeping MDIO access is legal; reg_mutex is
 * held to match the driver's locking. get_stats64 is deliberately NOT provided:
 * it runs in atomic context and the SDK MDIO path sleeps.
 */
struct rtl8366ub_mib_counter {
    const char *name;
    rtksw_stat_port_type_t idx;
};

static const struct rtl8366ub_mib_counter rtl8366ub_mib_counters[] = {
    { "IfInOctets",              STAT_IfInOctets },
    { "IfInUcastPkts",           STAT_IfInUcastPkts },
    { "IfInMulticastPkts",       STAT_IfInMulticastPkts },
    { "IfInBroadcastPkts",       STAT_IfInBroadcastPkts },
    { "Dot3StatsFCSErrors",      STAT_Dot3StatsFCSErrors },
    { "Dot3StatsSymbolErrors",   STAT_Dot3StatsSymbolErrors },
    { "EtherStatsDropEvents",    STAT_EtherStatsDropEvents },
    { "EtherStatsUnderSizePkts", STAT_EtherStatsUnderSizePkts },
    { "EtherOversizeStats",      STAT_EtherOversizeStats },
    { "EtherStatsFragments",     STAT_EtherStatsFragments },
    { "EtherStatsJabbers",       STAT_EtherStatsJabbers },
    { "Dot3InPauseFrames",       STAT_Dot3InPauseFrames },
    { "IfOutOctets",             STAT_IfOutOctets },
    { "IfOutUcastPkts",          STAT_IfOutUcastPkts },
    { "IfOutMulticastPkts",      STAT_IfOutMulticastPkts },
    { "IfOutBroadcastPkts",      STAT_IfOutBroadcastPkts },
    { "EtherStatsCollisions",    STAT_EtherStatsCollisions },
    { "Dot3OutPauseFrames",      STAT_Dot3OutPauseFrames },
    { "IfOutDiscards",           STAT_IfOutDiscards },
};

#define RTL8366UB_MIB_COUNT ARRAY_SIZE(rtl8366ub_mib_counters)

static void rtl8366ub_sw_get_strings(struct dsa_switch *ds, int port,
                                     u32 stringset, u8 *data)
{
    unsigned int i;

    if (stringset != ETH_SS_STATS)
        return;

    for (i = 0; i < RTL8366UB_MIB_COUNT; i++)
        ethtool_puts(&data, rtl8366ub_mib_counters[i].name);
}

static int rtl8366ub_sw_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
    if (sset != ETH_SS_STATS)
        return -EOPNOTSUPP;

    return RTL8366UB_MIB_COUNT;
}

static void rtl8366ub_sw_get_ethtool_stats(struct dsa_switch *ds, int port,
                                           uint64_t *data)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_stat_counter_t val;
    unsigned int i;

    mutex_lock(&priv->reg_mutex);
    for (i = 0; i < RTL8366UB_MIB_COUNT; i++) {
        if (rtk_stat_port_get(port, rtl8366ub_mib_counters[i].idx,
                              &val) == RT_ERR_OK)
            data[i] = val;
        else
            data[i] = 0;
    }
    mutex_unlock(&priv->reg_mutex);
}

/*
 * Concurrency and MDIO-bus locking - verified correct, documented so neither
 * is "fixed" into a regression:
 *
 *   (a) The SDK's global rtksw_api_mutex that serialises every rtk_*() call is
 *       DEFINE_MUTEX(rtksw_api_mutex) at rtk_switch.c:35 (declared extern
 *       struct mutex at rtk_switch.h:46). It is statically initialised - do
 *       NOT add a mutex_init() for it and do NOT try to "cover" it with
 *       priv->reg_mutex. In the kernel build RTK_X86_CLE is undefined, so
 *       RTKSW_API_LOCK expands to mutex_lock(&rtksw_api_mutex) (not pthread):
 *       every rtk_* entry point the driver calls holds it around register
 *       access, so all chip access is globally serialised. Lock ordering is
 *       consistent (priv->reg_mutex is always outer, rtksw_api_mutex always
 *       inner) so there is no AB-BA deadlock.
 *
 *   (b) dal/smi.c holds __mii_bus->mdio_lock across the whole indirect
 *       transaction (rtlglue_drvMutexLock -> mutex_lock(&__mii_bus->mdio_lock))
 *       and issues it via the __mdiobus_* (unlocked) accessors - exactly the
 *       in-tree realtek-mdio.c pattern. That is what keeps switch indirect
 *       access atomic vs the WAN c45 PHY at mdio addr 15 on the shared bus.
 *       Do NOT "simplify" smi.c to the self-locking mdiobus_read/write: that
 *       would drop the lock between the address-latch write and the data read
 *       and let the WAN-PHY poll corrupt the indirect access.
 */

/*
 * CPU-tag / port-ID mapping - verified safe, documented so it is not "fixed"
 * into a blackout:
 *
 * tag_rtl8_4 (DSA_TAG_PROTO_RTL8_4) carries the switch port in the DSA
 * port-index number space: on RX it reads TX = source port and calls
 * dsa_conduit_find_user(dev, 0, port) (matches dp->index); on TX it writes
 * RX mask = BIT(dp->index). This mirrors the in-tree rtl8365mb driver, where
 * dp->index IS the chip port number.
 *
 * On the RTL8371C the SDK logical<->physical map is the IDENTITY for the UTP
 * ports: logical 0..3 -> physical 0..3. The two MT5000 user ports are DTS
 * port@0 (lan1) and port@1 (lan2), i.e. dp->index 0/1 == chip physical 0/1.
 * So the chip emits TX=0/1 for frames from lan1/lan2 and honours RX mask
 * BIT(0)/BIT(1) to egress on lan1/lan2 - exactly what the tagger assumes.
 *
 * The CPU port is DTS port@17 = EXT_PORT1 (logical 17), which the SDK maps to
 * physical 7. This logical!=physical divergence NEVER enters the tag TX/RX
 * fields for user traffic (the CPU port is never a tag source or an xmit
 * destination), so it is harmless. Do NOT "correct" the CPU port to reg=7:
 * rtl8366ub_find_cpu_port() only accepts EXT_PORT0(16)/EXT_PORT1(17), and the
 * SDK performs the 17->7 translation internally.
 */
static const struct dsa_switch_ops rtl8366ub_switch_ops = {
    .get_tag_protocol = rtl8366ub_sw_get_tag_protocol,
    .setup = rtl8366ub_sw_setup,
    .phy_read = rtl8366ub_sw_phy_read,
    .phy_write = rtl8366ub_sw_phy_write,
    .port_enable = rtl8366ub_sw_port_enable,
    .port_disable = rtl8366ub_sw_port_disable,
    .port_stp_state_set = rtl8366ub_sw_stp_state_set,
    .port_bridge_join = rtl8366ub_sw_port_bridge_join,
    .port_bridge_leave = rtl8366ub_sw_port_bridge_leave,
    .port_vlan_filtering = rtl8366ub_sw_port_vlan_filtering,
    .port_vlan_add = rtl8366ub_sw_port_vlan_add,
    .port_vlan_del = rtl8366ub_sw_port_vlan_del,
    .phylink_get_caps = rtl8366ub_sw_phylink_get_caps,
    .phylink_mac_config = rtl8366ub_sw_phylink_mac_config,
    .phylink_mac_link_down = rtl8366ub_sw_phylink_mac_link_down,
    .phylink_mac_link_up = rtl8366ub_sw_phylink_mac_link_up,
    .port_fast_age = rtl8366ub_sw_port_fast_age,
    .port_pre_bridge_flags = rtl8366ub_sw_port_pre_bridge_flags,
    .port_bridge_flags = rtl8366ub_sw_port_bridge_flags,
    .port_fdb_add = rtl8366ub_sw_port_fdb_add,
    .port_fdb_del = rtl8366ub_sw_port_fdb_del,
    .port_change_mtu = rtl8366ub_sw_port_change_mtu,
    .port_max_mtu = rtl8366ub_sw_port_max_mtu,
    .get_strings = rtl8366ub_sw_get_strings,
    .get_ethtool_stats = rtl8366ub_sw_get_ethtool_stats,
    .get_sset_count = rtl8366ub_sw_get_sset_count,
};

static int port_isolation_show(struct seq_file *s, void *v)
{
    rtksw_portmask_t mask;
    int i;

    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        rtk_port_isolation_get(i, &mask);
        seq_printf(s, "port%d: 0x%x\n", i, mask.bits[0]);
    }

    return 0;
}
DEFINE_SHOW_ATTRIBUTE(port_isolation);

static ssize_t phy_reg_read(struct file *file,
                            const char __user *user_buf,
                            size_t count, loff_t *ppos)
{
    char buf[256] = "";
    unsigned int reg, val = 0;
    int phy;
    size_t len;

    len = min(count, sizeof(buf) - 1);
    if (copy_from_user(buf, user_buf, len))
        return -EFAULT;
    buf[len] = '\0';

    /* Expect "<phy> <hexreg>"; reject anything we cannot fully parse so we
     * never hand stack garbage to the DAL nor print an uninitialised value.
     * rtk_port_phyReg_get is a Clause-22 accessor (reg 0..0x1f), and 'val' is
     * read into an rtksw_port_phy_data_t (unsigned int), not a signed int.
     */
    if (sscanf(buf, "%d %x", &phy, &reg) != 2)
        return -EINVAL;
    if (phy < 0 || phy >= RTL8366UB_NUM_PORTS || reg > 0x1f)
        return -EINVAL;

    if (rtk_port_phyReg_get(phy, reg, &val) != RT_ERR_OK)
        return -EIO;

    pr_info("phy: %d, reg: 0x%x = 0x%x\n", phy, reg, val);

    return count;
}

static const struct file_operations phy_reg_fops = {
    .open = simple_open,
    .write = phy_reg_read,
};

static int rtl8366ub_mdio_probe(struct mdio_device *mdiodev)
{
    struct rtl8366ub_priv *priv;
    int ret;

    priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->ds = devm_kzalloc(&mdiodev->dev, sizeof(*priv->ds), GFP_KERNEL);
    if (!priv->ds)
        return -ENOMEM;

    dev_info(&mdiodev->dev, "RTL8366UB DSA driver, version %s, mdio addr %d\n",
             DRIVER_VERSION, mdiodev->addr);

    priv->reset = devm_gpiod_get_optional(&mdiodev->dev, "reset",
                                          GPIOD_OUT_LOW);
    if (IS_ERR(priv->reset)) {
        dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
        return PTR_ERR(priv->reset);
    }

    priv->bus = mdiodev->bus;
    priv->dev = &mdiodev->dev;
    priv->mdio_addr = mdiodev->addr;
    mutex_init(&priv->reg_mutex);
    /*
     * NOTE: the SDK's global rtksw_api_mutex that serialises every rtk_*()
     * call is a DEFINE_MUTEX() in rtk_switch.c (statically initialised) - do
     * NOT add a mutex_init() for it. There is no uninitialised-mutex/crash
     * risk here.
     */
    dev_set_drvdata(&mdiodev->dev, priv);

    priv->ds->dev = &mdiodev->dev;
    priv->ds->num_ports = EXT_PORT1 + 1;
    priv->ds->priv = priv;
    priv->ds->ops = &rtl8366ub_switch_ops;

    /* User ports 0 and 1 carry the integrated PHYs. Without this mask the
     * DSA fallback user MII bus sets phy_mask = ~phys_mii_mask = ~0 and scans
     * no addresses, so lan1/lan2 PHYs never attach and registration aborts.
     */
    priv->ds->phys_mii_mask = BIT(UTP_PORT0) | BIT(UTP_PORT1);

    /* Commit bridge VLANs to hardware even while a bridge is not in
     * vlan_filtering mode, so toggling vlan_filtering later cannot desync the
     * chip VLAN table from the bridge's view.
     */
    priv->ds->configure_vlan_while_not_filtering = true;

    /* The rtl8_4 tagger sets LEARN_DIS on every CPU-injected frame
     * (tag_rtl8_4.c), so the switch never learns the router/host MAC by
     * itself and would flood all replies. Let DSA software-learn conduit
     * source MACs and install them as static entries via .port_fdb_add on
     * the CPU port.
     */
    priv->ds->assisted_learning_on_cpu_port = true;

    rtk_set_mdc_mdio(priv->bus, priv->mdio_addr);

    ret = dsa_register_switch(priv->ds);
    if (ret) {
        if (ret == -EPROBE_DEFER)
            dev_err(priv->dev, "unable to register switch, deferred\n");
        else
            dev_err(priv->dev, "unable to register switch ret = %d\n", ret);
        return ret;
    }

    priv->dbgfs = debugfs_create_dir("rtl8366ub", NULL);
    debugfs_create_file("port_isolation", S_IRUSR, priv->dbgfs, NULL,
                        &port_isolation_fops);
    debugfs_create_file("phy_reg", S_IWUSR, priv->dbgfs, NULL, &phy_reg_fops);

    return 0;
}

static void rtl8366ub_mdio_remove(struct mdio_device *mdiodev)
{
    struct rtl8366ub_priv *priv = dev_get_drvdata(&mdiodev->dev);

    if (!priv)
        return;

    debugfs_remove_recursive(priv->dbgfs);
    dsa_unregister_switch(priv->ds);
}

static void rtl8366ub_mdio_shutdown(struct mdio_device *mdiodev)
{
    struct rtl8366ub_priv *priv = dev_get_drvdata(&mdiodev->dev);

    if (!priv)
        return;

    dsa_switch_shutdown(priv->ds);

    dev_set_drvdata(&mdiodev->dev, NULL);
}

static const struct of_device_id rtl8366ub_mdio_of_match[] = {
    { .compatible = "realtek,rtl8366ub" },
    { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rtl8366ub_mdio_of_match);

static struct mdio_driver rtl8366ub_mdio_driver = {
    .mdiodrv.driver = {
        .name = "rtl8366ub-mdio",
        .of_match_table = rtl8366ub_mdio_of_match,
    },
    .probe  = rtl8366ub_mdio_probe,
    .remove = rtl8366ub_mdio_remove,
    .shutdown = rtl8366ub_mdio_shutdown,
};

mdio_module_driver(rtl8366ub_mdio_driver);

MODULE_AUTHOR("Jianhui Zhao <jianhui.zhao@gl-inet.com>");
MODULE_DESCRIPTION("DSA driver for the RTL8371C (RTL8366UB) 2.5G switch");
MODULE_LICENSE("GPL");
