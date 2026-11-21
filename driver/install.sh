#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(dirname "${HERE}")
BOOT=0
[ "${1:-}" = "--boot" ] && BOOT=1

if [ -z "${SYSDIR:-}" ]; then
	for d in "${TOP}"/src/*/sys; do
		[ -f "${d}/sys/param.h" ] && SYSDIR=${d} && break
	done
fi
: ${SYSDIR:=/usr/src/sys}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
PRE=${HERE}/prebuilt/xgs_octep.ko
if [ ! -f "${PRE}" ]; then
	[ -f "${SYSDIR}/sys/param.h" ] || { echo "no kernel sources at ${SYSDIR}; run: sh ${TOP}/fetch-sources.sh" >&2; exit 1; }
fi
/usr/sbin/pciconf -l | grep -qE 'chip=0xa300177d|vendor=0x177d[[:space:]]+device=0xa300' ||
	{ echo "no OCTEON TX (177d:a300) found; this is for the XGS 2300" >&2; exit 1; }
echo "kernel : $(uname -v)"
install -d /boot/modules
if [ -f "${PRE}" ]; then
	echo "module : prebuilt ${PRE}"
	install -m 0555 "${PRE}" /boot/modules/xgs_octep.ko
else
	command -v make >/dev/null && command -v cc >/dev/null ||
		{ echo "make and cc are needed to build; put a built xgs_octep.ko in ${HERE}/prebuilt/" >&2; exit 1; }
	echo "sources: ${SYSDIR}"
	WORK=$(mktemp -d)
	trap 'rm -rf "${WORK}"' EXIT
	cp "${HERE}/xgs_octep.c" "${HERE}/Makefile" "${WORK}/"
	(cd "${WORK}" && make SYSDIR="${SYSDIR}")
	install -m 0555 "${WORK}/xgs_octep.ko" /boot/modules/xgs_octep.ko
fi

/sbin/kldstat -q -n xgs_octep && /sbin/kldunload xgs_octep
/sbin/kldload xgs_octep

if [ "${BOOT}" -eq 1 ]; then
	LC=/boot/loader.conf.local
	touch "${LC}"
	grep -q '^xgs_octep_load=' "${LC}" || echo 'xgs_octep_load="YES"' >> "${LC}"
	grep -q '^hw.xgs_octep.autostart=' "${LC}" || echo 'hw.xgs_octep.autostart="1"' >> "${LC}"
	grep -q '^hw.xgs_octep.ifaces=' "${LC}" || echo 'hw.xgs_octep.ifaces="1"' >> "${LC}"
	grep -q '^hw.xgs_octep.tx=' "${LC}" || echo 'hw.xgs_octep.tx="1"' >> "${LC}"
fi

echo "loaded. The NPU can take minutes after boot to answer; check with:"
echo "  /sbin/sysctl dev.xgs_octep.0.status"
echo "  /sbin/dmesg | grep xgs_octep"
