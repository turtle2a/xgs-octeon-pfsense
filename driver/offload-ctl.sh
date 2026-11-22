#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Switch the NPU firmware offload gate at run time and show proof it switched.
#   offload-ctl.sh on | off | status
# Run as root. A runtime switch is not the same as offload on at boot.
D=dev.xgs_octep.0
S=/sbin/sysctl

alloc_val() {
	$S -n $D.alloc | /usr/bin/tr ' ' '\n' | /usr/bin/grep "^$1=" | /usr/bin/cut -d= -f2
}

show() {
	echo "gate now: offload=$(alloc_val offload) fw_cfg=$(alloc_val fw_cfg)"
	echo "last switch in dmesg:"
	/sbin/dmesg | /usr/bin/grep "firmware offload" | /usr/bin/tail -2
}

case "$1" in
on | off)
	[ "$(/usr/bin/id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
	v=0
	[ "$1" = on ] && v=1
	$S $D.offload=$v || exit 1
	/bin/sleep 2
	show
	[ "$1" = on ] && echo "note: a runtime ON measured about 366 Mbit/s; offload on at boot gives line rate"
	;;
status | "")
	show
	;;
*)
	echo "usage: $0 on|off|status" >&2
	exit 2
	;;
esac
