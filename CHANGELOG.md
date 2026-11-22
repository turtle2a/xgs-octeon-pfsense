# Changelog

SPDX-License-Identifier: GPL-2.0-or-later

## Stage B: SDP rings and EP-mode handshake (driver/xgs_octep.c)

Change: after the barmap is ready the driver can arm the 8 SDP rings (ring 0
also gets the input queue), then run the EP-mode handshake with the NPU, then
the network agent discovery answers. Ported from our Linux driver
(`octep_dp_start_locked`, `sdp_handshake_store`).

Safety rules built in:
- refuses if `SLI_EPF_SCRATCH` is not 0 (the NPU may own the rings);
- refuses if RINFO declares VFs (not supported on FreeBSD);
- once rings are armed the module **cannot be unloaded** (`kldunload` fails
  with EBUSY): the NPU keeps DMA-ing into that memory until the NPU reboots;
- nothing runs at boot unless `hw.xgs_octep.autostart=1` is set in
  `/boot/loader.conf.local`.

Test spec (run in this order on the XGS 2300; offload must be verified before
any traffic test, and must be on at start-up):
1. `kldload xgs_octep`; `sysctl dev.xgs_octep.0.status` shows `ready=1`.
   PASS: `ready=1 barmap_version=0x2`.
2. `sysctl dev.xgs_octep.0.start=1` (root).
   PASS: returns without error within about 10 s; `dmesg` shows
   `rings armed` and `handshake complete`; status shows `dp=1 hs=1`.
   FAIL: any errno, `kldstat` still shows the module, host stays responsive.
3. Within a few minutes: `sysctl dev.xgs_octep.0.status`.
   PASS: `nwa=1 ports=N` with N greater than 0.
4. `kldunload xgs_octep`.
   PASS: refused (EBUSY). A successful unload after arming is a FAIL.
5. Reboot with `hw.xgs_octep.autostart=1`; repeat steps 2-3 without step 2.
   PASS: same result with no manual start.
6. Offload gate: see Stage C below.

## Stage C: RPC ring and firmware offload gate (driver/xgs_octep.c)

Change: after the handshake the driver publishes the RPC low ring, waits for
the NPU to acknowledge, then sends FW_CFG_PARAMS_SET with fw_cfg 0x0003
(offload + TCP sequence check). Offload is ON by default
(`hw.xgs_octep.offload=1`); `dev.xgs_octep.0.offload` turns it off/on at run
time. The poll task retries up to 60 times, so it needs no manual step.
Only this one command is ever sent; nothing programs flows yet.

Test spec:
1. After step 2 above, wait up to 2 minutes, then
   `sysctl dev.xgs_octep.0.status`.
   PASS: `rpc_ready=1 offload=1 fw_cfg=0x3 rpc_err=0`; `dmesg` shows
   `firmware offload ON: fw_cfg=0x0003 acknowledged`.
   FAIL: `rpc_err` non-zero after 60 tries, or `rpc_rc` non-zero.
2. `sysctl dev.xgs_octep.0.offload=0` then `=1`.
   PASS: status follows (`offload=0` then `offload=1`, `fw_cfg=0x2` then `0x3`).
3. Offload must read `offload=1` in the status before any traffic test, and
   after a reboot with `hw.xgs_octep.autostart=1` it must come up on with no
   manual step.

## Stage D: raise the NPU ports (driver/xgs_octep.c)

Change: once the network agent has listed its ports, the driver sends one
NWA request per listed port tag: SET / STATE (op 3, sub 0) with value 1, which
is the same "raise port" request the Linux port-setup script uses. On by
default (`hw.xgs_octep.portup=1`); `dev.xgs_octep.0.portup` 1/0 raises or
lowers all ports at run time. No netdev, MAC, LIF or RX/TX path yet: link
only. Offload stays on (stage C) and must read `offload=1` first.

Test spec:
1. After start=1 and stage C pass, wait up to 2 minutes, then
   `sysctl dev.xgs_octep.0.status`.
   PASS: `ports_up=<n>` equals `ports=` (12) and `portup_fail=0`;
   `dmesg` shows `ports raised: 12 of 12`.
   FAIL: `portup_fail` non-zero or `ports_up` below `ports`.
2. Plug a live cable or SFP into a port.
   PASS: its link light comes on (the physical check).
   FAIL: no light with `ports_up` correct: report which port; a netdev or
   MAC step is then needed.
3. `sysctl dev.xgs_octep.0.portup=0` then `=1`.
   PASS: `ports_up` goes to 0 and back to 12, no error in dmesg.

## Autostart at boot (driver/install.sh, no module change)

Change: `install.sh --boot` also writes `hw.xgs_octep.autostart="1"` to
`/boot/loader.conf.local`, so the rings, handshake, offload gate (stage C) and
port-up (stage D) all happen with no manual `start=1`.

Test spec:
1. With the line in place, reboot and do nothing. After about 3 minutes
   `sysctl dev.xgs_octep.0.status`.
   PASS: `dp=1 hs=1 rpc_ready=1 offload=1 ports_up=12 portup_fail=0`.
   FAIL: `dp=0`, or `offload=0` (offload must be on at boot).
   Result (2026-11-20, test box): PASS. `dp=1 hs=1 rpc_ready=1 offload=1 fw_cfg=0x3
   ports_up=12 portup_fail=0 portup_tries=1` with no manual step.

## Stage E1: front-port interfaces and receive (driver/xgs_octep.c)

Change: for each port tag the network agent lists, the driver reads the port
MAC (NWA GET / MAC), registers it with the NPU (LIF_ADD_UPDATE, then
PPORT_UPDATE RPCs) and creates one ifnet `octepN`. A poll task reaps the
receive queues, passes frames to the matching ifnet by tag, and hands the
buffers back to the NPU. Offload stays ON (stage C) and the links are raised
(stage D). Receive only: nothing is transmitted yet (that is stage E2).

Test spec:
1. After boot (autostart on): `sysctl dev.xgs_octep.0.status`.
   PASS: `offload=1 ports_up=12`, plus `ifaces=<n>` equal to the number of
   ports with a valid MAC, and `rx_err=0`.
2. `ifconfig -l` lists `octep0`...; `ifconfig octep0` shows the NPU MAC and
   `status: active` when a cable is in.
   FAIL: no `octepN`, or a wrong MAC.
3. Plug a live device into one port, then `tcpdump -ni octepN -c 5` on that
   interface.
   PASS: frames from the device are captured; `netstat -I octepN` Ipkts rises.
   FAIL: Ipkts stays 0 with link up: report the status and dmesg.
4. `kldunload` still returns EBUSY while armed (expected; reboot to reload).

Result: FAIL. The first E1 build (sha256 c7e28ec5...) panic-looped the box at
boot, so the disk was reinstalled. Likely cause: it was compiled against
FreeBSD `main` headers (1600031), where `struct pkthdr` differs from the
pfSense fork (1600014), so inlined mbuf code used the wrong layout.

## Stage E1 rebuild

Change: build against the pfSense fork headers (`pfsense/FreeBSD-src`
`devel-main`), and `hw.xgs_octep.ifaces` now defaults to 0 so a bad build
cannot loop the box.

Test spec:
1. Boot with the default (ifaces=0): `sysctl dev.xgs_octep.0.status`.
   PASS: `offload=1 ports_up=12 ifaces=0`, box stays up for 10 minutes.
2. Only then set `hw.xgs_octep.ifaces="1"` in `/boot/loader.conf.local`
   and reboot. PASS/FAIL as the Stage E1 spec above.

Result: FAIL (2026-11-20). With `ifaces=1` the kernel took a page fault
inside `ether_ifattach`, called from `xo_ifaces_create` on the driver
taskqueue thread (backtrace: `xo_poll_task` -> `xo_ifaces_create` ->
`ether_ifattach` -> `if_attach_internal` -> `if_addgroup`, fault virtual
address 0x28, supervisor read, page not present; screenshot in logs_pics/).
Cause: that thread has no `curvnet` set, and the pfSense kernel is VIMAGE.

## Stage E1 vnet fix

Change: `xo_ifaces_create` and the RX task now run inside
`CURVNET_SET_QUIET(vnet0)` / `CURVNET_RESTORE()`, so `if_alloc`,
`ether_ifattach` and `if_input` see a valid vnet. The module must be built with
`-DVIMAGE` (the pfSense kernel has it); without it `CURVNET_SET` compiles to
nothing. Build sha256 2e7b4e25...ec1a.

Test spec:
1. Keep `hw.xgs_octep.ifaces` unset (0) first. Boot and check
   `sysctl dev.xgs_octep.0.status`.
   PASS: `offload=1 ports_up=12 ifaces=0`, stable for 10 minutes.
2. Then set `hw.xgs_octep.ifaces="1"` in `/boot/loader.conf.local` and
   reboot. PASS/FAIL as the Stage E1 spec above (no panic, `ifaces=<n>`,
   `octepN` listed, `rx_err=0`).
   FAIL: a panic again. Report the `Fatal trap` block (`instruction pointer`,
   `fault virtual address`) and the top `bt` frames.

Result (2026-11-21, build 2e7b4e25...ec1a): PASS 1 and PASS 2 (list only).
Step 1 (ifaces=0): offload=1 ports_up=12, stable >15 min.
Step 2 (ifaces=1): no panic. `offload=1 ports_up=12 ifaces=12 rx_pkts=4
rx_err=0 rx_cmsg=0 rx_notag=0`. `ifconfig -l` lists octep0..octep11.
Tag map: 33024+256*n -> octep0..9; tag 0 -> octep10; tag 32768 -> octep11.
Pending: PASS 2 (`ifconfig octepN` MAC/status) and PASS 3 (tcpdump with a
live device, Ipkts rising).
PASS 2: `ifconfig octep0` shows ether xx:xx:xx:xx:xx:xx, status: active
(media status is fixed "active" in the driver, not a real link state).
PASS 3 (2026-11-21 14:22): `tcpdump -ni octep0 -c 5` captured ARP requests
from 10.1.1.10 and UDP broadcast from 10.4.4.2 (and a fragment). RX works end
to end. TX is still the E2 stub: DHCP on octep1 gets no address, replies fail.
E1 result: PASS (build 2e7b4e25...ec1a). Next: E2 (TX path).

## Stage E2: transmit (driver/xgs_octep.c)

Change: `octepN` can now send. `if_transmit` copies the frame (mbuf chain) into
a DMA slot behind a 66-byte NPU tag header (port tag, flag byte 1), pads to 60
bytes, writes a 64-byte SDP instruction on input ring 0 and rings the doorbell.
Built from the Linux `octep_dp_xmit` / `octep_dp_build_instr`. If the ring is
full the frame is dropped (counted as `tx_full` / oerrors); there is no queue
yet. New tunable `hw.xgs_octep.tx` (default 0 = E1 stub, frames dropped), so a
bad build cannot loop the box. The TX slot buffers are allocated only when
tx=1. New status keys: `tx_pkts tx_drop tx_full`. Build must use fork headers
and `-DVIMAGE`.

Test spec (offload must be on first: `offload=1 ports_up=12`):
1. With `tx` unset (0): boot, `ifaces=1`; status `tx_pkts=0`, box stable as E1.
2. Set `hw.xgs_octep.tx="1"` in /boot/loader.conf.local, reboot. PASS: no
   panic, `ifaces=12`, `rx_err=0`.
3. `ifconfig octep0 inet 10.4.4.10/28 up` (manual), then from a device on
   port 0 (10.4.4.x) ping 10.4.4.10.
   PASS: replies received on the device; `tcpdump -ni octep0` shows the ICMP
   request AND the echo reply leaving; `netstat -I octep0` Opkts rises,
   Oerrs stays 0; status `tx_pkts` rises, `tx_drop=0`.
   FAIL: Opkts rises but the device sees nothing (check the tag/iface in the
   instruction), `tx_full` rises, or a panic (report the `db>` screen).
4. WAN octep1 DHCP gets an address from the upstream server.
5. Disable/unload behaviour unchanged (kldunload EBUSY while armed).

### E2 result (2026-11-21) and diagnostic build
Control + tx=1 boot: no panic, `offload=1 ports_up=12 ifaces=12`, `tx_pkts` and
Opkts rise, Oerrs/tx_drop/tx_full 0. The stack's ARP reply to 10.4.4.2 appears
in tcpdump (BPF tap, pre-ring) but the device does not receive frames: FAIL on
the wire. TX path matches Linux octep_dp_xmit/build_instr and ring setup, so
the status line gained `iq_ctl iq_en iq_dbell tx_prod` to show whether the NPU
consumes ring-0 instructions (iq_dbell should fall back to 0 after a send;
iq_en bit 0 must be 1).

## Stage E3: hold boot until octepN exist (driver/xgs_octep.c)

Change: with `ifaces=1`, attach registers a `config_intrhook` (only while
`cold`), which stalls root mount. The poll task releases it once the
interfaces are created (`ifaces_done`), after 300 s, or on detach. pfSense
then finds `octep0..11` when it checks assigned interfaces, with no manual
re-assignment prompt. Applies only when `hw.xgs_octep.ifaces=1`.

Test spec (offload on; tx as in E2):
1. Reboot. PASS: no interface-assignment prompt; boot pauses 1-3 min at
   mountroot/before login; `ifconfig -l` has octep0..11 as soon as pfSense
   starts; status `offload=1 ports_up=12 ifaces=12`.
2. FAIL: boot hangs more than 5 min (hook timeout broken) or panics. Recover
   at the loader: `set hw.xgs_octep.ifaces="0"`, `boot`.
3. If the NPU is not ready within 300 s, boot continues and the interfaces
   still appear later (as before this change).

## Stage E3b: advertise interface capabilities (driver/xgs_octep.c)

Change: octepN ifnets set `IFCAP_VLAN_MTU` (capabilities and enabled) before
`ether_ifattach`, as igb does at attach, so `ifconfig` shows `options=` set.
No checksum/TSO offload is claimed; the NPU data path is unchanged.

Test spec (offload on; tx as in E2):
1. `/sbin/ifconfig octep0` shows `options=` containing VLAN_MTU.
2. PASS: boot and E2 status unchanged (`offload=1 ports_up=12 ifaces=12`),
   no panic. Re-test ARP/ping to 10.4.4.2 and report tx_pkts before/after.
3. FAIL: panic or ifconfig error; recover with `hw.xgs_octep.ifaces="0"`.

## Stage E3e: write each port's MAC back to the switch (driver/xgs_octep.c)

Finding: switch counters show unicast replies arrive at Port1 (`ucast_rx` +10
over 5 pings) but `octep0` Ipkts never rises (tcpdump -Q in: 0 packets).
Linux `rpc-port-setup.sh configure_port` writes the port MAC back with NWA
`request 3 3 <tag> <mac_w0> <mac_w1>` (op SET, sub MAC) before all-multicast
and LIF add; we only read it.

Change: `xo_nwa_req` gains `param2` (request word 5); `xo_port_register` issues
NWA SET sub 3 with the MAC read from the switch, before ALLMULTI/LIF_ADD.

Test spec (offload on; tx as in E2):
1. Boot clean: `offload=1 ports_up=12 ifaces=12`, no panic, no WAN prompt.
2. `/sbin/ping -c 3 10.4.4.2` and `/usr/sbin/tcpdump -nev -Q in -i octep0 -c 6`.
3. PASS: inbound frames captured on octep0 and ping replies received.
4. FAIL: still 0 inbound; then compare LIF_READ-first/mask 0x1f logic next.
   Panic: recover with `hw.xgs_octep.ifaces="0"`.

Result (PASS, build a14369a8): after reboot ping to 10.4.4.2 works from octep0.
Root cause: the switch only steered unicast to the host once the port MAC was
written back (NWA SET sub 3), as Linux does; E2 TX/RX code was otherwise right.
Evidence: `tcpdump -Q in -i octep0` captured ARP replies and ICMP echo replies
(seq 0-2), 0 dropped; status `offload=1 ports_up=12 ifaces=12 tx_drop=0 rx_err=0`.
Note: two devices answered ARP for 10.4.4.2 (duplicate IP/proxy ARP on the LAN).
Before the fix (E2 FAIL, builds be30d28f/97eebcbe): `links` showed Port1 link up,
bcast_tx +5/octets_tx +320 per 5 pings (our frames left the NPU) and ucast_rx
+10 (replies reached the switch port) while octep0 Ipkts stayed flat.
Reference: OPNsense octep.ko runs 8 rings, options=80000<LINKSTATE,IPSEC> on
all oxpN (carrier up or down), PROMISC only on the bridge member.

## Stage E3d: match the reference interface options (driver/xgs_octep.c)

Change: octepN capabilities/enabled become `IFCAP_LINKSTATE` only (0x80000),
replacing `IFCAP_VLAN_MTU`, matching OPNsense `oxpN` (`options=80000`). The
IPSEC flag there is IFCAP2 inline offload; not claimed (IPsec runs on the CPU).
Carrier is still set up at attach; real link polling is a later stage.

Test spec (offload on; tx as in E2):
1. `/sbin/ifconfig octep0` shows `options=80000<LINKSTATE>`.
2. PASS: boot and status unchanged (`offload=1 ports_up=12 ifaces=12`), no panic.
3. FAIL: panic or ifconfig error; recover with `hw.xgs_octep.ifaces="0"`.

Result (PASS, build a14369a8): shipped with E3e, boot clean, no panic;
`ifconfig octep0` shows options=80000<LINKSTATE> (verified on the box).
User report: setup runs at boot and NAT works through the octep ports.

## Stage E3c: per-port link readout (driver/xgs_octep.c)

Reference: the working OPNsense `octep.ko` (logs_pics/opensense) advertises
only `LINKSTATE,IPSEC`, reads link with NetAgent `PORT_ATTR_GET` sub 0 (STATE)
on the port tag, and shows real media/carrier; no checksum/VLAN/jumbo options.
So no igb-style capabilities are added here.

Change: new read-only sysctl `dev.xgs_octep.0.links` printing, for every
discovered port tag, the raw word returned by NWA op 4 sub 0. Nothing else in
the data path changes (VLAN_MTU only, as E3b).

Test spec (offload on; tx as in E2):
1. `/sbin/sysctl dev.xgs_octep.0.links` lists 12 tags with a word each.
2. PASS: the word for the cabled port differs from an unplugged port
   (OPNsense showed 0 for no carrier); unplug/replug flips it.
3. FAIL: sysctl errors or status/boot regresses (panic: recover with
   `hw.xgs_octep.ifaces="0"`).

## Result: iperf (no accel)

User-run iperf to 192.168.1.1 through the octep path with flow acceleration not
validated: OK, 0 retries (no numbers recorded yet). Flow offload/accel is still
NOT validated (Task 6).

## Stage E4a: read-only accel inspection (driver/xgs_octep.c) - Task 4

Goal: see what the NPU fast path is doing before programming any flow. Linux
programs flows from the netfilter flowtable (NHOP 6, MFLOW 8, FLOW_CREATE_FP 10);
FreeBSD has no flowtable offload hook, so flow programming (E4b+) will need its
own source of flows (for example pf states). E4a changes no state.

Change: new read-only sysctl `dev.xgs_octep.0.accel` (root only,
`PRIV_DRIVER`). It posts only read commands on the RPC ring, under the softc lock:
1. PLATFORM_READ (36 + shift, shift 0..4): accepted if the reply is 228 or 232
   bytes and starts with `XGS_`; prints the platform name, the conn table size
   (u32 at reply+204) and the nhop table size (u32 at reply+208); remembers the
   shift. Probed once, then cached.
2. WORKER_SYS_CNT_READ (44 + shift): request u16 at +4 = 128, u32 at +8 = 127;
   reply must be 1024 bytes (128 x u64); prints the non-zero counters as `idx=value`.
Needs `rpc_ready=1`; otherwise prints `accel: rpc not ready`.

Test spec (offload on; tx as in E2; no iperf by the agent):
1. Boot clean: `offload=1 ports_up=12 ifaces=12`, no panic, no WAN prompt.
2. `/sbin/sysctl dev.xgs_octep.0.accel` prints `platform=XGS_...` with non-zero
   `conn` and `nhop`, then the counters; run it twice, no `rpc:` errors in dmesg.
3. PASS: reply parsed, status line unchanged afterwards (`rpc_ready=1`,
   `tx_drop=0 rx_err=0`), traffic still passes.
4. FAIL: `rpc: command N refused`/timeout, or `rpc_ready=0` afterwards. A timeout
   drops `rpc_ready` (existing behaviour); recover with a reboot. Panic: recover with
   `hw.xgs_octep.ifaces="0"`.
Next (E4b): with the user running iperf, compare the counters before and after to
see whether any traffic is fast-pathed with no flows programmed.

Result E4a (PASS, build 4800ad82): status `offload=1 fw_cfg=0x3 rpc_ready=1
ports_up=12 ifaces=12 tx_drop=0 rx_err=0` after the reboot, no panic.
`accel`: `platform=XGS_1US shift=0 conn=2000000 nhop=65536`; non-zero counters
0=1252, 1=1327, 3=1327, 12=1252, 40=38, 59=1214, 97=1327 (7 of 128). Counters
0/12 are close to rx_pkts 1239 and 1/3/97 to tx_pkts 1313, so they look like
host-path packet counts (inference only). No flows are programmed.

Result E4b (PASS, baseline, build 4800ad82): iperf3 to the box itself, 30 s,
606 Mbit/s, 0 retr (logs_pics/accel_tests/acceltest1_211126.txt). Counters
0/12 rose 1313 -> 3139413, 1/3/97 1388 -> 3139511, 59 1275 -> 3139375; 40
stayed 38. Over the same run rx_pkts 1239 -> 3139429, tx_pkts 1313 -> 3139527.
Counter deltas track the host rx/tx packet counts within ~100, so they count
packets passing through the host path. No flows programmed; no fast-path
counter moves. This is the pre-offload baseline for Task 6.

## E4a2 - settle after a slow RPC acknowledgement

Evidence (dmesg, two boots): the NPU acknowledged the RPC ring slowly (414 and
474 ms), the first command (2, FW_CFG_PARAMS_SET) then timed out after 2 s
(`fw_cfg=0x0003 FAILED`), and the retry (ack 7 ms) succeeded. Acks of 6, 7, 43
and 231 ms had no failure. Offload ended ON each time, so this costs a 2 s
stall and log noise only.

Change: record the ack time in `xo_rpc_configure`; if it is >= 200 ms, pause
500 ms before the first command. No other behaviour change.

Test spec (user, 3 reboots, offload stays on):
1. After each boot: `/sbin/dmesg | /usr/bin/grep -i xgs_octep | /usr/bin/tail -25`
   and `/sbin/sysctl dev.xgs_octep.0.status`.
2. PASS: every boot ends `firmware offload ON: fw_cfg=0x0003 acknowledged`,
   status `offload=1 rpc_ready=1 ports_up=12 ifaces=12 rpc_err=0`, and no
   `command 2 timed out` after a slow ack. A slow ack (>= 200 ms) may not occur
   in 3 boots; then the result is "not exercised", not a pass of the fix.
3. FAIL: a timeout after a slow ack, or any panic. Recovery at the loader:
   `set hw.xgs_octep.ifaces="0"`; previous build 4800ad82 is in the backup.

Result E4a2 (PASS, 1 boot, build c9f9a601): ack took 302 ms (>= 200, so the
settle ran), first command succeeded: `firmware offload ON: fw_cfg=0x0003
acknowledged`, no timeout. status `offload=1 rpc_ready=1 rpc_err=0 ports_up=12
ifaces=12 rx_err=0 tx_drop=0`. Earlier slow acks (414/474 ms) failed without the
settle. One sample only; more reboots will show the rate.

## E4c - RX flow-identity tracker (read-only, no programming)

Finding (Linux octep.c 904-2100, 5300-5420): a flow cannot be programmed from a
tuple alone. MFLOW_PROGRAM / FLOW_CREATE_FP need the NPU's own microflow `id`
and `revision`, which the NPU stamps into the 82-byte RX prefix of packets of
that flow (signature 0xb44399a2 at prefix+18, valid byte (meta>>8)==1, flow
word at prefix+26: id = bits 0-24, revision = bits 25-30, valid = bit 31). The
driver pairs the id with the packet's 5-tuple (and MACs, VLAN, tag) and keeps a
24-entry table; a flow is later programmed using the hot id plus a partner id
for the other direction. Also needed: the firmware revision (`rpc_fw_rev`).

So the hard-coded one-shot flow idea is dropped. Order is now:
- E4c: parse the RX prefix, track id/revision/tuple per flow, show it in a
  read-only sysctl `dev.xgs_octep.0.flows`. No RPC writes. This also counts
  `rx_cmsg` style metadata frames.
- E4d: NHOP + FLOW_CREATE_FP + MFLOW for one live pf state (default off).
- E4e: re-arm, reclaim, many flows. Then Task 6 validation.

Test spec E4c (user):
1. Boot clean, `offload=1`, `ports_up=12`.
2. Run iperf3 10.4.4.2 -> 192.168.1.1 (user), then
   `/sbin/sysctl dev.xgs_octep.0.flows`.
3. PASS: lines of `id=.. rev=.. tag=.. proto=6 src=10.4.4.2:P dst=192.168.1.1:5201
   frames=N`, an entry for the iperf data stream with large frames, metadata
   frames > 0 in status, and rx_err/tx_drop unchanged, throughput unchanged
   (~600 Mbit/s).
4. FAIL / not exercised: no valid metadata (signature never matches): then the
   NPU needs the offload gate or flow config before it stamps ids; record it.

Result E4c (PASS, build a2d21280): iperf3 10.4.4.2 -> 192.168.1.1, 30 s, 732
Mbit/s, 0 retr (logs_pics/iperf/211126_iperftest2.txt). flows: meta_frames
3789508, sig 3789508 (every frame carries the signature), valid 3789469. The
data stream is seen in both directions at once with equal frame counts:
id=120 rev=0 tag=33024 (octep0) 10.4.4.2:34925 > 192.168.1.1:5201 frames
1893794 (original, pre-NAT) and id=495 rev=0 tag=33280 (octep1)
192.168.1.1:5201 > 192.168.1.13:62717 frames 1893795 (reply, post-NAT).
rx_pkts 3789527 ~= 2 x 1893794, rx_err 0, tx_drop 0. All rev=0. Ids for other
tuples change over time (10.4.4.2:43636 > 34.54.185.247:443 seen as ids 372,
239, 628), so an id must be read live right before programming. Table is
24/24 with LRU churn from background traffic; the iperf entries persist.
Control run planned: the writable `dev.xgs_octep.0.offload` sysctl (RW, already
present) switches the gate off/on live; no new build needed.

Result control run (PASS, build a2d21280, runtime sysctl, no rebuild):
dev.xgs_octep.0.offload=0 -> status offload=0 fw_cfg=0x2, rpc_err 0, 12/12
ports. iperf3 30 s: gate OFF 730 Mbit/s 0 retr (iperftest3_ooflaod_off_sysctl)
vs gate ON 732 Mbit/s 0 retr (iperftest2). Identical: with no flows programmed
the gate changes nothing; host CPU path is the limit (~730 Mbit/s). Gate set
back to 1 and verified: offload=1 fw_cfg=0x3 rpc_ready=1 rx_err=0 tx_drop=0.
Run 1 (606) was a noisy first run. Offload-OFF accel counters not captured.

Finding (toggled-ON run 366 Mbit/s, iperftest3_ooflaod_ON_sysctl): accel after
that run: 0=9472618 1=9472703 3=9472703 12=9472618 39=3780426 40=56
59=5692136 97=9472703 (8 non-zero; counter 39 is new and has no name in the
Linux table). Identity: 0 = 39 + 40 + 59 exactly (9472618), so every frame goes
to the host. Counter 39 ~= host rx_pkts during the gate-OFF run (7570306 -
3789888 = 3780418), i.e. 39 counts frames while fw_cfg=0x2 (gate off). Counter
59 = 3.79M (earlier ON runs) + 1.90M (the 366 run): the toggled-ON run took the
same MFLOW_NOT_ACTIVE path as boot-ON, and moved half as many frames (1.90M vs
3.78M in 30 s). So the NPU classification did not change; cause of the halved
rate is not the gate/NPU path. Next: rerun ON with before/after accel, then
reboot-ON rerun if still slow.

Result rerun (iperftest4_ooflaod_ON_sysctl, gate ON via sysctl, no reboot): 367
Mbit/s, 0 retr. accel before/after: 0 9472961 -> 11367953 (+1894992), 59
+1894986, 39 flat (3780426), 40 62. Not transient: ON after a runtime toggle
repeats ~366 with half the frames of the 732 boot-ON run; path identical (all
frames class 59). Next: reboot with gate ON, rerun iperf, compare.

Compare vs Linux octep.c (offload/allocate), no code change: FW_CFG_PARAMS_SET
payload and bit logic identical (octep.c 4457-4493 vs xo_offload_set). LIF_ADD
(cmd 3) payload identical (index, MAC, MTU 1500, FWD_L3 2, PRIV_MASK_DF 1,
M_ALL 0x00ff) and PPORT_UPDATE (cmd 5) identical. We send LESS than Linux in
three places: (1) no LO_LIF_READ (37) before LIF_ADD, so no skip-if-matches and
no M_MANAGED (0x001f) fallback when the existing LIF flags & 3; (2) never send
FW_STATE_REV_SET (0), no rpc_fw_rev tracking; (3) none of the allocation
commands: NHOP_PROGRAM 6, FLOW_CREATE_FP 10, MFLOW_PROGRAM 8, CONN_RECLAIM_FP
16, LO_CONN_READ 38, LO_MFLOW_READ 40 (E4d/E4e). Items 1-2 are candidates to
test only if the reboot-ON run stays at ~367.

Spec E4f (stall + allocation telemetry; default off, not yet built):
Why: the toggled-ON runs halve throughput and the driver cannot say when or
why; Linux has dp_debug, we have no event log. Change (driver only, no new
RPC): (1) runtime RW int dev.xgs_octep.0.dp_debug (loader hw.xgs_octep.dp_debug,
default 0). When 1, the 1 s poll task logs one dmesg line per event: "stall
start" when rx_pkts delta is 0 for 2 s after >1000 pkts/s, "stall end after N
s", and any increase of rx_err, tx_drop or tx_full; plus a "rate" line every
10 s with rx/tx pkts/s. (2) status gains stalls=N stall_max_s=N. (3) new RO
sysctl dev.xgs_octep.0.alloc: conn_used, conn_total (2000000), nhop_used,
nhop_total (65536), programmed, failed, reclaimed, rearmed, all 0 until E4d/E4e
fill them in; shows gate fw_cfg as last sent (the firmware has no read-back
command we know of). Test: build, deploy with hash checks, reboot, offload
verified ON, dp_debug=1, iperf 30 s. PASS: rate lines track iperf, stall lines
appear only if rx stops, status/alloc print, throughput unchanged (~730),
default 0 adds no log lines. FAIL: any change in rx_err/tx_drop or throughput
with dp_debug=0. Rollback: previous .ko a2d21280.

Spec E4f revision (port the Linux telemetry instead of inventing it; still not
built): Linux octep.c has it: dp_debug (rw param) logs each host-side front-port
drop with its reason (octep.c ~157-174, 2367-2547) and, via
octep_ft_cnt_moved_report (6036), one "dp-debug: npu counters moved: NAME[id]+N"
dmesg line per 1 s SYS_CNT_READ listing every NPU counter that moved, with names
(table at 6018); flow_stats sysfs (6763) shows tag254 frames, cmsg_rx, conn
stats messages/entries/tracked/stale, per-index orig/reply packets and bytes;
"npu-alloc" lines (6063) give one line per offloaded conn with both microflow
ts/state and counter deltas. FreeBSD port: (1) dp_debug RW sysctl as above, drop
reasons logged where rx_err/tx_drop/tx_full are counted; (2) counters-moved line
from the existing SYS_CNT_READ using the Linux name table (counter 39 stays
"ctr"); (3) accel prints names; (4) flow_stats equivalent fed by CMSG conn-stats
frames (rx_cmsg is 0 today, so first confirm the NPU sends them once a flow is
offloaded); (5) npu-alloc lines come with E4d. Stall start/end lines from the
earlier E4f text stay. The alloc sysctl is kept for conn/nhop used counts.
Test and pass/fail as the E4f spec above.

E4f port map (Linux routines to carry over, from octep.c): stall = reply-frozen
guard octep_ft_reply_frozen_locked (5681) and fast guard (5796), which log the
release and SDP ring state; dropped = dp_debug rx/tx drop reasons (2367-2547,
e.g. "rx drop: skb alloc failed"); not allocated = "ft: program failed at
<step> (err)" (5449) and "ft: dbg no free spare for hot id" (6005). The guards
and allocation lines need flows, so they land with E4d/E4e; the drop-reason
and counters-moved lines are the part of E4f that can be built now.

Plan note: once flow offload works properly (E4d/E4e validated), clean up the
E4f debug code (dp_debug lines, stall and moved-counter logging): keep only what
is useful in production, default off, and re-run the baseline iperf after.

Build E4f (built, not deployed): sha256
718b5edb5de5f4c3487af24b172f9d0dcb1723b4d1d9ce58c1dd51545c6cf6dd; previous
build a2d21280 saved as ~/Downloads/xgs-backups/xgs_octep-a2d21280.ko. Clean
compile, no warnings. Implemented: loader tunable hw.xgs_octep.dp_debug (RDTUN,
default 0) and runtime RW dev.xgs_octep.0.dp_debug (0/1); XO_DBG rate-limited
(5/s) drop reasons at the tx bad-length, tx queue-full, rx unknown-tag, rx
bad-length, rx mbuf-alloc and rx ring-copy sites; 1 s monitor task (runs only
while dp_debug=1) logging "errors rx_err+N tx_drop+N tx_full+N", "stall
start"/"stall end after N s" (no rx for 2 s after >=1000 pkts/s), "rate rx N tx
N pkts/s (10 s avg)" every 10 s, and "npu counters moved: NAME[id]+N" from
SYS_CNT_READ with the Linux name table; status gains dp_debug= stalls=
stall_max_s=; new RO dev.xgs_octep.0.alloc (conn/nhop totals, programmed/
failed/reclaimed/rearmed 0 until E4d/E4e, fw_cfg as last sent). accel output is
unchanged (offload-check.sh parses it). Not done: flow_stats (needs CMSG conn
stats), reply-frozen guards and npu-alloc lines (need flows, E4d/E4e).
Test (user): deploy with hash checks, reboot, status shows offload=1 fw_cfg=0x3,
then 🟩 `/sbin/sysctl dev.xgs_octep.0.dp_debug=1`, run iperf3 -B 10.4.4.2 -c
192.168.1.1 -t 30, `/sbin/dmesg | /usr/bin/tail -60`. PASS: rate lines track
iperf, "npu counters moved" lines show RX_WIRE and TO_KN_MFLOW_NOT_ACTIVE
rising together, no stall lines unless rx stops, throughput ~730 Mbit/s, status
and alloc print; with dp_debug=0 no new log lines and no change in rx_err or
tx_drop. FAIL: throughput change with dp_debug=0, any panic. Rollback: the
a2d21280 build.

## E4f deployed, boot-ON result, gate timeout (2026-11-21)

E4f (718b5edb...) deployed with hash checks. Boot-ON iperf, no sysctl toggle:
732 and 728 Mbit/s, 0 retransmits (logs_pics/iperf/*test5*). The halved rate
(366/367) therefore only occurs after the runtime sysctl toggle (xo_stage_c
path); with the gate on at boot the rate matches gate-off (about 730).
offload-check.sh: gate checks all ok; WIRE_TO_WIRE not yet checked (stop
output pending). No flows are allocated: the driver has no flow programming
until E4d, so NOT OFFLOADED is expected.
dmesg (3 boots in msgbuf): gate ON acknowledged on 2 boots; on the 20:41 boot
"rpc: NPU acknowledged after 91 ms" (others 359 and 435 ms), then "rpc:
command 2 timed out" and "firmware offload ON: fw_cfg=0x0003 FAILED". The
driver reset rpc_ready, re-ran the handshake (acknowledged after 7 ms) and
resent the gate: acknowledged, status offload=1 fw_cfg=0x3 rpc_err=0. One
transient in three boots, self-recovered. Candidate hardening (needs its own
spec): settle delay after the early acknowledgement plus one explicit retry.

## E4d spec: program ONE live flow (2026-11-21, before any code)

Why: alloc shows conn_used=0 nhop_used=0 programmed=0; no flow is ever
programmed, so WIRE_TO_WIRE [2] cannot move. E4d programs exactly one TCP
connection by hand to prove the NPU forwards it. Ported from Linux octep.c
(octep_ft_program_locked 5303, flow_write_locked 5058, check_mflow_candidate
4978, check_connection 5031, teardown 5457, payload structs 690-850).
Default OFF: loader tunable hw.xgs_octep.ft (RDTUN, default 0). With ft=0 the
two sysctls below return EPERM and no flow RPC is ever sent.

Controls (ft=1, rpc_ready, plat_known and the offload gate on, else EPERM):
- dev.xgs_octep.0.ft_add (write 1): pick the pair from the live flows table:
  TCP, tuple valid, frames >= 100, age <= 4000 ms, B is the reverse of A
  (B.src=A.dst, B.sport=A.dport). Orientation comes from NAT: orig is the flow
  whose peer's dst/dport differ from its own src/sport (SNAT). No NAT = ambiguous
  = refused and logged (no pf lookup yet). Best pair = most frames. One slot only.
- dev.xgs_octep.0.ft_del (write 1): reclaim the connection and clear both
  microflows.
- Slot 0 = conn 10000, NHOP 100/101 (orig, reply). NHOP iface = port iface + 2
  (from the RX tag via the port table), tag 0, flags L3 (0x01), mtu 1500, MACs:
  orig egress dst = reply frame smac, src = reply frame dmac; reply egress dst =
  orig frame smac, src = orig frame dmac.
- Payload constants as Linux: conn rev 1, verdict 2 (cut-through), mflow action
  1 (FWD), brctl 0xe, state ACTIVE, mflow idle timeout 60 s, fw_rev 0, l3_rev 0,
  nat block (orig src/dst, nat src/dst, dst-port before src-port), SNAT bit 20
  when src or sport changed, DNAT bit 19 when dst or dport changed, valid mask 3.
- Sequence: validate -> NHOP_PROGRAM x2 -> LO_MFLOW_READ both ids (state active,
  rev matches) -> FLOW_CREATE_FP -> verify by LO_MFLOW_READ x2 and LO_CONN_READ
  (3 tries, 5 ms apart). Any failure after FLOW_CREATE: CONN_RECLAIM_FP and
  clear both mflows (rollback). Logs: "ft: offloaded conn ..." and "ft: ids
  ...", "ft: program failed at <step> (err)", "ft: rolled back"; payload hex
  dumps when dp_debug=1.
- alloc line now reports conn_used, nhop_used, programmed, failed from real state.
- Not in E4d: re-arm every 45 s (E4e; Linux saw microflows collapse 60-90 s into
  a transfer without it), multiple flows, reclaim sweeps, pf lookup, TCP state
  block, LIF_READ, FW_STATE_REV_SET. Keep iperf at 30 s.

Test (user, with offload verified on at boot): set hw.xgs_octep.ft="1" in
/boot/loader.conf.local, deploy with hash checks, reboot; offload-check.sh
(gate ON); dp_debug=1; offload-check.sh start; start iperf3 -B 10.4.4.2 -c
192.168.1.1 -t 30; about 5 s in run 🟩 ft_add=1; after iperf: ft_del=1;
offload-check.sh stop; dmesg | tail -80; alloc.
PASS: "ft: offloaded conn 10000" logged; WIRE_TO_WIRE [2] rises and the verdict
is OFFLOADED or PARTIAL; host rx_pkts/tx_pkts rise far less than the ~2.2 M of a
host-path run; iperf at least the 730 baseline. PARTIAL (data, not failure): [2]
rises then stalls (stall lines from the monitor, as in Linux ~450-550 frames).
FAIL: ft=0 sends any flow RPC, panic, or traffic blackholed after ft_del.
Rollback: hw.xgs_octep.ft="0" at the loader prompt, or build 718b5edb.

### E4d build

Build `79ee7dde976981ce11110d96532b8bbbe7c96d4ecaae289595f09744bdaec0a5` (compiled clean, no warnings). Default off: needs `hw.xgs_octep.ft="1"` in /boot/loader.conf.local. Sysctls: `dev.xgs_octep.0.ft_add=1`, `ft_del=1`; `alloc` now shows real conn_used/nhop_used/programmed/failed/reclaimed. Rollback: set `hw.xgs_octep.ft="0"` or E4f `718b5edb`. Result: PARTIAL (21:50). ft_add programmed conn 10000 (programmed=1 failed=0), NHOP/mflow/flow payloads decode correctly. WIRE_TO_WIRE +550 then traffic stalled (iperf 740 Mbit/s to 0 from 6 s). ft_del released cleanly (reclaimed=1), slow path recovered. Matches the Linux ~450-550 frame stall (Task 9 item 7). Task 4 paused; resume with E4e (re-arm, TCP state block, high-priority RPC ring).

## Task 8, step 1: P3 and P8 in driver/install.sh (spec, code not written yet)

Scope: `driver/install.sh` only, plus two small files next to the prebuilt module. No driver change, no reboot needed.

P3 (integrity): `driver/prebuilt/xgs_octep.ko.sha256` holds the pinned SHA-256 of the shipped module. Before `install -m 0555`, install.sh runs `/sbin/sha256 -q` on the prebuilt file and refuses (exit 1, nothing copied, nothing unloaded) when the pin file is missing or the hash differs. The pin must be refreshed with every build (added to the build recipe). Limit, stated plainly: a pin shipped beside the module catches corruption, a bad upload and a swapped file, not an attacker who can edit both; the real trust anchor is the signed-off commit on GitHub.

P8 (kernel match): `driver/prebuilt/xgs_octep.kver` holds the `__FreeBSD_version` the module was built against. install.sh compares it with `/sbin/sysctl -n kern.osreldate` and refuses on a mismatch, with a message naming both values. For the source-build path it reads `__FreeBSD_version` from `${SYSDIR}/sys/param.h` and compares the same way. No override variable (the old `XGS_KERN_VERSION` is a build-only define and is not an install bypass).

Test (user, on the box; hash-check the uploaded files first):
1. `/sbin/sysctl -n kern.osreldate` (needed to write the .kver file).
2. Normal run: `/bin/sh driver/install.sh`. PASS: prints `pin ok` and `kernel ok`, module installs, the usual "already loaded" message.
3. Negative P3: change one byte of the pin file copy, run again. PASS: refuses with "hash mismatch", `/boot/modules/xgs_octep.ko` unchanged (`sha256` before and after equal).
4. Negative P8: put a wrong number in the .kver copy, run again. PASS: refuses with both versions shown, module unchanged.
5. Restore both files, repeat step 2.
FAIL: any refusal path still replaces `/boot/modules/xgs_octep.ko`, or the normal run refuses.
Rollback: restore the previous driver/install.sh (the old copy stays on the box as install.sh.orig).

Result (22/11 18:10): PASS. Positive run printed `pin ok` and `kernel ok : 1600018`. Bad pin (0000) refused with `hash mismatch`, bad .kver (1600031) refused with `kernel mismatch`; /boot/modules/xgs_octep.ko stayed 79ee7dde both times. Files restored, positive run repeated OK. Note: a loaded, in-use module now ends with `kldunload: can't unload file: Device busy` (set -e stops there; the new .ko is already installed, reboot loads it). Not part of P3/P8; the old script showed `module already loaded` in the same situation.

## Tasks 4a, 4b, 5: test scripts (spec, scripts only, no module change)

Why: the three ways to get offload on (loader tunable at boot, runtime sysctl, live flow cut-in) were run by hand. The user needs scripts to run them alone and keep a log.

Files (driver/): `offload-ctl.sh on|off|status` (4b: live gate switch) and `offload-test.sh boot|sysctl|cutin|plain` (4a, 4b, 5). Both are root-only, use full paths, change nothing outside the gate and the ft sysctls, and write `/var/tmp/xgs-tests/<time>-<mode>.log`.

offload-ctl.sh: `on` / `off` write `dev.xgs_octep.0.offload`, wait 2 s, then print the new `offload=` and `fw_cfg=` from `alloc` and the last `firmware offload ON/OFF ... acknowledged` dmesg line (the visible proof the gate switched). `on` prints that a runtime switch gave about 366 Mbit/s, not the boot rate. `status` prints the same without changing anything.

offload-test.sh modes (the user runs iperf from the client; the script prompts):
- `boot`: gate must already be on from boot. `offload-check.sh start`, prompt, `stop` verdict.
- `sysctl`: `offload-ctl.sh off`, then `on` (switch evidence in the log), then as `boot`.
- `cutin`: needs `hw.xgs_octep.ft="1"` and the gate on. `start`; prompt (Enter when iperf starts); wait `delay` s (default 5); `ft_add=1`; refuse to continue claiming success if `programmed` did not rise; wait `hold` s (default 25); `ft_del=1`; `stop`; `ft:` dmesg lines and `alloc`. `DEBUG=1` sets `dp_debug=1` and resets it.
- `plain` (Task 5): records gate state and runs start/prompt/stop with no gate requirement, so a gate-off baseline can be compared with the other modes.

Test (user): upload both scripts, `/bin/sh /home/xgs-octeon-pfsense-ce/driver/offload-ctl.sh status`; `offload-test.sh boot` then iperf3 -B 10.4.4.2 -c 192.168.1.1 -t 30. PASS: log written, `OFFLOAD GATE: ON`, verdict printed, rate near 730. Then `cutin` (PASS: `programmed` rises, `ft:` line logged; the ~550-frame stall is the known open item, not a script fault) and `sysctl` (PASS: dmesg shows OFF then ON acknowledged). FAIL: a script leaves `ft_add` programmed after exit, or the gate in the wrong state.

## E4g: always settle after the RPC acknowledgement (spec, 2026-11-22, before code)

Evidence (dmesg, user box): ack 91 ms (20:41 boot) and 116 ms (22/11 boot) were both followed by `rpc: command 2 timed out` / `firmware offload ON ... FAILED`, then the retry (ack 7 ms) was acknowledged. Acks of 6, 7, 43 ms had no failure; acks of 267, 302, 359, 435 ms had none after E4a2 added the 500 ms settle (only when the ack took >= 200 ms). Without that settle 414 and 474 ms acks failed. So the failure is not tied to the ack time, and a threshold cannot cover it.
Rate evidence: the 22/11 boot ended via the retry and iperf measured about 367 Mbit/s (boot-ON via a clean first ack gave 732 and 728). The runtime sysctl toggle also gave about 366. Hypothesis, not proven: any gate applied after a failed or repeated send leaves the slower state.

Change (driver, `xo_rpc_configure`): remove the `i >= XO_RPC_SLOW_ACK_MS` condition so the 500 ms settle runs after every acknowledgement. No other change; the existing retry path stays. Cost: 500 ms at boot.

Test (user, offload stays on, boot-ON): upload, hash-check, `install.sh`, reboot. After each boot run `/sbin/dmesg | /usr/bin/grep -E "offload|rpc:"` and `/bin/sh /home/xgs-octeon-pfsense-ce/driver/offload-test.sh boot` with iperf3 -B 10.4.4.2 -c 192.168.1.1 -t 30. Do 5 boots.
PASS: every boot shows one `NPU acknowledged` then `firmware offload ON: fw_cfg=0x0003 acknowledged` (no `timed out`, no `FAILED`), and iperf is about 730 Mbit/s. A boot with an ack under 200 ms is the case under test; if none occurs in 5 boots the result is "not exercised".
FAIL: a `timed out` after the settle, a boot with the gate retried, or any panic. Rollback: previous build `79ee7dde` (E4d), recovery at the loader `set hw.xgs_octep.ifaces="0"`.
Build E4g: `0f86caa182cc2b76edb68a76d01dc40b99fc7ed719d5046e896a099939154f6b` (no warnings; E4d `79ee7dde` backed up in xgs-backups). Pins refreshed.
Open (separate): why a retried gate runs at half rate; checked only after this fix shows whether first-ack boots always give full rate.

### 4a/4b/5 test scripts: timed flow and invalid 367 run (22 Nov)
- `offload-test.sh` now prints READY, waits a window (default 40 s) and stops itself; `cutin` delay 8 s, hold 25 s. `sh -n` clean on all three scripts. Hashes: offload-test ec34b24c, offload-ctl 100e6cae, offload-check 5f44d34c.
- The ~367 Mbit/s boot run is INVALID: the iperf client's Intel adapter failed (firmware update, buffer errors, link loss, ARP MAC change for 10.4.4.2). The gate-retry theory is dropped for now. That boot did show `ON FAILED` then `acknowledged`.
- Retest: `offload-test.sh boot` with iperf running in the window. PASS: OFFLOAD GATE ON, NOT OFFLOADED verdict, ~730 Mbit/s.

### E4g boot 1 result and next test (22 Nov, 19:12-19:16)
- E4g `0f86caa1`: boot 1 had ack 125 ms, one gate send `acknowledged`, no `timed out`, no retry (E4g settle PASS on 1 boot; 4 boots to go).
- Client adapter fixed (duplicate address on the host). iperf 10.4.4.2 -> 192.168.1.1, 30 s: gate ON 367 Mbit/s (0 retr, dips at 10 s and 20 s); after `offload-ctl.sh off` (offload=0 fw_cfg=0x0002) 732 Mbit/s, 0 retr (logs_pics/iperf/221126_19_12_iperf_offload_on_low.txt, 221126_19_16_iperf_offload_OFF.txt). Same host, path and boot, so the half rate is real and tied to gate ON, not to a retry or the client.
- Pre-E4d boots with gate ON gave 732 and 728. Difference since then: E4d code and `hw.xgs_octep.ft="1"` in loader.conf.local.
- Next test (config only, no code change): set `hw.xgs_octep.ft="0"` in /boot/loader.conf.local, reboot, `offload-test.sh boot 40` with iperf. PASS = ~730 with gate ON (ft is the cause); FAIL = ~367 (E4d/E4g code or gate, not ft). Restore `ft="1"` afterwards.

### E4g boot 2 and the ack-time correlation (22 Nov, 19:30)
- Boot 2 (startup.sh in as a service, ft=1, E4g): ack 352 ms, one clean send, iperf 732 Mbit/s, 0 retr (221126_19_30_iperf_offload_ON_AFTER_BOOT_WITH_STARTSCRIPT_IN.txt). `ifconfig octep0` shows `options=80000<LINKSTATE>` only: the driver exposes no TSO/LRO/csum caps and xo_if_ioctl ignores SIOCSIFCAP, so startup.sh cannot change octep behaviour.
- Pairing ack time with rate: 116 ms (retry) 367; 125 ms (clean) 367; 352 ms 732; 267/359/435 ms ~730. Fast acks (< ~200 ms) coincide with the half-rate boots, with or without a retry; the 500 ms settle fixed the timeout but not the rate. n is small. Hypothesis: a fast ack means the NPU came up in a different state. Next: more boots, record ack ms and rate each; no code change until the pattern holds.
- Boot 3 (startup.sh OUT, warm reboot, ft=1, E4g): ack 441 ms, clean, iperf 729 Mbit/s, 0 retr, counter[59] +3687294, WIRE_TO_WIRE +0 (221126_19_41_iperf_offload_ON_AFTER_BOOT_WITH_STARTSCRIPT_OUT.txt). With boot 2 (script in, 732) this shows the script does not change the rate; the ack-time pairing (fast = 367, slow = ~730) stands at 2 fast / 5 slow boots. Still to do: a fast-ack boot with the script out, and two cold shutdown boots.
- Cold boot 1 (power cut and unplugged, script out): ack 447 ms, clean, iperf 732 Mbit/s, 0 retr, counter[59] +3786993, WIRE_TO_WIRE +0 (221126_20_17_iperf_offload_ON_COLD_BOOT_WITH_STARTSCRIPT_OUT.txt). Pairing now 2 fast / 6 slow. Cold start gave a slow ack, consistent with fast acks occurring only on warm reboots where the NPU is already running.
- Warm boot 4 (script out): ack 193 ms, clean, 732 Mbit/s, 0 retr (221126_20_23_iperf_offload_ON_WARM_BOOT_WITH_STARTSCRIPT_OUT.txt). Boundary so far: <= 125 ms gives 367, >= 193 ms gives ~730. Fast-ack boots cannot be forced; parked as an open item (Task 6 / NPU state) and the test scripts move on.

### Cut-in stall trace (22 Nov, 20:35) - spec, script only, no driver change
- Evidence: after `ft_add` iperf collapses to 0 in about 1 s. Cumulative `accel` after the 20:27 cut-in: WIRE_TO_WIRE[2]=514 (one TCP window), no TCP-seq/MFLOW_ACTION/MTU/retrans drop counters moved, [52]=1 and [58]=1 from `ft_del`. The NPU does not drop or punt the stalled traffic, so the TCP state block is unlikely. Lead: frames forwarded but out the wrong port or tag (NHOP egress iface = port + 2).
- Change: `offload-test.sh cutin` gains a passive trace, on by default (`TRACE=0` disables). Before `ft_add`: `accel`, `netstat -I octep0/octep1`, and `tcpdump -s 64 -c 20000 tcp port 5201` on octep0 and octep1, started 1 s before `ft_add`, to `/var/tmp/xgs-tests/<stamp>-octepN.pcap`. After `ft_add`: one sample per second for 8 s of `accel` (nonzero counters), `flows`, and octep0/octep1 packet counters. At the end a counter diff (after minus before). Only the newest 4 pcaps are kept.
- PASS (script works): trace block present in the log and the diff printed. The diagnostic answer: which of [2] WIRE_TO_WIRE, [3] RX_KN, [12], [40], [59] and any drop counter move per second after cut-in, and whether octep1 host tx/rx stop at the same second as iperf (frames offloaded) or octep0 keeps seeing frames (not offloaded). A WAN-side capture at the server decides the wrong-port theory.
- Module debug: cut-in now defaults `DEBUG=1` (`dp_debug` sysctl, existing driver monitor) and prints the last 40 `dp-debug` dmesg lines at the end: per-second `npu counters moved`, `stall start/end`, `errors rx_err/tx_drop/tx_full`, 10 s rx/tx rates.
- Time deltas: samples every 0.25 s for 10 s after `ft_add`, each stamped `+Nms` from T0 (just before `ft_add`); `ft_add` duration printed. pcap timestamps give frame-level timing for the same T0 (printed wall time). The stall moment = first sample where WIRE_TO_WIRE[2] stops rising.
- Fix: tcpdump path was /sbin (Linux); FreeBSD is /usr/sbin/tcpdump. First cut-in trace run (20:35) produced no pcaps. stderr now kept in <stamp>-octepN.err and printed.

### Offload OFF at boot must still create interfaces (23 Nov) - spec, driver change (E4h)
- Evidence: status page set `hw.xgs_octep.offload="0"` and rebooted. Result: `offload=0 fw_cfg=0 ifaces=0`, `ifconfig -l` has no octep*, boot held the full 300 s (`still waiting ... for xo_hold_noop` at 120/180 s), then pfSense prompted repeatedly for WAN/LAN assignment. `loader.conf.local` was intact (ifaces="1", offload="0"). Cause: `xo_if_poll` requires `sc->offload_on` before `xo_ifaces_create`, so with offload off the interfaces never appear.
- Change: create interfaces when `offload_on == want_offload` (the gate has reached what was asked for, on or off) instead of requiring `offload_on`. Offload ON behaviour is unchanged (still waits for the gate ack). Only `xo_if_poll`'s condition changes.
- Test spec: (1) offload="0" boot: PASS `offload=0 ports_up=12 ifaces=12`, octep0..11 in `ifconfig -l`, boot hold ends within ~3 min, no WAN/LAN prompt, saved assignments retained. (2) offload="1" boot (regression): `offload=1 fw_cfg=0x0003 ifaces=12`, no prompt, same as E4g. FAIL: any prompt, panic, or `ifaces=0`.
- Built: `.ko` sha256 `87b10611a1e952e11560159f7041cc6605fd73f17055251ec06d256ceeb3640c`, kver 1600018, pins refreshed, E4g backed up. Needs: refresh `.ko.sha256` and `.ko.kver`, back up E4g `.ko` (`0f86caa1`), upload, install, reboot.

### Task 10 - NPU status and testing page (23 Nov) - spec, PHP/scripts only, no driver change. Accel dev paused today.
- Problem: `xgs_octep_status.php` loads but no values update (user report, 23 Nov). Cause not yet known; first step is a diagnostic block on the page (raw `sysctl` output and exit code, `id`, loaded state) to separate "command fails under the web server" from "parsing wrong". Fix the current page first; nothing else lands before it shows live values.
- (f) Rename: project folder `www/` becomes `npu-status-testing/` (no spaces in the path); `install.sh`, README paths and the uninstall line follow. Installed URL stays `/xgs_octep_status.php`. Same rename applied in the GitHub repo.
- (a) New "Testing" section below Status. One button per `offload-test.sh` mode (boot, sysctl, cutin, plain) and an iperf3 start switch (local test or via WAN, client `-B` address and server fields, duration), each with a one-line description. Buttons run the existing scripts through `/bin/sh`; log path shown, output tailed on the page.
- (b) Options: pcap file name (written under `/home/`, snaplen and size cap, newest 4 kept) and switches for offload ON/OFF live (`offload-ctl.sh on|off`).
- (c) Offload-at-boot switch: sets/clears `hw.xgs_octep.offload` in `/boot/loader.conf.local` (takes effect next boot) and a live ON/OFF switch; the page shows the current boot value and the live gate (`fw_cfg`). Note shown: the offload ON at boot is the required state for tests.
- (d) Stats while testing: offloaded packets (`WIRE_TO_WIRE`, `TX_WIRE` deltas from `accel`) vs not offloaded (`TO_KN_*`, `RX_KN`), brief, refreshed on a timer.
- (e) View switch for iperf / flow rate: Mbit/s and pkts/s from `netstat -I octep0/octep1` deltas and `accel` deltas, for both offload-on and offload-off runs.
- Built (23 Nov): page rewritten in `npu-status-testing/`, `install.sh` writes `/usr/local/etc/xgs_octep.path`, `offload-test.sh` gains `PCAP_DIR`/`PCAP_NAME` env, folder README added. Driver keeps no byte counters (netstat bytes = 0), so Mbit/s on the page is an estimate (1500 B/frame); the measured figure is the iperf result. `php -l` and `sh -n` clean; not yet tested on the box.
- Fix (23 Nov): `driver/install.sh` aborted at `kldunload ... Device busy` (set -e), so the top-level `install.sh` never copied the rc.d hook, status page or path file. A busy unload now prints "takes effect after a reboot" and continues.
- Added (23 Nov): top-of-page OFFLOAD indicator, green ON / red OFF from the live gate (`offload=1` and `fw_cfg=0x3`), with the boot value beside it. Main section carries 1 boot ON / 0 boot OFF buttons; the Testing section keeps its four offload switches.
- Added (23 Nov): one-line OCTEP health under the offload banner: HEALTHY (ports up = ports, interfaces created, rpc ready, no errors) or NOT HEALTHY with the reasons; an unreadable `status` sysctl shows as NOT HEALTHY with its rc, so empty values are no longer silent.
- Spec (23 Nov, user "go" 08:02) live flow indicators, page JS only, no driver change: refresh 1 s. Per row (offloaded, not offloaded, octep0, octep1) add Max pkts/s and Min pkts/s (min over seconds with traffic, ignoring idle zeros) since Reset. A FLOW state line: IDLE (no packets), HOST (traffic only on the host path), OFFLOADED (W2W pkts/s > 0), CUT-IN seen (time of the first second W2W went from 0 to >0, shown as seconds since the first traffic). STALL indicator: if total traffic (offloaded + not offloaded + octep) was >0 and drops to 0 for 2+ s, show STALLED and count seconds; keep "stalls" (count) and "longest stall (s)". Reset counts clears all of it. Driver `stalls`/`stall_max_s` from `status` shown beside it. PASS: during iperf the values move each second; cut-in time and Max pkts/s are shown; a gap in traffic shows STALLED and the longest stall.
- Spec (23 Nov, user request 08:05) iperf source select, page PHP only: the iperf3 client gets a "from" select: octep0 (LAN), octep1 (WAN) or custom IP, and the server field defaults to 192.168.1.1. The source address is read from `ifconfig <if> inet` (first IPv4) and passed as `-B`; an interface with no IPv4 is rejected with a message. Server and custom address must still be valid IPs, seconds 1-300. Note shown: iperf started on this box is host-originated traffic and is not forwarded, so it is not offloaded; the offload test is iperf from the LAN client through the box. PASS: choosing octep0 starts `iperf3 -B <octep0 IP> -c 192.168.1.1 -t N` with a log under `/var/tmp/xgs-tests/`; octep1 uses the WAN IP; a bad address is rejected.
- Safety: the page only calls fixed scripts with validated arguments (name charset, numeric ranges, no raw shell strings), requires the page privilege `page-status-xgs-npu`, and uses the pfSense CSRF token on every POST. Test buttons refuse to run when the gate is not ON (rule: no test without verified offload), except the explicit offload-off baseline.
- PASS: (1) page shows live `status` values, refreshing; (2) each button runs its script and shows its log; (3) boot switch edits loader.conf.local and shows the new value; (4) during an iperf run the offloaded/not offloaded and Mbit/s figures move; (5) bad input (path traversal, shell metacharacters) is rejected. Not pushed until the user approves the CHANGELOG and the page.
- Result (23 Nov): E4h PASS on the box (module `87b10611...`): boot with offload OFF gives `offload=0 fw_cfg=0 ports_up=12 ifaces=12`, interfaces created, hold released. The earlier failure (E4g) was `ifaces=0` and a 300 s hold followed by the WAN/LAN prompt, which also lost the pfSense assignments (re-set in the GUI). The page was installed (`99e3e2e4...`), then updated with the live Flow line (max/min pkts/s, STALLED, cut-in seen) and the iperf source select (`b4091650...`); both installed OK. Still to test on the box: regression boot with offload ON, cut-in and iperf from the page, and whether the assignments survive a reboot.
