# Appliances that may be compatible

> **Disclaimer.** The pfSense CE driver is verified on **one** appliance only: an
> XGS 2300 (OCTEON TX, PCI `177d:a300`). Everything else in this table is an
> *inference from the vendor appliance property spec files*, not a test result.
> Hardware with a similar name can use different switch wiring, PCI IDs and
> firmware. Do not run the installer on hardware you cannot recover, and test at
> your own risk. This project is not affiliated with or endorsed by any
> appliance vendor.

The same appliance list, derived the same way, is kept in the Linux project:
<https://github.com/turtle2a/xgs-octeon-linux>.

Full list of all 138 spec files as a tree: [appliance-tree.md](appliance-tree.md).

## How this was derived

The firmware property file for each appliance has an `NPU=` field. The 138 spec
files fall into these groups:

| `NPU=` value | Files | Meaning for this project |
|---|---|---|
| `NO` | 95 | x86 appliances and virtual images. Out of scope (no NPU). |
| `ARMADA` | 20 | Marvell Armada SoC with its own switch. A different design from the OCTEON PCIe NPU. Not expected to work. |
| `OCTEON_TX` | 4 | Same family as the tested XGS 2300. Most likely to work. |
| `OCTEON_TX2` | 17 | Newer OCTEON TX2. Different generation; unknown. |
| `OCTEON_TX2_98XX` | 2 | OCTEON TX2 98xx, high end. Different generation; unknown. |

## OCTEON TX (same family as the tested unit)

| Model | CPU cores | RAM | Port layout (spec string) | pfSense CE status |
|---|---|---|---|---|
| XGS 2100 | 4 | 8 GB | `Port8_F2MA` | Untested. |
| **XGS 2300** | 4 | 8 GB | `Port8_F2MA` | **Tested.** `octep0` to `octep11`, RX, TX, ping and NAT work. Flow offload / acceleration is not validated yet. SFP cages and 10G ports are not tested. |
| XGS 3100 | 4 | 12 GB | `Port8_F4MA` | Untested. |
| XGS 3300 | 8 | 16 GB | `Port8_F4MA` | Untested on pfSense (tested with the Linux driver). |

## OCTEON TX2 and TX2 98xx (unknown)

| Model | CPU cores | RAM |
|---|---|---|
| XGS 4300 | 12 | 32 GB |
| XGS 4380 | 12 | 32 GB |
| XGS 4500 | 16 | 32 GB |
| XGS 4580 | 16 | 32 GB |
| XGS 5500 | 32 | 64 GB |
| XGS 5510 | 24 | 96 GB |
| XGS 5580 | 24 | 64 GB |
| XGS 6500 | 48 | 80 GB |
| XGS 6510 | 48 | 192 GB |
| XGS 6580 | 48 | 96 GB |
| XGS 7580 | 64 | 192 GB |
| XGS 7500 (98xx) | 64 | 128 GB |
| XGS 8500 (98xx) | 128 | 256 GB |

## Armada (not expected to work)

XGS 87, 87w, 107, 107w, 116, 116w, 126, 126w, 136, 136w and 138, plus the
116w VCR variants. These use `NPU=ARMADA`, which is not the OCTEON PCIe NPU this
driver targets.

## Reading the port layout string

`PortN_Fx` is the number of copper ports (`N`) and fibre or SFP ports (`x`), for
example `Port8_F2MA` is 8 copper plus 2 fibre. On the tested XGS 2300 the
firmware reports 12 NetAgent ports, which become `octep0` to `octep11` on
pfSense CE. How the fibre cages map to those interfaces is still being verified.

## Help wanted

If you test another model, please report the PCI ID (`pciconf -lv | grep 177d`),
the `xgs_octep` lines from `dmesg`, and whether the `octepN` interfaces appear.
Remove serial numbers and MAC addresses before posting.
