#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
TOP=$(cd "$(dirname "$0")" && pwd)

sh "${TOP}/driver/install.sh" --boot
install -m 0555 "${TOP}/rc.d/xgs_octep" /usr/local/etc/rc.d/xgs_octep
echo "boot hook installed: /usr/local/etc/rc.d/xgs_octep"
install -m 0644 "${TOP}/npu-status-testing/xgs_octep_status.php" /usr/local/www/xgs_octep_status.php
echo "${TOP}" > /usr/local/etc/xgs_octep.path
chmod +x "${TOP}"/driver/*.sh "${TOP}"/install.sh 2>/dev/null || true
echo "status page: https://<firewall>/xgs_octep_status.php (scripts from ${TOP}/driver)"
