/* SPDX-License-Identifier: GPL-2.0-or-later AND BSD-2-Clause */
/* Copyright (c) 2026 turtle2a <354865938+turtle2a@users.noreply.github.com> */
/* Portions derived from os-xgs-npu (https://github.com/AbdelmonemAwad/os-xgs-npu), Copyright (c) 2026 Abdelmonem Awad, BSD-2-Clause */
/* xgs_octep: FreeBSD driver for the OCTEON TX PCIe endpoint on the XGS 2300. */

#include <sys/param.h>
#ifdef XGS_KERN_VERSION

#undef __FreeBSD_version
#define __FreeBSD_version XGS_KERN_VERSION
#endif
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/priv.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/endian.h>
#include <sys/time.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/vnet.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#define	XO_VENDOR_ID		0x177d
#define	XO_DEVICE_ID		0xa300

#define	XO_SDP_SCRATCH		0x20180
#define	XO_READY_MAGIC		0xabcdabcdU

#define	XO_FACILITY_COUNT	5
#define	XO_BARMAP_VERSION	0x00000002U
#define	XO_BARMAP_VERSION_OFF	0
#define	XO_BARMAP_FACILITY_OFF	4
#define	XO_FACILITY_ENTRY_SIZE	16
#define	XO_BARMAP_GICD_OFF	(XO_BARMAP_FACILITY_OFF + XO_FACILITY_COUNT * XO_FACILITY_ENTRY_SIZE)
#define	XO_BARMAP_PEM_OFF	(XO_BARMAP_GICD_OFF + 4)
#define	XO_BARMAP_SIZE		(XO_BARMAP_PEM_OFF + 4)

enum xo_facility_id {
	XO_FCLT_CONTROL,
	XO_FCLT_MGMT,
	XO_FCLT_NW_AGENT,
	XO_FCLT_RPC,
	XO_FCLT_GIU,
};

static const char *const xo_facility_names[XO_FACILITY_COUNT] = {
	"control", "mgmt_netdev", "nw_agent", "rpc", "giu"
};

#define	XO_NWA_COOKIE		0x00
#define	XO_NWA_COOKIE_VALUE	0xcafebabeU
#define	XO_NWA_BODY_OFF		0x04
#define	XO_NWA_BODY_EXPECTED	0x34
#define	XO_NWA_MAX_REQ		0x08
#define	XO_NWA_TURN		0x18
#define	XO_NWA_TURN_REQUEST	1
#define	XO_NWA_TURN_ACK		2
#define	XO_NWA_REQ_LEN		0x1c
#define	XO_NWA_STATUS		0x20
#define	XO_NWA_STATUS_IDLE	0
#define	XO_NWA_STATUS_REPLY	1
#define	XO_NWA_REPLY_LEN	0x24
#define	XO_NWA_REQ_SIZE		0x20
#define	XO_NWA_MAX_WORDS	512
#define	XO_NWA_OP_DISCOVER	1
#define	XO_NWA_REPLY_MARKER	0x14
#define	XO_NWA_REPLY_HEADER	8
#define	XO_NWA_IDLE_TRIES	100
#define	XO_NWA_REPLY_TRIES	300
#define	XO_NWA_WORDS_PER_PORT	5
#define	XO_NWA_OP_SET		3
#define	XO_NWA_SUB_STATE	0
#define	XO_NWA_SUB_STATS	14
#define	XO_PORTUP_MAX_TRIES	10

#define	XO_POLL_TICKS		hz
#define	XO_HOLD_SECS		300

#define	XO_SLI_SCRATCH		0x28100
#define	XO_SDP_RINFO		0x20190
#define	XO_RING_STRIDE		0x20000
#define	XO_RING_SPAN		0x10198
#define	XO_R_IN_CONTROL		0x10000
#define	XO_R_IN_ENABLE		0x10010
#define	XO_R_IN_INSTR_BADDR	0x10020
#define	XO_R_IN_INSTR_RSIZE	0x10030
#define	XO_R_IN_INSTR_DBELL	0x10040
#define	XO_R_IN_INT_LEVELS	0x10060
#define	XO_R_OUT_INT_LEVELS	0x10110
#define	XO_R_OUT_CNTS		0x10100
#define	XO_R_OUT_SLIST_BADDR	0x10120
#define	XO_R_OUT_SLIST_RSIZE	0x10130
#define	XO_R_OUT_SLIST_DBELL	0x10140
#define	XO_R_OUT_CONTROL	0x10150
#define	XO_R_OUT_ENABLE		0x10160
#define	XO_IN_CTL_IDLE		(1ULL << 28)
#define	XO_IN_CTL_RDSIZE	(3ULL << 25)
#define	XO_IN_CTL_IS_64B	(1ULL << 24)
#define	XO_IN_CTL_ESR		(1ULL << 1)
#define	XO_OUT_CTL_IDLE		(1ULL << 36)
#define	XO_OUT_CTL_ES_P		(1ULL << 26)
#define	XO_OUT_CTL_SIZE_MASK	0x7fffffULL
#define	XO_OUT_CTL_ATTR_MASK	((1ULL << 23) | (7ULL << 32) | (7ULL << 28) | (7ULL << 24))

#define	XO_RING_COUNT		8
#define	XO_IQ_DESCS		256
#define	XO_OQ_DESCS		1024
#define	XO_INSTR_SIZE		64
#define	XO_SLIST_ENTRY		16
#define	XO_BUF_SIZE		1602
#define	XO_BUF_STRIDE		1664
#define	XO_OQ_INFO_SIZE		16
#define	XO_OQ_INTR_PKT		32
#define	XO_OQ_INTR_TIME		50
#define	XO_OQ_FETCH		16
#define	XO_OQ_WMARK		1024
#define	XO_IDLE_TRIES		1000
#define	XO_OQ_GRANT		(XO_OQ_DESCS + XO_OQ_WMARK - XO_OQ_FETCH)
#define	XO_RX_PREFIX		82
#define	XO_RX_TAG_OFF		16
#define	XO_RX_META_OFF		18
#define	XO_RX_META_SIG		0xb44399a2U
#define	XO_RX_META_VALID_SHIFT	8
#define	XO_RX_FLOW_OFF		(XO_RX_META_OFF + 8)
#define	XO_RX_FLOW_ID_MASK	0x01ffffffU
#define	XO_RX_FLOW_REV_SHIFT	25
#define	XO_RX_FLOW_REV_MASK	0x3fU
#define	XO_RX_FLOW_VALID	0x80000000U
#define	XO_FLOW_MAX		24
#define	XO_FLOW_HDR_MAX		128
#define	XO_RX_BUDGET		128
#define	XO_RX_TICKS		howmany(hz, 500)
#define	XO_PKT_MAX		65535
#define	XO_CMSG_TAG		254
#define	XO_PORT_MAX		32
#define	XO_IF_MAX_TRIES		5
#define	XO_NWA_OP_GET		4
#define	XO_NWA_SUB_MAC		3
#define	XO_NWA_SUB_ALLMULTI	0x46
#define	XO_RPC_CMD_LIF_ADD	3
#define	XO_RPC_CMD_PPORT_UPDATE	5
#define	XO_LIF_MTU		1500
#define	XO_LIF_FWD_L3		2
#define	XO_LIF_PRIV_MASK_DF	1
#define	XO_LIF_M_ALL		0x00ff
#define	XO_TX_TAG_LEN		66
#define	XO_TX_PPORT_HLEN	2
#define	XO_TX_MIN_FRAME		60
#define	XO_TX_P_KIND		40
#define	XO_TX_OP_PACKET		0x1220
#define	XO_TX_INSTR_FSZ		28
#define	XO_TX_INSTR_SL		(XO_TX_INSTR_FSZ + XO_TX_TAG_LEN)
#define	XO_TX_CKSUM_OFFSET	(XO_TX_TAG_LEN + ETHER_HDR_LEN + 1)
#define	XO_TX_BUF_STRIDE	1600

#define	XO_HS_HOST_LOADED	0xdeadbeefULL
#define	XO_HS_GET_HOST_INFO	0xbeefdeedULL
#define	XO_HS_INFO_RECEIVED	0x00deaddeULL
#define	XO_HS_COMPLETED		0xdeeddeedULL
#define	XO_HS_APP_MODE_NIC	2

#define	XO_RPC_CFG_MAGIC	0xd7d3ab00U
#define	XO_RPC_STATE_SIZE	232
#define	XO_RPC_STATE_CFG	0
#define	XO_RPC_STATE_RING_LO	72
#define	XO_RPC_STATE_RINGS	104
#define	XO_RPC_RING_SIZE	32
#define	XO_RPC_RING_POSTED	0
#define	XO_RPC_RING_DONE	8
#define	XO_RPC_RING_OFFSET	16
#define	XO_RPC_RING_DESC_OFF	20
#define	XO_RPC_RING_DESC_CNT	24
#define	XO_RPC_RING_CFG		28
#define	XO_RPC_BAR_DESC_SIZE	16
#define	XO_RPC_DESC_NO_AGG_DMA	2
#define	XO_RPC_BUF_DESC_SIZE	8
#define	XO_RPC_DATA_MAX_SIZE	4096
#define	XO_RPC_DESC_COUNT	256
#define	XO_RPC_DESC_OFFSET	4328
#define	XO_RPC_CFG_WAIT_MS	3000
#define	XO_RPC_CMD_WAIT_MS	2000
#define	XO_RPC_SETTLE_MS	500
#define	XO_RPC_CMD_FW_CFG_PARAMS_SET	2
#define	XO_RPC_CMD_PLATFORM_READ	36
#define	XO_RPC_CMD_SYS_CNT_READ	44
#define	XO_RPC_CMD_SHIFT_MAX	4
#define	XO_RPC_CMD_NHOP_PROGRAM	6
#define	XO_RPC_CMD_MFLOW_PROGRAM	8
#define	XO_RPC_CMD_FLOW_CREATE_FP	10
#define	XO_RPC_CMD_CONN_RECLAIM_FP	16
#define	XO_RPC_CMD_LO_CONN_READ	38
#define	XO_RPC_CMD_LO_MFLOW_READ	40
#define	XO_FT_CONN_BASE		10000
#define	XO_FT_NHOP_BASE		100
#define	XO_FT_NWA_IFACE_BASE	2
#define	XO_FT_MIN_FRAMES	100
#define	XO_FT_MAX_AGE_MS	4000
#define	XO_FT_TIMEOUT_S		60
#define	XO_FT_BRCTL		0xe
#define	XO_FT_VERDICT		2
#define	XO_FT_CONN_REV		1
#define	XO_FT_ACTION_FWD	1
#define	XO_FT_STATE_ACTIVE	2
#define	XO_FT_NHOP_LEN		32
#define	XO_FT_MFLOW_LEN		32
#define	XO_FT_FLOW_LEN		180
#define	XO_FT_FLOW_OFF_VALID	112
#define	XO_FT_FLOW_OFF_MFLOW_O	116
#define	XO_FT_FLOW_OFF_MFLOW_R	148
#define	XO_FT_FLOW_OFF_NAT	84
#define	XO_RPC_PLAT_CONN_OFF	204
#define	XO_RPC_PLAT_NHOP_OFF	208
#define	XO_SYS_CNT_COUNT	128
#define	XO_FW_CFG_OFFLOAD	0x0001
#define	XO_FW_CFG_TCP_SEQ_CHK	0x0002
#define	XO_RPC_MAX_TRIES	60

static MALLOC_DEFINE(M_XGSOCTEP, "xgs_octep", "xgs_octep driver state");

SYSCTL_NODE(_hw, OID_AUTO, xgs_octep, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "XGS OCTEON TX endpoint");
static int xo_autostart = 0;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, autostart, CTLFLAG_RDTUN, &xo_autostart, 0,
    "arm the SDP rings and run the handshake as soon as the NPU is ready");
static int xo_offload = 1;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, offload, CTLFLAG_RDTUN, &xo_offload, 1,
    "turn the firmware offload gate on once the RPC ring is up (default on)");
static int xo_ifaces = 0;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, ifaces, CTLFLAG_RDTUN, &xo_ifaces, 1,
    "create an octepN interface per port and receive on it (default off)");
static int xo_tx = 0;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, tx, CTLFLAG_RDTUN, &xo_tx, 0,
    "transmit frames from the octepN interfaces (default off)");
static int xo_portup = 1;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, portup, CTLFLAG_RDTUN, &xo_portup, 1,
    "raise every port the network agent lists once it answers (default on)");

static int xo_dp_debug = 0;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, dp_debug, CTLFLAG_RDTUN, &xo_dp_debug, 0,
    "log drops, stalls, packet rates and moved NPU counters (default off)");

static int xo_ft = 0;
SYSCTL_INT(_hw_xgs_octep, OID_AUTO, ft, CTLFLAG_RDTUN, &xo_ft, 0,
    "allow programming one NPU flow by hand with ft_add (default off)");

struct xo_dma {
	bus_dma_tag_t	tag;
	bus_dmamap_t	map;
	void		*vaddr;
	bus_addr_t	paddr;
	bus_size_t	size;
};

struct xo_ring {
	struct xo_dma	instr;
	struct xo_dma	txbuf;
	struct xo_dma	slist;
	struct xo_dma	bufs;
	struct xo_dma	info;
	uint32_t	ring;
	bool		in_cfg;
	bool		out_cfg;
	uint32_t	oq_rd;
	uint32_t	credit_carry;
};

struct xo_softc;

struct xo_flow {
	bool		used;
	bool		tuple_valid;
	uint32_t	id;
	uint8_t		rev;
	uint16_t	tag;
	uint64_t	frames;
	uint64_t	seq;
	int		last_seen;
	uint8_t		proto;
	uint8_t		src[4];
	uint8_t		dst[4];
	uint16_t	sport;
	uint16_t	dport;
	uint8_t		smac[ETHER_ADDR_LEN];
	uint8_t		dmac[ETHER_ADDR_LEN];
	uint16_t	vlan;
};

struct xo_port {
	struct xo_softc	*sc;
	if_t		ifp;
	struct ifmedia	media;
	uint16_t	tag;
	uint8_t		iface;
	uint8_t		mac[ETHER_ADDR_LEN];
};

struct xo_facility {
	uint32_t	offset;
	uint32_t	size;
	uint32_t	dbell_start;
	uint32_t	dbell_count;
};

struct xo_softc {
	device_t		dev;
	struct resource		*bar0;
	struct resource		*bar2;
	int			rid0;
	int			rid2;
	bus_size_t		bar0_len;
	bus_size_t		bar2_len;

	struct sx		lock;
	struct taskqueue	*tq;
	struct xo_ring		ring[XO_RING_COUNT];
	uint32_t		ring_count;
	uint32_t		time_thr;
	uint32_t		ticks_per_us;
	bool			dp_up;
	bool			hs_done;
	bool			hs_reply;
	bool			start_tried;
	struct xo_dma		rpc_cmd;
	bool			rpc_ready;
	bool			offload_on;
	bool			want_offload;
	bool			fw_cfg_known;
	uint16_t		fw_cfg;
	uint64_t		rpc_posted;
	uint32_t		rpc_tries;
	uint32_t		rpc_last_rc;
	uint16_t		rpc_last_len;
	bool			rpc_quiet;
	bool			plat_known;
	uint8_t			plat_shift;
	uint32_t		plat_conn;
	uint32_t		plat_nhop;
	char			plat_name[65];
	int			rpc_err;
	bool			want_portup;
	bool			portup_on;
	uint32_t		ports_up;
	uint32_t		portup_fail;
	uint32_t		portup_tries;
	bool			want_ifaces;
	bool			ifaces_done;
	bool			hook_on;
	int			hook_start;
	struct intr_config_hook	hook;
	uint32_t		if_tries;
	uint32_t		if_made;
	int			if_err;
	volatile u_int		nports_if;
	struct xo_port		port[XO_PORT_MAX];
	struct taskqueue	*rxtq;
	struct timeout_task	rxpoll;
	uint64_t		rx_frames;
	uint64_t		rx_drop;
	uint64_t		rx_cmsg;
	uint64_t		rx_notag;
	struct mtx		flow_lock;
	struct xo_flow		flow[XO_FLOW_MAX];
	uint64_t		flow_seq;
	uint64_t		meta_frames;
	uint64_t		meta_sig;
	uint64_t		meta_valid;
	bool			want_tx;
	struct mtx		tx_lock;
	uint32_t		tx_prod;
	uint64_t		tx_frames;
	uint64_t		tx_drop;
	uint64_t		tx_full;

	struct timeout_task	poll;
	struct timeout_task	mon;
	int			dp_debug;
	struct timeval		dbg_last;
	int			dbg_pps;
	bool			mon_run;
	bool			mon_cnt_ok;
	bool			mon_stall;
	uint32_t		mon_zero_s;
	uint32_t		mon_secs;
	uint64_t		mon_hi_rate;
	uint64_t		mon_rx, mon_tx, mon_rxdrop, mon_txdrop, mon_txfull, mon_rx10, mon_tx10;
	uint64_t		mon_cnt[XO_SYS_CNT_COUNT];
	uint32_t		stalls;
	uint32_t		stall_max_s;
	bool			ft_used;
	uint32_t		ft_id[2];
	uint32_t		ft_rev[2];
	uint32_t		ft_programmed;
	uint32_t		ft_failed;
	uint32_t		ft_reclaimed;
	bool			stopping;
	bool			ready;
	bool			nwa_done;
	uint64_t		scratch;
	uint32_t		barmap_off;
	uint32_t		barmap_ver;
	uint32_t		gicd_off;
	uint32_t		pem;
	struct xo_facility	fac[XO_FACILITY_COUNT];

	uint32_t		nwa_marker;
	uint32_t		nwa_status;
	uint32_t		nwa_reply_len;
	uint32_t		nwa_words;
	uint32_t		nwa_nports;
	uint32_t		*nwa_reply;
	int			last_err;
};

#define	XO_DBG(sc, fmt, ...) do {					\
	if ((sc)->dp_debug != 0 && ppsratecheck(&(sc)->dbg_last, &(sc)->dbg_pps, 5))	\
		device_printf((sc)->dev, "dp-debug: " fmt "\n", ##__VA_ARGS__);	\
} while (0)

static uint32_t
xo_rd2(struct xo_softc *sc, bus_size_t off)
{
	return (bus_read_4(sc->bar2, off));
}

static void
xo_wr2(struct xo_softc *sc, bus_size_t off, uint32_t v)
{
	bus_write_4(sc->bar2, off, v);
}

static uint32_t
xo_nwa_rd(struct xo_softc *sc, bus_size_t off)
{
	return (xo_rd2(sc, sc->fac[XO_FCLT_NW_AGENT].offset + off));
}

static void
xo_nwa_wr(struct xo_softc *sc, bus_size_t off, uint32_t v)
{
	xo_wr2(sc, sc->fac[XO_FCLT_NW_AGENT].offset + off, v);
}

static bool
xo_nwa_range_valid(struct xo_softc *sc, uint32_t offset, uint32_t length)
{
	uint64_t start = (uint64_t)sc->fac[XO_FCLT_NW_AGENT].offset + offset;
	uint64_t end = start + length;

	return (end >= start && end <= sc->bar2_len &&
	    end <= (sc->gicd_off != 0 ? sc->gicd_off : sc->bar2_len));
}

static int
xo_nwa_wait(struct xo_softc *sc, uint32_t want, unsigned int tries)
{
	unsigned int i;
	uint32_t status;

	for (i = 0; i < tries; i++) {
		status = xo_nwa_rd(sc, XO_NWA_STATUS);
		if (status == want)
			return (0);
		if (status == UINT32_MAX)
			return (ENXIO);
		pause_sbt("xonwa", 10 * SBT_1MS, 0, C_HARDCLOCK);
	}
	return (ETIMEDOUT);
}

static int
xo_ring_dbell(struct xo_softc *sc, uint32_t spi)
{
	unsigned int i;

	if (!sc->ready || sc->gicd_off == 0 || sc->gicd_off > sc->bar2_len - sizeof(uint32_t)) {
		device_printf(sc->dev, "doorbell SPI %u refused: invalid GICD offset %#x\n",
		    spi, sc->gicd_off);
		return (ENXIO);
	}
	for (i = 0; i < XO_FACILITY_COUNT; i++) {
		const struct xo_facility *f = &sc->fac[i];
		uint64_t end = (uint64_t)f->dbell_start + f->dbell_count;

		if (f->dbell_count != 0 && spi >= f->dbell_start && (uint64_t)spi < end) {
			xo_wr2(sc, sc->gicd_off, spi);
			bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
			return (0);
		}
	}
	device_printf(sc->dev, "doorbell SPI %u refused: not in any advertised range\n", spi);
	return (EINVAL);
}

static int
xo_read_barmap(struct xo_softc *sc)
{
	uint32_t magic;
	uint64_t end;
	unsigned int i;

	sc->ready = false;
	sc->nwa_done = false;
	memset(sc->fac, 0, sizeof(sc->fac));

	sc->scratch = bus_read_8(sc->bar0, XO_SDP_SCRATCH);
	if (sc->scratch == UINT64_MAX) {
		device_printf(sc->dev, "BAR0 reads all ones; the endpoint is not decoding\n");
		return (ENXIO);
	}
	magic = (uint32_t)sc->scratch;
	sc->barmap_off = (uint32_t)(sc->scratch >> 32);
	if (magic != XO_READY_MAGIC)
		return (EAGAIN);

	if (sc->barmap_off > sc->bar2_len ||
	    sc->bar2_len - sc->barmap_off < XO_BARMAP_SIZE) {
		device_printf(sc->dev, "barmap offset %#x is outside BAR2 (size %#jx)\n",
		    sc->barmap_off, (uintmax_t)sc->bar2_len);
		return (EINVAL);
	}
	sc->barmap_ver = xo_rd2(sc, sc->barmap_off + XO_BARMAP_VERSION_OFF);
	if (sc->barmap_ver == UINT32_MAX)
		return (EAGAIN);
	if (sc->barmap_ver != XO_BARMAP_VERSION) {
		device_printf(sc->dev, "barmap version %#x, expected %#x\n",
		    sc->barmap_ver, XO_BARMAP_VERSION);
		return (EINVAL);
	}
	for (i = 0; i < XO_FACILITY_COUNT; i++) {
		bus_size_t e = sc->barmap_off + XO_BARMAP_FACILITY_OFF +
		    i * XO_FACILITY_ENTRY_SIZE;

		sc->fac[i].offset = xo_rd2(sc, e);
		sc->fac[i].size = xo_rd2(sc, e + 4);
		sc->fac[i].dbell_start = xo_rd2(sc, e + 8);
		sc->fac[i].dbell_count = xo_rd2(sc, e + 12);
	}
	sc->gicd_off = xo_rd2(sc, sc->barmap_off + XO_BARMAP_GICD_OFF);
	sc->pem = xo_rd2(sc, sc->barmap_off + XO_BARMAP_PEM_OFF) & 0xff;

	for (i = 0; i < XO_FACILITY_COUNT; i++) {
		if (sc->fac[i].size == 0)
			continue;
		end = (uint64_t)sc->fac[i].offset + sc->fac[i].size;
		if (end > sc->bar2_len || (sc->gicd_off != 0 && end > sc->gicd_off)) {
			device_printf(sc->dev, "%s facility window is invalid\n",
			    xo_facility_names[i]);
			return (EINVAL);
		}
	}
	sc->ready = true;
	return (0);
}

static int
xo_nwa_discover(struct xo_softc *sc)
{
	const struct xo_facility *f = &sc->fac[XO_FCLT_NW_AGENT];
	uint32_t body, max_req, reply_len, payload_len, words, reply_off, marker, status;
	unsigned int i;
	int err, ack_err;

	sc->nwa_done = false;
	sc->nwa_nports = 0;
	sc->nwa_words = 0;
	if (!sc->ready || f->size == 0 || f->offset == 0)
		return (ENXIO);
	if (!xo_nwa_range_valid(sc, 0, 0x10) ||
	    !xo_nwa_range_valid(sc, XO_NWA_STATUS, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_REQ_SIZE, sizeof(uint32_t)))
		return (EINVAL);

	if (xo_nwa_rd(sc, XO_NWA_COOKIE) != XO_NWA_COOKIE_VALUE)
		return (EAGAIN);
	body = xo_nwa_rd(sc, XO_NWA_BODY_OFF);
	max_req = xo_nwa_rd(sc, XO_NWA_MAX_REQ);
	if (body != XO_NWA_BODY_EXPECTED || max_req < XO_NWA_REQ_SIZE)
		return (EPROTO);
	if (!xo_nwa_range_valid(sc, body, XO_NWA_REQ_SIZE) ||
	    !xo_nwa_range_valid(sc, XO_NWA_REQ_LEN, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_TURN, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_REPLY_LEN, sizeof(uint32_t)))
		return (EINVAL);

	err = xo_nwa_wait(sc, XO_NWA_STATUS_IDLE, XO_NWA_IDLE_TRIES);
	if (err != 0)
		return (err);

	xo_nwa_wr(sc, body, XO_NWA_OP_DISCOVER);
	for (i = 1; i < XO_NWA_REQ_SIZE / sizeof(uint32_t); i++)
		xo_nwa_wr(sc, body + i * sizeof(uint32_t), 0);
	xo_nwa_wr(sc, XO_NWA_REQ_LEN, XO_NWA_REQ_SIZE);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	xo_nwa_wr(sc, XO_NWA_TURN, XO_NWA_TURN_REQUEST);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	err = xo_ring_dbell(sc, f->dbell_start);
	if (err != 0)
		return (err);

	err = xo_nwa_wait(sc, XO_NWA_STATUS_REPLY, XO_NWA_REPLY_TRIES);
	if (err != 0)
		return (err);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_READ);

	reply_len = xo_nwa_rd(sc, XO_NWA_REPLY_LEN);
	reply_off = body + XO_NWA_REQ_SIZE;
	if (reply_len < XO_NWA_REPLY_HEADER ||
	    reply_len > XO_NWA_REPLY_HEADER + XO_NWA_MAX_WORDS * sizeof(uint32_t) ||
	    !xo_nwa_range_valid(sc, reply_off, reply_len)) {
		err = EPROTO;
		goto ack;
	}
	marker = xo_nwa_rd(sc, reply_off);
	status = xo_nwa_rd(sc, reply_off + sizeof(uint32_t));
	payload_len = reply_len - XO_NWA_REPLY_HEADER;
	words = howmany(payload_len, sizeof(uint32_t));
	for (i = 0; i < words; i++)
		sc->nwa_reply[i] = xo_nwa_rd(sc, reply_off + XO_NWA_REPLY_HEADER +
		    i * sizeof(uint32_t));
	if (words != 0 && (payload_len & 3) != 0)
		sc->nwa_reply[words - 1] &= (1U << ((payload_len & 3) * 8)) - 1;

	sc->nwa_marker = marker;
	sc->nwa_status = status;
	sc->nwa_reply_len = reply_len;
	sc->nwa_words = words;
	if (marker != XO_NWA_REPLY_MARKER || status != 0) {
		err = EIO;
	} else {
		sc->nwa_done = true;
		if (words != 0)
			sc->nwa_nports = sc->nwa_reply[0];
		if (sc->nwa_nports > (words != 0 ? (words - 1) / XO_NWA_WORDS_PER_PORT : 0)) {
			sc->nwa_done = false;
			err = EPROTO;
		}
	}
ack:
	xo_nwa_wr(sc, XO_NWA_TURN, XO_NWA_TURN_ACK);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	ack_err = xo_ring_dbell(sc, f->dbell_start);
	if (err == 0 && ack_err != 0)
		err = ack_err;
	if (ack_err == 0)
		(void)xo_nwa_wait(sc, XO_NWA_STATUS_IDLE, XO_NWA_IDLE_TRIES);
	return (err);
}

static uint64_t
xo_ring_rd(struct xo_softc *sc, uint32_t ring, bus_size_t off)
{
	return (bus_read_8(sc->bar0, (bus_size_t)ring * XO_RING_STRIDE + off));
}

static void
xo_ring_wr(struct xo_softc *sc, uint32_t ring, bus_size_t off, uint64_t v)
{
	bus_write_8(sc->bar0, (bus_size_t)ring * XO_RING_STRIDE + off, v);
}

static void
xo_dma_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	if (error == 0 && nseg == 1)
		*(bus_addr_t *)arg = segs[0].ds_addr;
}

static void
xo_dma_free(struct xo_dma *m)
{
	if (m->paddr != 0)
		bus_dmamap_unload(m->tag, m->map);
	if (m->vaddr != NULL)
		bus_dmamem_free(m->tag, m->vaddr, m->map);
	if (m->tag != NULL)
		bus_dma_tag_destroy(m->tag);
	memset(m, 0, sizeof(*m));
}

static int
xo_dma_alloc(struct xo_softc *sc, struct xo_dma *m, bus_size_t size)
{
	int err;

	memset(m, 0, sizeof(*m));
	err = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 64, 0, BUS_SPACE_MAXADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL, size, 1, size, 0, NULL, NULL, &m->tag);
	if (err != 0)
		return (err);
	err = bus_dmamem_alloc(m->tag, &m->vaddr, BUS_DMA_WAITOK | BUS_DMA_ZERO |
	    BUS_DMA_COHERENT, &m->map);
	if (err != 0) {
		m->vaddr = NULL;
		goto fail;
	}
	err = bus_dmamap_load(m->tag, m->map, m->vaddr, size, xo_dma_cb, &m->paddr,
	    BUS_DMA_NOWAIT);
	if (err != 0 || m->paddr == 0) {
		m->paddr = 0;
		err = err != 0 ? err : ENOMEM;
		goto fail;
	}
	m->size = size;
	return (0);
fail:
	xo_dma_free(m);
	return (err);
}

static void
xo_dp_free_allocations(struct xo_softc *sc)
{
	unsigned int i;

	for (i = 0; i < sc->ring_count; i++) {
		xo_dma_free(&sc->ring[i].instr);
		xo_dma_free(&sc->ring[i].txbuf);
		xo_dma_free(&sc->ring[i].slist);
		xo_dma_free(&sc->ring[i].bufs);
		xo_dma_free(&sc->ring[i].info);
	}
	sc->ring_count = 0;
}

static int
xo_wait_ring_idle(struct xo_softc *sc, uint32_t ring, bool output)
{
	bus_size_t reg = output ? XO_R_OUT_CONTROL : XO_R_IN_CONTROL;
	uint64_t idle = output ? XO_OUT_CTL_IDLE : XO_IN_CTL_IDLE;
	unsigned int i;

	for (i = 0; i < XO_IDLE_TRIES; i++) {
		if ((xo_ring_rd(sc, ring, reg) & idle) != 0)
			return (0);
		pause_sbt("xoidle", SBT_1MS, 0, C_HARDCLOCK);
	}
	device_printf(sc->dev, "ring %u %s never went idle (control=%#jx)\n", ring,
	    output ? "output" : "input", (uintmax_t)xo_ring_rd(sc, ring, reg));
	return (ETIMEDOUT);
}

static int
xo_ring_available(struct xo_softc *sc, uint32_t ring, bool check_input)
{
	uint64_t v;

	v = xo_ring_rd(sc, ring, XO_R_OUT_CONTROL);
	if (v == UINT64_MAX)
		return (ENXIO);
	if ((v & XO_OUT_CTL_IDLE) == 0 || (xo_ring_rd(sc, ring, XO_R_OUT_ENABLE) & 1) != 0 ||
	    xo_ring_rd(sc, ring, XO_R_OUT_SLIST_BADDR) != 0) {
		device_printf(sc->dev, "output ring %u is busy or already configured\n", ring);
		return (EBUSY);
	}
	if (!check_input)
		return (0);
	v = xo_ring_rd(sc, ring, XO_R_IN_CONTROL);
	if (v == UINT64_MAX)
		return (ENXIO);
	if ((v & XO_IN_CTL_IDLE) == 0 || (xo_ring_rd(sc, ring, XO_R_IN_ENABLE) & 1) != 0 ||
	    xo_ring_rd(sc, ring, XO_R_IN_INSTR_BADDR) != 0) {
		device_printf(sc->dev, "input ring %u is busy or already configured\n", ring);
		return (EBUSY);
	}
	return (0);
}

static int
xo_alloc_ring(struct xo_softc *sc, struct xo_ring *r, uint32_t ring_id, bool with_instr)
{
	uint64_t *e;
	unsigned int i;
	int err;

	r->ring = ring_id;
	if (with_instr) {
		err = xo_dma_alloc(sc, &r->instr, XO_IQ_DESCS * XO_INSTR_SIZE);
		if (err != 0)
			return (err);
		if (sc->want_tx) {
			err = xo_dma_alloc(sc, &r->txbuf,
			    (bus_size_t)XO_IQ_DESCS * XO_TX_BUF_STRIDE);
			if (err != 0)
				return (err);
		}
	}
	err = xo_dma_alloc(sc, &r->slist, XO_OQ_DESCS * XO_SLIST_ENTRY);
	if (err == 0)
		err = xo_dma_alloc(sc, &r->bufs, (bus_size_t)XO_OQ_DESCS * XO_BUF_STRIDE);
	if (err == 0)
		err = xo_dma_alloc(sc, &r->info, XO_OQ_DESCS * XO_OQ_INFO_SIZE);
	if (err != 0)
		return (err);

	memset(r->bufs.vaddr, 0xa5, r->bufs.size);
	e = r->slist.vaddr;
	for (i = 0; i < XO_OQ_DESCS; i++) {
		e[i * 2] = htole64(r->bufs.paddr + (bus_addr_t)i * XO_BUF_STRIDE);
		e[i * 2 + 1] = htole64(r->info.paddr + (bus_addr_t)i * XO_OQ_INFO_SIZE);
	}
	bus_dmamap_sync(r->slist.tag, r->slist.map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(r->bufs.tag, r->bufs.map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(r->info.tag, r->info.map, BUS_DMASYNC_PREWRITE);
	return (0);
}

static int
xo_configure_output(struct xo_softc *sc, struct xo_ring *r)
{
	uint64_t v;
	int err;

	err = xo_wait_ring_idle(sc, r->ring, true);
	if (err != 0)
		return (err);
	r->out_cfg = true;
	xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_BADDR, r->slist.paddr);
	xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_RSIZE, XO_OQ_DESCS);
	v = xo_ring_rd(sc, r->ring, XO_R_OUT_CONTROL);
	v &= ~(XO_OUT_CTL_SIZE_MASK | XO_OUT_CTL_ATTR_MASK);
	v |= XO_OUT_CTL_ES_P | XO_BUF_SIZE;
	xo_ring_wr(sc, r->ring, XO_R_OUT_CONTROL, v);
	xo_ring_wr(sc, r->ring, XO_R_OUT_INT_LEVELS,
	    ((uint64_t)sc->time_thr << 32) | XO_OQ_INTR_PKT);
	return (0);
}

static void
xo_enable_output(struct xo_softc *sc, struct xo_ring *r)
{
	uint64_t en;

	xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_DBELL, UINT32_MAX);
	en = xo_ring_rd(sc, r->ring, XO_R_OUT_ENABLE);
	xo_ring_wr(sc, r->ring, XO_R_OUT_ENABLE, en | 1);
	bus_barrier(sc->bar0, 0, 0, BUS_SPACE_BARRIER_WRITE);
	xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_DBELL, XO_OQ_DESCS + XO_OQ_WMARK - XO_OQ_FETCH);
}

static void
xo_dp_abort(struct xo_softc *sc)
{
	unsigned int i;
	bool quiet = true;

	for (i = 0; i < sc->ring_count; i++) {
		if (sc->ring[i].out_cfg)
			xo_ring_wr(sc, sc->ring[i].ring, XO_R_OUT_ENABLE, 0);
		if (sc->ring[i].in_cfg)
			xo_ring_wr(sc, sc->ring[i].ring, XO_R_IN_ENABLE, 0);
	}
	bus_barrier(sc->bar0, 0, 0, BUS_SPACE_BARRIER_WRITE);
	for (i = 0; i < sc->ring_count; i++) {
		struct xo_ring *r = &sc->ring[i];

		if (r->out_cfg) {
			if (xo_wait_ring_idle(sc, r->ring, true) != 0) {
				quiet = false;
			} else {
				xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_BADDR, 0);
				xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_RSIZE, 0);
				r->out_cfg = false;
			}
		}
		if (r->in_cfg) {
			if (xo_wait_ring_idle(sc, r->ring, false) != 0) {
				quiet = false;
			} else {
				xo_ring_wr(sc, r->ring, XO_R_IN_INSTR_BADDR, 0);
				xo_ring_wr(sc, r->ring, XO_R_IN_INSTR_RSIZE, 0);
				r->in_cfg = false;
			}
		}
	}
	pause_sbt("xoabort", 10 * SBT_1MS, 0, C_HARDCLOCK);
	if (quiet)
		xo_dp_free_allocations(sc);
	else
		device_printf(sc->dev, "rings did not quiesce; DMA memory is kept\n");
	sc->dp_up = false;
}

static int
xo_dp_start(struct xo_softc *sc)
{
	uint64_t rinfo, v;
	uint32_t base, pf_rings, bar0_rings, i;
	int err;

	if (sc->dp_up)
		return (0);
	if (!sc->ready)
		return (EAGAIN);
	v = bus_read_8(sc->bar0, XO_SLI_SCRATCH);
	if (v != 0) {
		device_printf(sc->dev,
		    "SLI_EPF_SCRATCH is %#jx; the NPU may own the rings, refusing to arm\n",
		    (uintmax_t)v);
		return (EBUSY);
	}
	rinfo = bus_read_8(sc->bar0, XO_SDP_RINFO);
	if (rinfo == UINT64_MAX)
		return (ENXIO);
	base = rinfo & 0x7f;
	pf_rings = (rinfo >> 16) & 0xff;
	if (((rinfo >> 32) & 0x1f) != 0 || ((rinfo >> 48) & 0x7f) != 0) {
		device_printf(sc->dev, "RINFO %#jx declares VFs; not supported here\n",
		    (uintmax_t)rinfo);
		return (EINVAL);
	}
	for (bar0_rings = 0; (uint64_t)bar0_rings * XO_RING_STRIDE + XO_RING_SPAN <=
	    sc->bar0_len; bar0_rings++)
		;
	if (pf_rings < XO_RING_COUNT || base + XO_RING_COUNT > bar0_rings ||
	    base + XO_RING_COUNT > 64) {
		device_printf(sc->dev, "need %u rings; RINFO is %#jx\n", XO_RING_COUNT,
		    (uintmax_t)rinfo);
		return (EINVAL);
	}
	for (i = 0; i < XO_RING_COUNT; i++) {
		err = xo_ring_available(sc, base + i, i == 0);
		if (err != 0)
			return (err);
	}
	for (i = 0; i < XO_RING_COUNT; i++) {
		sc->ring_count = i + 1;
		err = xo_alloc_ring(sc, &sc->ring[i], base + i, i == 0);
		if (err != 0) {
			xo_dp_free_allocations(sc);
			return (err);
		}
	}
	sc->dp_up = true;

	err = xo_wait_ring_idle(sc, sc->ring[0].ring, false);
	if (err != 0)
		goto fail;
	v = xo_ring_rd(sc, base, XO_R_IN_CONTROL);
	v |= XO_IN_CTL_RDSIZE | XO_IN_CTL_IS_64B | XO_IN_CTL_ESR;
	xo_ring_wr(sc, base, XO_R_IN_CONTROL, v);
	xo_ring_wr(sc, base, XO_R_IN_INSTR_BADDR, sc->ring[0].instr.paddr);
	xo_ring_wr(sc, base, XO_R_IN_INSTR_RSIZE, XO_IQ_DESCS);
	xo_ring_wr(sc, base, XO_R_IN_INT_LEVELS, UINT32_MAX);
	sc->ring[0].in_cfg = true;

	for (i = 0; i < XO_RING_COUNT; i++) {
		err = xo_configure_output(sc, &sc->ring[i]);
		if (err != 0)
			goto fail;
	}
	xo_ring_wr(sc, base, XO_R_IN_INSTR_DBELL, UINT32_MAX);
	for (i = 0; i < XO_IDLE_TRIES; i++) {
		if ((xo_ring_rd(sc, base, XO_R_IN_INSTR_DBELL) & UINT32_MAX) == 0)
			break;
		DELAY(10);
	}
	if (i == XO_IDLE_TRIES) {
		device_printf(sc->dev, "input ring %u doorbell did not drain\n", base);
		err = ETIMEDOUT;
		goto fail;
	}
	v = xo_ring_rd(sc, base, XO_R_IN_ENABLE);
	xo_ring_wr(sc, base, XO_R_IN_ENABLE, v | 1);
	for (i = 0; i < XO_RING_COUNT; i++)
		xo_enable_output(sc, &sc->ring[i]);
	device_printf(sc->dev, "rings armed: %u rings from %u (iq %u x %u B, oq %u x %u B)\n",
	    XO_RING_COUNT, base, XO_IQ_DESCS, XO_INSTR_SIZE, XO_OQ_DESCS, XO_BUF_SIZE);
	return (0);
fail:
	xo_dp_abort(sc);
	return (err);
}

static int
xo_handshake(struct xo_softc *sc)
{
	uint64_t rinfo, info, v;
	uint32_t pf_start, i;

	if (sc->hs_done)
		return (0);
	if (!sc->dp_up)
		return (ENXIO);
	v = bus_read_8(sc->bar0, XO_SLI_SCRATCH);
	if (v != 0) {
		device_printf(sc->dev, "handshake scratch is %#jx; refusing to overwrite it\n",
		    (uintmax_t)v);
		return (EBUSY);
	}
	rinfo = bus_read_8(sc->bar0, XO_SDP_RINFO);
	if (rinfo == UINT64_MAX)
		return (ENXIO);
	pf_start = rinfo & 0x7f;
	info = ((uint64_t)XO_HS_APP_MODE_NIC << 40) | ((uint64_t)pf_start << 32) |
	    ((uint64_t)XO_RING_COUNT << 24);

	bus_write_8(sc->bar0, XO_SLI_SCRATCH, XO_HS_HOST_LOADED);
	if (bus_read_8(sc->bar0, XO_SLI_SCRATCH) != XO_HS_HOST_LOADED) {
		device_printf(sc->dev, "HOST_LOADED did not read back; not continuing\n");
		return (EIO);
	}
	for (i = 0; i < 5 * 50; i++) {
		if (bus_read_8(sc->bar0, XO_SLI_SCRATCH) == XO_HS_GET_HOST_INFO)
			break;
		pause_sbt("xohs", 20 * SBT_1MS, 0, C_HARDCLOCK);
	}
	if (i == 5 * 50) {
		bus_write_8(sc->bar0, XO_SLI_SCRATCH, 0);
		device_printf(sc->dev, "the NPU did not request host info; scratch reset to 0\n");
		return (ETIMEDOUT);
	}
	bus_write_8(sc->bar0, XO_SLI_SCRATCH, info);
	device_printf(sc->dev, "NPU asked; published %#jx (pf_srn %u, %u rings)\n",
	    (uintmax_t)info, pf_start, XO_RING_COUNT);
	for (i = 0; i < 2 * 50; i++) {
		v = bus_read_8(sc->bar0, XO_SLI_SCRATCH);
		if ((v >> 16) == XO_HS_INFO_RECEIVED)
			break;
		pause_sbt("xohs", 20 * SBT_1MS, 0, C_HARDCLOCK);
	}
	if (i == 2 * 50) {
		bus_write_8(sc->bar0, XO_SLI_SCRATCH, XO_HS_COMPLETED);
		sc->hs_done = true;
		device_printf(sc->dev, "no reply to the info word; wrote HANDSHAKE_COMPLETED to release the NPU\n");
		return (ETIMEDOUT);
	}
	sc->ticks_per_us = v & 0xffff;
	sc->hs_reply = true;
	sc->time_thr = (uint32_t)(((uint64_t)sc->ticks_per_us * XO_OQ_INTR_TIME) / 1024);
	for (i = 0; i < sc->ring_count; i++)
		xo_ring_wr(sc, sc->ring[i].ring, XO_R_OUT_INT_LEVELS,
		    ((uint64_t)sc->time_thr << 32) | XO_OQ_INTR_PKT);
	bus_write_8(sc->bar0, XO_SLI_SCRATCH, XO_HS_COMPLETED);
	sc->hs_done = true;
	device_printf(sc->dev, "handshake complete: the NPU reports %u ticks/us\n",
	    sc->ticks_per_us);
	return (0);
}

static uint32_t
xo_rpc_rd(struct xo_softc *sc, bus_size_t off)
{
	return (xo_rd2(sc, sc->fac[XO_FCLT_RPC].offset + off));
}

static void
xo_rpc_wr(struct xo_softc *sc, bus_size_t off, uint32_t v)
{
	xo_wr2(sc, sc->fac[XO_FCLT_RPC].offset + off, v);
}

static uint64_t
xo_rpc_rd64(struct xo_softc *sc, bus_size_t off)
{
	uint64_t lo = xo_rpc_rd(sc, off);
	uint64_t hi = xo_rpc_rd(sc, off + 4);

	return (lo | (hi << 32));
}

static void
xo_rpc_wr64(struct xo_softc *sc, bus_size_t off, uint64_t v)
{
	xo_rpc_wr(sc, off, (uint32_t)v);
	xo_rpc_wr(sc, off + 4, (uint32_t)(v >> 32));
}

static int
xo_rpc_configure(struct xo_softc *sc)
{
	const struct xo_facility *f = &sc->fac[XO_FCLT_RPC];
	uint64_t cfg, live, posted, done, desc_end;
	uint32_t revision, i, hi;
	int err;

	if (sc->rpc_ready)
		return (0);
	if (!sc->dp_up || !sc->hs_reply || !sc->ready || f->size == 0 || f->dbell_count == 0)
		return (ENXIO);
	desc_end = (uint64_t)XO_RPC_DESC_OFFSET + (uint64_t)XO_RPC_DESC_COUNT * XO_RPC_BAR_DESC_SIZE;
	if (XO_RPC_DESC_OFFSET < XO_RPC_STATE_SIZE || desc_end > f->size ||
	    f->dbell_start > UINT32_MAX - f->dbell_count) {
		device_printf(sc->dev, "rpc: descriptors or doorbell outside the facility\n");
		return (EINVAL);
	}
	if (sc->rpc_cmd.vaddr == NULL) {
		err = xo_dma_alloc(sc, &sc->rpc_cmd, XO_RPC_DATA_MAX_SIZE);
		if (err != 0)
			return (err);
	}
	cfg = xo_rpc_rd64(sc, XO_RPC_STATE_CFG);
	if (cfg == UINT64_MAX)
		return (ENXIO);
	revision = (((cfg >> 32) & 0xffff) + 1) & 0xffff;

	xo_rpc_wr64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_POSTED, 0);
	xo_rpc_wr64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_DONE, 0);
	xo_rpc_wr(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_OFFSET, 0);
	xo_rpc_wr(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_DESC_OFF, XO_RPC_DESC_OFFSET);
	xo_rpc_wr(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_DESC_CNT, XO_RPC_DESC_COUNT);
	xo_rpc_wr(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_CFG, 0);
	for (i = 0; i < 4; i++) {
		hi = XO_RPC_STATE_RINGS + i * XO_RPC_RING_SIZE;
		xo_rpc_wr64(sc, hi + XO_RPC_RING_POSTED, 0);
		xo_rpc_wr64(sc, hi + XO_RPC_RING_DONE, 0);
		xo_rpc_wr(sc, hi + XO_RPC_RING_OFFSET, 0);
		xo_rpc_wr(sc, hi + XO_RPC_RING_DESC_OFF, 0);
		xo_rpc_wr(sc, hi + XO_RPC_RING_DESC_CNT, 0);
		xo_rpc_wr(sc, hi + XO_RPC_RING_CFG, 0);
	}
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	xo_rpc_wr64(sc, XO_RPC_STATE_CFG, XO_RPC_CFG_MAGIC | ((uint64_t)revision << 32));
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	err = xo_ring_dbell(sc, f->dbell_start);
	if (err != 0)
		return (err);

	for (i = 0; i < XO_RPC_CFG_WAIT_MS; i++) {
		live = xo_rpc_rd64(sc, XO_RPC_STATE_CFG);
		if (((live >> 56) & 0xff) != 0) {
			posted = xo_rpc_rd64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_POSTED);
			done = xo_rpc_rd64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_DONE);
			if (posted != done) {
				device_printf(sc->dev, "rpc: acknowledged with outstanding commands\n");
				return (EBUSY);
			}
			sc->rpc_posted = posted;
			sc->rpc_ready = true;
			sc->fw_cfg_known = false;
			device_printf(sc->dev, "rpc: NPU acknowledged after %u ms\n", i);
			pause_sbt("xorpc", XO_RPC_SETTLE_MS * SBT_1MS, 0, C_HARDCLOCK);
			return (0);
		}
		pause_sbt("xorpc", SBT_1MS, 0, C_HARDCLOCK);
	}
	return (ETIMEDOUT);
}

static int
xo_rpc_post(struct xo_softc *sc, uint8_t command, const uint8_t *payload, uint16_t len)
{
	const struct xo_facility *f = &sc->fac[XO_FCLT_RPC];
	uint8_t *buf = sc->rpc_cmd.vaddr;
	uint64_t posted, done, desc;
	uint32_t index, offset, i;
	uint16_t rc, reply_len;
	int err;

	if (!sc->rpc_ready)
		return (ENXIO);
	if (XO_RPC_BUF_DESC_SIZE + len > XO_RPC_DATA_MAX_SIZE)
		return (EMSGSIZE);
	posted = xo_rpc_rd64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_POSTED);
	done = xo_rpc_rd64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_DONE);
	if (posted != sc->rpc_posted || done != posted) {
		device_printf(sc->dev, "rpc: low ring is not idle\n");
		return (EBUSY);
	}
	memset(buf, 0x5a, XO_RPC_DATA_MAX_SIZE);
	buf[0] = (XO_RPC_DATA_MAX_SIZE - XO_RPC_BUF_DESC_SIZE) & 0xff;
	buf[1] = (XO_RPC_DATA_MAX_SIZE - XO_RPC_BUF_DESC_SIZE) >> 8;
	buf[2] = 0;
	buf[3] = command;
	memset(buf + 4, 0, 4);
	if (len != 0)
		memcpy(buf + XO_RPC_BUF_DESC_SIZE, payload, len);
	bus_dmamap_sync(sc->rpc_cmd.tag, sc->rpc_cmd.map,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	index = (uint32_t)(posted & (XO_RPC_DESC_COUNT - 1));
	offset = XO_RPC_DESC_OFFSET + index * XO_RPC_BAR_DESC_SIZE;
	desc = (uint64_t)f->offset + offset;
	if (desc + XO_RPC_BAR_DESC_SIZE > sc->bar2_len ||
	    (sc->gicd_off != 0 && desc + XO_RPC_BAR_DESC_SIZE > sc->gicd_off))
		return (EINVAL);
	xo_rpc_wr64(sc, offset, sc->rpc_cmd.paddr);
	xo_rpc_wr(sc, offset + 8, len | ((uint32_t)XO_RPC_DESC_NO_AGG_DMA << 16));
	xo_rpc_wr(sc, offset + 12, 0);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	xo_rpc_wr64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_POSTED, posted + 1);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	sc->rpc_posted = posted + 1;
	err = xo_ring_dbell(sc, f->dbell_start);
	if (err != 0)
		return (err);

	for (i = 0; i < XO_RPC_CMD_WAIT_MS; i++) {
		done = xo_rpc_rd64(sc, XO_RPC_STATE_RING_LO + XO_RPC_RING_DONE);
		if (done > posted)
			break;
		pause_sbt("xorpc", SBT_1MS, 0, C_HARDCLOCK);
	}
	if (i == XO_RPC_CMD_WAIT_MS) {
		sc->rpc_ready = false;
		sc->fw_cfg_known = false;
		device_printf(sc->dev, "rpc: command %u timed out\n", command);
		return (ETIMEDOUT);
	}
	bus_dmamap_sync(sc->rpc_cmd.tag, sc->rpc_cmd.map, BUS_DMASYNC_POSTREAD);
	rc = buf[0] | ((uint16_t)buf[1] << 8);
	reply_len = buf[6] | ((uint16_t)buf[7] << 8);
	sc->rpc_last_rc = rc;
	sc->rpc_last_len = reply_len;
	if (reply_len > XO_RPC_DATA_MAX_SIZE - XO_RPC_BUF_DESC_SIZE) {
		sc->rpc_ready = false;
		sc->fw_cfg_known = false;
		return (EPROTO);
	}
	if (rc != 0) {
		if (!sc->rpc_quiet)
			device_printf(sc->dev, "rpc: command %u refused, return code %#x\n",
			    command, rc);
		return (EIO);
	}
	return (0);
}

static int
xo_offload_set(struct xo_softc *sc, bool on)
{
	uint8_t payload[4] = {};
	uint16_t fw;
	int err;

	fw = sc->fw_cfg_known ? sc->fw_cfg : XO_FW_CFG_TCP_SEQ_CHK;
	if (on)
		fw |= XO_FW_CFG_OFFLOAD;
	else
		fw &= ~XO_FW_CFG_OFFLOAD;
	payload[0] = fw & 0xff;
	payload[1] = fw >> 8;
	err = xo_rpc_post(sc, XO_RPC_CMD_FW_CFG_PARAMS_SET, payload, sizeof(payload));
	if (err == 0) {
		sc->fw_cfg = fw;
		sc->fw_cfg_known = true;
		sc->offload_on = on;
	}
	device_printf(sc->dev, "firmware offload %s: fw_cfg=%#06x %s\n", on ? "ON" : "OFF", fw,
	    err != 0 ? "FAILED" : "acknowledged");
	return (err);
}

static int
xo_stage_c(struct xo_softc *sc)
{
	int err = 0;

	sx_xlock(&sc->lock);
	if (sc->hs_reply && !sc->rpc_ready) {
		sc->rpc_tries++;
		err = xo_rpc_configure(sc);
	}
	if (err == 0 && sc->rpc_ready && sc->offload_on != sc->want_offload)
		err = xo_offload_set(sc, sc->want_offload);
	sc->rpc_err = err;
	sx_xunlock(&sc->lock);
	return (err);
}

static int
xo_nwa_req(struct xo_softc *sc, uint32_t op, uint32_t sub, uint32_t port, uint32_t param,
    uint32_t param2, uint32_t *out, uint32_t nout)
{
	const struct xo_facility *f = &sc->fac[XO_FCLT_NW_AGENT];
	uint32_t body, max_req, reply_len, reply_off, marker, status, words;
	uint32_t req[XO_NWA_REQ_SIZE / sizeof(uint32_t)] = {};
	unsigned int i;
	int err, ack_err;

	if (!sc->ready || f->size == 0 || f->offset == 0)
		return (ENXIO);
	if (!xo_nwa_range_valid(sc, 0, 0x10) ||
	    !xo_nwa_range_valid(sc, XO_NWA_STATUS, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_REQ_SIZE, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_REQ_LEN, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_TURN, sizeof(uint32_t)) ||
	    !xo_nwa_range_valid(sc, XO_NWA_REPLY_LEN, sizeof(uint32_t)))
		return (EINVAL);
	if (xo_nwa_rd(sc, XO_NWA_COOKIE) != XO_NWA_COOKIE_VALUE)
		return (EAGAIN);
	body = xo_nwa_rd(sc, XO_NWA_BODY_OFF);
	max_req = xo_nwa_rd(sc, XO_NWA_MAX_REQ);
	if (body != XO_NWA_BODY_EXPECTED || max_req < XO_NWA_REQ_SIZE)
		return (EPROTO);
	if (!xo_nwa_range_valid(sc, body, XO_NWA_REQ_SIZE))
		return (EINVAL);
	err = xo_nwa_wait(sc, XO_NWA_STATUS_IDLE, XO_NWA_IDLE_TRIES);
	if (err != 0)
		return (err);

	req[0] = op;
	req[1] = sub;
	req[2] = port;
	req[4] = param;
	req[5] = param2;
	for (i = 0; i < nitems(req); i++)
		xo_nwa_wr(sc, body + i * sizeof(uint32_t), req[i]);
	xo_nwa_wr(sc, XO_NWA_REQ_LEN, XO_NWA_REQ_SIZE);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	xo_nwa_wr(sc, XO_NWA_TURN, XO_NWA_TURN_REQUEST);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	err = xo_ring_dbell(sc, f->dbell_start);
	if (err != 0)
		return (err);
	err = xo_nwa_wait(sc, XO_NWA_STATUS_REPLY, XO_NWA_REPLY_TRIES);
	if (err != 0)
		return (err);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_READ);

	reply_len = xo_nwa_rd(sc, XO_NWA_REPLY_LEN);
	reply_off = body + XO_NWA_REQ_SIZE;
	if (reply_len < XO_NWA_REPLY_HEADER ||
	    reply_len > XO_NWA_REPLY_HEADER + XO_NWA_MAX_WORDS * sizeof(uint32_t) ||
	    !xo_nwa_range_valid(sc, reply_off, reply_len)) {
		err = EPROTO;
	} else {
		marker = xo_nwa_rd(sc, reply_off);
		status = xo_nwa_rd(sc, reply_off + sizeof(uint32_t));
		err = (marker != XO_NWA_REPLY_MARKER || status != 0) ? EIO : 0;
		if (err == 0 && out != NULL) {
			words = howmany(reply_len - XO_NWA_REPLY_HEADER, sizeof(uint32_t));
			for (i = 0; i < nout; i++)
				out[i] = i < words ? xo_nwa_rd(sc, reply_off +
				    XO_NWA_REPLY_HEADER + i * sizeof(uint32_t)) : 0;
		}
	}
	xo_nwa_wr(sc, XO_NWA_TURN, XO_NWA_TURN_ACK);
	bus_barrier(sc->bar2, 0, 0, BUS_SPACE_BARRIER_WRITE);
	ack_err = xo_ring_dbell(sc, f->dbell_start);
	if (err == 0 && ack_err != 0)
		err = ack_err;
	if (ack_err == 0)
		(void)xo_nwa_wait(sc, XO_NWA_STATUS_IDLE, XO_NWA_IDLE_TRIES);
	return (err);
}

static int
xo_nwa_set(struct xo_softc *sc, uint32_t sub, uint32_t port, uint32_t param)
{
	return (xo_nwa_req(sc, XO_NWA_OP_SET, sub, port, param, 0, NULL, 0));
}

static int
xo_portup_set(struct xo_softc *sc, bool on)
{
	uint32_t i, ok = 0, bad = 0;
	int err;

	if (!sc->nwa_done)
		return (ENXIO);
	sx_xlock(&sc->lock);
	sc->portup_tries++;
	for (i = 0; i < sc->nwa_nports; i++) {
		err = xo_nwa_set(sc, XO_NWA_SUB_STATE,
		    sc->nwa_reply[1 + i * XO_NWA_WORDS_PER_PORT] & 0xffff, on ? 1 : 0);
		if (err == 0)
			ok++;
		else
			bad++;
	}
	sc->portup_fail = bad;
	sc->ports_up = on ? ok : sc->nwa_nports - bad;
	if (bad == 0)
		sc->portup_on = on;
	sx_xunlock(&sc->lock);
	device_printf(sc->dev, "ports %s: %u of %u%s\n", on ? "raised" : "lowered",
	    ok, sc->nwa_nports, bad != 0 ? " (some failed)" : "");
	return (bad != 0 ? EIO : 0);
}

static struct xo_port *
xo_port_by_tag(struct xo_softc *sc, uint16_t tag)
{
	u_int i, n = atomic_load_acq_int(&sc->nports_if);

	for (i = 0; i < n; i++)
		if (sc->port[i].tag == tag)
			return (&sc->port[i]);
	return (NULL);
}

static int
xo_iface_for_tag(uint16_t tag)
{
	if (tag >= 33024 && tag <= 33024 + 256 * 9 && ((tag - 33024) & 0xff) == 0)
		return ((tag - 33024) >> 8);
	if (tag == 0 || tag == 1)
		return (10);
	if (tag == 2 || tag == 32768)
		return (11);
	return (-1);
}

static void
xo_if_init_fn(void *arg)
{
	struct xo_port *p = arg;

	if_setdrvflagbits(p->ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);
}

static int
xo_if_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct xo_port *p = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;

	switch (cmd) {
	case SIOCSIFMTU:
		if (ifr->ifr_mtu < ETHERMIN || ifr->ifr_mtu > XO_LIF_MTU)
			return (EINVAL);
		if_setmtu(ifp, ifr->ifr_mtu);
		return (0);
	case SIOCSIFFLAGS:
		if ((if_getflags(ifp) & IFF_UP) != 0)
			if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
		else
			if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
		return (0);
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return (0);
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		return (ifmedia_ioctl(ifp, ifr, &p->media, cmd));
	default:
		return (ether_ioctl(ifp, cmd, data));
	}
}

static void
xo_tx_instr(struct xo_ring *r, uint32_t slot, bus_addr_t dptr, uint32_t len, uint8_t iface)
{
	uint8_t *e = (uint8_t *)r->instr.vaddr + (size_t)slot * XO_INSTR_SIZE;
	uint64_t ih3, pki_ih3, irh;

	ih3 = (uint64_t)(len + XO_TX_INSTR_FSZ) | ((uint64_t)XO_TX_P_KIND << 36) |
	    ((uint64_t)XO_TX_INSTR_FSZ << 42);
	pki_ih3 = (1ULL << 47) | ((uint64_t)XO_TX_INSTR_SL << 48) | (1ULL << 63);
	irh = ((uint64_t)XO_TX_CKSUM_OFFSET << 20) | ((uint64_t)iface << 40) |
	    ((uint64_t)XO_TX_OP_PACKET << 48);
	memset(e, 0, XO_INSTR_SIZE);
	le64enc(e, dptr);
	le64enc(e + 8, ih3);
	le64enc(e + 16, pki_ih3);
	be64enc(e + 24, 0);
	be64enc(e + 32, irh);
}

static int
xo_if_transmit(if_t ifp, struct mbuf *m)
{
	struct xo_port *p = if_getsoftc(ifp);
	struct xo_softc *sc = p->sc;
	struct xo_ring *r = &sc->ring[0];
	uint32_t slot, len, wire_len, pending;
	uint8_t *buf;

	if (!sc->want_tx || !sc->dp_up || !r->in_cfg || sc->stopping ||
	    (m->m_flags & M_PKTHDR) == 0) {
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		m_freem(m);
		return (ENETDOWN);
	}
	len = m->m_pkthdr.len;
	if (len < ETHER_HDR_LEN || len > XO_LIF_MTU + ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN) {
		atomic_add_64(&sc->tx_drop, 1);
		XO_DBG(sc, "tx drop: bad length %u", len);
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		m_freem(m);
		return (EMSGSIZE);
	}

	mtx_lock(&sc->tx_lock);
	pending = (uint32_t)xo_ring_rd(sc, r->ring, XO_R_IN_INSTR_DBELL);
	if (pending >= XO_IQ_DESCS - 1) {
		sc->tx_full++;
		mtx_unlock(&sc->tx_lock);
		XO_DBG(sc, "tx drop: instruction queue full (pending %u)", pending);
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		m_freem(m);
		return (ENOBUFS);
	}
	slot = sc->tx_prod;
	buf = (uint8_t *)r->txbuf.vaddr + (size_t)slot * XO_TX_BUF_STRIDE;
	memset(buf, 0, XO_TX_TAG_LEN);
	be16enc(buf, p->tag);
	buf[XO_TX_PPORT_HLEN] = 1;
	wire_len = len < XO_TX_MIN_FRAME ? XO_TX_MIN_FRAME : len;
	memset(buf + XO_TX_TAG_LEN + len, 0, wire_len - len);
	m_copydata(m, 0, len, (caddr_t)(buf + XO_TX_TAG_LEN));
	bus_dmamap_sync(r->txbuf.tag, r->txbuf.map, BUS_DMASYNC_PREWRITE);
	xo_tx_instr(r, slot, r->txbuf.paddr + (bus_addr_t)slot * XO_TX_BUF_STRIDE,
	    XO_TX_TAG_LEN + wire_len, p->iface);
	bus_dmamap_sync(r->instr.tag, r->instr.map, BUS_DMASYNC_PREWRITE);
	atomic_thread_fence_rel();
	sc->tx_prod = (slot + 1) % XO_IQ_DESCS;
	xo_ring_wr(sc, r->ring, XO_R_IN_INSTR_DBELL, 1);
	sc->tx_frames++;
	mtx_unlock(&sc->tx_lock);

	if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, len);
	ETHER_BPF_MTAP(ifp, m);
	m_freem(m);
	return (0);
}

static void
xo_if_qflush(if_t ifp __unused)
{
}

static int
xo_media_change(struct ifnet *ifp __unused)
{
	return (0);
}

static void
xo_media_status(struct ifnet *ifp __unused, struct ifmediareq *ifmr)
{
	ifmr->ifm_status = IFM_AVALID | IFM_ACTIVE;
	ifmr->ifm_active = IFM_ETHER | IFM_AUTO;
}

static int
xo_port_register(struct xo_softc *sc, struct xo_port *p)
{
	uint8_t lif[18] = {}, pp[4];
	uint32_t index = (uint32_t)p->iface << 12;
	int err;

	err = xo_nwa_req(sc, XO_NWA_OP_SET, XO_NWA_SUB_MAC, p->tag, le32dec(p->mac),
	    le16dec(p->mac + 4), NULL, 0);
	if (err != 0)
		return (err);
	err = xo_nwa_set(sc, XO_NWA_SUB_ALLMULTI, p->tag, 1);
	if (err != 0)
		return (err);
	le32enc(lif, index);
	memcpy(lif + 4, p->mac, ETHER_ADDR_LEN);
	le16enc(lif + 10, XO_LIF_MTU);
	lif[12] = XO_LIF_FWD_L3;
	lif[14] = XO_LIF_PRIV_MASK_DF;
	le16enc(lif + 16, XO_LIF_M_ALL);
	err = xo_rpc_post(sc, XO_RPC_CMD_LIF_ADD, lif, sizeof(lif));
	if (err != 0)
		return (err);
	pp[0] = p->iface;
	pp[1] = 0;
	pp[2] = p->tag & 0xff;
	pp[3] = p->tag >> 8;
	return (xo_rpc_post(sc, XO_RPC_CMD_PPORT_UPDATE, pp, sizeof(pp)));
}

static int
xo_ifaces_create(struct xo_softc *sc)
{
	uint32_t i, mac[2], made = 0;
	struct xo_port *p;
	uint16_t tag;
	int err, iface;
	u_int n = 0;

	for (i = 0; i < sc->nwa_nports && n < XO_PORT_MAX; i++) {
		tag = sc->nwa_reply[1 + i * XO_NWA_WORDS_PER_PORT] & 0xffff;
		iface = xo_iface_for_tag(tag);
		if (iface < 0) {
			device_printf(sc->dev, "port tag %u has no interface mapping\n", tag);
			continue;
		}
		p = &sc->port[n];
		memset(p, 0, sizeof(*p));
		p->sc = sc;
		p->tag = tag;
		p->iface = iface;
		sx_xlock(&sc->lock);
		err = xo_nwa_req(sc, XO_NWA_OP_GET, XO_NWA_SUB_MAC, tag, 0, 0, mac, 2);
		sx_xunlock(&sc->lock);
		if (err != 0) {
			device_printf(sc->dev, "tag %u: MAC read failed: %d\n", tag, err);
			continue;
		}
		le32enc(p->mac, mac[0]);
		le16enc(p->mac + 4, mac[1]);
		if ((p->mac[0] & 1) != 0 || (p->mac[0] | p->mac[1] | p->mac[2] | p->mac[3] |
		    p->mac[4] | p->mac[5]) == 0) {
			device_printf(sc->dev, "tag %u: invalid MAC\n", tag);
			continue;
		}
		sx_xlock(&sc->lock);
		err = xo_port_register(sc, p);
		sx_xunlock(&sc->lock);
		if (err != 0) {
			device_printf(sc->dev, "tag %u: registration failed: %d\n", tag, err);
			continue;
		}
		p->ifp = if_alloc(IFT_ETHER);
		if_setsoftc(p->ifp, p);
		if_initname(p->ifp, "octep", iface);
		if_setflags(p->ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
		if_setinitfn(p->ifp, xo_if_init_fn);
		if_setioctlfn(p->ifp, xo_if_ioctl);
		if_settransmitfn(p->ifp, xo_if_transmit);
		if_setqflushfn(p->ifp, xo_if_qflush);
		if_setmtu(p->ifp, XO_LIF_MTU);
		if_setbaudrate(p->ifp, IF_Gbps(1));
		ifmedia_init(&p->media, 0, xo_media_change, xo_media_status);
		ifmedia_add(&p->media, IFM_ETHER | IFM_AUTO, 0, NULL);
		ifmedia_set(&p->media, IFM_ETHER | IFM_AUTO);
		if_setcapabilities(p->ifp, IFCAP_LINKSTATE);
		if_setcapenable(p->ifp, IFCAP_LINKSTATE);
		ether_ifattach(p->ifp, p->mac);
		if_setdrvflagbits(p->ifp, IFF_DRV_RUNNING, 0);
		if_link_state_change(p->ifp, LINK_STATE_UP);
		device_printf(sc->dev, "octep%d: tag %u mac %02x:%02x:%02x:%02x:%02x:%02x\n",
		    iface, tag, p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5]);
		n++;
		made++;
		atomic_store_rel_int(&sc->nports_if, n);
	}
	sc->if_made = made;
	return (made == 0 ? EIO : 0);
}

static void
xo_credit_ring(struct xo_softc *sc, struct xo_ring *r, uint32_t buffers)
{
	int32_t have = (int32_t)(xo_ring_rd(sc, r->ring, XO_R_OUT_SLIST_DBELL) & 0xffffffff);
	int64_t room;
	uint64_t owed = (uint64_t)r->credit_carry + buffers;
	uint32_t give;

	if (owed == 0)
		return;
	if (have < 0)
		have = 0;
	room = (int64_t)XO_OQ_GRANT - have;
	if (room < 0)
		room = 0;
	give = MIN(owed, (uint64_t)room);
	r->credit_carry = owed - give;
	if (give != 0)
		xo_ring_wr(sc, r->ring, XO_R_OUT_SLIST_DBELL, give);
}

static uint32_t
xo_rx_span(uint64_t len)
{
	if (len <= XO_BUF_SIZE - 8)
		return (1);
	if (len > XO_PKT_MAX)
		return (0);
	return (1 + howmany(len + 8 - XO_BUF_SIZE, XO_BUF_SIZE));
}

static bool
xo_rx_copy(struct xo_ring *r, uint32_t idx, uint32_t span, uint32_t frame_len, uint8_t *dst)
{
	uint32_t copied = 0, j, off, chunk;
	uint8_t *buf;

	for (j = 0; j < span && copied < frame_len; j++) {
		off = j != 0 ? 0 : XO_RX_PREFIX;
		buf = (uint8_t *)r->bufs.vaddr + (size_t)((idx + j) % XO_OQ_DESCS) * XO_BUF_STRIDE;
		chunk = MIN(frame_len - copied, XO_BUF_SIZE - off);
		memcpy(dst + copied, buf + off, chunk);
		copied += chunk;
	}
	return (copied == frame_len);
}

static bool
xo_flow_tuple(const uint8_t *f, uint32_t len, struct xo_flow *t)
{
	uint32_t off = ETHER_HDR_LEN, l4;
	uint16_t et, ip_len;
	uint8_t ihl;

	if (len < ETHER_HDR_LEN)
		return (false);
	memcpy(t->dmac, f, ETHER_ADDR_LEN);
	memcpy(t->smac, f + ETHER_ADDR_LEN, ETHER_ADDR_LEN);
	et = be16dec(f + 12);
	while (et == ETHERTYPE_VLAN || et == 0x88a8) {
		if (len < off + ETHER_VLAN_ENCAP_LEN)
			return (false);
		t->vlan = be16dec(f + off) & 0x0fff;
		et = be16dec(f + off + 2);
		off += ETHER_VLAN_ENCAP_LEN;
	}
	if (et != ETHERTYPE_IP || len < off + 20 || (f[off] >> 4) != 4)
		return (false);
	ihl = (f[off] & 0x0f) * 4;
	if (ihl < 20 || len < off + ihl)
		return (false);
	ip_len = be16dec(f + off + 2);
	if (ip_len < ihl || (be16dec(f + off + 6) & 0x3fff) != 0)
		return (false);
	t->proto = f[off + 9];
	memcpy(t->src, f + off + 12, 4);
	memcpy(t->dst, f + off + 16, 4);
	l4 = off + ihl;
	if ((t->proto == 6 || t->proto == 17) && len >= l4 + 4 && ip_len >= ihl + 4) {
		t->sport = be16dec(f + l4);
		t->dport = be16dec(f + l4 + 2);
	}
	return (true);
}

static void
xo_flow_note(struct xo_softc *sc, const uint8_t *buf, uint16_t tag, uint32_t frame_len)
{
	struct xo_flow t, *e = NULL, *oldest = NULL;
	uint32_t sig, meta, ident, i, avail, rev;
	bool flow_valid;

	sig = le32dec(buf + XO_RX_META_OFF);
	meta = le32dec(buf + XO_RX_META_OFF + 4);
	ident = le32dec(buf + XO_RX_FLOW_OFF);
	flow_valid = sig == XO_RX_META_SIG &&
	    ((meta >> XO_RX_META_VALID_SHIFT) & 0xff) == 1 && (ident & XO_RX_FLOW_VALID) != 0;
	sc->meta_frames++;
	if (sig != XO_RX_META_SIG)
		return;
	sc->meta_sig++;
	if (!flow_valid)
		return;
	sc->meta_valid++;
	rev = (ident >> XO_RX_FLOW_REV_SHIFT) & XO_RX_FLOW_REV_MASK;
	avail = MIN(MIN(frame_len, XO_FLOW_HDR_MAX), XO_BUF_SIZE - XO_RX_PREFIX);
	memset(&t, 0, sizeof(t));
	t.tuple_valid = xo_flow_tuple(buf + XO_RX_PREFIX, avail, &t);
	if (!t.tuple_valid)
		memset(&t, 0, sizeof(t));

	mtx_lock(&sc->flow_lock);
	for (i = 0; i < XO_FLOW_MAX; i++) {
		struct xo_flow *c = &sc->flow[i];

		if (c->used && c->id == (ident & XO_RX_FLOW_ID_MASK) && c->rev == rev &&
		    c->tag == tag) {
			e = c;
			break;
		}
		if (!c->used) {
			e = c;
			break;
		}
		if (oldest == NULL || c->seq < oldest->seq)
			oldest = c;
	}
	if (e == NULL)
		e = oldest;
	if (!e->used || e->id != (ident & XO_RX_FLOW_ID_MASK) || e->rev != rev || e->tag != tag) {
		memset(e, 0, sizeof(*e));
		e->used = true;
		e->id = ident & XO_RX_FLOW_ID_MASK;
		e->rev = rev;
		e->tag = tag;
	}
	if (e->tuple_valid != t.tuple_valid || e->proto != t.proto || e->sport != t.sport ||
	    e->dport != t.dport || e->vlan != t.vlan || memcmp(e->src, t.src, 4) != 0 ||
	    memcmp(e->dst, t.dst, 4) != 0 || memcmp(e->smac, t.smac, ETHER_ADDR_LEN) != 0 ||
	    memcmp(e->dmac, t.dmac, ETHER_ADDR_LEN) != 0) {
		e->tuple_valid = t.tuple_valid;
		e->proto = t.proto;
		memcpy(e->src, t.src, 4);
		memcpy(e->dst, t.dst, 4);
		e->sport = t.sport;
		e->dport = t.dport;
		memcpy(e->smac, t.smac, ETHER_ADDR_LEN);
		memcpy(e->dmac, t.dmac, ETHER_ADDR_LEN);
		e->vlan = t.vlan;
		e->frames = 0;
	}
	e->frames++;
	e->seq = ++sc->flow_seq;
	e->last_seen = ticks;
	mtx_unlock(&sc->flow_lock);
}

static void
xo_rx_ring(struct xo_softc *sc, struct xo_ring *r)
{
	uint32_t pending, packets = 0, buffers = 0;

	pending = (uint32_t)(xo_ring_rd(sc, r->ring, XO_R_OUT_CNTS) & 0xffffffff);
	if (pending == 0) {
		if (r->credit_carry != 0)
			xo_credit_ring(sc, r, 0);
		return;
	}
	pending = MIN(pending, XO_RX_BUDGET);
	atomic_thread_fence_acq();
	bus_dmamap_sync(r->bufs.tag, r->bufs.map, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	while (packets < pending) {
		struct xo_port *p;
		struct mbuf *m;
		uint8_t *buf;
		uint64_t wire_len;
		uint32_t idx, span, frame_len, j;
		uint16_t tag;

		idx = r->oq_rd;
		buf = (uint8_t *)r->bufs.vaddr + (size_t)idx * XO_BUF_STRIDE;
		wire_len = be64toh(*(volatile uint64_t *)buf);
		if (wire_len == 0 || wire_len == 0xa5a5a5a5a5a5a5a5ULL ||
		    wire_len <= XO_RX_PREFIX - sizeof(uint64_t))
			break;
		span = xo_rx_span(wire_len);
		if (span == 0 || span >= XO_OQ_DESCS)
			span = 1;
		frame_len = wire_len - (XO_RX_PREFIX - sizeof(uint64_t));
		tag = be16dec(buf + XO_RX_TAG_OFF);
		p = xo_port_by_tag(sc, tag);

		if (tag != XO_CMSG_TAG)
			xo_flow_note(sc, buf, tag, frame_len);
		if (tag == XO_CMSG_TAG) {
			sc->rx_cmsg++;
		} else if (p == NULL) {
			sc->rx_notag++;
			XO_DBG(sc, "rx drop: unknown tag 0x%04x len %u", tag, frame_len);
		} else if (frame_len < ETHER_HDR_LEN || frame_len >
		    (uint32_t)if_getmtu(p->ifp) + ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN) {
			if_inc_counter(p->ifp, IFCOUNTER_IERRORS, 1);
			sc->rx_drop++;
			XO_DBG(sc, "rx drop: bad length %u tag 0x%04x", frame_len, tag);
		} else if ((m = m_get2(frame_len, M_NOWAIT, MT_DATA, M_PKTHDR)) == NULL) {
			if_inc_counter(p->ifp, IFCOUNTER_IQDROPS, 1);
			sc->rx_drop++;
			XO_DBG(sc, "rx drop: mbuf alloc failed (len %u tag 0x%04x)", frame_len, tag);
		} else if (!xo_rx_copy(r, idx, span, frame_len, mtod(m, uint8_t *))) {
			m_freem(m);
			if_inc_counter(p->ifp, IFCOUNTER_IERRORS, 1);
			sc->rx_drop++;
			XO_DBG(sc, "rx drop: ring copy failed (len %u tag 0x%04x)", frame_len, tag);
		} else {
			m->m_len = m->m_pkthdr.len = frame_len;
			m->m_pkthdr.rcvif = p->ifp;
			if_inc_counter(p->ifp, IFCOUNTER_IPACKETS, 1);
			if_inc_counter(p->ifp, IFCOUNTER_IBYTES, frame_len);
			sc->rx_frames++;
			if_input(p->ifp, m);
		}

		for (j = 0; j < span; j++)
			memset((uint8_t *)r->bufs.vaddr +
			    (size_t)((idx + j) % XO_OQ_DESCS) * XO_BUF_STRIDE, 0xa5, XO_BUF_SIZE);
		r->oq_rd = (idx + span) % XO_OQ_DESCS;
		packets++;
		buffers += span;
	}

	bus_dmamap_sync(r->bufs.tag, r->bufs.map, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	if (packets == 0) {
		if (r->credit_carry != 0)
			xo_credit_ring(sc, r, 0);
		return;
	}
	atomic_thread_fence_rel();
	xo_credit_ring(sc, r, buffers);
	xo_ring_wr(sc, r->ring, XO_R_OUT_CNTS, packets);
}

static void
xo_rx_task(void *arg, int pending __unused)
{
	struct xo_softc *sc = arg;
	uint32_t i;

	if (sc->stopping)
		return;
	CURVNET_SET_QUIET(vnet0);
	for (i = 0; i < XO_RING_COUNT; i++)
		if (sc->ring[i].out_cfg)
			xo_rx_ring(sc, &sc->ring[i]);
	CURVNET_RESTORE();
	if (!sc->stopping)
		taskqueue_enqueue_timeout(sc->rxtq, &sc->rxpoll, XO_RX_TICKS);
}

static void
xo_if_poll(struct xo_softc *sc)
{
	int err;

	if (!sc->ifaces_done && sc->want_ifaces && sc->nwa_done && sc->rpc_ready &&
	    sc->portup_on && sc->offload_on == sc->want_offload &&
	    sc->if_tries < XO_IF_MAX_TRIES) {
		sc->if_tries++;
		CURVNET_SET_QUIET(vnet0);
		err = xo_ifaces_create(sc);
		CURVNET_RESTORE();
		sc->if_err = err;
		if (err == 0) {
			sc->ifaces_done = true;
			taskqueue_enqueue_timeout(sc->rxtq, &sc->rxpoll, XO_RX_TICKS);
		}
	}
}

static int
xo_sysctl_portup(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	int v = sc->portup_on, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != 0 && v != 1)
		return (EINVAL);
	sc->want_portup = v != 0;
	return (xo_portup_set(sc, v != 0));
}

static int
xo_sysctl_offload(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	int v = sc->offload_on, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != 0 && v != 1)
		return (EINVAL);
	sc->want_offload = v != 0;
	return (xo_stage_c(sc));
}

static int
xo_start(struct xo_softc *sc)
{
	int err;

	sx_xlock(&sc->lock);
	err = xo_dp_start(sc);
	if (err == 0)
		err = xo_handshake(sc);
	sx_xunlock(&sc->lock);
	return (err);
}

static void
xo_hold_noop(void *arg __unused)
{
}

static void
xo_hold_release(struct xo_softc *sc)
{
	if (!sc->hook_on)
		return;
	sc->hook_on = false;
	config_intrhook_disestablish(&sc->hook);
}

static void
xo_poll_task(void *arg, int pending __unused)
{
	struct xo_softc *sc = arg;
	int err;

	if (sc->stopping)
		return;
	err = 0;
	if (!sc->ready)
		err = xo_read_barmap(sc);
	if (err == 0 && sc->ready && xo_autostart != 0 && !sc->start_tried && !sc->hs_done) {
		sc->start_tried = true;
		err = xo_start(sc);
		if (err != 0)
			device_printf(sc->dev, "autostart failed: %d\n", err);
		err = 0;
	}
	if (err == 0 && sc->ready && !sc->nwa_done)
		err = xo_nwa_discover(sc);
	if (sc->hs_reply && (!sc->rpc_ready || sc->offload_on != sc->want_offload) &&
	    sc->rpc_tries < XO_RPC_MAX_TRIES)
		(void)xo_stage_c(sc);
	if (sc->nwa_done && sc->want_portup && !sc->portup_on &&
	    sc->portup_tries < XO_PORTUP_MAX_TRIES)
		(void)xo_portup_set(sc, true);
	xo_if_poll(sc);
	if (sc->hook_on && (sc->ifaces_done || sc->if_tries >= XO_IF_MAX_TRIES ||
	    (int)(ticks - sc->hook_start) > XO_HOLD_SECS * hz))
		xo_hold_release(sc);
	if (err != sc->last_err) {
		if (err == 0)
			device_printf(sc->dev, "NPU ready: %u ports\n", sc->nwa_nports);
		else if (err != EAGAIN && err != ETIMEDOUT)
			device_printf(sc->dev, "bring-up error %d\n", err);
		sc->last_err = err;
	}
	if (!sc->stopping && (!sc->nwa_done || sc->hook_on ||
	    (sc->hs_reply && (!sc->rpc_ready || sc->offload_on != sc->want_offload) &&
	    sc->rpc_tries < XO_RPC_MAX_TRIES) ||
	    (sc->want_portup && !sc->portup_on && sc->portup_tries < XO_PORTUP_MAX_TRIES) ||
	    (sc->want_ifaces && !sc->ifaces_done && sc->if_tries < XO_IF_MAX_TRIES)))
		taskqueue_enqueue_timeout(sc->tq, &sc->poll, XO_POLL_TICKS);
}

static int
xo_sysctl_start(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	int v = sc->dp_up && sc->hs_done, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != 1)
		return (EINVAL);
	return (xo_start(sc));
}

static int
xo_sysctl_status(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	struct sbuf sb;
	unsigned int i;
	int error;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	sbuf_printf(&sb, "ready=%d barmap_version=%#x nwa=%d ports=%u marker=%#x last_err=%d dp=%d hs=%d ticks_per_us=%u rpc_ready=%d offload=%d fw_cfg=%#x rpc_err=%d rpc_rc=%#x ports_up=%u portup_fail=%u portup_tries=%u ifaces=%u rx_pkts=%ju rx_err=%ju rx_cmsg=%ju rx_notag=%ju tx=%d tx_pkts=%ju tx_drop=%ju tx_full=%ju iq_ctl=%#jx iq_en=%#jx iq_dbell=%#jx tx_prod=%u dp_debug=%d stalls=%u stall_max_s=%u",
	    sc->ready, sc->barmap_ver, sc->nwa_done, sc->nwa_nports, sc->nwa_marker,
	    sc->last_err, sc->dp_up, sc->hs_done, sc->ticks_per_us, sc->rpc_ready,
	    sc->offload_on, sc->fw_cfg, sc->rpc_err, sc->rpc_last_rc,
	    sc->ports_up, sc->portup_fail, sc->portup_tries, sc->if_made,
	    (uintmax_t)sc->rx_frames, (uintmax_t)sc->rx_drop, (uintmax_t)sc->rx_cmsg,
	    (uintmax_t)sc->rx_notag, sc->want_tx, (uintmax_t)sc->tx_frames,
	    (uintmax_t)sc->tx_drop, (uintmax_t)sc->tx_full,
	    (uintmax_t)(sc->dp_up ? xo_ring_rd(sc, sc->ring[0].ring, XO_R_IN_CONTROL) : 0),
	    (uintmax_t)(sc->dp_up ? xo_ring_rd(sc, sc->ring[0].ring, XO_R_IN_ENABLE) : 0),
	    (uintmax_t)(sc->dp_up ? xo_ring_rd(sc, sc->ring[0].ring, XO_R_IN_INSTR_DBELL) : 0),
	    sc->tx_prod, sc->dp_debug, sc->stalls, sc->stall_max_s);
	for (i = 0; i < XO_FACILITY_COUNT; i++)
		sbuf_printf(&sb, " %s=%#x+%#x", xo_facility_names[i], sc->fac[i].offset,
		    sc->fac[i].size);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
xo_sysctl_links(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	struct sbuf sb;
	uint32_t i, tag, link, st[64];
	int error, e1, e2;

	sbuf_new_for_sysctl(&sb, NULL, 1024, req);
	if (!sc->nwa_done) {
		sbuf_printf(&sb, "network agent not ready");
		error = sbuf_finish(&sb);
		sbuf_delete(&sb);
		return (error);
	}
	sbuf_printf(&sb, "tag link octets_rx octets_tx bcast_rx bcast_tx ucast_rx ucast_tx mcast_tx\n");
	for (i = 0; i < sc->nwa_nports; i++) {
		tag = sc->nwa_reply[1 + i * XO_NWA_WORDS_PER_PORT] & 0xffff;
		link = 0;
		memset(st, 0, sizeof(st));
		sx_xlock(&sc->lock);
		e1 = xo_nwa_req(sc, XO_NWA_OP_GET, XO_NWA_SUB_STATE, tag, 0, 0, &link, 1);
		e2 = xo_nwa_req(sc, XO_NWA_OP_GET, XO_NWA_SUB_STATS, tag, 0, 0, st, nitems(st));
		sx_xunlock(&sc->lock);
		sbuf_printf(&sb, "%u %s%#x", tag, e1 != 0 ? "err" : "", link);
		if (e2 != 0)
			sbuf_printf(&sb, " stats_err=%d\n", e2);
		else
			sbuf_printf(&sb, " %ju %ju %ju %ju %ju %ju %ju\n",
			    (uintmax_t)(st[0] | (uint64_t)st[1] << 32),
			    (uintmax_t)(st[62] | (uint64_t)st[63] << 32),
			    (uintmax_t)(st[6] | (uint64_t)st[7] << 32),
			    (uintmax_t)(st[26] | (uint64_t)st[27] << 32),
			    (uintmax_t)(st[50] | (uint64_t)st[51] << 32),
			    (uintmax_t)(st[52] | (uint64_t)st[53] << 32),
			    (uintmax_t)(st[24] | (uint64_t)st[25] << 32));
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
xo_accel_platform(struct xo_softc *sc)
{
	const uint8_t *reply;
	uint32_t shift;

	sx_assert(&sc->lock, SA_XLOCKED);
	reply = (const uint8_t *)sc->rpc_cmd.vaddr + XO_RPC_BUF_DESC_SIZE;
	if (sc->plat_known)
		return (0);
	sc->rpc_quiet = true;
	for (shift = 0; shift <= XO_RPC_CMD_SHIFT_MAX; shift++) {
		if (xo_rpc_post(sc, XO_RPC_CMD_PLATFORM_READ + shift, NULL, 0) != 0)
			continue;
		if ((sc->rpc_last_len != 228 && sc->rpc_last_len != 232) ||
		    memcmp(reply, "XGS_", 4) != 0)
			continue;
		sc->plat_conn = le32dec(reply + XO_RPC_PLAT_CONN_OFF);
		sc->plat_nhop = le32dec(reply + XO_RPC_PLAT_NHOP_OFF);
		if (sc->plat_conn == 0 || sc->plat_nhop == 0)
			continue;
		memset(sc->plat_name, 0, sizeof(sc->plat_name));
		strlcpy(sc->plat_name, (const char *)reply, sizeof(sc->plat_name));
		sc->plat_shift = shift;
		sc->plat_known = true;
		break;
	}
	sc->rpc_quiet = false;
	return (sc->plat_known ? 0 : ENOENT);
}

static int
xo_sysctl_accel(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	const uint8_t *reply;
	uint8_t payload[12];
	struct sbuf sb;
	uint64_t v;
	uint32_t i, nz;
	int error, err;

	error = priv_check(req->td, PRIV_DRIVER);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 2048, req);
	sx_xlock(&sc->lock);
	if (!sc->rpc_ready) {
		sx_xunlock(&sc->lock);
		sbuf_printf(&sb, "accel: rpc not ready");
		error = sbuf_finish(&sb);
		sbuf_delete(&sb);
		return (error);
	}
	reply = (const uint8_t *)sc->rpc_cmd.vaddr + XO_RPC_BUF_DESC_SIZE;
	err = xo_accel_platform(sc);
	if (err != 0) {
		sx_xunlock(&sc->lock);
		sbuf_printf(&sb, "accel: platform read failed err=%d rpc_ready=%d", err, sc->rpc_ready);
		error = sbuf_finish(&sb);
		sbuf_delete(&sb);
		return (error);
	}
	sbuf_printf(&sb, "platform=%s shift=%u conn=%u nhop=%u\n", sc->plat_name, sc->plat_shift,
	    sc->plat_conn, sc->plat_nhop);
	memset(payload, 0, sizeof(payload));
	le16enc(payload + 4, XO_SYS_CNT_COUNT);
	le32enc(payload + 8, XO_SYS_CNT_COUNT - 1);
	err = xo_rpc_post(sc, XO_RPC_CMD_SYS_CNT_READ + sc->plat_shift, payload, sizeof(payload));
	if (err != 0 || sc->rpc_last_len != XO_SYS_CNT_COUNT * sizeof(uint64_t)) {
		sbuf_printf(&sb, "counters: read failed err=%d len=%u rpc_ready=%d\n", err,
		    sc->rpc_last_len, sc->rpc_ready);
	} else {
		nz = 0;
		for (i = 0; i < XO_SYS_CNT_COUNT; i++) {
			v = le64dec(reply + i * sizeof(uint64_t));
			if (v != 0) {
				sbuf_printf(&sb, "%u=%ju\n", i, (uintmax_t)v);
				nz++;
			}
		}
		sbuf_printf(&sb, "nonzero=%u of %u\n", nz, XO_SYS_CNT_COUNT);
	}
	sx_xunlock(&sc->lock);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static const struct { uint8_t id; const char *name; } xo_cnt_names[] = {
	{ 0, "RX_WIRE" }, { 1, "TX_WIRE" }, { 2, "WIRE_TO_WIRE" }, { 3, "RX_KN" },
	{ 8, "TX_DROP" }, { 9, "TX_DROP_QUEUE_FULL" }, { 10, "TX_DROP_LEN_ERR" },
	{ 11, "TX_DROP_SHAPER_INV" }, { 21, "TX_DROP_POLL" }, { 22, "TX_DROP_ERR" },
	{ 24, "DROP_LIF_INDEX_INVALID" }, { 25, "DROP_LIF_LU_NULL" },
	{ 26, "DROP_LIF_INVALID" }, { 27, "DROP_LIF_ADMIN_DISABLED" },
	{ 28, "DROP_LIF_NOT_MY_MAC" }, { 29, "DROP_MTU_EXCEEDED" },
	{ 30, "DROP_TCP_SEQ_CHK_INVALID" }, { 31, "DROP_IPS_VERDICT" },
	{ 33, "DROP_MFLOW_ACTION" }, { 34, "DROP_MFLOW_UNSUPPORTED_ACTION" },
	{ 35, "DROP_FRAG_ERROR" }, { 36, "DROP_IG_ERR" },
	{ 40, "TO_KN_NON_ACCEL" }, { 42, "TO_KN_FW_REV_MISMATCH" },
	{ 43, "TO_KN_L3_FW_REV_MISMATCH" }, { 52, "TO_KN_CONN_RECLAIM_PENDING" },
	{ 53, "TO_KN_CONN_RECLAIMED" }, { 56, "TO_KN_TCP_SEQ_CHK_INVALID" },
	{ 57, "TO_KN_TCP_MAX_RETRANS" }, { 58, "TO_KN_TCP_FIN_SYN_RST" },
	{ 59, "TO_KN_MFLOW_NOT_ACTIVE" }, { 60, "TO_KN_EG_ERR" },
};

static void
xo_mon_counters(struct xo_softc *sc)
{
	const uint8_t *reply;
	uint8_t payload[12];
	char line[400];
	uint64_t now, delta;
	size_t n = 0;
	uint32_t i, k;

	sx_xlock(&sc->lock);
	if (!sc->rpc_ready || xo_accel_platform(sc) != 0) {
		sx_xunlock(&sc->lock);
		return;
	}
	reply = (const uint8_t *)sc->rpc_cmd.vaddr + XO_RPC_BUF_DESC_SIZE;
	memset(payload, 0, sizeof(payload));
	le16enc(payload + 4, XO_SYS_CNT_COUNT);
	le32enc(payload + 8, XO_SYS_CNT_COUNT - 1);
	sc->rpc_quiet = true;
	if (xo_rpc_post(sc, XO_RPC_CMD_SYS_CNT_READ + sc->plat_shift, payload, sizeof(payload)) != 0 ||
	    sc->rpc_last_len != XO_SYS_CNT_COUNT * sizeof(uint64_t)) {
		sc->rpc_quiet = false;
		sx_xunlock(&sc->lock);
		return;
	}
	sc->rpc_quiet = false;
	line[0] = '\0';
	for (i = 0; i < XO_SYS_CNT_COUNT; i++) {
		const char *name = NULL;

		now = le64dec(reply + i * sizeof(uint64_t));
		delta = now - sc->mon_cnt[i];
		sc->mon_cnt[i] = now;
		if (!sc->mon_cnt_ok || delta == 0)
			continue;
		for (k = 0; k < nitems(xo_cnt_names); k++)
			if (xo_cnt_names[k].id == i)
				name = xo_cnt_names[k].name;
		if (n < sizeof(line))
			n += snprintf(line + n, sizeof(line) - n, " %s[%u]+%ju",
			    name != NULL ? name : "ctr", i, (uintmax_t)delta);
	}
	sc->mon_cnt_ok = true;
	sx_xunlock(&sc->lock);
	if (n != 0)
		device_printf(sc->dev, "dp-debug: npu counters moved:%s\n", line);
}

static void
xo_mon_task(void *arg, int pending __unused)
{
	struct xo_softc *sc = arg;
	uint64_t rx, tx, rxd, txd, txf, drx;

	sc->mon_run = false;
	if (sc->stopping || sc->dp_debug == 0)
		return;
	rx = sc->rx_frames;
	tx = sc->tx_frames;
	rxd = sc->rx_drop;
	txd = sc->tx_drop;
	txf = sc->tx_full;
	drx = rx - sc->mon_rx;
	if (rxd != sc->mon_rxdrop || txd != sc->mon_txdrop || txf != sc->mon_txfull)
		device_printf(sc->dev, "dp-debug: errors rx_err+%ju tx_drop+%ju tx_full+%ju\n",
		    (uintmax_t)(rxd - sc->mon_rxdrop), (uintmax_t)(txd - sc->mon_txdrop),
		    (uintmax_t)(txf - sc->mon_txfull));
	if (drx == 0) {
		sc->mon_zero_s++;
		if (sc->mon_zero_s == 2 && sc->mon_hi_rate >= 1000 && !sc->mon_stall) {
			sc->mon_stall = true;
			device_printf(sc->dev, "dp-debug: stall start: no rx for 2 s after %ju pkts/s\n",
			    (uintmax_t)sc->mon_hi_rate);
		}
	} else {
		if (sc->mon_stall) {
			sc->mon_stall = false;
			sc->stalls++;
			if (sc->mon_zero_s > sc->stall_max_s)
				sc->stall_max_s = sc->mon_zero_s;
			device_printf(sc->dev, "dp-debug: stall end after %u s\n", sc->mon_zero_s);
		}
		sc->mon_zero_s = 0;
		sc->mon_hi_rate = drx;
	}
	sc->mon_rx = rx;
	sc->mon_tx = tx;
	sc->mon_rxdrop = rxd;
	sc->mon_txdrop = txd;
	sc->mon_txfull = txf;
	if (++sc->mon_secs % 10 == 0) {
		device_printf(sc->dev, "dp-debug: rate rx %ju tx %ju pkts/s (10 s avg)\n",
		    (uintmax_t)((rx - sc->mon_rx10) / 10), (uintmax_t)((tx - sc->mon_tx10) / 10));
		sc->mon_rx10 = rx;
		sc->mon_tx10 = tx;
	}
	xo_mon_counters(sc);
	if (!sc->stopping && sc->dp_debug != 0) {
		sc->mon_run = true;
		taskqueue_enqueue_timeout(sc->tq, &sc->mon, hz);
	}
}

static int
xo_sysctl_dp_debug(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	int v = sc->dp_debug, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v < 0 || v > 1)
		return (EINVAL);
	if (v == 1 && sc->dp_debug == 0) {
		sc->mon_cnt_ok = false;
		sc->mon_stall = false;
		sc->mon_zero_s = 0;
		sc->mon_hi_rate = 0;
		sc->mon_secs = 0;
		sc->mon_rx = sc->mon_rx10 = sc->rx_frames;
		sc->mon_tx = sc->mon_tx10 = sc->tx_frames;
		sc->mon_rxdrop = sc->rx_drop;
		sc->mon_txdrop = sc->tx_drop;
		sc->mon_txfull = sc->tx_full;
	}
	sc->dp_debug = v;
	if (v == 1 && !sc->mon_run && !sc->stopping) {
		sc->mon_run = true;
		taskqueue_enqueue_timeout(sc->tq, &sc->mon, hz);
	}
	return (0);
}

static void
xo_ft_dump(struct xo_softc *sc, const char *name, const uint8_t *p, uint32_t len)
{
	char line[3 * 16 + 1];
	uint32_t i, j, n;

	if (sc->dp_debug == 0)
		return;
	for (i = 0; i < len; i += 16) {
		n = MIN(16, len - i);
		for (j = 0; j < n; j++)
			snprintf(line + j * 3, 4, "%02x ", p[i + j]);
		device_printf(sc->dev, "ft: dbg %s +%u: %s\n", name, i, line);
	}
}

static void
xo_ft_mflow(uint8_t *p, uint32_t id, uint32_t rev, uint32_t dir, uint32_t conn, uint32_t nhop,
    bool clear)
{
	memset(p, 0, XO_FT_MFLOW_LEN);
	le32enc(p, id | rev << XO_RX_FLOW_REV_SHIFT | 0x80000000U);
	if (!clear) {
		le32enc(p + 4, XO_FT_ACTION_FWD << 16 | dir << 23 | (XO_FT_BRCTL & 0xf) << 24 |
		    XO_FT_STATE_ACTIVE << 28);
		le32enc(p + 28, XO_FT_TIMEOUT_S);
	}
	le32enc(p + 12, conn);
	le32enc(p + 16, XO_FT_CONN_REV << 16);
	le32enc(p + 20, nhop);
}

static void
xo_ft_nhop(uint8_t *p, uint32_t index, const uint8_t *dmac, const uint8_t *smac, uint8_t iface)
{
	memset(p, 0, XO_FT_NHOP_LEN);
	le32enc(p, index);
	p[4] = 1;
	memcpy(p + 8, dmac, ETHER_ADDR_LEN);
	memcpy(p + 14, smac, ETHER_ADDR_LEN);
	be16enc(p + 20, ETHERTYPE_IP);
	p[26] = 1;
	p[27] = iface;
	le16enc(p + 28, XO_LIF_MTU);
}

static int
xo_ft_table_read(struct xo_softc *sc, uint8_t cmd, uint32_t index, const uint8_t **reply)
{
	uint8_t req[12] = {};
	int err;

	le32enc(req, index);
	le16enc(req + 4, 1);
	le16enc(req + 6, 2);
	le32enc(req + 8, index);
	err = xo_rpc_post(sc, cmd + sc->plat_shift, req, sizeof(req));
	if (err != 0)
		return (err);
	*reply = (const uint8_t *)sc->rpc_cmd.vaddr + XO_RPC_BUF_DESC_SIZE;
	return (0);
}

static int
xo_ft_mflow_read(struct xo_softc *sc, uint32_t index, const uint8_t **reply)
{
	int err;

	err = xo_ft_table_read(sc, XO_RPC_CMD_LO_MFLOW_READ, index, reply);
	if (err != 0)
		return (err);
	if (sc->rpc_last_len != 116 || le32dec(*reply) != index)
		return (EPROTO);
	return (0);
}

static int
xo_ft_conn_read(struct xo_softc *sc, uint32_t index, const uint8_t **entry)
{
	const uint8_t *reply;
	int err;

	err = xo_ft_table_read(sc, XO_RPC_CMD_LO_CONN_READ, index, &reply);
	if (err != 0)
		return (err);
	if ((sc->rpc_last_len != 112 && sc->rpc_last_len != 116) || le32dec(reply) != index)
		return (EPROTO);
	*entry = reply + sc->rpc_last_len - 108;
	return (0);
}

static int
xo_ft_check_mflow(struct xo_softc *sc, const uint8_t *payload, bool verify)
{
	const uint8_t *reply, *op;
	uint32_t identity = le32dec(payload);
	uint32_t index = identity & XO_RX_FLOW_ID_MASK;
	uint32_t rev = (identity >> XO_RX_FLOW_REV_SHIFT) & XO_RX_FLOW_REV_MASK;
	uint32_t state;
	int err;

	err = xo_ft_mflow_read(sc, index, &reply);
	if (err != 0)
		return (err);
	state = le32dec(reply + 84);
	if (((state >> 16) & 0xff) == 0)
		return (EAGAIN);
	if (((state >> 8) & 0x3f) != rev) {
		device_printf(sc->dev, "ft: mflow %u state %#010x want rev %u\n", index, state, rev);
		return (ESTALE);
	}
	if (!verify)
		return (0);
	op = reply + 92;
	if (((state >> 24) & 0xff) == 0 ||
	    ((le32dec(op) ^ le32dec(payload + 4)) & 0xff8fffffU) != 0 ||
	    ((le32dec(op + 4) ^ le32dec(payload + 8)) & 0x01ffffffU) != 0 ||
	    le32dec(op + 8) != le32dec(payload + 12) ||
	    le32dec(op + 12) != le32dec(payload + 16) ||
	    le32dec(op + 16) != le32dec(payload + 20) ||
	    le16dec(op + 20) != le16dec(payload + 24)) {
		device_printf(sc->dev, "ft: verify mismatch mflow %u state %#010x\n", index, state);
		xo_ft_dump(sc, "want", payload + 4, 22);
		xo_ft_dump(sc, "have", op, 22);
		return (EIO);
	}
	return (0);
}

static int
xo_ft_check_conn(struct xo_softc *sc, const uint8_t *payload)
{
	const uint8_t *entry;
	int err;

	err = xo_ft_conn_read(sc, le32dec(payload), &entry);
	if (err != 0)
		return (err);
	if (((le32dec(entry) ^ le32dec(payload + 4)) & 0xc078ffffU) != 0 ||
	    le32dec(entry + 8) != le32dec(payload + 12) ||
	    memcmp(entry + 80, payload + XO_FT_FLOW_OFF_NAT, 24) != 0) {
		device_printf(sc->dev, "ft: verify mismatch conn %u\n", le32dec(payload));
		xo_ft_dump(sc, "conn want", payload + 4, 16);
		xo_ft_dump(sc, "conn have", entry, 16);
		return (EIO);
	}
	return (0);
}

static int
xo_ft_flow_write(struct xo_softc *sc, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
	unsigned int attempt;
	int err;

	if (cmd == XO_RPC_CMD_FLOW_CREATE_FP) {
		err = xo_ft_check_mflow(sc, payload + XO_FT_FLOW_OFF_MFLOW_O, false);
		if (err == 0)
			err = xo_ft_check_mflow(sc, payload + XO_FT_FLOW_OFF_MFLOW_R, false);
		if (err != 0)
			return (err);
	}
	err = xo_rpc_post(sc, cmd, payload, len);
	if (err != 0 || cmd != XO_RPC_CMD_FLOW_CREATE_FP)
		return (err);
	for (attempt = 0; attempt < 3; attempt++) {
		if (attempt != 0)
			pause_sbt("xoft", 5 * SBT_1MS, 0, C_HARDCLOCK);
		err = xo_ft_check_mflow(sc, payload + XO_FT_FLOW_OFF_MFLOW_O, true);
		if (err == 0)
			err = xo_ft_check_mflow(sc, payload + XO_FT_FLOW_OFF_MFLOW_R, true);
		if (err == 0)
			err = xo_ft_check_conn(sc, payload);
		if (err != EIO)
			break;
	}
	return (err);
}

static void
xo_ft_teardown(struct xo_softc *sc)
{
	uint8_t req[8] = {}, m[XO_FT_MFLOW_LEN];
	const uint8_t *entry;
	uint32_t conn = XO_FT_CONN_BASE, flags, i;

	if (xo_ft_conn_read(sc, conn, &entry) == 0) {
		flags = le32dec(entry);
		if ((flags >> 30) == 1 || (flags >> 30) == 2) {
			le32enc(req, conn);
			le16enc(req + 4, flags & 0xffff);
			if (xo_rpc_post(sc, XO_RPC_CMD_CONN_RECLAIM_FP, req, sizeof(req)) == 0)
				sc->ft_reclaimed++;
		}
	}
	for (i = 0; i < 2; i++) {
		xo_ft_mflow(m, sc->ft_id[i], sc->ft_rev[i], 0, conn, XO_FT_NHOP_BASE + i, true);
		(void)xo_rpc_post(sc, XO_RPC_CMD_MFLOW_PROGRAM, m, sizeof(m));
	}
	sc->ft_used = false;
}

/* Copy the best reverse-direction TCP pair; orig is the side whose peer shows SNAT. */
static bool
xo_ft_pick(struct xo_softc *sc, struct xo_flow *orig, struct xo_flow *reply)
{
	uint64_t best = 0;
	bool found = false, ambiguous = false;
	uint32_t i, j;

	mtx_lock(&sc->flow_lock);
	for (i = 0; i < XO_FLOW_MAX; i++) {
		const struct xo_flow *a = &sc->flow[i];

		if (!a->used || !a->tuple_valid || a->proto != 6 || a->frames < XO_FT_MIN_FRAMES ||
		    (ticks - a->last_seen) * 1000 / hz > XO_FT_MAX_AGE_MS)
			continue;
		for (j = 0; j < XO_FLOW_MAX; j++) {
			const struct xo_flow *b = &sc->flow[j];

			if (j == i || !b->used || !b->tuple_valid || b->proto != 6 ||
			    b->frames < XO_FT_MIN_FRAMES ||
			    (ticks - b->last_seen) * 1000 / hz > XO_FT_MAX_AGE_MS ||
			    a->tag == b->tag || memcmp(b->src, a->dst, 4) != 0 ||
			    b->sport != a->dport)
				continue;
			if (memcmp(b->dst, a->src, 4) == 0 && b->dport == a->sport) {
				ambiguous = true;
				continue;
			}
			if (a->frames + b->frames > best) {
				best = a->frames + b->frames;
				*orig = *a;
				*reply = *b;
				found = true;
			}
		}
	}
	mtx_unlock(&sc->flow_lock);
	if (!found && ambiguous)
		device_printf(sc->dev, "ft: pair has no NAT, orientation ambiguous: refused\n");
	return (found);
}

static int
xo_ft_add(struct xo_softc *sc)
{
	uint8_t nh[2][XO_FT_NHOP_LEN], fc[XO_FT_FLOW_LEN];
	struct xo_flow a, b;
	uint8_t *nat;
	uint32_t flags;
	int err, ia, ib;
	const char *step = "none";
	bool snat, dnat;

	sx_assert(&sc->lock, SA_XLOCKED);
	if (!sc->rpc_ready || !sc->offload_on)
		return (ENXIO);
	if (xo_accel_platform(sc) != 0)
		return (ENOENT);
	if (sc->ft_used)
		return (EBUSY);
	if (!xo_ft_pick(sc, &a, &b))
		return (ENOENT);
	ia = xo_iface_for_tag(a.tag);
	ib = xo_iface_for_tag(b.tag);
	if (ia < 0 || ib < 0)
		return (ENODEV);

	xo_ft_nhop(nh[0], XO_FT_NHOP_BASE, b.smac, b.dmac, ib + XO_FT_NWA_IFACE_BASE);
	xo_ft_nhop(nh[1], XO_FT_NHOP_BASE + 1, a.smac, a.dmac, ia + XO_FT_NWA_IFACE_BASE);

	memset(fc, 0, sizeof(fc));
	le32enc(fc, XO_FT_CONN_BASE);
	le32enc(fc + 12, XO_FT_CONN_BASE);
	nat = fc + XO_FT_FLOW_OFF_NAT;
	memcpy(nat, a.src, 4);
	memcpy(nat + 4, a.dst, 4);
	memcpy(nat + 8, b.dst, 4);
	memcpy(nat + 12, b.src, 4);
	be16enc(nat + 16, a.dport);
	be16enc(nat + 18, a.sport);
	be16enc(nat + 20, b.sport);
	be16enc(nat + 22, b.dport);
	snat = memcmp(b.dst, a.src, 4) != 0 || b.dport != a.sport;
	dnat = memcmp(b.src, a.dst, 4) != 0 || b.sport != a.dport;
	flags = XO_FT_CONN_REV | (XO_FT_VERDICT & 3) << 21 | 1U << 30 | (uint32_t)snat << 20 |
	    (uint32_t)dnat << 19;
	le32enc(fc + 4, flags);
	le32enc(fc + XO_FT_FLOW_OFF_VALID, 3);
	xo_ft_mflow(fc + XO_FT_FLOW_OFF_MFLOW_O, a.id, a.rev, 0, XO_FT_CONN_BASE, XO_FT_NHOP_BASE,
	    false);
	xo_ft_mflow(fc + XO_FT_FLOW_OFF_MFLOW_R, b.id, b.rev, 1, XO_FT_CONN_BASE,
	    XO_FT_NHOP_BASE + 1, false);

	sc->ft_id[0] = a.id;
	sc->ft_rev[0] = a.rev;
	sc->ft_id[1] = b.id;
	sc->ft_rev[1] = b.rev;
	step = "nhop0";
	err = xo_rpc_post(sc, XO_RPC_CMD_NHOP_PROGRAM, nh[0], XO_FT_NHOP_LEN);
	if (err == 0) {
		step = "nhop1";
		err = xo_rpc_post(sc, XO_RPC_CMD_NHOP_PROGRAM, nh[1], XO_FT_NHOP_LEN);
	}
	if (err == 0) {
		step = "flow-create";
		err = xo_ft_flow_write(sc, XO_RPC_CMD_FLOW_CREATE_FP, fc, sizeof(fc));
	}
	if (err != 0) {
		sc->ft_failed++;
		device_printf(sc->dev, "ft: program failed at %s (%d) ids %u/%u\n", step, err,
		    a.id, b.id);
		if (err != EAGAIN && err != ESTALE && err != EBUSY && strcmp(step, "flow-create") == 0) {
			xo_ft_teardown(sc);
			device_printf(sc->dev, "ft: rolled back conn %u\n", XO_FT_CONN_BASE);
		}
		return (err);
	}
	sc->ft_used = true;
	sc->ft_programmed++;
	xo_ft_dump(sc, "nhop-orig", nh[0], sizeof(nh[0]));
	xo_ft_dump(sc, "nhop-reply", nh[1], sizeof(nh[1]));
	xo_ft_dump(sc, "flow-create", fc, sizeof(fc));
	device_printf(sc->dev,
	    "ft: offloaded conn %u orig id %u rev %u reply id %u rev %u%s "
	    "%u.%u.%u.%u:%u > %u.%u.%u.%u:%u nat %u.%u.%u.%u:%u > %u.%u.%u.%u:%u "
	    "frames %ju/%ju out if %d/%d\n",
	    XO_FT_CONN_BASE, a.id, a.rev, b.id, b.rev, snat || dnat ? " (NAT)" : "",
	    a.src[0], a.src[1], a.src[2], a.src[3], a.sport,
	    a.dst[0], a.dst[1], a.dst[2], a.dst[3], a.dport,
	    b.dst[0], b.dst[1], b.dst[2], b.dst[3], b.dport,
	    b.src[0], b.src[1], b.src[2], b.src[3], b.sport,
	    (uintmax_t)a.frames, (uintmax_t)b.frames, ib, ia);
	return (0);
}

static int
xo_sysctl_ft_add(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	int error, v = 0;

	error = priv_check(req->td, PRIV_DRIVER);
	if (error != 0)
		return (error);
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != 1)
		return (EINVAL);
	if (xo_ft == 0)
		return (EPERM);
	sx_xlock(&sc->lock);
	error = xo_ft_add(sc);
	sx_xunlock(&sc->lock);
	return (error);
}

static int
xo_sysctl_ft_del(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	int error, v = 0;

	error = priv_check(req->td, PRIV_DRIVER);
	if (error != 0)
		return (error);
	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (v != 1)
		return (EINVAL);
	if (xo_ft == 0)
		return (EPERM);
	sx_xlock(&sc->lock);
	if (!sc->rpc_ready || !sc->plat_known || !sc->ft_used)
		error = ENOENT;
	else {
		xo_ft_teardown(sc);
		device_printf(sc->dev, "ft: released conn %u\n", XO_FT_CONN_BASE);
	}
	sx_xunlock(&sc->lock);
	return (error);
}

static int
xo_sysctl_alloc(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	struct sbuf sb;
	int error;

	error = priv_check(req->td, PRIV_DRIVER);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	sbuf_printf(&sb, "conn_used=%u conn_total=%u nhop_used=%u nhop_total=%u programmed=%u failed=%u "
	    "reclaimed=%u rearmed=0 ft=%d offload=%d fw_cfg=%#06x fw_cfg_source=last_sent",
	    sc->ft_used ? 1 : 0, sc->plat_known ? sc->plat_conn : 0, sc->ft_used ? 2 : 0,
	    sc->plat_known ? sc->plat_nhop : 0, sc->ft_programmed, sc->ft_failed, sc->ft_reclaimed,
	    xo_ft, sc->offload_on, sc->fw_cfg);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
xo_sysctl_flows(SYSCTL_HANDLER_ARGS)
{
	struct xo_softc *sc = arg1;
	struct sbuf sb;
	uint32_t i, n = 0;
	int error;

	error = priv_check(req->td, PRIV_DRIVER);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 4096, req);
	mtx_lock(&sc->flow_lock);
	sbuf_printf(&sb, "meta_frames=%ju sig=%ju valid=%ju\n", (uintmax_t)sc->meta_frames,
	    (uintmax_t)sc->meta_sig, (uintmax_t)sc->meta_valid);
	for (i = 0; i < XO_FLOW_MAX; i++) {
		const struct xo_flow *e = &sc->flow[i];

		if (!e->used)
			continue;
		n++;
		sbuf_printf(&sb, "id=%u rev=%u tag=%u ", e->id, e->rev, e->tag);
		if (e->tuple_valid)
			sbuf_printf(&sb, "proto=%u %u.%u.%u.%u:%u > %u.%u.%u.%u:%u vlan=%u %02x:%02x:%02x:%02x:%02x:%02x > %02x:%02x:%02x:%02x:%02x:%02x ",
			    e->proto, e->src[0], e->src[1], e->src[2], e->src[3], e->sport,
			    e->dst[0], e->dst[1], e->dst[2], e->dst[3], e->dport, e->vlan,
			    e->smac[0], e->smac[1], e->smac[2], e->smac[3], e->smac[4], e->smac[5],
			    e->dmac[0], e->dmac[1], e->dmac[2], e->dmac[3], e->dmac[4], e->dmac[5]);
		else
			sbuf_printf(&sb, "no-tuple ");
		sbuf_printf(&sb, "frames=%ju age_ms=%d\n", (uintmax_t)e->frames,
		    (int)((ticks - e->last_seen) * 1000 / hz));
	}
	mtx_unlock(&sc->flow_lock);
	sbuf_printf(&sb, "entries=%u of %u\n", n, XO_FLOW_MAX);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

static int
xo_probe(device_t dev)
{
	if (pci_get_vendor(dev) != XO_VENDOR_ID || pci_get_device(dev) != XO_DEVICE_ID)
		return (ENXIO);
	device_set_desc(dev, "XGS OCTEON TX PCIe endpoint");
	return (BUS_PROBE_DEFAULT);
}

static void
xo_release(struct xo_softc *sc)
{
	if (sc->bar0 != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, sc->rid0, sc->bar0);
	if (sc->bar2 != NULL)
		bus_release_resource(sc->dev, SYS_RES_MEMORY, sc->rid2, sc->bar2);
	sc->bar0 = sc->bar2 = NULL;
	if (sc->nwa_reply != NULL)
		free(sc->nwa_reply, M_XGSOCTEP);
	sc->nwa_reply = NULL;
}

static int
xo_attach(device_t dev)
{
	struct xo_softc *sc = device_get_softc(dev);

	sc->dev = dev;
	sc->last_err = -1;
	pci_enable_busmaster(dev);

	sc->rid0 = PCIR_BAR(0);
	sc->bar0 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->rid0, RF_ACTIVE);
	sc->rid2 = PCIR_BAR(2);
	sc->bar2 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->rid2, RF_ACTIVE);
	if (sc->bar0 == NULL || sc->bar2 == NULL) {
		device_printf(dev, "cannot map BAR0/BAR2\n");
		xo_release(sc);
		return (ENXIO);
	}
	sc->bar0_len = rman_get_size(sc->bar0);
	sc->bar2_len = rman_get_size(sc->bar2);
	if (sc->bar0_len < XO_SDP_SCRATCH + sizeof(uint64_t)) {
		device_printf(dev, "BAR0 too small (%#jx)\n", (uintmax_t)sc->bar0_len);
		xo_release(sc);
		return (ENXIO);
	}
	sc->nwa_reply = malloc(XO_NWA_MAX_WORDS * sizeof(uint32_t), M_XGSOCTEP,
	    M_WAITOK | M_ZERO);

	sx_init(&sc->lock, "xgs_octep");
	sc->want_offload = xo_offload != 0;
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "offload",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_offload, "I",
	    "firmware offload gate: 1 on (default), 0 off");
	sc->want_portup = xo_portup != 0;
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "portup",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_portup, "I",
	    "raise (1, default) or lower (0) all network agent ports");
	sc->want_ifaces = xo_ifaces != 0;
	sc->want_tx = xo_tx != 0;
	mtx_init(&sc->tx_lock, "xgs_octep_tx", NULL, MTX_DEF);
	mtx_init(&sc->flow_lock, "xgs_octep_flow", NULL, MTX_DEF);
	sc->tq = taskqueue_create("xgs_octep", M_WAITOK, taskqueue_thread_enqueue, &sc->tq);
	taskqueue_start_threads(&sc->tq, 1, PWAIT, "xgs_octep taskq");
	sc->rxtq = taskqueue_create("xgs_octep_rx", M_WAITOK, taskqueue_thread_enqueue, &sc->rxtq);
	taskqueue_start_threads(&sc->rxtq, 1, PWAIT, "xgs_octep rx");
	TIMEOUT_TASK_INIT(sc->rxtq, &sc->rxpoll, 0, xo_rx_task, sc);

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "start",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_start, "I",
	    "write 1 to arm the SDP rings and run the handshake");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "status",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_status, "A",
	    "NPU bring-up status");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "links",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_links, "A",
	    "per-port link word and switch MIB counters");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "accel",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_accel, "A",
	    "read-only NPU fast-path platform and counters");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "flows",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_flows, "A",
	    "read-only NPU flow ids seen on RX, paired with the packet tuple");
	SYSCTL_ADD_U32(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "ports", CTLFLAG_RD,
	    &sc->nwa_nports, 0, "ports reported by the network agent");

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "alloc",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_alloc, "A",
	    "flow allocation counts and the offload gate as last sent");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "ft_add",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_ft_add, "I",
	    "write 1 to program the best live TCP pair as one NPU flow (needs hw.xgs_octep.ft=1)");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "ft_del",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_ft_del, "I",
	    "write 1 to reclaim the programmed NPU flow");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "dp_debug",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, xo_sysctl_dp_debug, "I",
	    "1 logs drops, stalls, packet rates and moved NPU counters (default 0)");

	TIMEOUT_TASK_INIT(sc->tq, &sc->poll, 0, xo_poll_task, sc);
	TIMEOUT_TASK_INIT(sc->tq, &sc->mon, 0, xo_mon_task, sc);
	if (xo_dp_debug != 0) {
		sc->dp_debug = 1;
		sc->mon_run = true;
		taskqueue_enqueue_timeout(sc->tq, &sc->mon, hz);
	}
	if (sc->want_ifaces && cold) {
		sc->hook.ich_func = xo_hold_noop;
		sc->hook.ich_arg = sc;
		sc->hook_start = ticks;
		if (config_intrhook_establish(&sc->hook) == 0)
			sc->hook_on = true;
	}
	taskqueue_enqueue_timeout(sc->tq, &sc->poll, 0);
	return (0);
}

static int
xo_detach(device_t dev)
{
	struct xo_softc *sc = device_get_softc(dev);

	if (sc->dp_up)
		return (EBUSY);
	sc->stopping = true;
	while (taskqueue_cancel_timeout(sc->tq, &sc->poll, NULL) != 0)
		taskqueue_drain_timeout(sc->tq, &sc->poll);
	while (taskqueue_cancel_timeout(sc->tq, &sc->mon, NULL) != 0)
		taskqueue_drain_timeout(sc->tq, &sc->mon);
	taskqueue_free(sc->tq);
	while (taskqueue_cancel_timeout(sc->rxtq, &sc->rxpoll, NULL) != 0)
		taskqueue_drain_timeout(sc->rxtq, &sc->rxpoll);
	taskqueue_free(sc->rxtq);
	xo_hold_release(sc);
	sx_destroy(&sc->lock);
	mtx_destroy(&sc->tx_lock);
	mtx_destroy(&sc->flow_lock);
	xo_release(sc);
	return (0);
}

static device_method_t xo_methods[] = {
	DEVMETHOD(device_probe,		xo_probe),
	DEVMETHOD(device_attach,	xo_attach),
	DEVMETHOD(device_detach,	xo_detach),
	DEVMETHOD_END
};

static driver_t xo_driver = {
	"xgs_octep",
	xo_methods,
	sizeof(struct xo_softc)
};

DRIVER_MODULE(xgs_octep, pci, xo_driver, 0, 0);
MODULE_DEPEND(xgs_octep, pci, 1, 1, 1);
MODULE_VERSION(xgs_octep, 1);
