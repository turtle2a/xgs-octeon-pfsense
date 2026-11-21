# XGS OCTEON pfSense CE port

Run **pfSense CE on the Sophos XGS 2300**. A FreeBSD kernel driver
(`xgs_octep`) for the OCTEON TX NPU (PCI `177d:a300`) so pfSense can use the
front ports as `octepN` interfaces. The driver is rewritten from our own Linux
OCTEP driver (GPL-2.0-or-later). You still configure NAT, firewall and routing
yourself, as on any pfSense install (see TODO).

Project page: https://turtle2a.github.io/xgs-octeon-pfsense/
Compatible appliances: [docs/compatible-appliances.md](docs/compatible-appliances.md)
and the [appliance tree](docs/appliance-tree.md), plus one file per spec in
[appliances/](appliances/)

Keywords: Sophos XGS 2300, pfSense CE, OCTEON TX, Cavium, Marvell, FreeBSD
driver, NPU, firewall, router.

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
- **Optional, install first:** the pfSense **Shellcmd** package
  (`pfSense-pkg-Shellcmd`, System > Package Manager > Available Packages) is
  needed before you can run `startup_scripts/startup.sh` at boot. See
  [Start-up commands](#start-up-commands-optional).

## Install pfSense CE (serial image)

The XGS 2300 has no video output, so install from the **serial** image.

1. Download the pfSense CE amd64 **serial memstick (USB)** image and its
   checksum from the pfSense CE download page. Verify the checksum.
2. Write the image to a USB stick. This erases the stick. The `dd` steps are
   for **Linux**. On Windows or macOS use an image writer (listed below).
   - Unpack it first if it is a `.gz`: `gunzip pfSense-CE-memstick-serial-*.img.gz`
   - Find the stick: `lsblk -o NAME,SIZE,MODEL,TRAN` (a USB stick shows
     `usb`, for example `/dev/sdb`). Check the size matches before you go on.
   - Unmount any mounted partitions: `umount /dev/sdb*`
   - Write it to the whole device, not a partition (`/dev/sdb`, not
     `/dev/sdb1`):
     `dd if=<image>.img of=/dev/<usb-drive> bs=1M conv=fsync status=progress`
     where `if=` is the input file (the unpacked pfSense serial image) and
     `of=` is the output (the whole USB stick from `lsblk`, for example
     `sdb`). Swapping them or choosing the wrong `of=` destroys data.
   - Run `sync`, then remove the stick.
   - A wrong `of=` can wipe your PC's disk. Never point it at the system drive.
   - Or use an image writer. Select the unpacked `.img`, pick the USB stick,
     and write:
     - Linux: GNOME Disks ("Restore Disk Image"), balenaEtcher, Raspberry Pi
       Imager ("Use custom").
     - Windows: Rufus (choose "DD Image" mode), balenaEtcher, Win32 Disk
       Imager, Raspberry Pi Imager ("Use custom").
     - macOS: balenaEtcher, Raspberry Pi Imager ("Use custom"), or `dd`:
       `diskutil unmountDisk /dev/diskN` then `dd ... of=/dev/rdiskN bs=1m`.
3. Plug the USB stick into the XGS and connect a serial cable (for example
   `/dev/ttyUSB0` on your PC, using PuTTY or `cu`). Power on and boot from the
   USB stick, not the internal disk (TODO: how to pick the USB boot device on
   the XGS).
4. The stick only runs the installer. Run it over serial and install pfSense
   onto the disk you will use permanently (the internal SSD, or another drive
   of your choice). Do not install onto the USB stick.
5. When the installer finishes, remove the USB stick and reboot. The XGS now
   boots from the permanent disk. Then go on with the Install section below.

## Serial console

The Sophos boot menu is only visible over serial, and both baud rates have to
be configured for it to show (TODO: confirm and write down the two rates).

Ports on the pfSense side are `/dev/cua*`. From another host, for example
`/dev/ttyUSB0` with PuTTY or `cu`.

`/boot/loader.conf.local` console lines used on the test box (see
`boot_loader/loader.conf.local`):

    console="comconsole"
    boot_multicons="NO"
    boot_serial="YES"

`console="comconsole,efi"` with `boot_multicons="YES"` repeats (triples) the
serial text, so the test box uses the serial console only.

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

## Start-up commands (optional)

`startup_scripts/startup.sh` is optional and is not part of the tested setup.
The installer does not install or run it, and the driver works without it. It
is a per-interface option script (hardware offload options off on `octepN`,
`ix`, `igc`, `igb`, `em` and `lo0`) kept as an example.

To run it at boot it needs the pfSense **Shellcmd** package, which must be
installed **first** (`pfSense-pkg-Shellcmd`, installed from System > Package Manager > Available
Packages). Add an entry of type `shellcmd` with `/bin/sh` and the full path to
the script. Without the package, run it by hand once the interfaces are stable.
Automation this way is untested.

## Layout

| Path | Purpose |
|---|---|
| `driver/xgs_octep.c`, `Makefile` | the driver |
| `driver/prebuilt/xgs_octep.ko` | built module for the 2.9.2 kernel |
| `driver/install.sh`, `install.sh` | module and full install |
| `rc.d/xgs_octep` | boot hook |
| `boot_loader/loader.conf.local` | loader settings used on the test box |
| `startup_scripts/startup.sh` | optional per-interface option script (not tested) |
| `www/xgs_octep_status.php` | status page (not working yet) |
| `vulnerabilities/`, `issues/vulnerabilities.md` | review findings and planned checks |

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
