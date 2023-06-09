// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#include <linux/of_device.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/if_vlan.h>
#include <linux/reset.h>
#include <linux/tcp.h>
#include <linux/interrupt.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/phylink.h>

#include "mtk_eth_soc.h"

#define MTK_QDMA_INT_STATUS		0x4050
#define MTK_QDMA_INT_MASK		0x4054
// MTK_QDMA_INT_MASK bits.
#define INT_STATUS_HWFWD_DSCP_LOW	BIT(10)
#define INT_STATUS_IRQ_FULL		BIT(9)
#define INT_STATUS_HWFWD_DSCP_EMPTY	BIT(8)
#define INT_STATUS_NO_RX0_CPU_DSCP      BIT(3)
#define INT_STATUS_NO_TX0_CPU_DSCP	BIT(2)
#define INT_STATUS_RX0_DONE		BIT(1)
#define INT_STATUS_TX0_DONE		BIT(0)

#define MTK_MAC_MCR(x)         (0xb000 + (x * 0x100))

static int mtk_msg_level = -1;
module_param_named(msg_level, mtk_msg_level, int, 0);
MODULE_PARM_DESC(msg_level, "Message level (-1=defaults,0=none,...,16=all)");

void mtk_w32(struct mtk_eth *eth, u32 val, unsigned reg)
{
	__raw_writel(val, eth->base + reg);
}

u32 mtk_r32(struct mtk_eth *eth, unsigned reg)
{
	return __raw_readl(eth->base + reg);
}

static u32 mtk_m32(struct mtk_eth *eth, u32 mask, u32 set, unsigned reg)
{
	u32 val;

	val = mtk_r32(eth, reg);
	val &= ~mask;
	val |= set;
	mtk_w32(eth, val, reg);
	return reg;
}

static int mtk_mdio_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;

	while (1) {
		if (!(mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_ACCESS))
			return 0;
		if (time_after(jiffies, t_start + PHY_IAC_TIMEOUT))
			break;
		usleep_range(10, 20);
	}

	dev_err(eth->dev, "mdio: MDIO timeout\n");
	return -1;
}

#define MTK_PHY_IAC		0xf01c

static u32 _mtk_mdio_write(struct mtk_eth *eth, u32 phy_addr,
			   u32 phy_register, u32 write_data)
{
	if (mtk_mdio_busy_wait(eth))
		return -1;

	write_data &= 0xffff;

	mtk_w32(eth, PHY_IAC_ACCESS | PHY_IAC_START | PHY_IAC_WRITE |
		(phy_register << PHY_IAC_REG_SHIFT) |
		(phy_addr << PHY_IAC_ADDR_SHIFT) | write_data,
		MTK_PHY_IAC);

	if (mtk_mdio_busy_wait(eth))
		return -1;

	return 0;
}

static u32 _mtk_mdio_read(struct mtk_eth *eth, int phy_addr, int phy_reg)
{
	u32 d;

	if (mtk_mdio_busy_wait(eth))
		return 0xffff;

	mtk_w32(eth, PHY_IAC_ACCESS | PHY_IAC_START | PHY_IAC_READ |
		(phy_reg << PHY_IAC_REG_SHIFT) |
		(phy_addr << PHY_IAC_ADDR_SHIFT),
		MTK_PHY_IAC);

	if (mtk_mdio_busy_wait(eth))
		return 0xffff;

	d = mtk_r32(eth, MTK_PHY_IAC) & 0xffff;

	return d;
}

static int mtk_mdio_write(struct mii_bus *bus, int phy_addr,
			  int phy_reg, u16 val)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_write(eth, phy_addr, phy_reg, val);
}

static int mtk_mdio_read(struct mii_bus *bus, int phy_addr, int phy_reg)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_read(eth, phy_addr, phy_reg);
}

static void mtk_mac_config(struct phylink_config *config, unsigned int mode,
			   const struct phylink_link_state *state)
{

}

static void mtk_mac_an_restart(struct phylink_config *config)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);

	mtk_sgmii_restart_an(mac->hw, mac->id);
}

static void mtk_mac_link_down(struct phylink_config *config, unsigned int mode,
			      phy_interface_t interface)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);

	u32 mcr = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));

	mcr &= ~(MAC_MCR_TX_EN | MAC_MCR_RX_EN);
	mtk_w32(mac->hw, mcr, MTK_MAC_MCR(mac->id));
}

static void mtk_mac_link_up(struct phylink_config *config,
			    struct phy_device *phy,
			    unsigned int mode, phy_interface_t interface,
			    int speed, int duplex, bool tx_pause, bool rx_pause)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);

	// TODO Looks like mac->id should be 5 and 6.

#if 0	
	u32 mcr = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));

	mcr &= ~(MAC_MCR_SPEED_100 | MAC_MCR_SPEED_1000 |
		 MAC_MCR_FORCE_DPX | MAC_MCR_FORCE_TX_FC |
		 MAC_MCR_FORCE_RX_FC);

	/* Configure speed */
	switch (speed) {
	case SPEED_2500:
	case SPEED_1000:
		mcr |= MAC_MCR_SPEED_1000;
		break;
	case SPEED_100:
		mcr |= MAC_MCR_SPEED_100;
		break;
	}

	/* Configure duplex */
	if (duplex == DUPLEX_FULL)
		mcr |= MAC_MCR_FORCE_DPX;

	/* Configure pause modes - phylink will avoid these for half duplex */
	if (tx_pause)
		mcr |= MAC_MCR_FORCE_TX_FC;
	if (rx_pause)
		mcr |= MAC_MCR_FORCE_RX_FC;

	mcr |= MAC_MCR_TX_EN | MAC_MCR_RX_EN;
	mtk_w32(mac->hw, mcr, MTK_MAC_MCR(mac->id));
#endif
}

static void mtk_validate(struct phylink_config *config,
			 unsigned long *supported,
			 struct phylink_link_state *state)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	if (state->interface != PHY_INTERFACE_MODE_NA &&
	    state->interface != PHY_INTERFACE_MODE_MII &&
	    state->interface != PHY_INTERFACE_MODE_GMII &&
	    !(MTK_HAS_CAPS(mac->hw->soc->caps, MTK_RGMII) &&
	      phy_interface_mode_is_rgmii(state->interface)) &&
	    !(MTK_HAS_CAPS(mac->hw->soc->caps, MTK_TRGMII) &&
	      !mac->id && state->interface == PHY_INTERFACE_MODE_TRGMII) &&
	    !(MTK_HAS_CAPS(mac->hw->soc->caps, MTK_SGMII) &&
	      (state->interface == PHY_INTERFACE_MODE_SGMII ||
	       phy_interface_mode_is_8023z(state->interface)))) {
		linkmode_zero(supported);
		return;
	}

	phylink_set_port_modes(mask);
	phylink_set(mask, Autoneg);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_TRGMII:
		phylink_set(mask, 1000baseT_Full);
		break;
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		phylink_set(mask, 1000baseX_Full);
		phylink_set(mask, 2500baseX_Full);
		break;
	case PHY_INTERFACE_MODE_GMII:
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		phylink_set(mask, 1000baseT_Half);
		fallthrough;
	case PHY_INTERFACE_MODE_SGMII:
		phylink_set(mask, 1000baseT_Full);
		phylink_set(mask, 1000baseX_Full);
		fallthrough;
	case PHY_INTERFACE_MODE_MII:
	case PHY_INTERFACE_MODE_RMII:
	case PHY_INTERFACE_MODE_REVMII:
	case PHY_INTERFACE_MODE_NA:
	default:
		phylink_set(mask, 10baseT_Half);
		phylink_set(mask, 10baseT_Full);
		phylink_set(mask, 100baseT_Half);
		phylink_set(mask, 100baseT_Full);
		break;
	}

	if (state->interface == PHY_INTERFACE_MODE_NA) {
		if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_SGMII)) {
			phylink_set(mask, 1000baseT_Full);
			phylink_set(mask, 1000baseX_Full);
			phylink_set(mask, 2500baseX_Full);
		}
		if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_RGMII)) {
			phylink_set(mask, 1000baseT_Full);
			phylink_set(mask, 1000baseT_Half);
			phylink_set(mask, 1000baseX_Full);
		}
		if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_GEPHY)) {
			phylink_set(mask, 1000baseT_Full);
			phylink_set(mask, 1000baseT_Half);
		}
	}

	phylink_set(mask, Pause);
	phylink_set(mask, Asym_Pause);

	linkmode_and(supported, supported, mask);
	linkmode_and(state->advertising, state->advertising, mask);

	/* We can only operate at 2500BaseX or 1000BaseX. If requested
	 * to advertise both, only report advertising at 2500BaseX.
	 */
	phylink_helper_basex_speed(state);
}

static void mtk_mac_pcs_get_state(struct phylink_config *config,
                                 struct phylink_link_state *state)
{
       struct mtk_mac *mac = container_of(config, struct mtk_mac,
                                          phylink_config);
       u32 pmsr = mtk_r32(mac->hw, MTK_MAC_MSR(mac->id));

       state->link = (pmsr & MAC_MSR_LINK);
       state->duplex = (pmsr & MAC_MSR_DPX) >> 1;

       switch (pmsr & (MAC_MSR_SPEED_1000 | MAC_MSR_SPEED_100)
) {
       case 0:
               state->speed = SPEED_10;
               break;
       case MAC_MSR_SPEED_100:
               state->speed = SPEED_100;
               break;
       case MAC_MSR_SPEED_1000:
               state->speed = SPEED_1000;
               break;
       default:
               state->speed = SPEED_UNKNOWN;
               break;
       }

       state->pause &= (MLO_PAUSE_RX | MLO_PAUSE_TX);
       if (pmsr & MAC_MSR_RX_FC)
               state->pause |= MLO_PAUSE_RX;
       if (pmsr & MAC_MSR_TX_FC)
               state->pause |= MLO_PAUSE_TX;
 }


static const struct phylink_mac_ops mtk_phylink_ops = {
	.validate = mtk_validate,
	.mac_pcs_get_state = mtk_mac_pcs_get_state,
	.mac_an_restart = mtk_mac_an_restart,
	.mac_config = mtk_mac_config,
	.mac_link_down = mtk_mac_link_down,
	.mac_link_up = mtk_mac_link_up,
};

static int mtk_mdio_init(struct mtk_eth *eth)
{
	struct device_node *mii_np;
	int ret;

	mii_np = of_get_child_by_name(eth->dev->of_node, "mdio-bus");
	if (!mii_np) {
		dev_err(eth->dev, "no %s child node found", "mdio-bus");
		return -ENODEV;
	}

	if (!of_device_is_available(mii_np)) {
		ret = -ENODEV;
		goto err_put_node;
	}

	eth->mii_bus = devm_mdiobus_alloc(eth->dev);
	if (!eth->mii_bus) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	eth->mii_bus->name = "mdio";
	eth->mii_bus->read = mtk_mdio_read;
	eth->mii_bus->write = mtk_mdio_write;
	eth->mii_bus->priv = eth;
	eth->mii_bus->parent = eth->dev;

	snprintf(eth->mii_bus->id, MII_BUS_ID_SIZE, "%pOFn", mii_np);
	ret = of_mdiobus_register(eth->mii_bus, mii_np);

err_put_node:
	of_node_put(mii_np);
	return ret;
}

static void mtk_mdio_cleanup(struct mtk_eth *eth)
{
	if (!eth->mii_bus)
		return;

	mdiobus_unregister(eth->mii_bus);
}

static inline void mtk_tx_irq_disable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->tx_irq_lock, flags);
	val = mtk_r32(eth, eth->tx_int_mask_reg);
	mtk_w32(eth, val & ~mask, eth->tx_int_mask_reg);
	spin_unlock_irqrestore(&eth->tx_irq_lock, flags);
}

static inline void mtk_tx_irq_enable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->tx_irq_lock, flags);
	val = mtk_r32(eth, eth->tx_int_mask_reg);
	mtk_w32(eth, val | mask, eth->tx_int_mask_reg);
	spin_unlock_irqrestore(&eth->tx_irq_lock, flags);
}

static inline void mtk_rx_irq_disable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->rx_irq_lock, flags);
	val = mtk_r32(eth, MTK_PDMA_INT_MASK);
	mtk_w32(eth, val & ~mask, MTK_PDMA_INT_MASK);
	spin_unlock_irqrestore(&eth->rx_irq_lock, flags);
}

static inline void mtk_rx_irq_enable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->rx_irq_lock, flags);
	val = mtk_r32(eth, MTK_PDMA_INT_MASK);
	mtk_w32(eth, val | mask, MTK_PDMA_INT_MASK);
	spin_unlock_irqrestore(&eth->rx_irq_lock, flags);
}

static int mtk_set_mac_address(struct net_device *dev, void *p)
{
	int ret = eth_mac_addr(dev, p);
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const char *macaddr = dev->dev_addr;
	
	if (ret)
		return ret;

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	spin_lock_bh(&mac->hw->page_lock);
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		mtk_w32(mac->hw, (macaddr[0] << 8) | macaddr[1],
			MT7628_SDM_MAC_ADRH);
		mtk_w32(mac->hw, (macaddr[2] << 24) | (macaddr[3] << 16) |
			(macaddr[4] << 8) | macaddr[5],
			MT7628_SDM_MAC_ADRL);
	} else {
		mtk_w32(mac->hw, (macaddr[0] << 8) | macaddr[1],
			MTK_GDMA_MAC_ADRH(mac->id));
		mtk_w32(mac->hw, (macaddr[2] << 24) | (macaddr[3] << 16) |
			(macaddr[4] << 8) | macaddr[5],
			MTK_GDMA_MAC_ADRL(mac->id));
	}
	spin_unlock_bh(&mac->hw->page_lock);

	return 0;
}

void mtk_stats_update_mac(struct mtk_mac *mac)
{
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	unsigned int base = MTK_GDM1_TX_GBCNT;
	u64 stats;

	base += hw_stats->reg_offset;

	u64_stats_update_begin(&hw_stats->syncp);

	hw_stats->rx_bytes += mtk_r32(mac->hw, base);
	stats =  mtk_r32(mac->hw, base + 0x04);
	if (stats)
		hw_stats->rx_bytes += (stats << 32);
	hw_stats->rx_packets += mtk_r32(mac->hw, base + 0x08);
	hw_stats->rx_overflow += mtk_r32(mac->hw, base + 0x10);
	hw_stats->rx_fcs_errors += mtk_r32(mac->hw, base + 0x14);
	hw_stats->rx_short_errors += mtk_r32(mac->hw, base + 0x18);
	hw_stats->rx_long_errors += mtk_r32(mac->hw, base + 0x1c);
	hw_stats->rx_checksum_errors += mtk_r32(mac->hw, base + 0x20);
	hw_stats->rx_flow_control_packets +=
					mtk_r32(mac->hw, base + 0x24);
	hw_stats->tx_skip += mtk_r32(mac->hw, base + 0x28);
	hw_stats->tx_collisions += mtk_r32(mac->hw, base + 0x2c);
	hw_stats->tx_bytes += mtk_r32(mac->hw, base + 0x30);
	stats =  mtk_r32(mac->hw, base + 0x34);
	if (stats)
		hw_stats->tx_bytes += (stats << 32);
	hw_stats->tx_packets += mtk_r32(mac->hw, base + 0x38);
	u64_stats_update_end(&hw_stats->syncp);
}

static void mtk_get_stats64(struct net_device *dev,
			    struct rtnl_link_stats64 *storage)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	unsigned int start;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock_bh(&hw_stats->stats_lock)) {
			mtk_stats_update_mac(mac);
			spin_unlock_bh(&hw_stats->stats_lock);
		}
	}

	do {
		start = u64_stats_fetch_begin_irq(&hw_stats->syncp);
		storage->rx_packets = hw_stats->rx_packets;
		storage->tx_packets = hw_stats->tx_packets;
		storage->rx_bytes = hw_stats->rx_bytes;
		storage->tx_bytes = hw_stats->tx_bytes;
		storage->collisions = hw_stats->tx_collisions;
		storage->rx_length_errors = hw_stats->rx_short_errors +
			hw_stats->rx_long_errors;
		storage->rx_over_errors = hw_stats->rx_overflow;
		storage->rx_crc_errors = hw_stats->rx_fcs_errors;
		storage->rx_errors = hw_stats->rx_checksum_errors;
		storage->tx_aborted_errors = hw_stats->tx_skip;
	} while (u64_stats_fetch_retry_irq(&hw_stats->syncp, start));

	storage->tx_errors = dev->stats.tx_errors;
	storage->rx_dropped = dev->stats.rx_dropped;
	storage->tx_dropped = dev->stats.tx_dropped;
}

typedef unsigned long int uint32;

/* Big endian. */
typedef struct {
        uint    resv1 ;
        struct {
                uint done                               : 1 ;
                uint drop_pkt                   : 1 ;
                uint resv2                              : 14 ;
                uint pkt_len                    : 16 ;
        } ctrl ;
        uint    pkt_addr ;
        uint resv3                                      : 20 ;
        uint next_idx                           : 12 ;
        uint msg[4] ;
} QDMA_DMA_DSCP_T ;

typedef union {
        struct {
                uint32 resv1            : 1;
                uint32 tsid             : 5;
                uint32 tse                      : 1;
                uint32 dei                      : 1;
                uint32 resv2            : 12;
                uint32 oam                      : 1;
                uint32 channel          : 8;
                uint32 queue            : 3;
                
                uint32 ico                      : 1;
                uint32 uco                      : 1;
                uint32 tco                      : 1;
                uint32 tso                      : 1;
                uint32 resv3            : 6;
                uint32 fport            : 3;
                uint32 vlanEn           : 1;
                uint32 vlanTpID         : 2;
                uint32 vlanTag          : 16;
        } raw ;
        uint msg[2] ;
} ETH_TX_MSG_T; // The name in 7512 header is ethTxMsg_t.

typedef struct {
        uint    pkt_addr ;
        struct {
#ifdef __BIG_ENDIAN
                uint ctx                                : 1 ;
                uint resv                               : 2 ;
                uint ctx_ring                   : 1 ;
                uint ctx_idx                    : 12 ;
                uint pkt_len                    : 16 ;
#else
                uint pkt_len                    : 16 ;
                uint ctx_idx                    : 12 ;
                uint ctx_ring                   : 1 ;
                uint resv                               : 2 ;
                uint ctx                                : 1 ;
#endif /* __BIG_ENDIAN */
        } ctrl ;
        uint msg[2] ;
} QDMA_HWFWD_DMA_DSCP_T ;

#define TX0_DSCP_NUM	4
#define RX0_DSCP_NUM	4
#define DSCP_NUM	(TX0_DSCP_NUM + RX0_DSCP_NUM)
#define HWFWD_DSCP_NUM	8

static QDMA_HWFWD_DMA_DSCP_T *hw_fwd_ary = NULL;
static void *hw_fwd_buff = NULL;

static QDMA_DMA_DSCP_T *dscp_ary = NULL;
static struct sk_buff *dscp_sk_buff_p_ary[DSCP_NUM];

static uint32 *irq_queue = NULL;

#define QDMA_CSR_TX_DSCP_BASE	0x4008
#define QDMA_CSR_RX_DSCP_BASE	0x400C
#define QDMA_CSR_RX_RING_CFG	0x4100
#define QDMA_CSR_RX_RING_THR	0x4104

#define QDMA_CSR_TX_CPU_IDX	0x4010
#define QDMA_CSR_TX_DMA_IDX	0x4014

#define QDMA_CSR_RX_CPU_IDX	0x4018
#define QDMA_CSR_RX_DMA_IDX	0x401C

#define QDMA_CSR_GLB_CFG	0x4004
#define GLB_CFG_TX_DMA_BUSY	(1 << 1)

/* #define DEBUG 1 */
/* #define TX_DEBUG 1 */
/* #define RX_DEBUG 1 */

static void print_dscp_ary(struct mtk_eth *eth) {
	int i;
	ETH_TX_MSG_T msg;
	QDMA_DMA_DSCP_T *dscp;
	
	for (i = 0; i < DSCP_NUM; i++) {
		dscp = &dscp_ary[i];
		msg.msg[0] = dscp->msg[0];
		msg.msg[1] = dscp->msg[1];
		printk("done = %d, next = %d, port = %d,"
		       "msg0 = %d, msg1 = %d, pkt_len = %d,"
		       "resv3 = %d, resv3 (int) = %d.",
		       dscp->ctrl.done, dscp->next_idx,
		       msg.raw.fport,
		       dscp->msg[0], dscp->msg[1],
		       dscp->ctrl.pkt_len,
		       msg.raw.resv3, dscp->resv3);
	}
}

static void print_hw_fwd_ary(struct mtk_eth *eth) {
        int i;
        QDMA_HWFWD_DMA_DSCP_T *p;
        
        for (i = 0; i < HWFWD_DSCP_NUM; i++) {
                p = &hw_fwd_ary[i];
                printk("pkt_addr = %x, pkt_len = %d, ctx = %d, "
		       "ctx_id = %d, msg0 = %x, msg1 = %x, "
		       "buf = %x, addr = %x, ctx_ring = %d.",
                       p->pkt_addr,
                       p->ctrl.pkt_len,
                       p->ctrl.ctx,
                       p->ctrl.ctx_idx,
                       p->msg[0],
                       p->msg[1],
                       mtk_r32(eth, 0x4024),
                       mtk_r32(eth, 0x4020),
                       p->ctrl.ctx_ring);
        }
}

void tx0_free_skb(struct mtk_eth *eth, int idx, QDMA_DMA_DSCP_T *dscp)
{
	struct sk_buff *skb;
	
	skb = dscp_sk_buff_p_ary[idx];
	dscp_sk_buff_p_ary[idx] = NULL;
	dma_map_single(eth->dev,
		       dscp->pkt_addr, skb_headlen(skb), DMA_FROM_DEVICE);
	dev_kfree_skb(skb);	
}

void tx0_free_some(struct mtk_eth *eth, QDMA_DMA_DSCP_T *dscp)
{
	int i, idx;
	
	for (i = 0; i < 2; i++) {
		idx = dscp->next_idx;
		dscp = &dscp_ary[idx];
		if (dscp->pkt_addr) {
			tx0_free_skb(eth, idx, dscp);
			dscp->pkt_addr = 0;
		}
		/* TODO: If done = 0, adjust drop counter. */
		
		/* Set done = 0. */
		memset(&dscp->ctrl, 0, sizeof(uint));
	}
}

/* TODO: Use mtk_busy_wait. */
static int qdma_busy_wait(struct mtk_eth *eth)
{
	int val;
	
	val = mtk_r32(eth, QDMA_CSR_GLB_CFG);
#if 0
	for (i = 0; (val & GLB_CFG_TX_DMA_BUSY) && i < 1000; i++) {
		val = mtk_r32(eth, QDMA_CSR_GLB_CFG);
	}
#endif	
	if (val & GLB_CFG_TX_DMA_BUSY) {
		printk("tx qdma is busy.");
		return -1;
	}
	return 0;
}

static int tx0_dscp_pkt_addr(struct mtk_eth *eth,
			     struct sk_buff *skb, int idx)
{
	dma_addr_t phys;
	
	phys = dma_map_single(eth->dev,
			      skb->data, skb_headlen(skb), DMA_TO_DEVICE);
	if (!phys) {
		return 0;
	}
	dscp_sk_buff_p_ary[idx] = skb;
	
	return phys;
}

static QDMA_DMA_DSCP_T *tx0_get_dscp(int idx)
{
	return &dscp_ary[idx];
}	

static int mtk_tx_map(struct sk_buff *skb, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	ETH_TX_MSG_T *tx_msg;
	QDMA_DMA_DSCP_T *dscp;
	int idx, val;

	if (qdma_busy_wait(eth)) {
		/* TODO: free skb. */
		return -1;
	}

#ifdef TX_DEBUG
	print_dscp_ary(eth);
	print_hw_fwd_ary(eth);
#endif
	
	idx = mtk_r32(eth, QDMA_CSR_TX_CPU_IDX) % TX0_DSCP_NUM;
#ifdef TX_DEBUG
	printk("(1) CPU idx %d, DMA idx %d.",
	       idx, mtk_r32(eth, QDMA_CSR_TX_DMA_IDX));
#endif		
	dscp = tx0_get_dscp(idx);

	tx_msg = (ETH_TX_MSG_T *) &dscp->msg;
	tx_msg->raw.fport = 1; /* GDM_P_GDMA1 */

	if (skb->len < 60) {
		skb_padto(skb, 60);
		skb_put(skb, 60 - skb->len);
	}
	
	dscp->pkt_addr = tx0_dscp_pkt_addr(eth, skb, idx);
	if (! dscp->pkt_addr) {
		/* free skb. */
		return -1;
	}
	dscp->ctrl.pkt_len = skb_headlen(skb);
	// dscp->ctrl.done = 0;

	/* QDMA_CSR_DMA_IDX will move to an element with
	   done = 0. If element is not found, `done` marking will stop.

	   Will become very busy, GLB_CFG_TX_DMA_BUSY, if it will not find 
	   a packet for sending. */

	/* TODO: We already freed some dscp's, no need to call this
	   everytime.
	   if (idx % 2) tx0_free_some(dscp); */
	tx0_free_some(eth, dscp);
	
	// wmb();
	
	mtk_w32(eth, dscp->next_idx, QDMA_CSR_TX_CPU_IDX);

#ifdef TX_DEBIG
	printk("(2) CPU idx %d, DMA idx %d, next_idx %d.",
	       mtk_r32(eth, QDMA_CSR_TX_CPU_IDX),
	       mtk_r32(eth, QDMA_CSR_TX_DMA_IDX),
	       dscp->next_idx);
#endif	
	return 0;
}

static void mtk_stop_queue(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		netif_stop_queue(eth->netdev[i]);
	}
}

static netdev_tx_t mtk_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct net_device_stats *stats = &dev->stats;
	
	/* normally we can rely on the stack not calling this more than once,
	 * however we have 2 queues running on the same ring so we need to lock
	 * the ring access
	 */
	spin_lock(&eth->page_lock);

	if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
		goto drop;


	if (mtk_tx_map(skb, dev) < 0)
		goto drop;

	spin_unlock(&eth->page_lock);

	return NETDEV_TX_OK;

drop:
	spin_unlock(&eth->page_lock);
	stats->tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static netdev_features_t mtk_fix_features(struct net_device *dev,
					  netdev_features_t features)
{
	return features;
}

static int mtk_set_features(struct net_device *dev, netdev_features_t features)
{
	int err = 0;
	return err;
}

/* wait for DMA to finish whatever it is doing before we start using it again */
static int mtk_dma_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;

	while (1) {
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
			if (!(mtk_r32(eth, MTK_QDMA_GLO_CFG) &
			      (MTK_RX_DMA_BUSY | MTK_TX_DMA_BUSY)))
				return 0;
		} else {
			if (!(mtk_r32(eth, MTK_PDMA_GLO_CFG) &
			      (MTK_RX_DMA_BUSY | MTK_TX_DMA_BUSY)))
				return 0;
		}

		if (time_after(jiffies, t_start + MTK_DMA_BUSY_TIMEOUT))
			break;
	}

	dev_err(eth->dev, "DMA init timeout\n");
	return -1;
}

static void mtk_dma_free(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++)
		if (eth->netdev[i])
			netdev_reset_queue(eth->netdev[i]);
}

static void mtk_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	eth->netdev[mac->id]->stats.tx_errors++;
	netif_err(eth, tx_err, dev,
		  "transmit timed out\n");
	schedule_work(&eth->pending_work);
}

static QDMA_DMA_DSCP_T *rx0_get_dscp(int idx) {
	return &dscp_ary[TX0_DSCP_NUM + idx];
}

static struct sk_buff *rx0_new_skb(struct mtk_eth *eth,
				   int idx, QDMA_DMA_DSCP_T *dscp)
{
	int len;
	struct sk_buff *new_skb;
	dma_addr_t phys;

	len = 2000;

	new_skb = alloc_skb(len, GFP_ATOMIC);
	if (!new_skb) {
		return NULL;
	}

	phys = dma_map_single(eth->dev, new_skb->data, len, DMA_TO_DEVICE);
	if (!phys) {
		// free new_skb.
		return NULL;
	}
	dscp_sk_buff_p_ary[TX0_DSCP_NUM + idx] = new_skb;
	dscp->pkt_addr = phys;
	
	return new_skb;
}

static struct sk_buff *rx0_pop_skb(struct mtk_eth *eth,
				   int idx, QDMA_DMA_DSCP_T *dscp)
{
	int len;
	struct sk_buff *skb;
	dma_addr_t phys;

	len = 2000;
	
	skb = dscp_sk_buff_p_ary[idx + TX0_DSCP_NUM];
	phys = dscp->pkt_addr;
	if (rx0_new_skb(eth, idx, dscp)) {
		dma_unmap_single(eth->dev, phys, len, DMA_FROM_DEVICE);
		return skb;
	} 
	return NULL;
}

static void rx0_dscp_defaults(QDMA_DMA_DSCP_T *dscp)
{
	/* TODO: May be this is not necessary. The is payload size. */
	dscp->ctrl.pkt_len = 1518;
}

static void rx0_done(struct mtk_eth *eth)
{
	int idx, val;
	QDMA_DMA_DSCP_T *dscp;
	struct sk_buff *skb;
	
	val = mtk_r32(eth, QDMA_CSR_RX_DMA_IDX);
#ifdef RX_DEBUG	
	printk("rx0_done (1) CPU = %d, DMA = %d.",
	       mtk_r32(eth, QDMA_CSR_RX_CPU_IDX), val);
#endif
	/* Get previous ring index. */
	idx = (val + (RX0_DSCP_NUM - 1)) % RX0_DSCP_NUM;

	dscp = rx0_get_dscp(idx);
	skb = rx0_pop_skb(eth, idx, dscp);	
	if (skb) {
		skb_put(skb, dscp->ctrl.pkt_len);
		/* TODO: Get netdev by switch port. How? */
		skb->protocol = eth_type_trans(skb, eth->netdev[0]);
		netif_rx(skb);
	} else {
		/* TODO: Update netdev drop counter. */
	}
	rx0_dscp_defaults(dscp);
	dscp->ctrl.done = 0;
	/* DMA ID will try to meet CPU ID, no need to assign
	   CPU ID on every new message. I do this to free
	   my eyes from if's. */
	mtk_w32(eth, idx, QDMA_CSR_RX_CPU_IDX);
#ifdef RX_DEBUG
	printk("rx0_done (2) CPU = %d, DMA = %d.",
	       mtk_r32(eth, QDMA_CSR_RX_CPU_IDX),
	       mtk_r32(eth, QDMA_CSR_RX_DMA_IDX));
#endif
}

#define QDMA_CSR_IRQ_STATUS		0x406C
#define QDMA_CSR_IRQ_CLEAR_LEN		0x4068
#define IRQ_STATUS_HEAD_IDX_MASK	0xFFF
#define IRQ_STATUS_ENTRY_LEN_SHIFT	16
#define IRQ_STATUS_ENTRY_LEN_MASK	(0xFFF << IRQ_STATUS_ENTRY_LEN_SHIFT)
#define IRQ_DEF_VALUE			0xFFFFFFFF

static void tx0_recycle_if_required(struct mtk_eth *eth)
{
	int val, idx, len, i;
	
	/* Irq queue keeps indexes of sent tx dscp's so we know
	   which skb's and dscp's we can free. TX interrupt can 
	   be configured to trigger once in N messages, 
	   QDMA_CSR_TX_DELAY_INT_CFG. 
	   See 7512_eth.c qdma_bm_transmit_done.
	   
	   I free skb's in xmit. It looks like irq queue is not necessary,
	   but this simplified mode did not work. 
	   
	   TODO. */

	/*  The IRQ_FULL interrupt will be triggered if len == QUEUE_DEPTH.

	    Clean the queue counter. 

	    The counter will not be set 0 by writing to CLEAR_LEN reg, it 
	    will continue until len == IRQ_DEPTH and then begin 
	    from 0. */
	
	val = mtk_r32(eth, QDMA_CSR_IRQ_STATUS);
	idx = val & IRQ_STATUS_HEAD_IDX_MASK;
	len = (val & IRQ_STATUS_ENTRY_LEN_MASK) >> IRQ_STATUS_ENTRY_LEN_SHIFT;
	/* printk("IRQ Q STATUS %x, %d, %d.", val, idx, len); */

	for (i = 0; i < len; i++) {
		irq_queue[i] = IRQ_DEF_VALUE;
	}
	mtk_w32(eth, len & 0x7F, QDMA_CSR_IRQ_CLEAR_LEN);
}

static irqreturn_t mtk_handle_irq(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;
	int status, mask;

	mask = mtk_r32(eth, MTK_QDMA_INT_MASK);
	status = mtk_r32(eth, MTK_QDMA_INT_STATUS);
	
	pr_debug("mtk int mask=%x status=%x.", mask, status);

	if (status & INT_STATUS_RX0_DONE) {
		rx0_done(eth);
	} else if (status & INT_STATUS_TX0_DONE) {
		tx0_recycle_if_required(eth);
	}
	
	mtk_w32(eth, status & mask, MTK_QDMA_INT_STATUS);
	
	return IRQ_HANDLED;
}

#define QDMA_CSR_HWFWD_DSCP_BASE	0x4020
#define QDMA_CSR_HWFWD_BUFF_BASE	0x4024
#define QDMA_CSR_HWFWD_DSCP_CFG		0x4028
#define QDMA_CSR_LMGR_INIT_CFG		0x4030

#define QDMA_CSR_LMGR_START_BIT		BIT(31)

static void qdma_initialize_hw_fwd(struct mtk_eth *eth) {
	dma_addr_t phys_addr;
	int i, val, len;

	// mtk/linux-2.6.36/*.i, qdma_bm_dscp_init().
	// DSCP "done" marking will not begin if this is not set.
	mtk_w32(eth, 0x14 << 16, QDMA_CSR_LMGR_INIT_CFG);
	
	// Alloc mem for HWFWD_DSCPs.
	len = sizeof(QDMA_HWFWD_DMA_DSCP_T) * HWFWD_DSCP_NUM;
	hw_fwd_ary = dma_alloc_coherent(eth->dev,
					len, &phys_addr, GFP_ATOMIC);
	memset(hw_fwd_ary, 0, len);
	mtk_w32(eth, phys_addr, QDMA_CSR_HWFWD_DSCP_BASE);

	// Alloc HWFWD buf, depends on payload size.
	hw_fwd_buff = dma_alloc_coherent(eth->dev, 2048, &phys_addr, GFP_ATOMIC);
	memset(hw_fwd_buff, 0, 2048);
	mtk_w32(eth, phys_addr, QDMA_CSR_HWFWD_BUFF_BASE);

	val = mtk_r32(eth, QDMA_CSR_LMGR_INIT_CFG);
	mtk_w32(eth, val | HWFWD_DSCP_NUM, QDMA_CSR_LMGR_INIT_CFG);
	// Payload.
	mtk_w32(eth, 0 << 28, QDMA_CSR_HWFWD_DSCP_CFG);
	// Set threshold.
	mtk_w32(eth, 1, QDMA_CSR_HWFWD_DSCP_CFG);

	// Bootloader register value.
	// mtk_w32(eth, 0x1180004, QDMA_CSR_LMGR_INIT_CFG);

	val = mtk_r32(eth, QDMA_CSR_LMGR_INIT_CFG);
	mtk_w32(eth, val | QDMA_CSR_LMGR_START_BIT, QDMA_CSR_LMGR_INIT_CFG);	
	// Wait for init.
	for (i = 0; i < 100; i++) {
		val = mtk_r32(eth, QDMA_CSR_LMGR_INIT_CFG);
		if ((val & QDMA_CSR_LMGR_START_BIT) == 0)
			break;
	}
	// TODO: report init failure.
}

#define QDMA_CSR_IRQ_BASE	0x4060
#define QDMA_CSR_IRQ_CFG	0x4064
#define QDMA_IRQ_QUEUE_DEPTH	20

static void qdma_initialize_irq_queue(struct mtk_eth *eth)
{
	dma_addr_t phys;
	int len;

	/* TODO: May be "<< 2" is not necessary, compare 7512_eth.c
	   buffer types. */
	len = QDMA_IRQ_QUEUE_DEPTH << 2;
	
	irq_queue = dma_alloc_coherent(eth->dev, len, &phys, GFP_ATOMIC);
	memset(irq_queue, IRQ_DEF_VALUE, len);
	mtk_w32(eth, phys, QDMA_CSR_IRQ_BASE);
	mtk_w32(eth, QDMA_IRQ_QUEUE_DEPTH, QDMA_CSR_IRQ_CFG);	
}

static void qdma_initialize_tx_ring(void) {
	int i;
	for (i = 0; i < TX0_DSCP_NUM - 1; i++)
		dscp_ary[i].next_idx = i + 1;
}

static void qdma_initialize_rx_ring(struct mtk_eth *eth) {
	QDMA_DMA_DSCP_T *dscp;
	int i;
	
	for (i = 0; i < RX0_DSCP_NUM; i++) {
		dscp = rx0_get_dscp(i);
		rx0_dscp_defaults(dscp);
		rx0_new_skb(eth, i, dscp);
	}	
}

static int qdma_config(struct mtk_eth *eth)
{
	int err, i, val;	
	dma_addr_t phys_addr;
	
	// Disable TX/RX.
	mtk_w32(eth, 0, QDMA_CSR_GLB_CFG);
	
	dscp_ary = dma_alloc_coherent(eth->dev,
			      sizeof(QDMA_DMA_DSCP_T) * DSCP_NUM,
				      &phys_addr,
				      GFP_ATOMIC);
	memset(dscp_ary, 0, sizeof(QDMA_DMA_DSCP_T) * DSCP_NUM);

	// Set TX and RX DSCP addresses.
	mtk_w32(eth, phys_addr, QDMA_CSR_TX_DSCP_BASE);
	mtk_w32(eth, phys_addr + sizeof(QDMA_DMA_DSCP_T) * TX0_DSCP_NUM, QDMA_CSR_RX_DSCP_BASE);
	
	mtk_w32(eth, DSCP_NUM - TX0_DSCP_NUM, QDMA_CSR_RX_RING_CFG);
	mtk_w32(eth, 0, QDMA_CSR_RX_RING_THR);

	qdma_initialize_irq_queue(eth);
	qdma_initialize_hw_fwd(eth);

	qdma_initialize_tx_ring();	
	// Set TX circular buffer/ring pointers.
	mtk_w32(eth, 0, QDMA_CSR_TX_CPU_IDX);
	mtk_w32(eth, 0, QDMA_CSR_TX_DMA_IDX);

	qdma_initialize_rx_ring(eth);       	
	mtk_w32(eth, 0, QDMA_CSR_RX_CPU_IDX);
	mtk_w32(eth, 0, QDMA_CSR_RX_DMA_IDX);
	mtk_w32(eth, RX0_DSCP_NUM, QDMA_CSR_RX_CPU_IDX);
	
	// QDMA_CSR_TX_DELAY_INT_CFG 
	mtk_w32(eth, 0, 0x4058);
	// RX_DELAY_INT_CFG
	mtk_w32(eth, 0, 0x405C);

	mtk_w32(eth, (1 << 27) | (1 << 26) | (1 << 28) | (0x3 << 4)
		| MTK_TX_DMA_EN | MTK_RX_DMA_EN |
		(1 << 6) | (1 << 4) | (1 << 5)
		/* GLB_CFG_RX_2B_OFFSET. 7512_eth.c  _receive_buffer. */
		/* | (1 << 31) */
		/* GLB_CFG_IRQ_EN */
	        | (1 << 19),
		QDMA_CSR_GLB_CFG);

	/* Select interrupts.
	   If INT_STATUS_TX0_DONE is off but GLB_CFG_IRQ_EN is on, 
	   TX0_DONE interrupt will be triggered. 
	   If INT_STATUS_TX0_DONE and GLB_CFG_IRQ_EN both on, TX0_DONE
	   will be triggered even if no message was received. */	
	mtk_w32(eth, INT_STATUS_HWFWD_DSCP_LOW |
		INT_STATUS_IRQ_FULL |
		INT_STATUS_HWFWD_DSCP_EMPTY |
		INT_STATUS_NO_RX0_CPU_DSCP |
		INT_STATUS_NO_TX0_CPU_DSCP |
		INT_STATUS_RX0_DONE /* | INT_STATUS_TX0_DONE */,
		MTK_QDMA_INT_MASK);
	
	// GDMA1_FWD_CFG from bootloader mem.
	mtk_w32(eth, 0xC0000000, 0x500);

	// GSW_PMCR from bootloader reg.
	mtk_w32(eth, 0x9E30B, 0x8000 + 0x3000 + 5 * 0x100);
	mtk_w32(eth, 0x9E30B, 0x8000 + 0x3000 + 6 * 0x100);
	
	// GSW_MFC, matches bootloader reg value.
	mtk_w32(eth, (0xff << 24) | (0xff << 16) | (0xff << 8) | (1 << 7) | (6 << 4), 0x8000 + 0x10);
	
	return 0;
}

static void mtk_gdm_config(struct mtk_eth *eth, u32 config)
{

}

static int mtk_open(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int err;

	err = phylink_of_phy_connect(mac->phylink, mac->of_node, 0);
	if (err) {
		netdev_err(dev, "%s: could not attach PHY: %d\n", __func__,
			   err);
		return err;
	}

	/* we run 2 netdevs on the same dma ring so we only bring it up once */
	if (!refcount_read(&eth->dma_refcnt)) {
		int err = qdma_config(eth);

		if (err)
			return err;

		mtk_gdm_config(eth, MTK_GDMA_TO_PDMA);

		// mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);
		// mtk_rx_irq_enable(eth, MTK_RX_DONE_INT);
		refcount_set(&eth->dma_refcnt, 1);
	}
	else
		refcount_inc(&eth->dma_refcnt);

	phylink_start(mac->phylink);
	netif_start_queue(dev);
	return 0;
}

static void mtk_stop_dma(struct mtk_eth *eth, u32 glo_cfg)
{
	u32 val;
	int i;

	/* stop the dma engine */
	spin_lock_bh(&eth->page_lock);
	val = mtk_r32(eth, glo_cfg);
	mtk_w32(eth, val & ~(MTK_TX_WB_DDONE | MTK_RX_DMA_EN | MTK_TX_DMA_EN),
		glo_cfg);
	spin_unlock_bh(&eth->page_lock);

	/* wait for dma stop */
	for (i = 0; i < 10; i++) {
		val = mtk_r32(eth, glo_cfg);
		if (val & (MTK_TX_DMA_BUSY | MTK_RX_DMA_BUSY)) {
			msleep(20);
			continue;
		}
		break;
	}
}

static int mtk_stop(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	phylink_stop(mac->phylink);

	netif_tx_disable(dev);

	phylink_disconnect_phy(mac->phylink);

	/* only shutdown DMA if this is the last user */
	if (!refcount_dec_and_test(&eth->dma_refcnt))
		return 0;

	mtk_gdm_config(eth, MTK_GDMA_DROP_ALL);

	mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_disable(eth, MTK_RX_DONE_INT);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		mtk_stop_dma(eth, MTK_QDMA_GLO_CFG);
	mtk_stop_dma(eth, MTK_PDMA_GLO_CFG);

	mtk_dma_free(eth);

	return 0;
}

static int mtk_hw_deinit(struct mtk_eth *eth)
{
	if (!test_and_clear_bit(MTK_HW_INIT, &eth->state))
		return 0;

	pm_runtime_put_sync(eth->dev);
	pm_runtime_disable(eth->dev);

	return 0;
}

static int __init mtk_init(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const char mac_addr[6] = {16, 163, 184, 106, 1, 8};

	// mac_addr = of_get_mac_address(mac->of_node);
	// if (!IS_ERR(mac_addr))
	ether_addr_copy(dev->dev_addr, mac_addr);

	/* If the mac address is invalid, use random mac address  */
	if (!is_valid_ether_addr(dev->dev_addr)) {
		eth_hw_addr_random(dev);
		dev_err(eth->dev, "generated random MAC address %pM\n",
			dev->dev_addr);
	}

	return 0;
}

static void mtk_uninit(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	phylink_disconnect_phy(mac->phylink);
	mtk_tx_irq_disable(eth, ~0);
	mtk_rx_irq_disable(eth, ~0);
}

static int mtk_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mtk_mac *mac = netdev_priv(dev);

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return phylink_mii_ioctl(mac->phylink, ifr, cmd);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int mtk_free_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		free_netdev(eth->netdev[i]);
	}

	return 0;
}

static int mtk_unreg_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		unregister_netdev(eth->netdev[i]);
	}

	return 0;
}

static int mtk_cleanup(struct mtk_eth *eth)
{
	mtk_unreg_dev(eth);
	mtk_free_dev(eth);
	cancel_work_sync(&eth->pending_work);

	return 0;
}

static const struct net_device_ops mtk_netdev_ops = {
	.ndo_init		= mtk_init,
	.ndo_uninit		= mtk_uninit,
	.ndo_open		= mtk_open,
	.ndo_stop		= mtk_stop,
	.ndo_start_xmit		= mtk_start_xmit,
	.ndo_set_mac_address	= mtk_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= mtk_do_ioctl,
	.ndo_tx_timeout		= mtk_tx_timeout,
	.ndo_get_stats64        = mtk_get_stats64,
	.ndo_fix_features	= mtk_fix_features,
	.ndo_set_features	= mtk_set_features,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= mtk_poll_controller,
#endif
};

static int mtk_add_mac(struct mtk_eth *eth, struct device_node *np)
{
	const __be32 *_id = of_get_property(np, "reg", NULL);
	phy_interface_t phy_mode;
	struct phylink *phylink;
	struct mtk_mac *mac;
	int id, err;

	if (!_id) {
		dev_err(eth->dev, "missing mac id\n");
		return -EINVAL;
	}

	id = be32_to_cpup(_id);
	if (id >= MTK_MAC_COUNT) {
		dev_err(eth->dev, "%d is not a valid mac id\n", id);
		return -EINVAL;
	}

	if (eth->netdev[id]) {
		dev_err(eth->dev, "duplicate mac id found: %d\n", id);
		return -EINVAL;
	}

	eth->netdev[id] = alloc_etherdev(sizeof(*mac));
	if (!eth->netdev[id]) {
		dev_err(eth->dev, "alloc_etherdev failed\n");
		return -ENOMEM;
	}
	mac = netdev_priv(eth->netdev[id]);
	eth->mac[id] = mac;
	mac->id = id;
	mac->hw = eth;
	mac->of_node = np;

	memset(mac->hwlro_ip, 0, sizeof(mac->hwlro_ip));
	mac->hwlro_ip_cnt = 0;

	mac->hw_stats = devm_kzalloc(eth->dev,
				     sizeof(*mac->hw_stats),
				     GFP_KERNEL);
	if (!mac->hw_stats) {
		dev_err(eth->dev, "failed to allocate counter memory\n");
		err = -ENOMEM;
		goto free_netdev;
	}
	spin_lock_init(&mac->hw_stats->stats_lock);
	u64_stats_init(&mac->hw_stats->syncp);
	mac->hw_stats->reg_offset = id * MTK_STAT_OFFSET;

	/* phylink create */
	err = of_get_phy_mode(np, &phy_mode);
	if (err) {
		dev_err(eth->dev, "incorrect phy-mode\n");
		goto free_netdev;
	}

	/* mac config is not set */
	mac->interface = PHY_INTERFACE_MODE_NA;
	mac->mode = MLO_AN_PHY;
	mac->speed = SPEED_UNKNOWN;

	mac->phylink_config.dev = &eth->netdev[id]->dev;
	mac->phylink_config.type = PHYLINK_NETDEV;

	phylink = phylink_create(&mac->phylink_config,
				 of_fwnode_handle(mac->of_node),
				 phy_mode, &mtk_phylink_ops);
	if (IS_ERR(phylink)) {
		err = PTR_ERR(phylink);
		goto free_netdev;
	}

	mac->phylink = phylink;

	SET_NETDEV_DEV(eth->netdev[id], eth->dev);
	eth->netdev[id]->watchdog_timeo = 5 * HZ;
	eth->netdev[id]->netdev_ops = &mtk_netdev_ops;
	eth->netdev[id]->base_addr = (unsigned long)eth->base;

	eth->netdev[id]->hw_features = eth->soc->hw_features;
	if (eth->hwlro)
		eth->netdev[id]->hw_features |= NETIF_F_LRO;

	eth->netdev[id]->vlan_features = eth->soc->hw_features &
		~(NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX);
	eth->netdev[id]->features |= eth->soc->hw_features;
	// eth->netdev[id]->ethtool_ops = &mtk_ethtool_ops;

	eth->netdev[id]->irq = eth->irq[0];
	eth->netdev[id]->dev.of_node = np;

	eth->netdev[id]->max_mtu = MTK_MAX_RX_LENGTH - MTK_RX_ETH_HLEN;

	return 0;

free_netdev:
	free_netdev(eth->netdev[id]);
	return err;
}

static int mtk_probe(struct platform_device *pdev)
{
	struct device_node *mac_np;
	struct mtk_eth *eth;
	int err, i;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->soc = of_device_get_match_data(&pdev->dev);

	eth->dev = &pdev->dev;
	eth->base = 0xBFB50000; // devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->base))
		return PTR_ERR(eth->base);

	eth->tx_int_mask_reg = MTK_QDMA_INT_MASK;
	eth->tx_int_status_reg = MTK_QDMA_INT_STATUS;

	spin_lock_init(&eth->page_lock);
	spin_lock_init(&eth->tx_irq_lock);
	spin_lock_init(&eth->rx_irq_lock);

	for (i = 0; i < 3; i++) {
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_SHARED_INT) && i > 0)
			eth->irq[i] = eth->irq[0];
		else
			eth->irq[i] = 22; // MAC_INT platform_get_irq(pdev, i);
		if (eth->irq[i] < 0) {
			dev_err(&pdev->dev, "no IRQ%d resource found\n", i);
			return -ENXIO;
		}
	}

	eth->msg_enable = netif_msg_init(mtk_msg_level, MTK_DEFAULT_MSG_ENABLE);

	for_each_child_of_node(pdev->dev.of_node, mac_np) {
		if (!of_device_is_compatible(mac_np,
					     "mediatek,eth-mac"))
			continue;

		if (!of_device_is_available(mac_np))
			continue;

		err = mtk_add_mac(eth, mac_np);
		if (err) {
			of_node_put(mac_np);
			goto err_deinit_hw;
		}
	}

	err = devm_request_irq(eth->dev, eth->irq[0],
			       mtk_handle_irq, 0,
			       dev_name(eth->dev), eth);
		
	if (err)
		goto err_free_dev;

#if 0
	/* No MT7628/88 support yet */
	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		err = mtk_mdio_init(eth);
		if (err)
			goto err_free_dev;
	}
#endif
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		err = register_netdev(eth->netdev[i]);
		if (err) {
			dev_err(eth->dev, "error bringing up device\n");
			goto err_deinit_mdio;
		} else
			netif_info(eth, probe, eth->netdev[i],
				   "mediatek frame engine at 0x%08lx, irq %d\n",
				   eth->netdev[i]->base_addr, eth->irq[0]);
	}

	platform_set_drvdata(pdev, eth);

	return 0;

err_deinit_mdio:
	mtk_mdio_cleanup(eth);
err_free_dev:
	mtk_free_dev(eth);
err_deinit_hw:
	mtk_hw_deinit(eth);

	return err;
}

static int mtk_remove(struct platform_device *pdev)
{
	struct mtk_eth *eth = platform_get_drvdata(pdev);
	struct mtk_mac *mac;
	int i;

	/* stop all devices to make sure that dma is properly shut down */
	for (i = 0; i < MTK_MAC_COUNT; i++) {
		if (!eth->netdev[i])
			continue;
		mtk_stop(eth->netdev[i]);
		mac = netdev_priv(eth->netdev[i]);
		phylink_disconnect_phy(mac->phylink);
	}

	mtk_hw_deinit(eth);

	mtk_cleanup(eth);
	mtk_mdio_cleanup(eth);

	return 0;
}

static const struct mtk_soc_data mt7621_data = {
	.caps = MT7621_CAPS,
	.hw_features = 0,
};

const struct of_device_id of_mtk_match[] = {
	{ .compatible = "mediatek,mt7621-eth", .data = &mt7621_data},
	{},
};
MODULE_DEVICE_TABLE(of, of_mtk_match);

static struct platform_driver mtk_driver = {
	.probe = mtk_probe,
	.remove = mtk_remove,
	.driver = {
		.name = "mtk_soc_eth",
		.of_match_table = of_mtk_match,
	},
};

module_platform_driver(mtk_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
MODULE_DESCRIPTION("Ethernet driver for MediaTek SoC");
