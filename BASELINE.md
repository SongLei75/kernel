# Kernel baseline provenance

This is an independent development tree derived from the checked company baseline directory. No deferred-writeback feature has been implemented yet.

| Item | Value |
| --- | --- |
| Upstream Linux | 6.1.158, `v6.1.158` |
| Upstream commit | `f6e38ae624cf7eb96fb444a8ca2d07caa8d9c8fe` |
| PREEMPT_RT patch | `patch-6.1.158-rt58.patch.xz` (applied to this working tree) |
| Patch SHA256 | `5acdb9d149c9f0897d7788f929eacac226b95f5f5bde43383627cca37ff27461` |
| Company ARM64 config | `configs/company-arm64-linux-6.1.158.config` |
| Config SHA256 | `e9ffaa51f9e78f185672badf3494a52d2ff4c574a74bc18812bf21f76053a8d1` |
| Intended branch | `main` |

Original source: `/home/songlei/project/company_sw_baseline/src/linux-6.1.158`.

Original patch: `/home/songlei/project/company_sw_baseline/src/patch-6.1.158-rt58.patch.xz`.

Company config: `/home/songlei/project/company_sw_baseline/config/company-arm64-linux-6.1.158.config`.

The source is the public upstream kernel plus the upstream RT patch. The original baseline notes explicitly exclude vendor-private kernel patches. The saved ARM64 `.config` is company-product configuration, **not** the separate GCP x86_64 test configuration. Kernel build outputs are not copied.

To prepare a local build, copy the tracked ARM64 config to an **ignored** top-level `.config`, then use the appropriate ARM64 cross compiler and kernel config update procedure. Avoid editing the tracked snapshot as a side effect of building.

Design document: [docs/vfat-deferred-writeback-plan.md](docs/vfat-deferred-writeback-plan.md).

**Publication:** The Linux source and RT patch are public upstream material, but the ARM64 config identifies the product/toolchain and may contain internal configuration. Explicit authorization to publish the complete company configuration should be obtained before pushing a public repository.

Kernel licensing remains as supplied by the upstream `COPYING` and `LICENSES/` files.
