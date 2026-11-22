# NPU status and testing page

`xgs_octep_status.php` is a pfSense page (URL `/xgs_octep_status.php`, privilege "Status: XGS NPU"). `install.sh` copies it to `/usr/local/www/` and writes the project path to `/usr/local/etc/xgs_octep.path`, so the page finds `driver/offload-test.sh` and `driver/offload-ctl.sh`.

Sections:

- **Coprocessor**: driver, NPU, ports, live offload gate (`fw_cfg`), offload-at-boot value, last error. Values come from `sysctl dev.xgs_octep.0.status`.
- **Diagnostics**: the user the page runs as and the raw `status` output, to see why values might be empty.
- **Testing**:
  1. Offload switches: live ON/OFF (`offload-ctl.sh`) and boot ON/OFF (`hw.xgs_octep.offload` in `/boot/loader.conf.local`, applies next boot).
  2. Tests: `boot`, `sysctl`, `cutin`, `plain` from `offload-test.sh`. All except `plain` refuse to run unless the gate is ON. Optional pcap file name (written to `/home/`).
  3. iperf3: client (box to a WAN server, 1-300 s) or a local one-shot server. Needs `iperf3` on the box.
- **Live packet summary**: offloaded and not-offloaded packet counts, packets/s and estimated Mbit/s (1500 B per frame; the driver keeps no byte counters), refreshed every 2 s. The measured rate is the iperf result.
- **Recent logs**: the newest 6 files from `/var/tmp/xgs-tests/`.

All inputs are validated (names, IP addresses, ranges) and POSTs carry the pfSense CSRF token. Status: under test.
