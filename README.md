# XGS OCTEON pfSense CE port

FreeBSD kernel driver (`xgs_octep`) for the OCTEON TX NPU (PCI `177d:a300`) in
the Sophos XGS 2300, so pfSense CE can use the front ports as `octepN`
interfaces. The driver is rewritten from our own Linux OCTEP driver
(GPL-2.0-or-later). You still configure NAT, firewall and routing yourself, as
on any pfSense install (see TODO).

## Status

| Item | State |
|---|---|
| Boot, NPU handshake, port discovery | verified |
| `octep0..11` interfaces, RX, TX, ping, NAT | working (Port1 verified by capture) |
| Firmware offload gate on at boot (`offload=1 fw_cfg=0x3`) | on, verified |
| iperf to 192.168.1.1, no accel, 0 retries | OK (user-run, Task 5) |
| Flow offload / acceleration | **NOT validated** (Task 6) |
| SFP cages / 10G ports | not tested (Task 7) |
| Real link state and media on `octepN` | not yet (media is fixed, status always active) |
| Status page `xgs_octep_status.php` | not working yet (Task 10) |

## Hardware and image

- XGS 2300 (AMD Ryzen Embedded R1606G + OCTEON TX NPU).
- pfSense CE 2.9.2 (FreeBSD 16.0-CURRENT), installed from the **serial image**.
- The driver is built against the pfSense FreeBSD fork headers matching the
  installed kernel, never FreeBSD `main`.

## Serial console

The install and the Sophos boot menu are only visible over serial. Both baud
rates have to be configured for the Sophos menu to show (TODO: confirm and
write down the two rates).

Ports on the pfSense side are `/dev/cua*`. From another host, for example
`/dev/ttyUSB0` with PuTTY or `cu`.

`/boot/loader.conf.local` console lines used on the test box (see
`boot_loader/`):

    console="comconsole,efi"
    boot_multicons="YES"
    boot_serial="YES"

With `boot_multicons` the serial text is repeated (tripled). Dropping
`boot_multicons` and setting `console="comconsole"` stops that.

## Install

Run as root on the firewall, from this directory:

    /bin/sh ./install.sh

This installs `xgs_octep.ko` into `/boot/modules`, loads it, adds the loader
tunables below to `/boot/loader.conf.local`, installs the `rc.d` hook, and
copies the status page.

Update an installed module (upload the new file first, then):

    /sbin/sha256 driver/prebuilt/xgs_octep.ko
    /bin/sh driver/install.sh
    /sbin/sha256 /boot/modules/xgs_octep.ko
    /sbin/shutdown -r now

Both hashes must match. `kldload: can't load xgs_octep: module already loaded`
is expected when updating, since the old module is still loaded. Reboot to run
the new one. The NPU can take 1 to 3 minutes after boot to answer.

## Loader tunables (`/boot/loader.conf.local`)

| Tunable | Default | Set by `install.sh` | Meaning |
|---|---|---|---|
| `xgs_octep_load="YES"` | off | yes | load the module at boot |
| `hw.xgs_octep.autostart` | 0 | `1` | arm rings and run the handshake as soon as the NPU is ready |
| `hw.xgs_octep.offload` | 1 | no | firmware offload gate on at boot (keep on) |
| `hw.xgs_octep.portup` | 1 | no | raise every NPU port |
| `hw.xgs_octep.ifaces` | 0 | `1` | create `octepN` interfaces and receive |
| `hw.xgs_octep.tx` | 0 | `1` | transmit from `octepN` |

Boot holds until the `octepN` interfaces exist (up to 300 s), so pfSense does
not stop at the "assign interfaces" prompt. If it still does, go on after the
NPU is ready (1 to 3 minutes).

### Recovery

At the loader prompt, disable the data path and boot:

    set hw.xgs_octep.ifaces="0"
    set hw.xgs_octep.tx="0"
    boot

Once the rings are armed the module cannot be unloaded (`kldunload` fails with
EBUSY) until the NPU is rebooted.

## Interfaces

Port tag to name: tags `33024 + 256n` (n = 0..9) are `octep0..9` (Port1..8,
F1, F2), tag `0` is `octep10`, tag `32768` is `octep11`. `octep0` is Port1.

Check:

    /sbin/sysctl dev.xgs_octep.0.status
    /sbin/sysctl dev.xgs_octep.0.links
    /sbin/ifconfig octep0

A healthy status line has `ready=1 dp=1 hs=1 rpc_ready=1 offload=1 fw_cfg=0x3
ports_up=12 ifaces=12 tx=1`, with `tx_drop=0 rx_err=0`.

`links` prints, per port, the link word and the switch counters (bytes and
unicast, broadcast and multicast frames). Use it to tell whether frames leave
or arrive at the switch port.

## Start-up commands

`startup_scripts/startup.sh` turns off the hardware options on the `octepN`,
`ix` and `lo0` interfaces that this setup does not use. Run it by hand once
the interfaces are stable, then automate it with the `Shellcmd` package.

## Layout

| Path | Purpose |
|---|---|
| `driver/xgs_octep.c`, `Makefile` | the driver |
| `driver/prebuilt/xgs_octep.ko` | built module for the 2.9.2 kernel |
| `driver/install.sh`, `install.sh` | module and full install |
| `rc.d/xgs_octep` | boot hook |
| `boot_loader/` | loader settings used on the test box |
| `startup_scripts/startup.sh` | per-interface option script |
| `www/xgs_octep_status.php` | status page (not working yet) |
| `CHANGELOG.md` | stages with test specs and results |

## TODO

- Configure NAT, firewall and routing as on any pfSense install. The driver
  only provides the interfaces.
- Validate flow offload and acceleration (Task 6).
- SFP cages and 10G ports (Task 7).
- Real link polling and media on `octepN`.
- Fix the status page.
- Vulnerability review (Task 8).

## License

GPL-2.0-or-later. See `LICENSE`.
