# OCTEP pfSense CE Driver and Install Tools

This project contains the FreeBSD OCTEP driver source (`xgs_octep`), a prebuilt
module, installer, boot hook and clean-install instructions for running pfSense
CE on the tested OCTEON TX NPU appliance. It is not a complete pfSense image or
a general-purpose installer for every OCTEON or XGS appliance. The driver is
rewritten from our own Linux OCTEP driver
([xgs-octeon-linux](https://github.com/turtle2a/xgs-octeon-linux)).

The primary appliance is the XGS 2300 (PCI `177d:a300`): the install is tested
on it with pfSense CE 2.9.2 (FreeBSD 16.0-CURRENT, serial image). Other
appliances, pfSense versions or kernels need separate testing. The driver gives
pfSense the front ports as `octepN` interfaces. You still configure NAT,
firewall and routing yourself, as on any pfSense install (see TODO).

Project page: https://turtle2a.github.io/xgs-octeon-pfsense/

Keywords: Sophos XGS 2300, pfSense CE, OCTEON TX, OCTEP, FreeBSD NPU driver,
Cavium, Marvell, kernel module, network appliance.





##    NOTE:

        The Optional setup is to install pfsense from an iso boot onto a donor computer ,
        setup the addresses ,services and boot loader as per docs ,copy over the git clone 
        folder to /home dir on the donor install.Then remove the drive and place in the 
        appliance for booting off,this would also ensure a near latest version of 
        pfsense ce is installed prior to install of xgs-octeon-pfsense.

        See section ## Installation below for full install details .
        
##



## Hardware Compatibility

| Appliance or family | Status | Notes |
| --- | --- | --- |
| XGS 2300 | Install tested | pfSense CE 2.9.2 on the serial image. SFP cages and hardware flow offload are not verified. |
| XGS 2100, 3100, 3300 | Not yet verified | Expected to work (same NPU family) but untested with pfSense by the maintainer. |
| Other OCTEON TX appliances | Unverified candidates | The installer expects PCI vendor/device `177d:a300`; matching that ID alone does not establish firmware, port-map or full hardware compatibility. |
| XGS 136 | Not supported yet | Armada platform; needs a separate driver and hardware validation. |

The full list of appliances is in
[docs/compatible-appliances.md](docs/compatible-appliances.md) and the
[appliance tree](docs/appliance-tree.md), with one file per spec in
[appliances/](appliances/).

## Status

| Item | State |
|---|---|
| Boot, NPU handshake, port discovery | verified |
| `octep0..11` interfaces, RX, TX, ping, NAT | working (Port1 verified by capture) |
| Firmware offload gate on at boot (`offload=1 fw_cfg=0x3`) | on, verified |
| iperf to 192.168.1.1, no accel, 0 retries | OK (user-run, Task 5) |
| Flow offload / acceleration | **partial**: one NAT'd flow programs and the NPU forwards about 550 frames, then stalls (the same stall as the Linux driver); work paused, default off (Task 4) |
| SFP cages / 10G ports | not tested (Task 7) |
| Real link state and media on `octepN` | not yet (media is fixed, status always active) |
| Status page `xgs_octep_status.php` | working: banners, flow indicators, pcap, iperf (Task 10; under test) |

## Vulnerabilities

Brief: the driver and install were reviewed for security. The findings are
documented but not all fixed yet (Task 8). Everything lives in the
[`vulnerabilities/`](vulnerabilities/) tree and
[issues/vulnerabilities.md](issues/vulnerabilities.md).

| Area | Where |
| --- | --- |
| Findings, mitigation options and risk register | [vulnerabilities/docs/vulnerability-mitigation.md](vulnerabilities/docs/vulnerability-mitigation.md) |
| Planned checks and test scripts | [vulnerabilities/scripts/](vulnerabilities/scripts/) |
| Review findings list | [issues/vulnerabilities.md](issues/vulnerabilities.md) |

## Boot Loader Configuration

The serial console settings and the loader tunables below go in
`/boot/loader.conf.local` (the used file is `boot_loader/loader.conf.local`).

### Serial console

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

### Loader tunables (`/boot/loader.conf.local`)

| Tunable | Default | Set by `install.sh` | Meaning |
|---|---|---|---|
| `xgs_octep_load="YES"` | off | yes | load the module at boot |
| `hw.xgs_octep.autostart` | 0 | `1` | arm rings and run the handshake as soon as the NPU is ready |
| `hw.xgs_octep.offload` | 1 | no | firmware offload gate on at boot (keep on) |
| `hw.xgs_octep.portup` | 1 | no | raise every NPU port |
| `hw.xgs_octep.ifaces` | 0 | `1` | create `octepN` interfaces and receive |
| `hw.xgs_octep.tx` | 0 | `1` | transmit from `octepN` |
| `hw.amdiommu.enable` | 0 | no | AMD IOMMU driver (P1). Do not enable yet: with it on, the test box lost its SATA disk at boot (see CHANGELOG Task 11) |

**IOMMU recommendation:** leave `hw.amdiommu.enable` unset and keep the IOMMU off in the BIOS/UEFI (names vary: IOMMU, AMD-Vi, DMA remapping). Turning it on made the test box lose its SATA disk at boot. This does not fix P1 (the NPU can still DMA anywhere in host RAM, as it already could); it only avoids that boot failure until the IOMMU is made to work with the NPU. If the box stops at `mountroot>`, power-cycle and either set `hw.amdiommu.enable="0"` at the loader prompt or switch the IOMMU off in the BIOS.

Boot holds until the `octepN` interfaces exist (up to 300 s), so pfSense does
not stop at the "assign interfaces" prompt. If it still does, go on after the
NPU is ready (1 to 3 minutes).

### Recovery

At the loader prompt, disable the data path and boot:

    set hw.xgs_octep.ifaces="0"
    set hw.xgs_octep.tx="0"
    set hw.amdiommu.enable="0"
    boot

Once the rings are armed the module cannot be unloaded (`kldunload` fails with
EBUSY) until the NPU is rebooted.

## Installation (in order)

##    NOTE:

        The Optional setup is to install pfsense from an iso boot onto a donor computer ,
        setup the addresses ,services and boot loader as per docs ,copy over the git clone 
        folder to /home dir on the donor install.Then remove the drive and place in the 
        appliance for booting off,this would also ensure a near latest version of 
        pfsense ce is installed prior to install of xgs-octeon-pfsense.
##


- Preparation for the admistration computer.

- Clone the project on your administration computer:

```sh
git clone https://github.com/turtle2a/xgs-octeon-pfsense.git
cd xgs-octeon-pfsense
```

- For preparation to ssh upload the /home directory on the appliance install.



1. Install pfSense CE from the serial image: [Install pfSense CE (serial image)](#install-pfsense-ce-serial-image)
   to the usb or boot drive.
   
2. Boot the appliance off the boot usb or drive you have imaged to ensuring you have a drive to install to that
   is attached to the appliance or its built in nvme or ssd. ( * NOTE - DEL enters the bios , Esc during boot gives boot options).
   
3. Finish the install of pfsense to the appliance drive as per prompts.
   
4. After boot check the igb0 or mgmgt lan adapter is configured with an ip address as per the pfsense menu (See pfsense Manual).
   dhcp on the lan will help.
   
5. Access the system via the web interface on the address set for igb0 or lan,this will be the only address at this point in the intall
   prior to the install completing.Go to /boot/loader.conf.local and follow the instructions below and in the docs ,make the changes.
   
6. Go to the admin and access control in the web interface , enable serial (alrady done in serial image)and ssh access.

7. Configure the boot loader: [Boot Loader Configuration](#boot-loader-configuration).
    
8. Copy the git clone from the admin pc to the pfsense firewall (below).
    
9. Install the driver as per docs and below, reboot, then verify: [Install and Verify](#install-and-verify).


*NOTE if doing via console -

Then copy the folder to the firewall (SSH must be enabled under System >
Advanced > Admin Access):

```sh
scp -r ../xgs-octeon-pfsense root@<firewall-ip>:/home/
```

Or clone on the firewall itself (needs internet access; `pkg install git` is
untested here):

```sh
/usr/sbin/pkg install -y git
/usr/local/bin/git clone https://github.com/turtle2a/xgs-octeon-pfsense.git /home/xgs-octeon-pfsense
```

Or without git, download the archive on the firewall (the folder becomes
`/home/xgs-octeon-pfsense-main`):

```sh
/usr/bin/fetch -o /tmp/xo.tar.gz https://github.com/turtle2a/xgs-octeon-pfsense/archive/refs/heads/main.tar.gz
/usr/bin/tar -xzf /tmp/xo.tar.gz -C /home
```

### Install pfSense CE (serial image)

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
   boots from the permanent disk. Then go on with [Install and Verify](#install-and-verify).

### Hardware and image

- XGS 2300 (AMD Ryzen Embedded R1606G + OCTEON TX NPU).
- pfSense CE 2.9.2 (FreeBSD 16.0-CURRENT), installed from the **serial image**.
- The driver is built against the pfSense FreeBSD fork headers matching the
  installed kernel, never FreeBSD `main`.
- **Optional, install first:** the pfSense **Shellcmd** package
  (`pfSense-pkg-Shellcmd`, System > Package Manager > Available Packages) is
  needed before you can run `startup_scripts/startup.sh` at boot. See
  [Start-up commands](#start-up-commands-optional).

## Install and Verify

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

### Verify

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

### NPU flow offload (on at boot)

The firmware offload gate is turned on at boot by the driver (`hw.xgs_octep.offload`
defaults to 1). Check it:

    /sbin/sysctl dev.xgs_octep.0.status

`offload=1 fw_cfg=0x3` means the gate is on. `/bin/sh driver/offload-check.sh`
prints `OFFLOAD GATE: ON` or `OFF`; with `start` and `stop` around a
transfer it shows whether frames were hardware-forwarded. Flow offload itself is
not validated yet (Task 6), so today all traffic still takes the host path. To
opt out, set `hw.xgs_octep.offload="0"` in `/boot/loader.conf.local` (or
`sysctl dev.xgs_octep.0.offload=0` at run time).

## Uninstall

There is no uninstall script yet (TODO). To remove the install by hand, from a
local console as root:

    /sbin/kldunload xgs_octep
    /bin/rm /boot/modules/xgs_octep.ko /usr/local/etc/rc.d/xgs_octep /usr/local/www/xgs_octep_status.php

Then delete the `xgs_octep_load` and `hw.xgs_octep.*` lines from
`/boot/loader.conf.local` and reboot. `kldunload` fails with `EBUSY` once the
rings are armed; in that case remove the files and the loader lines, then
reboot to finish removal.

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
| `npu-status-testing/xgs_octep_status.php` | status and testing page (offload banner, health, flow indicators, test and iperf buttons) |
| `vulnerabilities/`, `issues/vulnerabilities.md` | review findings and planned checks |

## Pre-Upload Test Record

For a reproducible test record, save the output of the following after
installation, and identify the pfSense version and kernel:

    /usr/bin/uname -a
    /sbin/kldstat -n xgs_octep
    /sbin/sysctl dev.xgs_octep.0.status
    /sbin/ifconfig -l
    /sbin/dmesg | /usr/bin/grep xgs_octep

Keep the saved record with the tested source or archive. Do not describe other
pfSense versions, kernels, SFP cages or hardware acceleration as tested unless
they were verified separately.

## Failure Logs

If bring-up fails, collect:

    /sbin/dmesg > /root/dmesg.txt
    /sbin/sysctl dev.xgs_octep.0.status
    /sbin/sysctl dev.xgs_octep.0.links

The driver messages are in `dmesg` (the message buffer persists across
reboots). Record the pfSense version and `uname -a` with the results. The
install is tested on the XGS 2300 with pfSense CE 2.9.2 only.

## TODO

- Configure NAT, firewall and routing as on any pfSense install. The driver
  only provides the interfaces.
- Validate flow offload and acceleration (Task 6).
- SFP cages and 10G ports (Task 7).
- Real link polling and media on `octepN`.
- Fix the status page.
- Vulnerability review (Task 8).

## License

This project is licensed under the GNU General Public License, version 2 or
(at your option) any later version. See [LICENSE](LICENSE). Source files use
the SPDX identifier `GPL-2.0-or-later`.
