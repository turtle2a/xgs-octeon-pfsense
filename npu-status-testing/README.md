# NPU status and testing page

`xgs_octep_status.php` is a pfSense page (URL `/xgs_octep_status.php`, privilege "Status: XGS NPU"). `install.sh` copies it to `/usr/local/www/` and writes the project path to `/usr/local/etc/xgs_octep.path`, so the page finds `driver/offload-test.sh` and `driver/offload-ctl.sh`.

Sections:

- **Coprocessor**: driver, NPU, ports, live offload gate (`fw_cfg`), offload-at-boot value, last error. Values come from `sysctl dev.xgs_octep.0.status`.
- **Diagnostics**: the user the page runs as and the raw `status` output, to see why values might be empty.
- **Testing**:
  1. Pcap name and **Start pcap / Stop pcap**: independent of the offload gate and of any test. A capture runs `tcpdump --immediate-mode -U -s 96` (tcp port 5201) on octep0 and octep1 into `/home/pcaps/NAME-octepN.pcap` (blank name = date-time; folder created automatically; newest 4 files kept). It stops by itself after 120 s, or the iperf seconds + 10 for a page-started client, and has a 1,000,000 packet cap. With a name entered, a capture also arms automatically on live ON, Start client, Local server and cutin, and stops on Stop iperf and live OFF. If a capture fails to start, the page message says why (log: `/var/tmp/xgs-tests/pcap-octepN.log`). The name and the iperf fields are remembered across reloads.
  2. Offload switches: live ON/OFF (`offload-ctl.sh`) and boot ON/OFF (`hw.xgs_octep.offload` in `/boot/loader.conf.local`, applies next boot).
  3. Tests: `boot`, `sysctl`, `cutin`, `plain` from `offload-test.sh`. All except `plain` refuse to run unless the gate is ON. Cut-in captures are written as `NAME-cutin-octepN.pcap` in the same folder, so they never overwrite the page captures.
  4. iperf3: client (source octep0, octep1 or a custom IP, to a server, 1-300 s), a local one-shot server, and **Stop iperf**. Needs `iperf3` on the box. Box-originated traffic is not forwarded, so it is never offloaded; for a real offload test run iperf from a LAN host.
- **Live packet summary**: offloaded and not-offloaded packet counts, packets/s with per-row max and min, and estimated Mbit/s (1500 B per frame; the driver keeps no byte counters), refreshed every second. A flow line shows IDLE, HOST, OFFLOADED or STALLED, whether a cut-in was seen, the stall count and the longest stall (a stall is a 2 to 10 s gap, counted when traffic resumes). Values persist across reloads; **Reset counts** clears them.
- **Recent logs**: the newest 6 files from `/var/tmp/xgs-tests/`.

All inputs are validated (names, IP addresses, ranges) and POSTs carry the pfSense CSRF token. Status: under test.
