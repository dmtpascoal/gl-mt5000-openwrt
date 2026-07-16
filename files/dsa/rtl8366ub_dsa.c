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
#include <linux/debugfs.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/bitops.h>
#include <linux/regmap.h>
#include <linux/phylink.h>
#include <net/dsa.h>
#include <net/switchdev.h>
#include <linux/gpio/consumer.h>

#include "rtk_switch.h"
#include "port.h"
#include "vlan.h"
#include "cpu.h"
#include "rtl8366ub_dsa.h"
#include "dal/smi.h"

extern void rtk_set_mdc_mdio(struct mii_bus *bus, int id);

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
    }
    rtk_port_macForceLinkExt_set(port, mode_ext, &mac_cfg);
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
    }
    rtk_port_isolation_set(EXT_PORT0, &portmask);
    rtk_port_isolation_set(priv->cpu_port, &portmask);

    rtl8366ub_cpu_mac_config(ds, priv->cpu_port, interface);
    rtk_cpu_tagPort_set(priv->cpu_port, CPU_INSERT_TO_ALL);
    rtk_cpu_enable_set(RTKSW_ENABLED);

    rtk_port_phyEnableAll_set(RTKSW_ENABLED);
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
    rtksw_portmask_t portmask_tmp;
    rtksw_portmask_t portmask;
    int i;

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

    return 0;
}

static void rtl8366ub_sw_port_bridge_leave(struct dsa_switch *ds, int port,
                                           struct dsa_bridge bridge)
{
    rtksw_portmask_t portmask_tmp;
    rtksw_portmask_t portmask;
    int i;

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

static void rtl8366ub_sw_phylink_mac_link_up(struct dsa_switch *ds, int port,
                                             unsigned int mode,
                                             phy_interface_t interface,
                                             struct phy_device *phydev,
                                             int speed, int duplex,
                                             bool tx_pause, bool rx_pause)
{
    rtksw_port_mac_ability_t mac_cfg;

    if (port >= RTL8366UB_NUM_PORTS)
        return;

    switch (speed) {
        case SPEED_10:
            mac_cfg.speed = RTKSW_PORT_SPEED_10M;
            break;
        case SPEED_100:
            mac_cfg.speed = RTKSW_PORT_SPEED_100M;
            break;
        case SPEED_1000:
            mac_cfg.speed = RTKSW_PORT_SPEED_1000M;
            break;
        case SPEED_2500:
            mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
            break;
        default:
            mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
    }
    mac_cfg.forcemode = PORT_MAC_NORMAL;
    mac_cfg.duplex = duplex;
    mac_cfg.link = RTKSW_PORT_LINKUP;
    mac_cfg.nway = RTKSW_DISABLED;
    mac_cfg.txpause = tx_pause;
    mac_cfg.rxpause = rx_pause;

    rtk_port_macForceLink_set(port, &mac_cfg);
}

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
    int phy, reg, val;

    if (copy_from_user(buf, user_buf, min(count, sizeof(buf) - 1)))
        return -EFAULT;

    sscanf(buf, "%d %x\n", &phy, (unsigned int *)&reg);

    rtk_port_phyReg_get(phy, reg, &val);

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

