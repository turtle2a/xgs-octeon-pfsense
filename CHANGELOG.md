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
