#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 turtle2a <354865938+turtle2a@users.noreply.github.com>
# Guided offload tests; you run iperf3 from the client when asked.
#   offload-test.sh boot [secs]     gate on from boot (default 40 s window)
#   offload-test.sh sysctl [secs]   gate switched off then on at run time
#   offload-test.sh cutin [delay] [hold]
#                                   program one live flow (needs hw.xgs_octep.ft=1);
#                                   cut in at READY+delay (default 8), hold 25 s
#   offload-test.sh plain [secs]    no gate requirement, for a baseline
# The script prints READY; start iperf3 on the client within a few seconds.
# It stops itself, no keypress needed. DEBUG=1 turns dp_debug on for the run.
# Log: /var/tmp/xgs-tests/
# Run as root.
HERE=$(cd "$(dirname "$0")" && pwd)
D=dev.xgs_octep.0
S=/sbin/sysctl
CHECK="/bin/sh $HERE/offload-check.sh"
CTL="/bin/sh $HERE/offload-ctl.sh"

alloc_val() {
	$S -n $D.alloc | /usr/bin/tr ' ' '\n' | /usr/bin/grep "^$1=" | /usr/bin/cut -d= -f2
}

debug_on() {
	[ "${DEBUG:-0}" = 1 ] && $S $D.dp_debug=1
}

debug_off() {
	[ "${DEBUG:-0}" = 1 ] && $S $D.dp_debug=0
}

ready() {
	echo "READY $(/bin/date +%T): start iperf3 -B 10.4.4.2 -c 192.168.1.1 -t 30 on the client within a few seconds"
}

run_boot() {
	secs=${1:-40}
	$CHECK start || exit 1
	debug_on
	ready
	echo "window ${secs} s"
	/bin/sleep "$secs"
	debug_off
	$CHECK stop
}

run_sysctl() {
	$CTL off || exit 1
	$CTL on || exit 1
	run_boot "$@"
}

ifstat() {
	/usr/bin/netstat -I "$1" -b -n | /usr/bin/awk 'NR==2 {print "'"$1"' ipkts=" $5 " ibytes=" $7 " opkts=" $8 " obytes=" $10}'
}

trace_start() {
	[ "${TRACE:-1}" = 1 ] || return 0
	echo "trace before: $($S -n $D.accel | /usr/bin/tr '\n' ' ')"
	ifstat octep0
	ifstat octep1
}

trace_pcap() {
	[ "${TRACE:-1}" = 1 ] || return 0
	st=${PCAP_NAME:+$PCAP_NAME-cutin}
	st=${st:-$(/bin/date +%Y%m%d-%H%M%S)}
	PD=${PCAP_DIR:-$LOGDIR}
	/bin/mkdir -p "$PD"
	pids=
	for i in octep0 octep1; do
		/usr/sbin/tcpdump -n -p -U -s 64 -c 20000 -i $i -w $PD/$st-$i.pcap tcp port 5201 2>$PD/$st-$i.err &
		pids="$pids $!"
	done
	/bin/ls -t $PD/*.pcap 2>/dev/null | /usr/bin/tail -n +5 | /usr/bin/xargs -r /bin/rm -f
	/bin/sleep 1
}

now() {
	/bin/date +%s.%N
}

trace_sample() {
	[ "${TRACE:-1}" = 1 ] || { /bin/sleep 2; return 0; }
	n=0
	while [ $n -lt 40 ]; do
		n=$((n + 1))
		/bin/sleep 0.25
		t=$(now)
		echo "trace +$(/usr/bin/awk -v a="$t" -v b="$T0" 'BEGIN {printf "%.0f", (a - b) * 1000}')ms: $($S -n $D.accel | /usr/bin/tr '\n' ' ')"
		echo "trace +$(/usr/bin/awk -v a="$t" -v b="$T0" 'BEGIN {printf "%.0f", (a - b) * 1000}')ms: $(ifstat octep0) | $(ifstat octep1)"
	done
	echo "trace flows: $($S -n $D.flows | /usr/bin/tr '\n' ' ')"
}

trace_stop() {
	[ "${TRACE:-1}" = 1 ] || return 0
	[ -n "$pids" ] && kill $pids 2>/dev/null
	echo "trace pcaps:"
	/bin/ls -l $PD/$st-* 2>&1
	/bin/cat $PD/$st-*.err 2>/dev/null
}

run_cutin() {
	DEBUG=${DEBUG:-1}
	delay=${1:-8}
	hold=${2:-25}
	[ "$($S -n hw.xgs_octep.ft 2>/dev/null)" = 1 ] ||
		{ echo "hw.xgs_octep.ft is not 1; set it in /boot/loader.conf.local and reboot" >&2; exit 1; }
	$CHECK start || exit 1
	debug_on
	p0=$(alloc_val programmed)
	echo "alloc before: $($S -n $D.alloc)"
	ready
	echo "cut-in at +${delay} s, hold ${hold} s"
	trace_start
	/bin/sleep "$delay"
	trace_pcap
	T0=$(now)
	echo "ft_add at $(/bin/date +%T.%N) (T0)"
	$S $D.ft_add=1
	echo "ft_add returned +$(/usr/bin/awk -v a="$(now)" -v b="$T0" 'BEGIN {printf "%.0f", (a - b) * 1000}')ms"
	trace_sample
	p1=$(alloc_val programmed)
	echo "alloc after cut-in: $($S -n $D.alloc)"
	if [ "${p1:-0}" -gt "${p0:-0}" ]; then
		echo "CUT-IN: flow programmed"
	else
		echo "CUT-IN FAILED: programmed did not rise (see ft: lines below)"
	fi
	echo "holding ${hold} s"
	/bin/sleep "$hold"
	trace_stop
	$S $D.ft_del=1
	/bin/sleep 1
	debug_off
	$CHECK stop
	echo "ft lines:"
	/sbin/dmesg | /usr/bin/grep "ft:" | /usr/bin/grep -v " dbg " | /usr/bin/tail -8
	echo "dp-debug lines:"
	/sbin/dmesg | /usr/bin/grep "dp-debug" | /usr/bin/tail -40
	echo "alloc final: $($S -n $D.alloc)"
}

run_plain() {
	secs=${1:-40}
	echo "gate: offload=$(alloc_val offload) fw_cfg=$(alloc_val fw_cfg) (baseline, not required on)"
	$CHECK start
	debug_on
	ready
	echo "window ${secs} s"
	/bin/sleep "$secs"
	debug_off
	$CHECK stop
}

[ "$(/usr/bin/id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
mode=$1
case "$mode" in
boot | sysctl | cutin | plain) ;;
*) echo "usage: $0 boot|sysctl|cutin [delay] [hold]|plain" >&2; exit 2 ;;
esac
shift
LOGDIR=/var/tmp/xgs-tests
/bin/mkdir -p $LOGDIR
LOG=$LOGDIR/$(/bin/date +%Y%m%d-%H%M%S)-$mode.log
echo "log: $LOG"
{
	echo "mode $mode  $(/bin/date)"
	run_$mode "$@"
} 2>&1 | /usr/bin/tee "$LOG"
