/*
 * CAN bus driver for Microchip 2517FD CAN Controller with SPI Interface
 *
 * Copyright 2017 Martin Sperl <kernel@martin.sperl.org>
 *
 * Based on Microchip MCP251x CAN controller driver written by
 * David Vrabel, Copyright 2006 Arcom Control Systems Ltd.
 *
 */

#include <linux/can/core.h>
#include <linux/can/dev.h>
#include <linux/can/led.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/freezer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>

#define DEVICE_NAME "mcp2517fd"

#define MCP2517FD_OST_DELAY_MS		3
#define MCP2517FD_MIN_CLOCK_FREQUENCY	1000000
#define MCP2517FD_MAX_CLOCK_FREQUENCY	40000000
#define MCP2517FD_PLL_MULTIPLIER	10
#define MCP2517FD_AUTO_PLL_MAX_CLOCK_FREQUENCY \
	(MCP2517FD_MAX_CLOCK_FREQUENCY / MCP2517FD_PLL_MULTIPLIER)
#define MCP2517FD_SCLK_DIVIDER		2

#define MCP2517FD_OSC_POLLING_JIFFIES	(HZ / 2)

#define TX_ECHO_SKB_MAX	32

#define INSTRUCTION_RESET		0x0000
#define INSTRUCTION_READ		0x3000
#define INSTRUCTION_WRITE		0x2000
#define INSTRUCTION_READ_CRC		0xB000
#define INSTRUCTION_WRITE_CRC		0xA000
#define INSTRUCTION_WRITE_SAVE		0xC000

#define ADDRESS_MASK 			0x0fff

#define MCP2517FD_SFR_BASE(x)		(0xE00 + (x))
#define MCP2517FD_OSC			MCP2517FD_SFR_BASE(0x00)
#  define MCP2517FD_OSC_PLLEN		BIT(0)
#  define MCP2517FD_OSC_OSCDIS		BIT(2)
#  define MCP2517FD_OSC_SCLKDIV		BIT(4)
#  define MCP2517FD_OSC_CLKODIV_BITS	2
#  define MCP2517FD_OSC_CLKODIV_SHIFT	5
#  define MCP2517FD_OSC_CLKODIV_MASK			\
	GENMASK(MCP2517FD_OSC_CLKODIV_SHIFT		\
		+ MCP2517FD_OSC_CLKODIV_BITS - 1,	\
		MCP2517FD_OSC_CLKODIV_SHIFT)
#  define MCP2517FD_OSC_CLKODIV_10	3
#  define MCP2517FD_OSC_CLKODIV_4	2
#  define MCP2517FD_OSC_CLKODIV_2	1
#  define MCP2517FD_OSC_CLKODIV_1	0
#  define MCP2517FD_OSC_PLLRDY		BIT(8)
#  define MCP2517FD_OSC_OSCRDY		BIT(10)
#  define MCP2517FD_OSC_SCLKRDY		BIT(12)
#define MCP2517FD_IOCON			MCP2517FD_SFR_BASE(0x04)
#  define MCP2517FD_IOCON_TRIS0		BIT(0)
#  define MCP2517FD_IOCON_TRIS1		BIT(1)
#  define MCP2517FD_IOCON_XSTBYEN	BIT(6)
#  define MCP2517FD_IOCON_LAT0		BIT(8)
#  define MCP2517FD_IOCON_LAT1		BIT(9)
#  define MCP2517FD_IOCON_GPIO0		BIT(16)
#  define MCP2517FD_IOCON_GPIO1		BIT(17)
#  define MCP2517FD_IOCON_PM0		BIT(24)
#  define MCP2517FD_IOCON_PM1		BIT(25)
#  define MCP2517FD_IOCON_TXCANOD	BIT(28)
#  define MCP2517FD_IOCON_SOF		BIT(29)
#  define MCP2517FD_IOCON_INTOD		BIT(29)
#define MCP2517FD_CRC			MCP2517FD_SFR_BASE(0x08)
#  define MCP2517FD_CRC_MASK		GENMASK(15, 0)
#  define MCP2517FD_CRC_CRCERRIE	BIT(16)
#  define MCP2517FD_CRC_FERRIE		BIT(17)
#  define MCP2517FD_CRC_CRCERRIF	BIT(24)
#  define MCP2517FD_CRC_FERRIF		BIT(25)
#define MCP2517FD_ECCCON		MCP2517FD_SFR_BASE(0x0C)
#  define MCP2517FD_ECCCON_ECCEN	BIT(0)
#  define MCP2517FD_ECCCON_SECIE	BIT(1)
#  define MCP2517FD_ECCCON_DEDIE	BIT(2)
#  define MCP2517FD_ECCCON_PARITY_BITS 6
#  define MCP2517FD_ECCCON_PARITY_SHIFT 8
#  define MCP2517FD_ECCCON_PARITY_MASK			\
	GENMASK(MCP2517FD_ECCCON_PARITY_SHIFT		\
		+ MCP2517FD_ECCCON_PARITY_BITS - 1,	\
		MCP2517FD_ECCCON_PARITY_SHIFT)
#define MCP2517FD_ECCSTAT		MCP2517FD_SFR_BASE(0x10)
#  define MCP2517FD_ECCSTAT_SECIF	BIT(1)
#  define MCP2517FD_ECCSTAT_DEDIF	BIT(2)
#  define MCP2517FD_ECCSTAT_ERRADDR_SHIFT 8
#  define MCP2517FD_ECCSTAT_ERRADDR_MASK	      \
	GENMASK(MCP2517FD_ECCSTAT_ERRADDR_SHIFT + 11, \
		MCP2517FD_ECCSTAT_ERRADDR_SHIFT)

#define CAN_SFR_BASE(x)			(0x000 + (x))
#define CAN_CON				CAN_SFR_BASE(0x00)
#  define CAN_CON_DNCNT_BITS		5
#  define CAN_CON_DNCNT_SHIFT		0
#  define CAN_CON_DNCNT_MASK					\
	GENMASK(CAN_CON_DNCNT_SHIFT + CAN_CON_DNCNT_BITS - 1,	\
		CAN_CON_DNCNT_SHIFT)
#  define CAN_CON_ISOCRCEN		BIT(5)
#  define CAN_CON_PXEDIS		BIT(6)
#  define CAN_CON_WAKFIL		BIT(8)
#  define CAN_CON_WFT_BITS		2
#  define CAN_CON_WFT_SHIFT		9
#  define CAN_CON_WFT_MASK					\
	GENMASK(CAN_CON_WFT_SHIFT + CAN_CON_WFT_BITS - 1,	\
		CAN_CON_WFT_SHIFT)
#  define CAN_CON_BUSY			BIT(11)
#  define CAN_CON_BRSDIS		BIT(12)
#  define CAN_CON_RTXAT			BIT(16)
#  define CAN_CON_ESIGM			BIT(17)
#  define CAN_CON_SERR2LOM		BIT(18)
#  define CAN_CON_STEF			BIT(19)
#  define CAN_CON_TXQEN			BIT(20)
#  define CAN_CON_OPMODE_BITS		3
#  define CAN_CON_OPMOD_SHIFT		21
#  define CAN_CON_OPMOD_MASK					\
	GENMASK(CAN_CON_OPMOD_SHIFT + CAN_CON_OPMODE_BITS -1,	\
		CAN_CON_OPMOD_SHIFT)
#  define CAN_CON_REQOP_BITS		3
#  define CAN_CON_REQOP_SHIFT		24
#  define CAN_CON_REQOP_MASK					\
	GENMASK(CAN_CON_REQOP_SHIFT + CAN_CON_REQOP_BITS - 1,	\
		CAN_CON_REQOP_SHIFT)
#    define CAN_CON_MODE_MIXED			0
#    define CAN_CON_MODE_SLEEP			1
#    define CAN_CON_MODE_INTERNAL_LOOPBACK	2
#    define CAN_CON_MODE_LISTENONLY		3
#    define CAN_CON_MODE_CONFIG			4
#    define CAN_CON_MODE_EXTERNAL_LOOPBACK	5
#    define CAN_CON_MODE_CAN2_0			6
#    define CAN_CON_MODE_RESTRICTED		7
#  define CAN_CON_ABAT			BIT(27)
#  define CAN_CON_TXBWS_BITS		3
#  define CAN_CON_TXBWS_SHIFT		28
#  define CAN_CON_TXBWS_MASK					\
	GENMASK(CAN_CON_TXBWS_SHIFT + CAN_CON_TXBWS_BITS - 1,	\
		CAN_CON_TXBWS_SHIFT)
#  define CAN_CON_DEFAULT					\
	( CAN_CON_ISOCRCEN					\
	  | CAN_CON_PXEDIS					\
	  | CAN_CON_WAKFIL					\
	  | (3 << CAN_CON_WFT_SHIFT)				\
	  | CAN_CON_STEF					\
	  | CAN_CON_TXQEN					\
	  | (CAN_CON_MODE_CONFIG << CAN_CON_OPMOD_SHIFT)	\
	  | (CAN_CON_MODE_CONFIG << CAN_CON_REQOP_SHIFT)	\
	)
#  define CAN_CON_DEFAULT_MASK			\
	( CAN_CON_DNCNT_MASK			\
	  |CAN_CON_ISOCRCEN			\
	  | CAN_CON_PXEDIS			\
	  | CAN_CON_WAKFIL			\
	  | CAN_CON_WFT_MASK			\
	  | CAN_CON_BRSDIS			\
	  | CAN_CON_RTXAT			\
	  | CAN_CON_ESIGM			\
	  | CAN_CON_SERR2LOM			\
	  | CAN_CON_STEF			\
	  | CAN_CON_TXQEN			\
	  | CAN_CON_OPMOD_MASK			\
	  | CAN_CON_REQOP_MASK			\
	  | CAN_CON_ABAT			\
	  | CAN_CON_TXBWS_MASK			\
	)
#define CAN_NBTCFG			CAN_SFR_BASE(0x04)
#  define CAN_NBTCFG_SJW_BITS		7
#  define CAN_NBTCFG_SJW_SHIFT		0
#  define CAN_NBTCFG_SJW_MASK					\
	GENMASK(CAN_NBTCFG_SJW_SHIFT + CAN_NBTCFG_SJW_BITS - 1, \
		CAN_NBTCFG_SJW_SHIFT)
#  define CAN_NBTCFG_TSEG2_BITS		7
#  define CAN_NBTCFG_TSEG2_SHIFT	8
#  define CAN_NBTCFG_TSEG2_MASK					    \
	GENMASK(CAN_NBTCFG_TSEG2_SHIFT + CAN_NBTCFG_TSEG2_BITS - 1, \
		CAN_NBTCFG_TSEG2_SHIFT)
#  define CAN_NBTCFG_TSEG1_BITS		8
#  define CAN_NBTCFG_TSEG1_SHIFT	16
#  define CAN_NBTCFG_TSEG1_MASK					    \
	GENMASK(CAN_NBTCFG_TSEG1_SHIFT + CAN_NBTCFG_TSEG1_BITS - 1, \
		CAN_NBTCFG_TSEG1_SHIFT)
#  define CAN_NBTCFG_BRP_BITS		8
#  define CAN_NBTCFG_BRP_SHIFT		24
#  define CAN_NBTCFG_BRP_MASK					\
	GENMASK(CAN_NBTCFG_BRP_SHIFT + CAN_NBTCFG_BRP_BITS - 1, \
		CAN_NBTCFG_BRP_SHIFT)
#define CAN_DBTCFG			CAN_SFR_BASE(0x08)
#  define CAN_DBTCFG_SJW_BITS		4
#  define CAN_DBTCFG_SJW_SHIFT		0
#  define CAN_DBTCFG_SJW_MASK					\
	GENMASK(CAN_DBTCFG_SJW_SHIFT + CAN_DBTCFG_SJW_BITS - 1, \
		CAN_DBTCFG_SJW_SHIFT)
#  define CAN_DBTCFG_TSEG2_BITS		4
#  define CAN_DBTCFG_TSEG2_SHIFT	8
#  define CAN_DBTCFG_TSEG2_MASK					    \
	GENMASK(CAN_DBTCFG_TSEG2_SHIFT + CAN_DBTCFG_TSEG2_BITS - 1, \
		CAN_DBTCFG_TSEG2_SHIFT)
#  define CAN_DBTCFG_TSEG1_BITS		5
#  define CAN_DBTCFG_TSEG1_SHIFT	16
#  define CAN_DBTCFG_TSEG1_MASK					    \
	GENMASK(CAN_DBTCFG_TSEG1_SHIFT + CAN_DBTCFG_TSEG1_BITS - 1, \
		CAN_DBTCFG_TSEG1_SHIFT)
#  define CAN_DBTCFG_BRP_BITS		8
#  define CAN_DBTCFG_BRP_SHIFT		24
#  define CAN_DBTCFG_BRP_MASK					\
	GENMASK(CAN_DBTCFG_BRP_SHIFT + CAN_DBTCFG_BRP_BITS - 1, \
		CAN_DBTCFG_BRP_SHIFT)
#define CAN_TDC				CAN_SFR_BASE(0x0C)
#  define CAN_TDC_TDCV_BITS		5
#  define CAN_TDC_TDCV_SHIFT		0
#  define CAN_TDC_TDCV_MASK					\
	GENMASK(CAN_TDC_TDCV_SHIFT + CAN_TDC_TDCV_BITS - 1, \
		CAN_TDC_TDCV_SHIFT)
#  define CAN_TDC_TDCO_BITS		5
#  define CAN_TDC_TDCO_SHIFT		8
#  define CAN_TDC_TDCO_MASK					\
	GENMASK(CAN_TDC_TDCO_SHIFT + CAN_TDC_TDCO_BITS - 1, \
		CAN_TDC_TDCO_SHIFT)
#  define CAN_TDC_TDCMOD_BITS		2
#  define CAN_TDC_TDCMOD_SHIFT		16
#  define CAN_TDC_TDCMOD_MASK					\
	GENMASK(CAN_TDC_TDCMOD_SHIFT + CAN_TDC_TDCMOD_BITS - 1, \
		CAN_TDC_TDCMOD_SHIFT)
#  define CAN_TDC_SID11EN		BIT(24)
#  define CAN_TDC_EDGFLTEN		BIT(25)
#define CAN_TBC				CAN_SFR_BASE(0x10)
#define CAN_TSCON			CAN_SFR_BASE(0x14)
#  define CAN_TSCON_TBCPRE_BITS		10
#  define CAN_TSCON_TBCPRE_SHIFT	0
#  define CAN_TSCON_TBCPRE_MASK					    \
	GENMASK(CAN_TSCON_TBCPRE_SHIFT + CAN_TSCON_TBCPRE_BITS - 1, \
		CAN_TSCON_TBCPRE_SHIFT)
#  define CAN_TSCON_TBCEN		BIT(24)
#  define CAN_TSCON_TSEOF		BIT(25)
#  define CAN_TSCON_TSRES		BIT(26)
#define CAN_VEC				CAN_SFR_BASE(0x18)
#  define CAN_VEC_ICODE_BITS		7
#  define CAN_VEC_ICODE_SHIFT		0
#  define CAN_VEC_ICODE_MASK					    \
	GENMASK(CAN_VEC_ICODE_SHIFT + CAN_VEC_ICODE_BITS - 1,	    \
		CAN_VEC_ICODE_SHIFT)
#  define CAN_VEC_FILHIT_BITS		5
#  define CAN_VEC_FILHIT_SHIFT		8
#  define CAN_VEC_FILHIT_MASK					\
	GENMASK(CAN_VEC_FILHIT_SHIFT + CAN_VEC_FILHIT_BITS - 1, \
		CAN_VEC_FILHIT_SHIFT)
#  define CAN_VEC_TXCODE_BITS		7
#  define CAN_VEC_TXCODE_SHIFT		16
#  define CAN_VEC_TXCODE_MASK					\
	GENMASK(CAN_VEC_TXCODE_SHIFT + CAN_VEC_TXCODE_BITS - 1, \
		CAN_VEC_TXCODE_SHIFT)
#  define CAN_VEC_RXCODE_BITS		7
#  define CAN_VEC_RXCODE_SHIFT		24
#  define CAN_VEC_RXCODE_MASK					\
	GENMASK(CAN_VEC_RXCODE_SHIFT + CAN_VEC_RXCODE_BITS - 1, \
		CAN_VEC_RXCODE_SHIFT)
#define CAN_INT				CAN_SFR_BASE(0x1C)
#  define CAN_INT_IF_SHIFT		0
#  define CAN_INT_TXIF			BIT(0)
#  define CAN_INT_RXIF			BIT(1)
#  define CAN_INT_TBCIF			BIT(2)
#  define CAN_INT_MODIF			BIT(3)
#  define CAN_INT_TEFIF			BIT(4)
#  define CAN_INT_ECCIF			BIT(8)
#  define CAN_INT_SPICRCIF		BIT(9)
#  define CAN_INT_TXATIF		BIT(10)
#  define CAN_INT_RXOVIF		BIT(11)
#  define CAN_INT_SERRIF		BIT(12)
#  define CAN_INT_CERRIF		BIT(13)
#  define CAN_INT_WAKIF			BIT(14)
#  define CAN_INT_IVMIF			BIT(15)
#  define CAN_INT_IF_MASK		\
	( CAN_INT_TXIF |		\
	  CAN_INT_RXIF |		\
	  CAN_INT_TBCIF	|		\
	  CAN_INT_MODIF	|		\
	  CAN_INT_TEFIF	|		\
	  CAN_INT_ECCIF	|		\
	  CAN_INT_SPICRCIF |		\
	  CAN_INT_TXATIF |		\
	  CAN_INT_RXOVIF |		\
	  CAN_INT_CERRIF |		\
	  CAN_INT_SERRIF |		\
	  CAN_INT_WAKEIF |		\
	  CAN_INT_IVMIF )
#  define CAN_INT_IE_SHIFT		16
#  define CAN_INT_TXIE			(CAN_INT_TXIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_RXIE			(CAN_INT_RXIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_TBCIE			(CAN_INT_TBCIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_MODIE			(CAN_INT_MODIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_TEFIE			(CAN_INT_TEFIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_ECCIE			(CAN_INT_ECCIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_SPICRCIE		\
	(CAN_INT_SPICRCIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_TXATIE		(CAN_INT_TXATIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_RXOVIE		(CAN_INT_RXOVIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_CERRIE		(CAN_INT_CERRIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_SERRIE		(CAN_INT_SERRIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_WAKIE			(CAN_INT_WAKIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_IVMIE			(CAN_INT_IVMIF << CAN_INT_IE_SHIFT)
#  define CAN_INT_IE_MASK		\
	( CAN_INT_TXIE |		\
	  CAN_INT_RXIE |		\
	  CAN_INT_TBCIE	|		\
	  CAN_INT_MODIE	|		\
	  CAN_INT_TEFIE	|		\
	  CAN_INT_ECCIE	|		\
	  CAN_INT_SPICRCIE |		\
	  CAN_INT_TXATIE |		\
	  CAN_INT_RXOVIE |		\
	  CAN_INT_CERRIE |		\
	  CAN_INT_SERRIE |		\
	  CAN_INT_WAKEIE |		\
	  CAN_INT_IVMIE )
#define CAN_RXIF			CAN_SFR_BASE(0x20)
#define CAN_TXIF			CAN_SFR_BASE(0x24)
#define CAN_RXOVIF			CAN_SFR_BASE(0x28)
#define CAN_TXATIF			CAN_SFR_BASE(0x2C)
#define CAN_TXREQ			CAN_SFR_BASE(0x30)
#define CAN_TREC			CAN_SFR_BASE(0x34)
#  define CAN_TREC_REC_BITS		8
#  define CAN_TREC_REC_SHIFT		0
#  define CAN_TREC_REC_MASK				    \
	GENMASK(CAN_TREC_REC_SHIFT + CAN_TREC_REC_BITS - 1, \
		CAN_TREC_REC_SHIFT)
#  define CAN_TREC_TEC_BITS		8
#  define CAN_TREC_TEC_SHIFT		8
#  define CAN_TREC_TEC_MASK				    \
	GENMASK(CAN_TREC_TEC_SHIFT + CAN_TREC_TEC_BITS - 1, \
		CAN_TREC_TEC_SHIFT)
#  define CAN_TREC_EWARN		BIT(16)
#  define CAN_TREC_RXWARN		BIT(17)
#  define CAN_TREC_TXWARN		BIT(18)
#  define CAN_TREC_RXBP			BIT(19)
#  define CAN_TREC_TXBP			BIT(20)
#  define CAN_TREC_TXBO			BIT(21)
#define CAN_BDIAG0			CAN_SFR_BASE(0x38)
#  define CAN_BDIAG0_NRERRCNT_BITS	8
#  define CAN_BDIAG0_NRERRCNT_SHIFT	0
#  define CAN_BDIAG0_NRERRCNT_MASK				\
	GENMASK(CAN_BDIAG0_NRERRCNT_SHIFT + CAN_BDIAG0_NRERRCNT_BITS - 1, \
		CAN_BDIAG0_NRERRCNT_SHIFT)
#  define CAN_BDIAG0_NTERRCNT_BITS	8
#  define CAN_BDIAG0_NTERRCNT_SHIFT	8
#  define CAN_BDIAG0_NTERRCNT_MASK					\
	GENMASK(CAN_BDIAG0_NTERRCNT_SHIFT + CAN_BDIAG0_NTERRCNT_BITS - 1, \
		CAN_BDIAG0_NTERRCNT_SHIFT)
#  define CAN_BDIAG0_DRERRCNT_BITS	8
#  define CAN_BDIAG0_DRERRCNT_SHIFT	16
#  define CAN_BDIAG0_DRERRCNT_MASK					\
	GENMASK(CAN_BDIAG0_DRERRCNT_SHIFT + CAN_BDIAG0_DRERRCNT_BITS - 1, \
		CAN_BDIAG0_DRERRCNT_SHIFT)
#  define CAN_BDIAG0_DTERRCNT_BITS	8
#  define CAN_BDIAG0_DTERRCNT_SHIFT	24
#  define CAN_BDIAG0_DTERRCNT_MASK					\
	GENMASK(CAN_BDIAG0_DTERRCNT_SHIFT + CAN_BDIAG0_DTERRCNT_BITS - 1, \
		CAN_BDIAG0_DTERRCNT_SHIFT)
#define CAN_BDIAG1			CAN_SFR_BASE(0x3C)
#  define CAN_BDIAG1_EFMSGCNT_BITS	16
#  define CAN_BDIAG1_EFMSGCNT_SHIFT	0
#  define CAN_BDIAG1_EFMSGCNT_MASK					\
	GENMASK(CAN_BDIAG1_EFMSGCNT_SHIFT + CAN_BDIAG1_EFMSGCNT_BITS - 1, \
		CAN_BDIAG1_EFMSGCNT_SHIFT)
#  define CAN_BDIAG1_NBIT0ERR		BIT(16)
#  define CAN_BDIAG1_NBIT1ERR		BIT(17)
#  define CAN_BDIAG1_NACKERR		BIT(18)
#  define CAN_BDIAG1_NSTUFERR		BIT(19)
#  define CAN_BDIAG1_NFORMERR		BIT(20)
#  define CAN_BDIAG1_NCRCERR		BIT(21)
#  define CAN_BDIAG1_TXBOERR		BIT(23)
#  define CAN_BDIAG1_DBIT0ERR		BIT(24)
#  define CAN_BDIAG1_DBIT1ERR		BIT(25)
#  define CAN_BDIAG1_DFORMERR		BIT(27)
#  define CAN_BDIAG1_STUFERR		BIT(28)
#  define CAN_BDIAG1_DCRCERR		BIT(29)
#  define CAN_BDIAG1_ESI		BIT(30)
#  define CAN_BDIAG1_DLCMM		BIT(31)
#define CAN_TEFCON			CAN_SFR_BASE(0x40)
#  define CAN_TEFCON_TEFNEIE		BIT(0)
#  define CAN_TEFCON_TEFHIE		BIT(1)
#  define CAN_TEFCON_TEFFIE		BIT(2)
#  define CAN_TEFCON_TEFOVIE		BIT(3)
#  define CAN_TEFCON_TEFTSEN		BIT(5)
#  define CAN_TEFCON_UINC		BIT(8)
#  define CAN_TEFCON_FRESET		BIT(10)
#  define CAN_TEFCON_FSIZE_BITS		5
#  define CAN_TEFCON_FSIZE_SHIFT	24
#  define CAN_TEFCON_FSIZE_MASK					    \
	GENMASK(CAN_TEFCON_FSIZE_SHIFT + CAN_TEFCON_FSIZE_BITS - 1, \
		CAN_TEFCON_FSIZE_SHIFT)
#define CAN_TEFSTA			CAN_SFR_BASE(0x44)
#  define CAN_TEFSTA_TEFNEIF		BIT(0)
#  define CAN_TEFSTA_TEFHIF		BIT(1)
#  define CAN_TEFSTA_TEFFIF		BIT(2)
#  define CAN_TEFSTA_TEVOVIF		BIT(3)
#define CAN_TEFUA			CAN_SFR_BASE(0x48)
#define CAN_RESERVED			CAN_SFR_BASE(0x4C)
#define CAN_TXQCON			CAN_SFR_BASE(0x50)
#  define CAN_TXQCON_TXQNIE		BIT(0)
#  define CAN_TXQCON_TXQEIE		BIT(2)
#  define CAN_TXQCON_TXATIE		BIT(4)
#  define CAN_TXQCON_TXEN		BIT(7)
#  define CAN_TXQCON_UINC		BIT(8)
#  define CAN_TXQCON_TXREQ		BIT(9)
#  define CAN_TXQCON_FRESET		BIT(10)
#  define CAN_TXQCON_TXPRI_BITS		5
#  define CAN_TXQCON_TXPRI_SHIFT	16
#  define CAN_TXQCON_TXPRI_MASK					    \
	GENMASK(CAN_TXQCON_TXPRI_SHIFT + CAN_TXQCON_TXPRI_BITS - 1, \
		CAN_TXQCON_TXPRI_SHIFT)
#  define CAN_TXQCON_TXAT_BITS		2
#  define CAN_TXQCON_TXAT_SHIFT		21
#  define CAN_TXQCON_TXAT_MASK					    \
	GENMASK(CAN_TXQCON_TXAT_SHIFT + CAN_TXQCON_TXAT_BITS - 1, \
		CAN_TXQCON_TXAT_SHIFT)
#  define CAN_TXQCON_FSIZE_BITS		5
#  define CAN_TXQCON_FSIZE_SHIFT	24
#  define CAN_TXQCON_FSIZE_MASK					    \
	GENMASK(CAN_TXQCON_FSIZE_SHIFT + CAN_TXQCON_FSIZE_BITS - 1, \
		CAN_TXQCON_FSIZE_SHIFT)
#  define CAN_TXQCON_PLSIZE_BITS	3
#  define CAN_TXQCON_PLSIZE_SHIFT	29
#  define CAN_TXQCON_PLSIZE_MASK				      \
	GENMASK(CAN_TXQCON_PLSIZE_SHIFT + CAN_TXQCON_PLSIZE_BITS - 1, \
		CAN_TXQCON_PLSIZE_SHIFT)
#    define CAN_TXQCON_PLSIZE_8		0
#    define CAN_TXQCON_PLSIZE_12	1
#    define CAN_TXQCON_PLSIZE_16	2
#    define CAN_TXQCON_PLSIZE_20	3
#    define CAN_TXQCON_PLSIZE_24	4
#    define CAN_TXQCON_PLSIZE_32	5
#    define CAN_TXQCON_PLSIZE_48	6
#    define CAN_TXQCON_PLSIZE_64	7

#define CAN_TXQSTA			CAN_SFR_BASE(0x54)
#  define CAN_TXQSTA_TXQNIF		BIT(0)
#  define CAN_TXQSTA_TXQEIF		BIT(2)
#  define CAN_TXQSTA_TXATIF		BIT(4)
#  define CAN_TXQSTA_TXERR		BIT(5)
#  define CAN_TXQSTA_TXLARB		BIT(6)
#  define CAN_TXQSTA_TXABT		BIT(7)
#  define CAN_TXQSTA_TXQCI_BITS		5
#  define CAN_TXQSTA_TXQCI_SHIFT	8
#  define CAN_TXQSTA_TXQCI_MASK					    \
	GENMASK(CAN_TXQSTA_TXQCI_SHIFT + CAN_TXQSTA_TXQCI_BITS - 1, \
		CAN_TXQSTA_TXQCI_SHIFT)

#define CAN_TXQUA			CAN_SFR_BASE(0x58)
#define CAN_FIFOCON(x)			CAN_SFR_BASE(0x5C + 12 * (x - 1))
#define CAN_FIFOCON_TFNRFNIE		BIT(0)
#define CAN_FIFOCON_TFHRFHIE		BIT(1)
#define CAN_FIFOCON_TFERFFIE		BIT(2)
#define CAN_FIFOCON_RXOVIE		BIT(3)
#define CAN_FIFOCON_TXATIE		BIT(4)
#define CAN_FIFOCON_RXTSEN		BIT(5)
#define CAN_FIFOCON_RTREN		BIT(6)
#define CAN_FIFOCON_TXEN		BIT(7)
#define CAN_FIFOCON_UINC		BIT(8)
#define CAN_FIFOCON_TXREQ		BIT(9)
#define CAN_FIFOCON_FRESET		BIT(10)
#  define CAN_FIFOCON_TXPRI_BITS	5
#  define CAN_FIFOCON_TXPRI_SHIFT	16
#  define CAN_FIFOCON_TXPRI_MASK					\
	GENMASK(CAN_FIFOCON_TXPRI_SHIFT + CAN_FIFOCON_TXPRI_BITS - 1,	\
		CAN_FIFOCON_TXPRI_SHIFT)
#  define CAN_FIFOCON_TXAT_BITS		2
#  define CAN_FIFOCON_TXAT_SHIFT	21
#  define CAN_FIFOCON_TXAT_MASK					    \
	GENMASK(CAN_FIFOCON_TXAT_SHIFT + CAN_FIFOCON_TXAT_BITS - 1, \
		CAN_FIFOCON_TXAT_SHIFT)
#  define CAN_FIFOCON_FSIZE_BITS	5
#  define CAN_FIFOCON_FSIZE_SHIFT	24
#  define CAN_FIFOCON_FSIZE_MASK					\
	GENMASK(CAN_FIFOCON_FSIZE_SHIFT + CAN_FIFOCON_FSIZE_BITS - 1,	\
		CAN_FIFOCON_FSIZE_SHIFT)
#  define CAN_FIFOCON_PLSIZE_BITS	3
#  define CAN_FIFOCON_PLSIZE_SHIFT	29
#  define CAN_FIFOCON_PLSIZE_MASK					\
	GENMASK(CAN_FIFOCON_PLSIZE_SHIFT + CAN_FIFOCON_PLSIZE_BITS - 1, \
		CAN_FIFOCON_PLSIZE_SHIFT)
#define CAN_FIFOSTA(x)			CAN_SFR_BASE(0x60 + 12 * (x - 1))
#  define CAN_FIFOSTA_TFNRFNIF		BIT(0)
#  define CAN_FIFOSTA_TFHRFHIF		BIT(1)
#  define CAN_FIFOSTA_TFERFFIF		BIT(2)
#  define CAN_FIFOSTA_RXOVIF		BIT(3)
#  define CAN_FIFOSTA_TXATIF		BIT(4)
#  define CAN_FIFOSTA_RXTSEN		BIT(5)
#  define CAN_FIFOSTA_RTREN		BIT(6)
#  define CAN_FIFOSTA_TXEN		BIT(7)
#  define CAN_FIFOSTA_FIFOCI_BITS	5
#  define CAN_FIFOSTA_FIFOCI_SHIFT	8
#  define CAN_FIFOSTA_FIFOCI_MASK					\
	GENMASK(CAN_FIFOSTA_FIFOCI_SHIFT + CAN_FIFOSTA_FIFOCI_BITS - 1, \
		CAN_FIFOSTA_FIFOCI_SHIFT)
#define CAN_FIFOUA(x)			CAN_SFR_BASE(0x64 + 12 * (x - 1))
#define CAN_FLTCON(x)			CAN_SFR_BASE(0x1D0 + (x & 0x1c))
#  define CAN_FILCON_SHIFT(x)		((x & 3) * 8)
#  define CAN_FILCON_BITS(x)		4
#  define CAN_FILCON_MASK(x)					\
	GENMASK(CAN_FILCON_SHIFT(x) + CAN_FILCON_BITS(x) - 1,	\
		CAN_FILCON_SHIFT(x))
#  define CAN_FIFOCON_FLTEN(x)		BIT(7 + CAN_FILCON_SHIFT(x))
#define CAN_FLTOBJ(x)			CAN_SFR_BASE(0x1F0 + 8 * x)
#  define CAN_FILOBJ_SID_BITS		11
#  define CAN_FILOBJ_SID_SHIFT		0
#  define CAN_FILOBJ_SID_MASK					\
	GENMASK(CAN_FILOBJ_SID_SHIFT + CAN_FILOBJ_SID_BITS - 1, \
		CAN_FILOBJ_SID_SHIFT)
#  define CAN_FILOBJ_EID_BITS		18
#  define CAN_FILOBJ_EID_SHIFT		12
#  define CAN_FILOBJ_EID_MASK					\
	GENMASK(CAN_FILOBJ_EID_SHIFT + CAN_FILOBJ_EID_BITS - 1, \
		CAN_FILOBJ_EID_SHIFT)
#  define CAN_FILOBJ_SID11		BIT(29)
#  define CAN_FILOBJ_EXIDE		BIT(30)
#define CAN_FLTMASK(x)			CAN_SFR_BASE(0x1F4 + 8 * x)
#  define CAN_FILMASK_MSID_BITS		11
#  define CAN_FILMASK_MSID_SHIFT	0
#  define CAN_FILMASK_MSID_MASK					\
	GENMASK(CAN_FILMASK_MSID_SHIFT + CAN_FILMASK_MSID_BITS - 1, \
		CAN_FILMASK_MSID_SHIFT)
#  define CAN_FILMASK_MEID_BITS		18
#  define CAN_FILMASK_MEID_SHIFT	12
#  define CAN_FILMASK_MEID_MASK					\
	GENMASK(CAN_FILMASK_MEID_SHIFT + CAN_FILMASK_MEID_BITS - 1, \
		CAN_FILMASK_MEID_SHIFT)
#  define CAN_FILMASK_MSID11		BIT(29)
#  define CAN_FILMASK_MIDE		BIT(30)

#define CAN_OBJ_ID_SID_BITS		11
#define CAN_OBJ_ID_SID_SHIFT		0
#define CAN_OBJ_ID_SID_MASK					\
	GENMASK(CAN_OBJ_ID_SID_SHIFT + CAN_OBJ_ID_SID_BITS - 1, \
		CAN_OBJ_ID_SID_SHIFT)
#define CAN_OBJ_ID_EID_BITS		18
#define CAN_OBJ_ID_EID_SHIFT		11
#define CAN_OBJ_ID_EID_MASK					\
	GENMASK(CAN_OBJ_ID_EID_SHIFT + CAN_OBJ_ID_EID_BITS - 1, \
		CAN_OBJ_ID_EID_SHIFT)
#define CAN_OBJ_ID_SID_BIT11		BIT(29)

#define CAN_OBJ_FLAGS_DLC_BITS		4
#define CAN_OBJ_FLAGS_DLC_SHIFT		0
#define CAN_OBJ_FLAGS_DLC_MASK					      \
	GENMASK(CAN_OBJ_FLAGS_DLC_SHIFT + CAN_OBJ_FLAGS_DLC_BITS - 1, \
		CAN_OBJ_FLAGS_DLC_SHIFT)
#define CAN_OBJ_FLAGS_IDE		BIT(4)
#define CAN_OBJ_FLAGS_RTR		BIT(5)
#define CAN_OBJ_FLAGS_BRS		BIT(6)
#define CAN_OBJ_FLAGS_FDF		BIT(7)
#define CAN_OBJ_FLAGS_ESI		BIT(8)
#define CAN_OBJ_FLAGS_SEQ_BITS		7
#define CAN_OBJ_FLAGS_SEQ_SHIFT		9
#define CAN_OBJ_FLAGS_SEQ_MASK					      \
	GENMASK(CAN_OBJ_FLAGS_SEQ_SHIFT + CAN_OBJ_FLAGS_SEQ_BITS - 1, \
		CAN_OBJ_FLAGS_SEQ_SHIFT)
#define CAN_OBJ_FLAGS_FILHIT_BITS	11
#define CAN_OBJ_FLAGS_FILHIT_SHIFT	5
#define CAN_OBJ_FLAGS_FILHIT_MASK				      \
	GENMASK(CAN_FLAGS_FILHIT_SHIFT + CAN_FLAGS_FILHIT_BITS - 1, \
		CAN_FLAGS_FILHIT_SHIFT)

#define MCP2517FD_BUFFER_TXRX_SIZE 2048

/* ideally these would be defined in uapi/linux/can.h */
#define CAN_EFF_SID_SHIFT	(CAN_EFF_ID_BITS - CAN_SFF_ID_BITS)
#define CAN_EFF_SID_BITS	CAN_SFF_ID_BITS
#define CAN_EFF_SID_MASK				      \
	GENMASK(CAN_EFF_SID_SHIFT + CAN_EFF_SID_BITS - 1,     \
		CAN_EFF_SID_SHIFT)
#define CAN_EFF_EID_SHIFT	0
#define CAN_EFF_EID_BITS	CAN_EFF_SID_SHIFT
#define CAN_EFF_EID_MASK				      \
	GENMASK(CAN_EFF_EID_SHIFT + CAN_EFF_EID_BITS - 1,     \
		CAN_EFF_EID_SHIFT)



struct mcp2517fd_obj_tef {
	u32 id;
	u32 flags;
	u32 ts;
};

struct mcp2517fd_obj_tx {
	u32 id;
	u32 flags;
	u32 data[];
};

struct mcp2517fd_obj_rx {
	u32 id;
	u32 flags;
	u32 ts;
	u8 data[];
};

#define FIFO_DATA(x)			(0x400 + (x))
#define FIFO_DATA_SIZE			0x800

static const struct can_bittiming_const mcp2517fd_nominal_bittiming_const = {
	.name		= DEVICE_NAME,
	.tseg1_min	= 2,
	.tseg1_max	= BIT(CAN_NBTCFG_TSEG1_BITS),
	.tseg2_min	= 1,
	.tseg2_max	= BIT(CAN_NBTCFG_TSEG2_BITS),
	.sjw_max	= BIT(CAN_NBTCFG_SJW_BITS),
	.brp_min	= 1,
	.brp_max	= BIT(CAN_NBTCFG_BRP_BITS),
	.brp_inc	= 1,
};

static const struct can_bittiming_const mcp2517fd_data_bittiming_const = {
	.name		= DEVICE_NAME,
	.tseg1_min	= 1,
	.tseg1_max	= BIT(CAN_DBTCFG_TSEG1_BITS),
	.tseg2_min	= 1,
	.tseg2_max	= BIT(CAN_DBTCFG_TSEG2_BITS),
	.sjw_max	= BIT(CAN_DBTCFG_SJW_BITS),
	.brp_min	= 1,
	.brp_max	= BIT(CAN_DBTCFG_BRP_BITS),
	.brp_inc	= 1,
};

enum mcp2517fd_model {
	CAN_MCP2517FD	= 0x2517,
};

enum mcp2517fd_gpio_mode {
	gpio_mode_int		= 0,
	gpio_mode_standby	= MCP2517FD_IOCON_XSTBYEN,
	gpio_mode_out_low	= MCP2517FD_IOCON_PM0,
	gpio_mode_out_high	= MCP2517FD_IOCON_PM0 | MCP2517FD_IOCON_LAT0,
	gpio_mode_in		= MCP2517FD_IOCON_PM0 | MCP2517FD_IOCON_TRIS0
};

struct mcp2517fd_priv {
	struct can_priv	   can;
	struct net_device *net;
	struct spi_device *spi;
	struct dentry *debugfs_dir;

	struct workqueue_struct *wq;

	struct work_struct tx_work;
	struct sk_buff *tx_work_skb;

	enum mcp2517fd_model model;
	bool clock_pll;
	bool clock_div2;
	int  clock_odiv;

	enum mcp2517fd_gpio_mode  gpio0_mode;
	enum mcp2517fd_gpio_mode  gpio1_mode;
	bool gpio_opendrain;

	/* flags that should stay in the con_register */
	u32 con_val;

	u32 spi_setup_speed_hz;
	u32 spi_speed_hz;

	int payload_size;
	u8 payload_mode;

	u32 tef_address_start;
	u32 tef_address_end;
	u32 tef_address;

	u32 fifo_address[32];

	u8 tx_fifos;
	u8 tx_fifo_start;
	u32 tx_fifo_mask;
	u32 tx_pending_mask;

	u8 rx_fifos;
	u8 rx_fifo_depth;
	u8 rx_fifo_start;
	u32 rx_fifo_mask;
	u64 rx_overflow;

	struct {
		u32 intf;
		/* ASSERT(CAN_INT + 4 == CAN_RXIF) */
		u32 rxif;
		/* ASSERT(CAN_RXIF + 4 == CAN_TXIF) */
		u32 txif;
		/* ASSERT(CAN_TXIF + 4 == CAN_RXOVIF) */
		u32 rxovif;
		/* ASSERT(CAN_RXOVIF + 4 == CAN_TXATIF) */
		u32 txatif;
		/* ASSERT(CAN_TXATIF + 4 == CAN_TXREQ) */
		u32 txreq;
		/* ASSERT(CAN_TXREQ + 4 == CAN_TREC) */
		u32 trec;
		/* ASSERT(CAN_TREC + 4 == CAN_BDIAG0) */
		u32 bdiag0;
		/* ASSERT(CAN_BDIAG0 + 4 == CAN_BDIAG1) */
		u32 bdiag1;
	} status;

	int force_quit;
	int after_suspend;
#define AFTER_SUSPEND_UP 1
#define AFTER_SUSPEND_DOWN 2
#define AFTER_SUSPEND_POWER 4
#define AFTER_SUSPEND_RESTART 8
	int restart_tx;
	struct regulator *power;
	struct regulator *transceiver;
	struct clk *clk;
	/* this should be sufficiently aligned - no idea how to force an u32 alignment here... */
	u8 fifo_data[MCP2517FD_BUFFER_TXRX_SIZE];
	u8 spi_tx[MCP2517FD_BUFFER_TXRX_SIZE];
	u8 spi_rx[MCP2517FD_BUFFER_TXRX_SIZE];

	u64 fifo_usage[32];
};

static int mcp2517fd_sync_transfer(struct spi_device *spi,
				 struct spi_transfer *xfer,
				 unsigned int xfers,
				 int speed_hz)
{
	int i;

	for(i = 0; i < xfers; i++)
		xfer[i].speed_hz = speed_hz;

	return spi_sync_transfer(spi, xfer, xfers);
}

static int mcp2517fd_write_then_read(struct spi_device *spi,
				     const void* tx_buf,
				     unsigned int tx_len,
				     void* rx_buf,
				     unsigned int rx_len,
				     int speed_hz)
{
	static u8 *txrx;
	struct spi_transfer xfer[2];
	int ret;

	memset(xfer, 0, sizeof(*xfer));

	/* when using a halfduplex controller or to big for buffer */
	if (spi->master->flags & SPI_MASTER_HALF_DUPLEX) {
		xfer[0].tx_buf = tx_buf;
		xfer[0].len = tx_len;

		xfer[1].rx_buf = rx_buf;
		xfer[1].len = rx_len;

		return mcp2517fd_sync_transfer(spi, xfer, 2, speed_hz);
	}

	txrx = kmalloc(MCP2517FD_BUFFER_TXRX_SIZE * 2, GFP_KERNEL | GFP_DMA);
	if (! txrx)
		return -ENOMEM;

	/* full duplex optimization */
	xfer[0].tx_buf = txrx;
	xfer[0].rx_buf = txrx + MCP2517FD_BUFFER_TXRX_SIZE;
	xfer[0].len = tx_len + rx_len;

	/* copy and clean */
	memcpy(txrx, tx_buf, tx_len);
	memset(txrx + tx_len, 0, rx_len);

	ret = mcp2517fd_sync_transfer(spi, xfer, 1, speed_hz);

	if (!ret)
		memcpy(rx_buf, xfer[0].rx_buf + tx_len, rx_len);

	kfree(txrx);

	return ret;
}

static int mcp2517fd_write(struct spi_device *spi,
			   const void* tx_buf,
			   unsigned int tx_len,
			   int speed_hz)
{
	struct spi_transfer xfer;

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = tx_buf;
	xfer.len = tx_len;

	return mcp2517fd_sync_transfer(spi, &xfer, 1, speed_hz);
}


static int mcp2517fd_write_then_write(struct spi_device *spi,
				     const void* tx_buf,
				     unsigned int tx_len,
				     const void* tx2_buf,
				     unsigned int tx2_len,
				     int speed_hz)
{
	static u8 *txrx;
	struct spi_transfer xfer;

	txrx = kmalloc(tx_len + tx2_len, GFP_KERNEL | GFP_DMA);
	if (! txrx)
		return -ENOMEM;

	memset(&xfer, 0, sizeof(xfer));
	xfer.len = tx_len + tx2_len;
	xfer.tx_buf = txrx;

	memcpy(txrx, tx_buf, tx_len);
	memcpy(txrx + tx_len, tx2_buf, tx2_len);

	return mcp2517fd_sync_transfer(spi, &xfer, 1, speed_hz);
}

static void mcp2517fd_calc_cmd_addr(u16 cmd, u16 addr, u8 *data)
{
	cmd = cmd | (addr & ADDRESS_MASK);

	data[0] = (cmd >> 8) & 0xff;
	data[1] = (cmd >> 0) & 0xff;
}

static int mcp2517fd_cmd_reset(struct spi_device *spi, u32 speed_hz)
{
	u8 cmd[2];

	mcp2517fd_calc_cmd_addr(INSTRUCTION_RESET, 0, cmd);

	/* write the reset command */
	return mcp2517fd_write(spi, cmd, 2, speed_hz);
}

/* read multiple bytes, transform some registers */
static int mcp2517fd_cmd_readn(struct spi_device *spi, u32 reg,
			       void *data, int n, u32 speed_hz)
{
	u8 cmd[2];
	int ret;

	mcp2517fd_calc_cmd_addr(INSTRUCTION_READ, reg, cmd);

	ret = mcp2517fd_write_then_read(spi, &cmd, 2, data, n, speed_hz);
	if (ret)
		return ret;

	return 0;
}

static int mcp2517fd_convert_to_cpu(u32* data, int n)
{
	int i;

	for(i = 0; i < n; i++)
		data[i] = le32_to_cpu(data[i]);

	return 0;
}

/* read a register, but we are only interrested in a few bytes */
static int mcp2517fd_cmd_read_mask(struct spi_device *spi, u32 reg,
				   u32 *data, u32 mask, u32 speed_hz)
{
	int first_byte, last_byte, len_byte;
	int ret;

	/* check that at least one bit is set */
	if (! mask)
		return -EINVAL;

	/* calculate first and last byte used */
	first_byte = (ffs(mask) - 1)  >> 3;
	last_byte = (fls(mask) - 1)  >> 3;
	len_byte = last_byte - first_byte +1;

	/* do a partial read */
	*data = 0;
	ret=mcp2517fd_cmd_readn(spi, reg,
				((void *)data + first_byte), len_byte,
				speed_hz);
	if (ret)
		return ret;

	return mcp2517fd_convert_to_cpu(data, 1);
}

static int mcp2517fd_cmd_read(struct spi_device *spi, u32 reg, u32 *data,
			      u32 speed_hz)
{
	return mcp2517fd_cmd_read_mask(spi, reg, data, -1, speed_hz);
}

/* read a register, but we are only interrested in a few bytes */
static int mcp2517fd_cmd_write_mask(struct spi_device *spi, u32 reg,
				    u32 data, u32 mask, u32 speed_hz)
{
	int first_byte, last_byte, len_byte;
	u8 cmd[2];

	/* check that at least one bit is set */
	if (! mask)
		return -EINVAL;

	/* calculate first and last byte used */
	first_byte = (ffs(mask) - 1)  >> 3;
	last_byte = (fls(mask) - 1)  >> 3;
	len_byte = last_byte - first_byte + 1;

	/* prepare buffer */
	mcp2517fd_calc_cmd_addr(INSTRUCTION_WRITE, reg + first_byte, cmd);
	data = cpu_to_le32(data);

	return mcp2517fd_write_then_write(spi,
					  cmd, sizeof(cmd),
					  ((void *) &data + first_byte),
					  len_byte,
					  speed_hz);
}

static int mcp2517fd_cmd_write(struct spi_device *spi, u32 reg, u32 data,
			       u32 speed_hz)
{
	return mcp2517fd_cmd_write_mask(spi, reg, data, -1, speed_hz);
}

static int mcp2517fd_transmit_message_common(
	struct spi_device *spi, int fifo,
	struct mcp2517fd_obj_tx *obj, int len, u8 *data)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	u32 addr = FIFO_DATA(priv->fifo_address[fifo]);
	u8 d[2 + sizeof(*obj) + 64];
	int ret;

	/* add fifo as seq */
	obj->flags |= fifo << CAN_OBJ_FLAGS_SEQ_SHIFT;

	/* transform to le32 */
	obj->id = cpu_to_le32(obj->id);
	obj->flags = cpu_to_le32(obj->flags);

	/* copy data to the right places- without leaking heap data */
	memset(d, 0, sizeof(*d));
	mcp2517fd_calc_cmd_addr(INSTRUCTION_WRITE,addr, d);
	memcpy(d + 2, obj, sizeof(*obj));
	memcpy(d + 2 + sizeof(*obj), data, len);

	/* transfers to FIFO RAM have to be multiple of 4 */
	len = 2 + sizeof(*obj) + ALIGN(len, 4);

	/* fill the fifo */
	ret = mcp2517fd_write(spi, d, len, priv->spi_speed_hz);
	if (ret)
		return NETDEV_TX_BUSY;

	/* and trigger it */
	ret = mcp2517fd_cmd_write_mask(spi, CAN_FIFOCON(fifo),
				       CAN_FIFOCON_TXREQ | CAN_FIFOCON_UINC,
				       CAN_FIFOCON_TXREQ | CAN_FIFOCON_UINC,
				       priv->spi_speed_hz);
	if (ret)
		return NETDEV_TX_BUSY;

	return NETDEV_TX_OK;
}

static void mcp2517fd_canid_to_mcpid(u32 can_id, u32 *id, u32 *flags)
{
	if (can_id & CAN_EFF_FLAG) {
		int sid = (can_id & CAN_EFF_SID_MASK) >> CAN_EFF_SID_SHIFT;
		int eid = (can_id & CAN_EFF_EID_MASK) >> CAN_EFF_EID_SHIFT;
		*id = (eid << CAN_OBJ_ID_EID_SHIFT) |
			(sid << CAN_OBJ_ID_SID_SHIFT);
		*flags = CAN_OBJ_FLAGS_IDE;
	} else {
		*id = can_id & CAN_SFF_MASK;
		*flags = 0;
	}

	*flags |= (can_id & CAN_RTR_FLAG) ? CAN_OBJ_FLAGS_RTR : 0;
}

static int mcp2517fd_transmit_fdmessage(struct spi_device *spi, int fifo,
					struct canfd_frame *frame)
{
	struct mcp2517fd_obj_tx obj;
	int dlc = can_len2dlc(frame->len);

	frame->len = can_dlc2len(dlc);

	mcp2517fd_canid_to_mcpid(frame->can_id, &obj.id, &obj.flags);

	obj.flags |= dlc << CAN_OBJ_FLAGS_DLC_SHIFT;
	obj.flags |= (frame->can_id & CAN_EFF_FLAG) ? CAN_OBJ_FLAGS_IDE : 0;
	obj.flags |= (frame->can_id & CAN_RTR_FLAG) ? CAN_OBJ_FLAGS_RTR : 0;
	obj.flags |= (frame->flags & CANFD_BRS) ? CAN_OBJ_FLAGS_BRS : 0;
	obj.flags |= (frame->flags & CANFD_ESI) ? CAN_OBJ_FLAGS_ESI : 0;
	obj.flags |= CAN_OBJ_FLAGS_FDF;

	return mcp2517fd_transmit_message_common(
		spi, fifo, &obj, frame->len, frame->data);
}

static int mcp2517fd_transmit_message(struct spi_device *spi, int fifo,
				      struct can_frame *frame)
{
	struct mcp2517fd_obj_tx obj;

	if (frame->can_dlc > 8)
		frame->can_dlc = 8;

	mcp2517fd_canid_to_mcpid(frame->can_id, &obj.id, &obj.flags);

	obj.flags |= frame->can_dlc << CAN_OBJ_FLAGS_DLC_SHIFT;
	obj.flags |= (frame->can_id & CAN_EFF_FLAG) ? CAN_OBJ_FLAGS_IDE : 0;
	obj.flags |= (frame->can_id & CAN_RTR_FLAG) ? CAN_OBJ_FLAGS_RTR : 0;

	return mcp2517fd_transmit_message_common(
		spi, fifo, &obj, frame->can_dlc, frame->data);
}

static void mcp2517fd_tx_work_handler(struct work_struct *ws)
{
        struct mcp2517fd_priv *priv = container_of(ws,
						   struct mcp2517fd_priv,
						   tx_work);
        struct spi_device *spi = priv->spi;
	struct sk_buff *skb = priv->tx_work_skb;
	int fifo;
	int ret;
	u32 pending_mask;

	/* trying to avoid a workqueue */
	pending_mask = priv->tx_pending_mask;

	/* decide on fifo to assign */
	if (pending_mask) {
		fifo = ffs(pending_mask) - 2;
	} else {
		fifo = priv->tx_fifo_start + priv->tx_fifos - 1;
	}

	/* handle error - this should not happen... */
	if (fifo < priv->tx_fifo_start) {
		dev_err(&spi->dev,
			"reached tx-fifo %i, which is not valid\n",
			fifo);
		return;
	}

	/* decide if we can reactivate the queue */
	priv->tx_work_skb = NULL;
	if (fifo > priv->tx_fifo_start)
		netif_start_queue(priv->net);

	/* mark as pending */
	priv->tx_pending_mask |= BIT(fifo);
	priv->fifo_usage[fifo]++;

	/* now process it for real */
	if (can_is_canfd_skb(skb))
		ret = mcp2517fd_transmit_fdmessage(
			spi, fifo, (struct canfd_frame *)skb->data);
	else
		ret = mcp2517fd_transmit_message(
			spi, fifo, (struct can_frame *)skb->data);

	/* keep it for reference until the message really got transmitted */
	if (ret == NETDEV_TX_OK)
		can_put_echo_skb(skb, priv->net, fifo);
}

static netdev_tx_t mcp2517fd_start_xmit(struct sk_buff *skb,
					struct net_device *net)
{
        struct mcp2517fd_priv *priv = netdev_priv(net);
	struct spi_device *spi = priv->spi;

        if (priv->tx_work_skb) {
		dev_warn(&spi->dev, "hard_xmit called while tx busy\n");
		return NETDEV_TX_BUSY;
	}

	if (can_dropped_invalid_skb(net, skb))
		return NETDEV_TX_OK;

	netif_stop_queue(net);

	priv->tx_work_skb = skb;
	queue_work(priv->wq, &priv->tx_work);

	return NETDEV_TX_OK;
}

static void mcp2517fd_hw_sleep(struct spi_device *spi)
{
}

static int mcp2517fd_power_enable(struct regulator *reg, int enable)
{
	if (IS_ERR_OR_NULL(reg))
		return 0;

	if (enable)
		return regulator_enable(reg);
	else
		return regulator_disable(reg);
}

static int mcp2517fd_do_set_mode(struct net_device *net, enum can_mode mode)
{

	switch (mode) {
	case CAN_MODE_START:
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mcp2517fd_do_set_nominal_bittiming(struct net_device *net)
{
	struct mcp2517fd_priv *priv = netdev_priv(net);
	struct can_bittiming *bt = &priv->can.bittiming;
	struct spi_device *spi = priv->spi;

	/* calculate nominal bit timing */
	u32 val = ((bt->sjw - 1) << CAN_NBTCFG_SJW_SHIFT)
		| ((bt->phase_seg2 - 1) << CAN_NBTCFG_TSEG2_SHIFT)
		| ((bt->phase_seg1 + bt->prop_seg - 1)
		   << CAN_NBTCFG_TSEG1_SHIFT)
		| ((bt->brp) << CAN_NBTCFG_BRP_SHIFT);

	return mcp2517fd_cmd_write(spi, CAN_NBTCFG, val,
				   priv->spi_setup_speed_hz);
}

static int mcp2517fd_do_set_data_bittiming(struct net_device *net)
{
	struct mcp2517fd_priv *priv = netdev_priv(net);
	struct can_bittiming *bt = &priv->can.data_bittiming;
	struct spi_device *spi = priv->spi;

	/* calculate data bit timing */
	u32 val = ((bt->sjw - 1) << CAN_DBTCFG_SJW_SHIFT)
		| ((bt->phase_seg2 - 1) << CAN_DBTCFG_TSEG2_SHIFT)
		| ((bt->phase_seg1 + bt->prop_seg - 1)
		   << CAN_DBTCFG_TSEG1_SHIFT)
		| ((bt->brp) << CAN_DBTCFG_BRP_SHIFT);

	return mcp2517fd_cmd_write(spi, CAN_DBTCFG, val,
				   priv->spi_setup_speed_hz);
}

static void mcp2517fd_open_clean(struct net_device *net)
{
	struct mcp2517fd_priv *priv = netdev_priv(net);
	struct spi_device *spi = priv->spi;

	free_irq(spi->irq, priv);
	mcp2517fd_hw_sleep(spi);
	mcp2517fd_power_enable(priv->transceiver, 0);
	close_candev(net);
}

static int mcp2517fd_hw_probe(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	u32 val;
	int ret;

	/* Wait for oscillator startup timer after power up */
	mdelay(MCP2517FD_OST_DELAY_MS);

	/* send a "blind" reset, hoping we are in Config mode */
	mcp2517fd_cmd_reset(spi, priv->spi_setup_speed_hz);

	/* Wait for oscillator startup again */
	mdelay(MCP2517FD_OST_DELAY_MS);

	/* check clock register that the clock is ready or disabled */
	ret = mcp2517fd_cmd_read(spi, MCP2517FD_OSC, &val,
				 priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	dev_err(&spi->dev, "Osc reg: %08x\n", val);

	/* there can only be one... */
	switch (val & (MCP2517FD_OSC_OSCRDY | MCP2517FD_OSC_OSCDIS)) {
	case MCP2517FD_OSC_OSCRDY: /* either the clock is ready */
		break;
	case MCP2517FD_OSC_OSCDIS: /* or the clock is disabled */
		/* setup clock with defaults - only CLOCKDIV 10 */
		ret = mcp2517fd_cmd_write(
			spi, MCP2517FD_OSC,
			MCP2517FD_OSC_CLKODIV_10
			<< MCP2517FD_OSC_CLKODIV_SHIFT,
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		break;
	default:
		/* otherwise it is no valid device (or in strange state) */

		/*
		 * if PLL is enabled but not ready, then there may be
		 * something "fishy"
		 * this happened during driver development
		 * (enabling pll, when when on wrong clock), so best warn about
		 * such a possibility
		 */
		if ((val & (MCP2517FD_OSC_PLLEN | MCP2517FD_OSC_PLLRDY))
		    == MCP2517FD_OSC_PLLEN)
			dev_err(&spi->dev,
				"mcp2517fd may be in a strange state"
				" - a power disconnect may be required\n");

		return -ENODEV;
		break;
	}

	/* check if we are in config mode already*/

	/* read CON register and match */
	ret = mcp2517fd_cmd_read(spi, CAN_CON, &val,
				 priv->spi_setup_speed_hz);
	if (ret)
		return ret;
	dev_err(&spi->dev, "CAN_CON 0x%08x\n",val);

	/* apply mask and check */
	if ((val & CAN_CON_DEFAULT_MASK) == CAN_CON_DEFAULT)
		return 0;

	/*
	 * as per datasheet a reset only works in Config Mode
	 * so as we have in principle no knowledge of the current
	 * mode that the controller is in we have no safe way
	 * to detect the device correctly
	 * hence we need to "blindly" put the controller into
	 * config mode.
	 * on the "save" side, the OSC reg has to be valid already,
	 * so there is a chance we got the controller...
	 */

	/* blindly force it into config mode */
	ret = mcp2517fd_cmd_write(spi, CAN_CON, CAN_CON_DEFAULT,
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* delay some time */
	mdelay(MCP2517FD_OST_DELAY_MS);

	/* reset can controller */
	mcp2517fd_cmd_reset(spi, priv->spi_setup_speed_hz);

	/* delay some time */
	mdelay(MCP2517FD_OST_DELAY_MS);

	/* read CON register and match a final time */
	ret = mcp2517fd_cmd_read(spi, CAN_CON, &val,
				 priv->spi_setup_speed_hz);
	if (ret)
		return ret;
	dev_dbg(&spi->dev, "CAN_CON 0x%08x\n",val);

	/* apply mask and check */
	return ((val & CAN_CON_DEFAULT_MASK) != CAN_CON_DEFAULT) ?
		-ENODEV : 0;
}

static int mcp2517fd_set_normal_mode(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	int type = 0;
	int ret;

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		type = CAN_CON_MODE_EXTERNAL_LOOPBACK;
	else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		type = CAN_CON_MODE_LISTENONLY;
	else if (priv->can.ctrlmode & CAN_CTRLMODE_FD)
		type = CAN_CON_MODE_MIXED;
	else
		type = CAN_CON_MODE_CAN2_0;

	/* set mode to normal */
	ret = mcp2517fd_cmd_write(spi, CAN_CON,
				  priv->con_val |
				  (type << CAN_CON_REQOP_SHIFT),
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	dev_err(&spi->dev, "  CanCTRL: %i\n",
		(priv->can.ctrlmode & CAN_CTRLMODE_FD));

	return 0;
}

static int mcp2517fd_setup_osc(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	int val = ((priv->clock_pll) ? MCP2517FD_OSC_PLLEN : 0)
		| ((priv->clock_div2) ? MCP2517FD_OSC_SCLKDIV : 0);
	int waitfor = ((priv->clock_pll) ? MCP2517FD_OSC_PLLRDY : 0)
		| ((priv->clock_div2) ? MCP2517FD_OSC_SCLKRDY : 0)
		| MCP2517FD_OSC_OSCRDY;
	int ret;
	unsigned long timeout;

	/* manage clock_out divider */
	switch(priv->clock_odiv) {
	case 10:
		val |= (MCP2517FD_OSC_CLKODIV_10)
			<< MCP2517FD_OSC_CLKODIV_SHIFT;
		break;
	case 4:
		val |= (MCP2517FD_OSC_CLKODIV_4)
			<< MCP2517FD_OSC_CLKODIV_SHIFT;
		break;
	case 2:
		val |= (MCP2517FD_OSC_CLKODIV_2)
			<< MCP2517FD_OSC_CLKODIV_SHIFT;
		break;
	case 1:
		val |= (MCP2517FD_OSC_CLKODIV_1)
			<< MCP2517FD_OSC_CLKODIV_SHIFT;
		break;
	case 0:
		/* this means implicitly SOF output */
		val |= (MCP2517FD_OSC_CLKODIV_10)
			<< MCP2517FD_OSC_CLKODIV_SHIFT;
		break;
	default:
		dev_err(&spi->dev,
			"Unsupported output clock divider %i\n",
			priv->clock_odiv);
		return -EINVAL;
	}

	/* write clock */
	ret = mcp2517fd_cmd_write(spi, MCP2517FD_OSC, val,
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* wait for synced pll/osc/sclk */
	timeout = jiffies + MCP2517FD_OSC_POLLING_JIFFIES;
	while(jiffies <= timeout) {
		ret = mcp2517fd_cmd_read(spi, MCP2517FD_OSC, &val,
					 priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		dev_err(&spi->dev,
			"Read OSC 0x%08x - wait 0x%08x\n",val,waitfor);
		if ((val & waitfor) == waitfor)
			return 0;
	}

	dev_err(&spi->dev,
		"Clock did not lock within the timeout period\n");

	/* we timed out */
	return -ENODEV;
}

static int mcp2517fd_setup_fifo(struct net_device *net,
				struct mcp2517fd_priv *priv,
				struct spi_device *spi)
{
	u32 con_val = priv->con_val;
	u32 val;
	int ret;
	int i, fifo;

	/* clear all filter */
	for (i = 0; i < 32; i++) {
		ret = mcp2517fd_cmd_write(spi, CAN_FLTOBJ(i), 0,
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		ret = mcp2517fd_cmd_write(spi, CAN_FLTMASK(i), 0,
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		ret = mcp2517fd_cmd_write_mask(
			spi, CAN_FLTCON(i), 0,
			CAN_FILCON_MASK(i),
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;
	}

	/* decide on TEF, tx and rx FIFOS */
	switch (net->mtu) {
	case CAN_MTU:
		/* note: if we have INT1 connected to a GPIO
		 * then we could handle this differently and more
		 * efficiently
		 */

		/* mtu is 8 */
		priv->payload_size = 8;
		priv->payload_mode = CAN_TXQCON_PLSIZE_8;

		/* 7 tx fifos starting at fifo 1 */
		priv->tx_fifo_start = 1;
		priv->tx_fifos = 7;

		/* 24 rx fifos starting at fifo 8 with 2 buffers/fifo */
		priv->rx_fifo_start = 8;
		priv->rx_fifos = 24;
		priv->rx_fifo_depth = 1;

		break;
	case CANFD_MTU:
		/* wish there was a way to have hw filters
		 * that can separate based on length ...
		 */
		/* MTU is 64 */
		priv->payload_size = 64;
		priv->payload_mode = CAN_TXQCON_PLSIZE_64;

		/* 7 tx fifos starting at fifo 1 */
		priv->tx_fifo_start = 1;
		priv->tx_fifos = 7;

		/* 19 rx fifos starting at fifo 8 with 1 buffer/fifo */
		priv->rx_fifo_start = 8;
		priv->rx_fifos = 19;
		priv->rx_fifo_depth = 1;

		break;
	default:
		return -EINVAL;
	}

	/* set up TEF SIZE to the number of tx_fifos and IRQ */
	ret = mcp2517fd_cmd_write(
		spi, CAN_TEFCON,
		CAN_TEFCON_FRESET |
		CAN_TEFCON_TEFNEIE |
		CAN_TEFCON_TEFTSEN |
		((priv->tx_fifos - 1) << CAN_TEFCON_FSIZE_SHIFT),
		priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* set up tx fifos */
	for (i = 0; i < priv->tx_fifos; i++) {
		fifo = priv->tx_fifo_start + i;
		ret = mcp2517fd_cmd_write(
			spi, CAN_FIFOCON(fifo),
			CAN_FIFOCON_FRESET | /* reset FIFO */
			(priv->payload_mode << CAN_FIFOCON_PLSIZE_SHIFT) |
			(0 << CAN_FIFOCON_FSIZE_SHIFT) | /* 1 FIFO only */
			(fifo << CAN_FIFOCON_TXPRI_SHIFT) | /* priority */
			CAN_FIFOCON_TXEN,
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		priv->tx_fifo_mask |= BIT(fifo);
	}

	/* now set up RX FIFO */
	for (i = 0; i < priv->rx_fifos; i++) {
		fifo = priv->rx_fifo_start + i;
		/* prepare the fifo itself */
		ret = mcp2517fd_cmd_write(
			spi, CAN_FIFOCON(fifo),
			(priv->payload_mode << CAN_FIFOCON_PLSIZE_SHIFT) |
			((priv->rx_fifo_depth - 1) << CAN_FIFOCON_FSIZE_SHIFT) |
			CAN_FIFOCON_RXTSEN | /* RX timestamps */
			CAN_FIFOCON_FRESET | /* reset FIFO */
			CAN_FIFOCON_TFERFFIE | /* FIFO Full */
			CAN_FIFOCON_TFHRFHIE | /* FIFO Half Full*/
			CAN_FIFOCON_TFNRFNIE | /* FIFO not empty */
			/* on the last fifo add overflow flag */
			((i == priv->rx_fifos - 1) ? CAN_FIFOCON_RXOVIE : 0),
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		/* prepare the rx filter config: filter i directs to fifo
		 * FLTMSK and FLTOBJ are 0 already, so they match everything
		 */
		ret = mcp2517fd_cmd_write_mask(
			spi, CAN_FLTCON(i),
			CAN_FIFOCON_FLTEN(i) | (fifo << CAN_FILCON_SHIFT(i)),
			CAN_FIFOCON_FLTEN(i) | CAN_FILCON_MASK(i),
			priv->spi_setup_speed_hz);
		if (ret)
			return ret;

		priv->rx_fifo_mask |= BIT(fifo);
	}

	/* we need to move out of CONFIG mode shortly to get the addresses */
	ret = mcp2517fd_cmd_write(
		spi, CAN_CON, con_val |
		(CAN_CON_MODE_INTERNAL_LOOPBACK << CAN_CON_REQOP_SHIFT),
		priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* for the TEF fifo */
	ret = mcp2517fd_cmd_read(spi, CAN_TEFUA, &val,
				 priv->spi_setup_speed_hz);
	if (ret)
		return ret;
	priv->tef_address = val;
	priv->tef_address_start = val;
	priv->tef_address_end = priv->tef_address_start +
		(priv->tx_fifos + 1) * sizeof(struct mcp2517fd_obj_tef) -
		1;
	dev_err(&spi->dev," TEF-FIFO: %03x - %03x\n",
		priv->tef_address_start, priv->tef_address_end);

	/* get all the relevant addresses for the transmit fifos */
	for (i = 0; i < priv->tx_fifos; i++) {
		fifo = priv->tx_fifo_start + i;
		ret = mcp2517fd_cmd_read(spi, CAN_FIFOUA(fifo),
					 &val, priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		/* normalize val to RAM address */
		priv->fifo_address[fifo] = val;

		dev_err(&spi->dev," TX-FIFO%02i: %04x\n",
			fifo, priv->fifo_address[fifo]);
	}

	for (i = 0; i < priv->rx_fifos; i++) {
		fifo = priv->rx_fifo_start + i;
		ret = mcp2517fd_cmd_read(spi, CAN_FIFOUA(fifo),
					 &val, priv->spi_setup_speed_hz);
		if (ret)
			return ret;
		priv->fifo_address[fifo] = val;

		dev_err(&spi->dev," RX-FIFO%02i: %04x\n",
			fifo, priv->fifo_address[fifo]);
	}

	/* now get back into config mode */
	ret = mcp2517fd_cmd_write(
		spi, CAN_CON, con_val |
		(CAN_CON_MODE_CONFIG << CAN_CON_REQOP_SHIFT),
		priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	return 0;
}

static int mcp2517fd_disable_interrupts(struct spi_device *spi,
					u32 speed_hz)
{
	return mcp2517fd_cmd_write(spi, CAN_INT, 0, speed_hz);
}

static int mcp2517fd_enable_interrupts(struct spi_device *spi,
				       u32 speed_hz)
{
	return mcp2517fd_cmd_write(spi, CAN_INT,
				   CAN_INT_TEFIE |
				   CAN_INT_RXIE,
				   speed_hz);
}

static int mcp2517fd_setup(struct net_device *net,
			   struct mcp2517fd_priv *priv,
			   struct spi_device *spi)
{
	u32 val;
	int ret;

	dev_err(&spi->dev, "Start_setup\n");

	/* set up pll/clock if required */
	ret = mcp2517fd_setup_osc(spi);
	if (ret)
		return ret;

	/* set up RAM ECC (but for now without interrupts) */
	ret = mcp2517fd_cmd_write(spi, MCP2517FD_ECCCON,
				  MCP2517FD_ECCCON_ECCEN,
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* GPIO handling - could expose this as gpios*/
	val = 0; /* PUSHPULL INT , TXCAN PUSH/PULL, no Standby */
	val |= MCP2517FD_IOCON_TXCANOD; /* OpenDrain TXCAN */
	val |= MCP2517FD_IOCON_INTOD; /* OpenDrain INT pins */

	/* SOF/CLOCKOUT pin 3 */
	if (priv->clock_odiv < 0)
		val |= MCP2517FD_IOCON_SOF;
	/* GPIO0 - pin 9 */
	switch (priv->gpio0_mode) {
	case gpio_mode_standby:
	case gpio_mode_int: /* asserted low on TXIF */
	case gpio_mode_out_low:
	case gpio_mode_out_high:
	case gpio_mode_in:
		val |= priv->gpio0_mode;
		break;
	default: /* GPIO IN */
		dev_err(&spi->dev,
			"GPIO1 does not support mode %08x\n",
			priv->gpio0_mode);
		return -EINVAL;
	}
	/* GPIO1 - pin 8 */
	switch (priv->gpio1_mode) {
	case gpio_mode_standby:
		dev_err(&spi->dev,
			"GPIO1 does not support transciever standby\n");
		return -EINVAL;
	case gpio_mode_int: /* asserted low on RXIF */
	case gpio_mode_out_low:
	case gpio_mode_out_high:
	case gpio_mode_in:
		val |= priv->gpio1_mode << 1;
		break;
	default:
		dev_err(&spi->dev,
			"GPIO1 does not support mode %08x\n",
			priv->gpio0_mode);
		return -EINVAL;
	}
	/* INT/GPIO pins as open drain */
	if (priv->gpio_opendrain)
		val |= MCP2517FD_IOCON_INTOD;

	ret = mcp2517fd_cmd_write(spi, MCP2517FD_IOCON, val,
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* set up Transmitter Delay compensation */
	ret = mcp2517fd_cmd_write(spi, CAN_TDC, CAN_TDC_EDGFLTEN,
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* time stamp control register - 1ns resolution, but disabled */
	ret = mcp2517fd_cmd_write(spi, CAN_TBC, 0,
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;
	ret = mcp2517fd_cmd_write(spi, CAN_TSCON,
				  CAN_TSCON_TBCEN |
				  ((priv->can.clock.freq / 1000000)
				   << CAN_TSCON_TBCPRE_SHIFT),
				  priv->spi_setup_speed_hz);
	if (ret)
		return ret;

	/* setup value of con_register */
	priv->con_val = CAN_CON_STEF /* enable TEF */;
	/* non iso FD mode */
	if (!(priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO))
		priv->con_val |= CAN_CON_ISOCRCEN;
	/* one shot */
	if (!(priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT))
		priv->con_val |= CAN_CON_RTXAT;

	/* setup fifos - this also puts the system into sleep mode */
	ret = mcp2517fd_setup_fifo(net, priv, spi);
	if (ret)
		return ret;

	/* interrupt configuration */
	return mcp2517fd_enable_interrupts(spi,
					   priv->spi_setup_speed_hz);
}

static void mcp2517fd_mcpid_to_canid(u32 mcpid, u32 mcpflags, u32 *id)
{
	u32 sid = (mcpid & CAN_OBJ_ID_SID_MASK) >> CAN_OBJ_ID_SID_SHIFT;
	u32 eid = (mcpid & CAN_OBJ_ID_EID_MASK) >> CAN_OBJ_ID_EID_SHIFT;
	if (mcpflags & CAN_OBJ_FLAGS_IDE) {
		*id = (eid << CAN_EFF_EID_SHIFT) |
			(sid << CAN_EFF_SID_SHIFT) |
			CAN_EFF_FLAG;
	} else {
		*id = sid;
	}

	*id |= (mcpflags & CAN_OBJ_FLAGS_RTR) ? CAN_RTR_FLAG : 0;
}

static int mcp2517fd_can_transform_rx_fd(struct spi_device *spi,
					 struct mcp2517fd_obj_rx *rx)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	struct canfd_frame *frame;
	struct sk_buff *skb;

	/* allocate the skb buffer */
	skb = alloc_canfd_skb(priv->net, &frame);
        if (!skb) {
                dev_err(&spi->dev, "cannot allocate RX skb\n");
                priv->net->stats.rx_dropped++;
                return -ENOMEM;
        }

	mcp2517fd_mcpid_to_canid(rx->id, rx->flags, &frame->can_id);
	frame->flags |= (rx->flags & CAN_OBJ_FLAGS_BRS) ? CANFD_BRS : 0;
	frame->flags |= (rx->flags & CAN_OBJ_FLAGS_ESI) ? CANFD_ESI : 0;

	frame->len = can_dlc2len((rx->flags & CAN_OBJ_FLAGS_DLC_MASK)
				 >> CAN_OBJ_FLAGS_DLC_SHIFT);

	memcpy(frame->data, rx->data, frame->len);

	priv->net->stats.rx_packets++;
        priv->net->stats.rx_bytes += frame->len;

        can_led_event(priv->net, CAN_LED_EVENT_RX);

        netif_rx_ni(skb);

	return 0;
}

static int mcp2517fd_can_transform_rx_normal(struct spi_device *spi,
					     struct mcp2517fd_obj_rx *rx)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	struct sk_buff *skb;
	struct can_frame *frame;
	int len;

	/* allocate the skb buffer */
	skb = alloc_can_skb(priv->net, &frame);
        if (!skb) {
                dev_err(&spi->dev, "cannot allocate RX skb\n");
                priv->net->stats.rx_dropped++;
                return -ENOMEM;
        }

	mcp2517fd_mcpid_to_canid(rx->id, rx->flags, &frame->can_id);

	frame->can_dlc = (rx->flags & CAN_OBJ_FLAGS_DLC_MASK)
		>> CAN_OBJ_FLAGS_DLC_SHIFT;

	len = can_dlc2len(frame->can_dlc);

	memcpy(frame->data, rx->data, len);

	priv->net->stats.rx_packets++;
        priv->net->stats.rx_bytes += len;

        can_led_event(priv->net, CAN_LED_EVENT_RX);

        netif_rx_ni(skb);

	return 0;
}


static int mcp2517fd_can_ist_handle_rxfifo(struct spi_device *spi,
					   int fifo)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	struct mcp2517fd_obj_rx *rx;
	int ret;

	/* calc the buffer address */
	rx = (struct mcp2517fd_obj_rx *)(priv->fifo_data +
					 priv->fifo_address[fifo]);
	priv->fifo_usage[fifo]++;

	/* transform the data to system byte order */
	rx->id = le32_to_cpu(rx->id);
	rx->flags = le32_to_cpu(rx->flags);
	rx->ts = le32_to_cpu(rx->ts);

	/* clear the fifo */
	ret = mcp2517fd_cmd_write_mask(
		spi, CAN_FIFOCON(fifo),
		CAN_FIFOCON_UINC | CAN_FIFOCON_FRESET*0,
		CAN_FIFOCON_UINC | CAN_FIFOCON_FRESET*0,
		priv->spi_speed_hz);
	if (ret)
		return ret;

	/* increment usage */

	/* submit the fifo to the network stack */
	if (rx->flags & CAN_OBJ_FLAGS_FDF)
		return mcp2517fd_can_transform_rx_fd(spi, rx);
	else
		return mcp2517fd_can_transform_rx_normal(spi, rx);
}

static int mcp2517fd_can_ist_handle_rxif(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	u32 mask = priv->status.rxif;
	u32 fifo_size = sizeof(struct mcp2517fd_obj_rx) +
		((priv->can.ctrlmode & CAN_CTRLMODE_FD) ? 64 : 8);
	int i, j;
	int ret;

	if (!mask)
		return 0;

	/* read all the "open" segments in big chunks */
	for(i = priv->rx_fifo_start ;
	    i < priv->rx_fifo_start + priv->rx_fifos;
	    i++) {
		if (mask & BIT(i)) {
			/* find the last set bit in sequence */
			for(j = i ;
			    (j < priv->rx_fifo_start + priv->rx_fifos) &&
				    (mask & BIT(j));
			    j++) {
				mask &= ~BIT(j);
			}

			/* now we got start and end, so read the range */
			ret = mcp2517fd_cmd_readn(
				spi, FIFO_DATA(priv->fifo_address[i]),
				priv->fifo_data + priv->fifo_address[i],
				(j - i) * fifo_size,
				priv->spi_speed_hz);
			if (ret)
				return ret;

			/* now handle those messages */
			for (; i < j ; i++) {
				ret = mcp2517fd_can_ist_handle_rxfifo(
					spi, i);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}


static int mcp2517fd_can_ist_handle_tefif(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	struct net_device *net = priv->net;
	struct mcp2517fd_obj_tef tef;
	u32 mask = 0;
	int i, count, ret, fifo;

	/* calculate the number of fifos that have been processed */
	count = hweight_long(priv->tx_pending_mask);
	count -= hweight_long(priv->status.txreq);

	/* now clear TEF for each */
	for(i = 0; i < count; i++) {
		ret = mcp2517fd_cmd_readn(spi,
					  FIFO_DATA(priv->tef_address),
					  &tef, sizeof(tef),
					  priv->spi_speed_hz);
		ret = mcp2517fd_cmd_write_mask(spi,
					CAN_TEFCON,
					CAN_TEFCON_UINC,
					CAN_TEFCON_UINC,
					priv->spi_speed_hz);
		/* and release it */
		fifo = (tef.flags & CAN_OBJ_FLAGS_SEQ_MASK) >>
			CAN_OBJ_FLAGS_SEQ_SHIFT;
		can_get_echo_skb(priv->net, fifo);

		/* increment tef */
		priv->tef_address += sizeof(tef);
		if (priv->tef_address > priv->tef_address_end)
			priv->tef_address = priv->tef_address_start;

		/* and set mask */
		mask |= BIT(fifo);

		net->stats.tx_packets++;
		net->stats.tx_bytes += 0/* TODO */;
		can_led_event(net, CAN_LED_EVENT_TX);
	}

	/* release fifos for the future */
	priv->tx_pending_mask &= ~mask;

	return 0;
}

static void mcp2517fd_error_skb(struct net_device *net,
			       int can_id, int data1)
{
	struct sk_buff *skb;
	struct can_frame *frame;

	skb = alloc_can_err_skb(net, &frame);
	if (skb) {
		frame->can_id = can_id;
		frame->data[1] = data1;
		netif_rx_ni(skb);
	} else {
		netdev_err(net, "cannot allocate error skb\n");
	}
}

static int mcp2517fd_can_ist_handle_rxovif(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	u32 mask = priv->status.rxovif;
	int canid = 0;
	int data1 = 0;
	int i;
	int ret;

	/* clear all fifos that have an overflow bit set */
	for(i = 0; i < 32; i++) {
		if (mask & BIT(i)) {
			ret = mcp2517fd_cmd_write_mask(spi,
						       CAN_FIFOSTA(i),
						       0,
						       CAN_FIFOSTA_RXOVIF,
						       priv->spi_speed_hz);
			if (ret)
				return ret;
			/* update statistics */
			priv->net->stats.rx_over_errors++;
			priv->net->stats.rx_errors++;
			priv->rx_overflow++;
			canid |= CAN_ERR_CRTL;
			data1 |= CAN_ERR_CRTL_RX_OVERFLOW;
		}
	}

	/* and send error packet */
	if (canid)
		mcp2517fd_error_skb(priv->net, canid, data1);

	return 0;
}

static int mcp2517fd_can_ist_handle_status(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	int ret;

	/* handle the rx */
	if (priv->status.intf & CAN_INT_RXIF) {
		ret = mcp2517fd_can_ist_handle_rxif(spi);
		if (ret)
			return ret;
	}

	/* handle the tef */
	if (priv->status.intf & CAN_INT_TEFIF) {
		ret = mcp2517fd_can_ist_handle_tefif(spi);
		if (ret)
			return ret;
	}

	/* handle errors */
	if (priv->status.rxovif) {
		ret = mcp2517fd_can_ist_handle_rxovif(spi);
		if (ret)
			return ret;
	}

	/* handle MODIF */
	//if (priv->status.intf & CAN_INT_MODIF)

	return 0;
}

static irqreturn_t mcp2517fd_can_ist(int irq, void *dev_id)
{
	struct mcp2517fd_priv *priv = dev_id;
	struct spi_device *spi = priv->spi;
	int ret;

	while (!priv->force_quit) {
		/* read interrupt status flags */
		ret = mcp2517fd_cmd_readn(spi, CAN_INT,
					  &priv->status,
					  sizeof(priv->status),
					  priv->spi_speed_hz);
		if (ret)
			return ret;

		/* only act if the mask is applied */
		if ((priv->status.intf &
		     (priv->status.intf >> CAN_INT_IE_SHIFT)) == 0)
			break;

		/* handle the status */
		ret = mcp2517fd_can_ist_handle_status(spi);
		if (ret)
			return ret;
	}

	return IRQ_HANDLED;
}

static int mcp2517fd_open(struct net_device *net)
{
	struct mcp2517fd_priv *priv = netdev_priv(net);
	struct spi_device *spi = priv->spi;
	int ret;

	ret = open_candev(net);
	if (ret) {
		dev_err(&spi->dev, "unable to set initial baudrate!\n");
		return ret;
	}

	mcp2517fd_power_enable(priv->transceiver, 1);

	priv->force_quit = 0;

	ret = request_threaded_irq(spi->irq, NULL,
				   mcp2517fd_can_ist,
				   IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
				   DEVICE_NAME, priv);
	if (ret) {
		dev_err(&spi->dev, "failed to acquire irq %d\n", spi->irq);
		mcp2517fd_power_enable(priv->transceiver, 0);
		close_candev(net);
		goto open_unlock;
	}

        priv->wq = alloc_workqueue("mcp2517fd_wq", WQ_FREEZABLE | WQ_MEM_RECLAIM,
				   0);
	INIT_WORK(&priv->tx_work, mcp2517fd_tx_work_handler);

	ret = mcp2517fd_hw_probe(spi);
	if (ret) {
		mcp2517fd_open_clean(net);
		goto open_unlock;
	}

	ret = mcp2517fd_setup(net, priv, spi);
	if (ret) {
		mcp2517fd_open_clean(net);
		goto open_unlock;
	}

	mcp2517fd_do_set_nominal_bittiming(net);

	ret = mcp2517fd_set_normal_mode(spi);
	if (ret) {
		mcp2517fd_open_clean(net);
		goto open_unlock;
	}

	can_led_event(net, CAN_LED_EVENT_OPEN);

	netif_wake_queue(net);

open_unlock:
	return ret;
}

static void mcp2517fd_clean(struct net_device *net)
{
        struct mcp2517fd_priv *priv = netdev_priv(net);
	int i;

	for (i = 0; i < priv->tx_fifos; i++) {
		if (priv->tx_pending_mask & BIT(i)) {
			can_free_echo_skb(priv->net, 0);
			net->stats.tx_errors++;
		}
	}

	priv->tx_pending_mask = 0;
}

static int mcp2517fd_stop(struct net_device *net)
{
	struct mcp2517fd_priv *priv = netdev_priv(net);
	struct spi_device *spi = priv->spi;

	close_candev(net);

	priv->force_quit = 1;
	free_irq(spi->irq, priv);
	destroy_workqueue(priv->wq);
	priv->wq = NULL;

	/* Disable and clear pending interrupts */
	mcp2517fd_disable_interrupts(spi, priv->spi_setup_speed_hz);

	mcp2517fd_clean(net);

	mcp2517fd_hw_sleep(spi);

	mcp2517fd_power_enable(priv->transceiver, 0);

	priv->can.state = CAN_STATE_STOPPED;

	can_led_event(net, CAN_LED_EVENT_STOP);

	return 0;
}

static const struct net_device_ops mcp2517fd_netdev_ops = {
	.ndo_open = mcp2517fd_open,
	.ndo_stop = mcp2517fd_stop,
	.ndo_start_xmit = mcp2517fd_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

static const struct of_device_id mcp2517fd_of_match[] = {
	{
		.compatible	= "microchip,mcp2517fd",
		.data		= (void *)CAN_MCP2517FD,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcp2517fd_of_match);

static const struct spi_device_id mcp2517fd_id_table[] = {
	{
		.name		= "mcp2517fd",
		.driver_data	= (kernel_ulong_t)CAN_MCP2517FD,
	},
	{ }
};
MODULE_DEVICE_TABLE(spi, mcp2517fd_id_table);

static void mcp2517fd_debugfs_add(struct mcp2517fd_priv *priv)
{
#if defined(CONFIG_DEBUG_FS)
	struct dentry *root;
	char name[32];
	int i;

	/* create the net device name */
	snprintf(name, sizeof(name), DEVICE_NAME"-%s",priv->net->name);
	priv-> debugfs_dir = debugfs_create_dir(name, NULL);
	root = priv-> debugfs_dir;

	/* export the status structure */
	debugfs_create_x32("intf", 0444, root, &priv->status.intf);
	debugfs_create_x32("rx_if", 0444, root, &priv->status.rxif);
	debugfs_create_x32("tx_if", 0444, root, &priv->status.txif);
	debugfs_create_x32("rx_ovif", 0444, root, &priv->status.rxovif);
	debugfs_create_x32("tx_atif", 0444, root, &priv->status.txatif);
	debugfs_create_x32("tx_req", 0444, root, &priv->status.txreq);
	debugfs_create_x32("trec", 0444, root, &priv->status.trec);
	debugfs_create_x32("bdiag0", 0444, root, &priv->status.bdiag0);
	debugfs_create_x32("bdiag1", 0444, root, &priv->status.bdiag1);

	/* information on fifos */
	debugfs_create_u8("rx_fifos", 0444, root, &priv->rx_fifos);
	debugfs_create_x32("rx_fifo_mask", 0444, root, &priv->rx_fifo_mask);
	debugfs_create_u8("tx_fifos", 0444, root, &priv->tx_fifos);
	debugfs_create_x32("tx_fifo_mask", 0444, root, &priv->tx_fifo_mask);
	debugfs_create_x32("tx_fifo_pending", 0444, root, &priv->tx_pending_mask);
	debugfs_create_u32("fifo_size", 0444, root, &priv->payload_size);
	debugfs_create_u64("rx_overflow", 0444, root, &priv->rx_overflow);

	/* statistics on fifo buffer usage */
	for (i = 1; i < 32; i++) {
		snprintf(name, sizeof(name), "fifo_usage_%02i", i);
		debugfs_create_u64(name, 0444, root,
				   &priv->fifo_usage[i]);
	}
#endif
}

static void mcp2517fd_debugfs_remove(struct mcp2517fd_priv *priv)
{
#if defined(CONFIG_DEBUG_FS)
       	if (priv-> debugfs_dir)
		debugfs_remove_recursive(priv-> debugfs_dir);
	priv-> debugfs_dir = NULL;
#endif
}

static int mcp2517fd_can_probe(struct spi_device *spi)
{
	const struct of_device_id *of_id =
		of_match_device(mcp2517fd_of_match, &spi->dev);
	struct net_device *net;
	struct mcp2517fd_priv *priv;
	struct clk *clk;
	int ret, freq;

	clk = devm_clk_get(&spi->dev, NULL);
	if (IS_ERR(clk)) {
		return PTR_ERR(clk);
	} else {
		freq = clk_get_rate(clk);
	}

	if (freq < MCP2517FD_MIN_CLOCK_FREQUENCY
	    || freq > MCP2517FD_MAX_CLOCK_FREQUENCY) {
		dev_err(&spi->dev,
			"Clock frequency %i is not in range\n", freq);
		return -ERANGE;
	}

	/* Allocate can/net device */
	net = alloc_candev(sizeof(*priv), TX_ECHO_SKB_MAX);
	if (!net)
		return -ENOMEM;

	if (!IS_ERR(clk)) {
		ret = clk_prepare_enable(clk);
		if (ret)
			goto out_free;
	}

	net->netdev_ops = &mcp2517fd_netdev_ops;
	net->flags |= IFF_ECHO;

	priv = netdev_priv(net);
	priv->can.bittiming_const = &mcp2517fd_nominal_bittiming_const;
	priv->can.do_set_bittiming = &mcp2517fd_do_set_nominal_bittiming;
	priv->can.data_bittiming_const = &mcp2517fd_data_bittiming_const;
	priv->can.do_set_data_bittiming = &mcp2517fd_do_set_data_bittiming;
	priv->can.do_set_mode = mcp2517fd_do_set_mode;

	priv->can.ctrlmode_supported =
		CAN_CTRLMODE_FD |
		CAN_CTRLMODE_LOOPBACK |
		CAN_CTRLMODE_LISTENONLY;
	/* CAN_CTRLMODE_BERR_REPORTING */

	if (of_id)
		priv->model = (enum mcp2517fd_model)of_id->data;
	else
		priv->model = spi_get_device_id(spi)->driver_data;
	priv->net = net;
	priv->clk = clk;

	spi_set_drvdata(spi, priv);

	/* set up gpio modes as GPIO INT*/
	priv->gpio0_mode = gpio_mode_int;
	priv->gpio1_mode = gpio_mode_int;

	/* if we have a clock that is smaller then 4MHz, then enable the pll */
	priv->clock_pll = (freq <= MCP2517FD_AUTO_PLL_MAX_CLOCK_FREQUENCY);
	/* do not use the SCK clock divider */
	priv->clock_div2 = false;
	/* clock output is divided by 10 - maybe expose this as a clock ?*/
	priv->clock_odiv = 10;

	/* decide on real can clock rate */
	priv->can.clock.freq = freq;
	if (priv->clock_pll) {
		priv->can.clock.freq *= MCP2517FD_PLL_MULTIPLIER;
		if (priv->can.clock.freq > MCP2517FD_MAX_CLOCK_FREQUENCY) {
			dev_err(&spi->dev,
				"PLL clock frequency %i would exceed limit\n",
				priv->can.clock.freq
				);
			return -EINVAL;
		}
	}
	if (priv->clock_div2)
		priv->can.clock.freq /= MCP2517FD_SCLK_DIVIDER;

	/* calclculate the clock frequencies to use */
	priv->spi_setup_speed_hz = freq / 2;
	priv->spi_speed_hz = priv->can.clock.freq / 2;
	if (priv->clock_div2) {
		priv->spi_setup_speed_hz /= MCP2517FD_SCLK_DIVIDER;
		priv->spi_speed_hz /= MCP2517FD_SCLK_DIVIDER;
	}

	if (spi->max_speed_hz) {
		priv->spi_setup_speed_hz = min_t(int,
						 priv->spi_setup_speed_hz,
						 spi->max_speed_hz);
		priv->spi_speed_hz = min_t(int,
					   priv->spi_speed_hz,
					   spi->max_speed_hz);
	}

	/* Configure the SPI bus */
	spi->max_speed_hz = priv->spi_speed_hz;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		goto out_clk;

	priv->power = devm_regulator_get_optional(&spi->dev, "vdd");
	priv->transceiver = devm_regulator_get_optional(&spi->dev, "xceiver");
	if ((PTR_ERR(priv->power) == -EPROBE_DEFER) ||
	    (PTR_ERR(priv->transceiver) == -EPROBE_DEFER)) {
		ret = -EPROBE_DEFER;
		goto out_clk;
	}

	ret = mcp2517fd_power_enable(priv->power, 1);
	if (ret)
		goto out_clk;

	priv->spi = spi;

	SET_NETDEV_DEV(net, &spi->dev);

	ret = mcp2517fd_hw_probe(spi);
	if (ret) {
		if (ret == -ENODEV)
			dev_err(&spi->dev,
				"Cannot initialize MCP%x. Wrong wiring?\n",
				priv->model);
		goto error_probe;
	}

	mcp2517fd_hw_sleep(spi);

	ret = register_candev(net);
	if (ret)
		goto error_probe;


	/* register debugfs */
	mcp2517fd_debugfs_add(priv);

	devm_can_led_init(net);

	netdev_info(net, "MCP%x successfully initialized.\n", priv->model);
	return 0;

error_probe:
	mcp2517fd_power_enable(priv->power, 0);

out_clk:
	if (!IS_ERR(clk))
		clk_disable_unprepare(clk);

out_free:
	free_candev(net);
	dev_err(&spi->dev, "Probe failed, err=%d\n", -ret);
	return ret;
}

static int mcp2517fd_can_remove(struct spi_device *spi)
{
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	struct net_device *net = priv->net;

	mcp2517fd_debugfs_remove(priv);

	unregister_candev(net);

	mcp2517fd_power_enable(priv->power, 0);

	if (!IS_ERR(priv->clk))
		clk_disable_unprepare(priv->clk);

	free_candev(net);

	return 0;
}

static int __maybe_unused mcp2517fd_can_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);
	struct net_device *net = priv->net;

	priv->force_quit = 1;
	disable_irq(spi->irq);

	if (netif_running(net)) {
		netif_device_detach(net);

		mcp2517fd_hw_sleep(spi);
		mcp2517fd_power_enable(priv->transceiver, 0);
		priv->after_suspend = AFTER_SUSPEND_UP;
	} else {
		priv->after_suspend = AFTER_SUSPEND_DOWN;
	}

	if (!IS_ERR_OR_NULL(priv->power)) {
		regulator_disable(priv->power);
		priv->after_suspend |= AFTER_SUSPEND_POWER;
	}

	return 0;
}

static int __maybe_unused mcp2517fd_can_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct mcp2517fd_priv *priv = spi_get_drvdata(spi);

	if (priv->after_suspend & AFTER_SUSPEND_POWER)
		mcp2517fd_power_enable(priv->power, 1);

	if (priv->after_suspend & AFTER_SUSPEND_UP) {
		mcp2517fd_power_enable(priv->transceiver, 1);
	} else {
		priv->after_suspend = 0;
	}

	priv->force_quit = 0;

	enable_irq(spi->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(mcp2517fd_can_pm_ops, mcp2517fd_can_suspend,
	mcp2517fd_can_resume);

static struct spi_driver mcp2517fd_can_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = mcp2517fd_of_match,
		.pm = &mcp2517fd_can_pm_ops,
	},
	.id_table = mcp2517fd_id_table,
	.probe = mcp2517fd_can_probe,
	.remove = mcp2517fd_can_remove,
};
module_spi_driver(mcp2517fd_can_driver);

MODULE_AUTHOR("Martin Sperl <kernel@martin.sperl.org>");
MODULE_DESCRIPTION("Microchip 2517FD CAN driver");
MODULE_LICENSE("GPL v2");
