#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 turtle2a <354865938+turtle2a@users.noreply.github.com>
# Read-only check of NPU flow offload, and a before/after counter comparison.
#   offload-check.sh            check the gate (run before every test)
#   offload-check.sh start      save a counter snapshot
#   offload-check.sh stop       compare with the snapshot and print a verdict
# Changes nothing on the NPU. Run as root.
D=dev.xgs_octep.0
SNAP=/tmp/xgs_offload_snap
SYSCTL=/sbin/sysctl

status_val() {
	$SYSCTL -n $D.status | /usr/bin/tr ' ' '\n' | /usr/bin/grep "^$1=" | /usr/bin/cut -d= -f2
}

counters() {
	$SYSCTL -n $D.accel | /usr/bin/grep -E '^[0-9]+=[0-9]+$'
}

cnt() {
	echo "$1" | /usr/bin/grep "^$2=" | /usr/bin/cut -d= -f2
}

check() {
	bad=0
	t=$($SYSCTL -n hw.xgs_octep.offload 2>/dev/null)
	[ "$t" = 1 ] && echo "ok    loader tunable offload=1" || { echo "FAIL  loader tunable offload=$t"; bad=1; }
	[ "$(status_val offload)" = 1 ] && echo "ok    firmware gate offload=1 fw_cfg=$(status_val fw_cfg)" || { echo "FAIL  firmware gate is off"; bad=1; }
	[ "$(status_val rpc_ready)" = 1 ] && echo "ok    rpc_ready=1" || { echo "FAIL  rpc not ready"; bad=1; }
	[ "$(status_val ports_up)" = 12 ] && echo "ok    ports_up=12" || { echo "FAIL  ports_up=$(status_val ports_up)"; bad=1; }
	if [ "$bad" = 0 ]; then
		echo "OFFLOAD GATE: ON"
		return 0
	fi
	echo "OFFLOAD GATE: OFF - do not start a test"
	return 1
}

start() {
	check || exit 1
	{
		echo "rx_pkts=$(status_val rx_pkts)"
		echo "tx_pkts=$(status_val tx_pkts)"
		counters
	} > $SNAP
	echo "snapshot saved; run the test, then: $0 stop"
}

stop() {
	[ -r $SNAP ] || { echo "no snapshot; run: $0 start"; exit 1; }
	old=$(cat $SNAP)
	new="rx_pkts=$(status_val rx_pkts)
tx_pkts=$(status_val tx_pkts)
$(counters)"
	delta() {
		a=$(cnt "$old" "$1"); b=$(cnt "$new" "$1")
		echo $(( ${b:-0} - ${a:-0} ))
	}
	hrx=$(delta rx_pkts); htx=$(delta tx_pkts)
	rxw=$(delta 0); txw=$(delta 1); w2w=$(delta 2); rxkn=$(delta 3)
	echo "host rx_pkts +$hrx  tx_pkts +$htx"
	echo "NPU  RX_WIRE[0] +$rxw  TX_WIRE[1] +$txw  WIRE_TO_WIRE[2] +$w2w  RX_KN[3] +$rxkn"
	echo "punts to host:"
	for i in 39 40 42 43 52 53 59; do
		v=$(delta $i)
		[ "$v" != 0 ] && echo "  counter[$i] +$v"
	done
	c39=$(delta 39); c59=$(delta 59)
	if [ "$c59" -gt 0 ] && [ "$c39" -le 0 ]; then
		echo "gate evidence: frames counted in [59] MFLOW_NOT_ACTIVE (gate ON classification)"
	elif [ "$c39" -gt 0 ] && [ "$c59" -le 0 ]; then
		echo "gate evidence: frames counted in [39] (gate OFF classification)"
	fi
	if [ "$w2w" -le 0 ]; then
		echo "VERDICT: NOT OFFLOADED (WIRE_TO_WIRE did not move; traffic went through the host)"
	elif [ "$rxw" -gt 0 ] && [ $(( w2w * 10 )) -ge $(( rxw * 8 )) ]; then
		echo "VERDICT: OFFLOADED (WIRE_TO_WIRE +$w2w is at least 80% of RX_WIRE +$rxw)"
	else
		echo "VERDICT: PARTIAL (WIRE_TO_WIRE +$w2w of RX_WIRE +$rxw)"
	fi
}

case "$1" in
start) start ;;
stop) stop ;;
"") check ;;
*) echo "usage: $0 [start|stop]"; exit 2 ;;
esac
