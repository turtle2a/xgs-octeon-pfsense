/*-
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This project is licensed under the GNU General Public License, version 2 or
 * (at your option) any later version. The complete license text is available at
 * https://www.gnu.org/licenses/old-licenses/gpl-2.0.html.
 *
 * xgs_octep - FreeBSD bring-up driver for the OCTEON TX (CN83XX) PCIe endpoint on an XGS
 * appliance. Written from the Linux octep driver in this project's tree.
 *
 * Stage A: attach, read the NPU's barmap, discover the network agent (ports), publish status
 * through sysctl dev.xgs_octep.N.  Rings, the management handshake, the RPC ring and the offload
 * gate are later stages.
 */

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
#define	XO_RPC_CMD_FW_CFG_PARAMS_SET	2
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
	bool			want_tx;
	struct mtx		tx_lock;
	uint32_t		tx_prod;
	uint64_t		tx_frames;
	uint64_t		tx_drop;
	uint64_t		tx_full;

	struct timeout_task	poll;
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
	if (reply_len > XO_RPC_DATA_MAX_SIZE - XO_RPC_BUF_DESC_SIZE) {
		sc->rpc_ready = false;
		sc->fw_cfg_known = false;
		return (EPROTO);
	}
	if (rc != 0) {
		device_printf(sc->dev, "rpc: command %u refused, return code %#x\n", command, rc);
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
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		m_freem(m);
		return (EMSGSIZE);
	}

	mtx_lock(&sc->tx_lock);
	pending = (uint32_t)xo_ring_rd(sc, r->ring, XO_R_IN_INSTR_DBELL);
	if (pending >= XO_IQ_DESCS - 1) {
		sc->tx_full++;
		mtx_unlock(&sc->tx_lock);
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

		if (tag == XO_CMSG_TAG) {
			sc->rx_cmsg++;
		} else if (p == NULL) {
			sc->rx_notag++;
		} else if (frame_len < ETHER_HDR_LEN || frame_len >
		    (uint32_t)if_getmtu(p->ifp) + ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN) {
			if_inc_counter(p->ifp, IFCOUNTER_IERRORS, 1);
			sc->rx_drop++;
		} else if ((m = m_get2(frame_len, M_NOWAIT, MT_DATA, M_PKTHDR)) == NULL) {
			if_inc_counter(p->ifp, IFCOUNTER_IQDROPS, 1);
			sc->rx_drop++;
		} else if (!xo_rx_copy(r, idx, span, frame_len, mtod(m, uint8_t *))) {
			m_freem(m);
			if_inc_counter(p->ifp, IFCOUNTER_IERRORS, 1);
			sc->rx_drop++;
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
	    sc->portup_on && sc->offload_on && sc->if_tries < XO_IF_MAX_TRIES) {
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
	sbuf_printf(&sb, "ready=%d barmap_version=%#x nwa=%d ports=%u marker=%#x last_err=%d dp=%d hs=%d ticks_per_us=%u rpc_ready=%d offload=%d fw_cfg=%#x rpc_err=%d rpc_rc=%#x ports_up=%u portup_fail=%u portup_tries=%u ifaces=%u rx_pkts=%ju rx_err=%ju rx_cmsg=%ju rx_notag=%ju tx=%d tx_pkts=%ju tx_drop=%ju tx_full=%ju iq_ctl=%#jx iq_en=%#jx iq_dbell=%#jx tx_prod=%u",
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
	    sc->tx_prod);
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
	SYSCTL_ADD_U32(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "ports", CTLFLAG_RD,
	    &sc->nwa_nports, 0, "ports reported by the network agent");

	TIMEOUT_TASK_INIT(sc->tq, &sc->poll, 0, xo_poll_task, sc);
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
	taskqueue_free(sc->tq);
	while (taskqueue_cancel_timeout(sc->rxtq, &sc->rxpoll, NULL) != 0)
		taskqueue_drain_timeout(sc->rxtq, &sc->rxpoll);
	taskqueue_free(sc->rxtq);
	xo_hold_release(sc);
	sx_destroy(&sc->lock);
	mtx_destroy(&sc->tx_lock);
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
