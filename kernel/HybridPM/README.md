# HybridPM: CPU clocks for hybrid Intel CPUs (`io.github.yusufklncc.HybridPM`, HybridPM.kext)

A small [Lilu](https://github.com/acidanthera/Lilu) plugin for 12th to 14th generation Intel Core desktop and laptop
CPUs (Alder Lake and Raptor Lake, with performance and efficiency cores) and Core Ultra 200S desktop CPUs (Arrow Lake). It is not part of the GPU driver, but without
it macOS runs these CPUs well below their clocks, and the GPU driver spends a lot of CPU time per command buffer, so the
GPU looks slow too.

## Why

macOS's CPU power management (XCPM) predates hybrid CPUs and gets two things wrong on them:

- **Units.** XCPM writes `IA32_HWP_REQUEST` (MSR 0x774) in 100 MHz ratio units. Hybrid P-cores count HWP performance in
  units of 78.741 MHz (Linux `intel_pstate`, `HYBRID_SCALING_FACTOR_ADL`). The turbo ratio 56 of an i7-14700K
  (5.6 GHz) becomes 56 x 78.741 MHz = 4.41 GHz on the P-cores: one thread never goes above 4.4 GHz.
- **Pinned clocks.** XCPM also writes its own per-CPU pick into the request's *desired* field, which pins a core to that
  clock. With CpuTopologyRebuild's grouping it left fully busy cores at 800 MHz while others ran at 3.3 GHz under the
  same load.

HybridPM finds the XCPM routine that builds and writes the request (by its byte pattern, searched forward from the
exported `_xcpm_enable_pstate_ctrs`; at +0x1140 on macOS 15.8.1, +0x1190 on 26.7.1), routes it, and after Apple's write
converts the request on the P-cores: *max* becomes the core's HWP *highest* (the turbo ratio table still applies) and
*desired* is converted to HWP units. By default it then sets *desired* to 0 (autonomous: the hardware picks each
core's clock from its own load and EPP, as Linux does), EPP 0, and turns energy-efficient turbo off (MSR_POWER_CTL
bit 19). The unit mismatch was first found by NootedGreen's XE-PM
(JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe).

Everything takes effect 20 s after boot: the HWP MSRs fault until XCPM sets `IA32_PM_ENABLE`.

## Measured

On the test machine (i7-14700K, Z790, macOS 26.7.1 and 15.8.1), a perl loop in million iterations per second, PL2
253 W either way:

| Load | Without | With HybridPM |
|---|---|---|
| 1 thread | ~55 | 128 (5.39 GHz) |
| 8 threads | ~400 | ~1010 |
| 28 threads | 920-1365 | ~1500 |

Idle package power stayed about 10 W for every EPP. Geekbench's GPU scores (Metal, OpenCL) on the Arc B580 were about a
third higher with it, because the driver's per-command-buffer CPU work runs at the higher clock. A user with an
i5-13400F (also Alder Lake / Raptor Lake hybrid) and no HybridPM got GPU scores about 30 % lower than the test machine.

## Which CPUs

It acts only on Intel family 6 CPUs that are hybrid (CPUID leaf 7 EDX bit 15), have HWP, and are one of the models whose
HWP unit is known: the models whose P-cores use the 78.741 MHz unit (the models Linux `intel_pstate` gives
`HYBRID_SCALING_FACTOR_ADL`; their E-cores count in 100 MHz units and are left alone), and Arrow Lake desktop, whose
P-cores and E-cores both count in smaller units:

| Model | Linux name | CPUs |
|---|---|---|
| 0x97 | `ALDERLAKE` | 12th gen desktop (and some 13th/14th gen i5, e.g. i5-13400F stepping C0) |
| 0x9A | `ALDERLAKE_L` | 12th gen laptop (H, P, U) |
| 0xB7 | `RAPTORLAKE` | 13th and 14th gen desktop (e.g. i7-14700K, i5-13400F stepping B0) |
| 0xBA | `RAPTORLAKE_P` | 13th gen laptop (H, P, U) |
| 0xBF | `RAPTORLAKE_S` | some 13th/14th gen desktop i3/i5 models |
| 0xC6 | `ARROWLAKE` | Core Ultra 200S desktop (e.g. Core Ultra 7 265K/KF): P-cores 62.5 MHz, E-cores ~70.8 MHz per unit, measured from Linux CPPC on a 265KF; both core types are converted |

`0xBE` (`ALDERLAKE_N`, N100 and the like) is listed as well but has efficiency cores only, so the hybrid check already
leaves it alone. Other Core Ultra models (Meteor Lake 0xAA/0xAC, Arrow Lake H/U 0xC5/0xB5, Lunar Lake 0xBD, Panther Lake 0xCC)
use other or unmeasured units (Linux: 80 MHz for Meteor Lake, 86.957 MHz for Lunar Lake) and is left alone, as is any model not
listed, any CPU that is not hybrid (an i5-12400 with P-cores only, every CPU before 12th gen) and any non-Intel CPU.
On those the plugin does nothing: it routes nothing, reads and writes no MSR, logs one line (`HybridPM hpm: off: ...`)
and shows the reason:

```sh
sysctl debug.hybridpm.enabled debug.hybridpm.why
# debug.hybridpm.enabled: 0
# debug.hybridpm.why: unsupported model 0xAA (Core Ultra: another HWP scale)
```

The model comes from the CPUID instruction itself, so OpenCore's `Cpuid1Data` (which changes what the kernel records)
does not hide it.

## Installing

It ships in the release's `-efi.zip` and macOS package (`EFI/OC/Kexts/HybridPM.kext`, with `hpmstat`). On macOS,
stage 4 of the install guide adds it: `efi.sh` and IntelXe Manager's EFI page copy it and put its `Kernel > Add` entry
right below `Lilu.kext`'s when the CPU's name is one of these models, and leave it out with a note when the EFI does
not load Lilu (Lilu is required and not shipped here). The Windows EFI tool (`IntelXe-EFI.ps1`) does the same, by the
name of the CPU of the PC it runs on (checked on macOS against `efi.sh`, not yet run on Windows). By hand, see the
install guide's *Hybrid Intel CPUs* section.

## Controls

Live, as root (`sudo sysctl ...`):

| sysctl | Default | What |
|---|---|---|
| `debug.hybridpm.hwp` | 1 | convert XCPM's ratios to HWP units on the P-cores (on Arrow Lake the E-cores too) |
| `debug.hybridpm.autonomous` | 1 | 1 = desired 0, the hardware picks the clock; 0 = XCPM's pick (converted) |
| `debug.hybridpm.epp` | 0 | EPP override 0 (performance) .. 255 (power saving); -1 keeps XCPM's |
| `debug.hybridpm.eet_off` | 1 | 1 = energy-efficient turbo off |

Read-only, any user:

| sysctl | What |
|---|---|
| `debug.hybridpm.enabled` | 1 once the conversion is active (20 s after boot) |
| `debug.hybridpm.why` | `on (model 0xB7)` when active; otherwise why not: `not hybrid (model 0x..)`, `unsupported model 0x..`, `not an Intel CPU`, `route 3: XCPM request writer not found`, ... |
| `debug.hybridpm.route` | 0 not tried (unsupported CPU); 1 routed; 2 a kernel symbol is missing; 3 the XCPM routine was not found (exactly one match is required); 4 routing failed |
| `debug.hybridpm.state` | (supported CPUs only) a binary sample for `hpmstat`: per-CPU APERF/MPERF, HWP capabilities and request, core type and temperature; package RAPL energy, power limits (MSR 0x610 and the MCHBAR copy, the lower one applies), perf-limit reasons, turbo ratio limits |

Boot-args: `-hpmoff` (Lilu: the plugin does not start, and `debug.hybridpm` does not exist), `hpmhwp=0`, `hpmauto=0`, `hpmepp=N` (-1..255), `hpmeet=0`;
`-hpmdbg` shows the debug lines of a `--debug` build, `-hpmbeta` lets it start on a macOS newer than 26.

`hpmstat [interval-ms [count]]` (no root) diffs two samples and prints each CPU's effective clock, busy share, HWP
request and capabilities and temperature, plus package power, power limits, perf-limit reasons and EET:

```sh
./hpmstat 1000 5
```

## Limits

- Tested on one machine, an i7-14700K (Raptor Lake S), on macOS 26.7.1 and 15.8.1, with CpuTopologyRebuild
  (`ctrsmt=off`). Lilu allows it from Big Sur to Tahoe; only those two releases were tried.
- The routine is found by byte pattern. A macOS update that changes it leaves `route` at 3 and the plugin inactive
  (XCPM's behaviour, nothing worse).
- It needs Lilu (any recent release; the test machine runs 1.7.2), which most OpenCore EFIs already have for
  VirtualSMC. Lilu is not shipped here.
- Package-level HWP control (request bit 42) is left alone; XCPM does not use it on these CPUs.
- Power limits (PL1/PL2) are not changed; `hpmstat` only shows them.

## Building

```sh
python3 kernel/HybridPM/build.py --sdk /path/to/MacKernelSDK --lilu /path/to/Lilu.kext
#   -> build/HybridPM/HybridPM.kext, build/HybridPM/hpmstat
```

`--lilu` takes a DEBUG `Lilu.kext` (its `Contents/Resources` has the headers and `plugin_start.cpp`), that folder, or a
Lilu source checkout. `--debug` builds -O0 with Lilu's debug logging. Lilu's `plugin_start.cpp` is compiled into the kext,
so its BSD-3-Clause notice (`LICENSE.Lilu`) goes into the bundle's `Contents/Resources`.
