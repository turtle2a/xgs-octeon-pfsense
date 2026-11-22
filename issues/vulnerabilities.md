# Vulnerability issues

Source: [vulnerabilities/docs/vulnerability-mitigation.md](../vulnerabilities/docs/vulnerability-mitigation.md)

| ID | Sev | Risk | Correction | Status | Outstanding |
|----|-----|------|------------|--------|-------------|
| P1 | 🟡 | NPU reads or writes any host RAM by DMA (DMA tags use `BUS_SPACE_MAXADDR`, bus mastering always on), so every bounds check on NPU data can be bypassed | Require an IOMMU; refuse to arm the rings without one | 🟠 Not yet enforced: the platform IOMMU is enabled in firmware, and the pfSense kernel has `options IOMMU` and the AMD IOMMU driver built in (`amd_drv.c`), but the driver is off by default (loader tunable `hw.amdiommu.enable=0`), so dmesg shows `pci0: <base peripheral, IOMMU> at device 0.2 (no driver attached)` (23 Nov) and the NPU DMA is not translated by the OS | 23 Nov 09:55: `hw.amdiommu.enable=1` made the test box lose its SATA disk at boot (mountroot prompt), so it is NOT enabled in the appliacne bios settigns under ABS. To do: find why (IVRS entries, SATA exception), then retry with console access and a config backup, confirm dmesg shows the driver attached and the NPU still comes up with offload, then update the system and docs; optionally refuse to arm the rings without it |
| P2 | 🟡 | Any local user reading `dev.xgs_octep.0.links` runs blocking mailbox round-trips under the softc lock | `priv_check(PRIV_DRIVER)` or `CTLFLAG_SECURE`; cache the counters | 🔴 Open | Fix and test |
| P3 | 🟡 | Prebuilt `.ko` is installed and loaded as root with no integrity check | Ship a pinned SHA-256; `install.sh` verifies it | ✅ Fixed (box-tested) | Push after go |
| P4 | 🟡 | `fetch-sources.sh` downloads kernel sources by mutable ref, with no digest or HTTPS pin | Pin a commit SHA and digest; `curl --proto '=https'`; validate `REPO` and `REF` | 🔴 Open | Fix and test |
| P5 | ⚪ | NPU-supplied port tags can create duplicate interfaces (`octep10`, `octep11` map from two tags each) | Reject duplicate tags and iface numbers | 🔴 Open | Fix and test |
| P6 | ⚪ | `wire_len` truncated to 32 bits on RX; a forged length passes the MTU check | Reject oversize or zero-span before computing `frame_len` | 🔴 Open | Fix and test |
| P7 | ⚪ | TX slot can be rewritten while the NPU still reads it (free test uses the doorbell pending count) | Track completion with the read index | 🔴 Open | Fix and test with TX load |
| P8 | ⚪ | Build against kernel sources that differ from the running kernel (an earlier build panic-looped the box) | `install.sh` refuses when `__FreeBSD_version` differs; drop the `XGS_KERN_VERSION` override | ✅ Fixed (box-tested) | Push after go |
| P9 | ⚪ | `debug.kassert.warn_only="1"`, no loader password, serial console open | Drop the kassert setting; set a loader password | ⚪ Accepted on the test box | Revisit before general use |
| P10 | ⚪ | Unpatchable BIOS and NPU firmware | Keep pfSense updated; treat the NPU as untrusted (P1) | Accepted | - |
