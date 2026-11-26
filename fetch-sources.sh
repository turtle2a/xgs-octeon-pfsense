#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later AND BSD-2-Clause
# Copyright (c) 2026 turtle2a <354865938+turtle2a@users.noreply.github.com>
# Portions derived from os-xgs-npu (https://github.com/AbdelmonemAwad/os-xgs-npu), Copyright (c) 2026 Abdelmonem Awad, BSD-2-Clause
# Fetch the pfSense kernel sources (pfsense/FreeBSD-src on GitHub) that match the running kernel,
# writing only sys/. Prints the SYSDIR to use with install.sh.
#
#   sh fetch-sources.sh            branch taken from `uname -v` (a RELENG_x_y_z token)
#   sh fetch-sources.sh RELENG_2_7_2   or any branch, tag or commit of the repo
#   REPO=freebsd/freebsd-src sh fetch-sources.sh releng/15.2   upstream FreeBSD instead
#
# Default repo is pfsense/FreeBSD-src, whose public branches are RELENG_2_2 .. RELENG_2_7_2 and
# devel-main. A newer pfSense CE kernel (FreeBSD 15 based) has no branch there: use upstream
# FreeBSD (releng/15.x or stable/15) or the commit named in `uname -v`. Building against sources
# that differ from the running kernel is luck rather than correctness, so check `uname -v`.
set -eu

REPO=${REPO:-pfsense/FreeBSD-src}
REF=${1:-}
if [ -z "${REF}" ]; then
	REF=$(uname -v | tr ' ' '\n' | sed -n 's/^\(RELENG_[0-9_]*\).*/\1/p' | head -1)
fi
if [ -z "${REF}" ]; then
	echo "no RELENG_ token in: $(uname -v)" >&2
	echo "pass the branch or commit by hand: [REPO=freebsd/freebsd-src] sh fetch-sources.sh <ref>" >&2
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
DST=${DST:-${HERE}/src/$(echo "${REPO}-${REF}" | tr '/' '-')}
if [ -f "${DST}/sys/sys/param.h" ]; then
	echo "already there."
	echo "SYSDIR=${DST}/sys"
	exit 0
fi

URL="https://codeload.github.com/${REPO}/tar.gz/${REF}"
echo "kernel      : $(uname -v)"
echo "source ref  : ${REF}"
echo "destination : ${DST}"
echo "fetching (the whole tree comes down, only sys/ is written)..."

rm -rf "${DST}.partial"
mkdir -p "${DST}.partial"
if command -v fetch >/dev/null 2>&1; then
	DL="fetch -q -o -"
else
	DL="curl -fsSL"
fi
if ! ${DL} "${URL}" | tar -xzf - -C "${DST}.partial" --strip-components=1 '*/sys/*'; then
	rm -rf "${DST}.partial"
	echo "fetch failed: ${URL}" >&2
	exit 1
fi
if [ ! -f "${DST}.partial/sys/sys/param.h" ]; then
	rm -rf "${DST}.partial"
	echo "the archive has no sys/sys/param.h; wrong ref?" >&2
	exit 1
fi
mv "${DST}.partial" "${DST}"
echo
echo "SYSDIR=${DST}/sys"
