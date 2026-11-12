# A second display output on the Arc B580 (pipe B) — design

> Status: implemented behind `-intelxefb_pipeb` (NVRAM `intelxefb-pipeb=1`), default off, **untested on hardware**:
> the test machine has one monitor. The dry-run sysctl `debug.intelxefb_pipeb_plan` works with one monitor and
> checks the math against the live pipe A registers. Since 12 Nov the second framebuffer is published at IntelXeFB's
> start, disconnected (§3); the one-monitor check of that is §7. Everything below cites Linux v6.18
> `drivers/gpu/drm/i915/display/<file>:<function>` (xe reuses that code for display 14.01, Xe2_HPD).

## 1. Where we start

IntelXeFB adopts the one pipe the GOP lit (`adoptFirmwareMode`: the first pipe whose transcoder and plane 1 are on;
on the B580 that is pipe A on DDI TC2) and never builds a pipe from nothing. IntelXeLink (`kernel/IntelXeFB/IntelXeLink.cpp`)
can already take *that* output down and bring it back (C20 PLL, link training, transcoder, plane), but it replays the
GOP's own timing/M/N/watermark registers from a snapshot. A second output needs everything the GOP did for pipe A, computed
instead of replayed, plus the parts that only exist with two pipes (DBUF split, MBUS unjoin, PMDEMAND for two pipes).

## 2. What a second output needs on display 14.01

### 2.1 Ports, PHYs, PLLs (intel_display_device.c `xe2_hpd_display`, intel_display.c `intel_port_to_phy`, intel_cx0_phy.c)

| enum port | name | PHY (display 13+) | AUX_CH_CTL | HPD (SDE, `hpd_mtp`) | TRANS_DDI_FUNC_CTL select | TRANS_CLK_SEL |
|---|---|---|---|---|---|---|
| 0 | A | PHY_A (0) | 0x64010 | bit 16 | 1 | 1 << 28 |
| 3 | TC1 | PHY_F (5) | 0x16F210 | bit 24 | 4 | 6 << 28 |
| 4 | TC2 | PHY_G (6) | 0x16F410 | bit 25 | 5 | 7 << 28 |
| 5 | TC3 | PHY_H (7) | 0x16F610 | bit 26 | 6 | 8 << 28 |
| 6 | TC4 | PHY_I (8) | 0x16F810 | bit 27 | 7 | 9 << 28 |

* BMG's `port_mask` is A + TC1-TC4 (no B). `intel_encoder_is_c10phy` is false for every BMG PHY, so **all five are C20**;
  `intel_phy_is_tc` is false on a dGPU, so there is no FIA/TCSS/DP-alt step. The "TC" ports are plain DP/HDMI DDIs.
* The PLL is the PHY's own MPLLA/MPLLB (intel_dpll_mgr.c `mtl_pll_mgr`, `mtl_port_to_pll_id`: one PLL per port, never
  shared). So a second output on another port brings its own PLL: no PLL sharing rules, no DPLL reassignment of pipe A.
  The C20 DP tables (`xe2hpd_c20_dp_tables`: RBR/HBR1/HBR2/HBR3 contexts of MTL) are the ones IntelXeLink already has.
* Which physical connector is which DDI comes from the VBT child devices; IntelXeFB has no VBT parser. On the test card
  the panel is on TC2. The second-pipe code therefore finds connectors at run time: every port whose SDE hotplug bit is
  live is probed over its own AUX channel (DPCD receiver caps 0x000-0x00F, sink count, EDID).
* HDMI: a native HDMI output (C20 in TMDS mode: `intel_c20_compute_hdmi_tmds_pll`, `intel_hdmi.c`) is **out of scope**:
  IntelXeLink has DP 8b/10b only. A port with HPD live but no AUX reply is logged as "not a DP sink" and left alone.
  A DP-to-HDMI protocol converter on a DP port answers on AUX as a DP branch device and is treated as DP SST (its
  SINK_COUNT must be non-zero).

### 2.2 Pipe B / transcoder B

No eDP/DSI transcoders on BMG, so transcoder = pipe. The registers are pipe A's + 0x1000 (scalers + 0x800, WM_LINETIME
+ 4, TRANS_CLK_SEL + 4). The enable order is `intel_display.c:hsw_crtc_enable`:

1. `intel_dpll_enable` → `intel_cx0pll_enable` (C20 program, PLL request/ack) — IntelXeLink `pllEnable`.
2. `mtl_ddi_pre_enable_dp` (intel_ddi.c): TRANS_CLK_SEL, `intel_ddi_config_transcoder_func`, sink D0, link training,
   `intel_ddi_set_dp_msa`.
3. `intel_set_pipe_src_size` (PIPESRC), `bdw_set_pipe_misc` (PIPE_MISC: BPC_8 | PIXEL_ROUNDING_TRUNC),
   `hsw_configure_cpu_transcoder`: M1/N1 (`intel_link_compute_m_n`), `intel_set_transcoder_timings` (display 13+:
   TRANS_SET_CONTEXT_LATENCY = vblank_start - vdisplay, VBLANK start field 0), TRANS_MULT 0, frame start delay 0
   (CHICKEN_TRANS), `hsw_set_transconf` (0: progressive, no enable).
4. `hsw_set_linetime_wm` (`hsw_linetime_wm` = round(htotal * 8000 / clock), max 0x1FF), `icl_set_pipe_chicken`
   (PER_PIXEL_ALPHA_BYPASS_EN | PIXEL_ROUNDING_TRUNC_FB_PASSTHRU | UNDERRUN_RECOVERY_DISABLE_ADLP).
5. `intel_ddi_enable`: TRANS_DDI_FUNC_CTL enable, TRANSCONF enable; then the plane update writes plane 1's
   noarm registers, PLANE_WM_*/PLANE_BUF_CFG (`skl_write_plane_wm`), PLANE_CTL and PLANE_SURF last.

The link config follows `intel_dp.c:intel_dp_compute_link_config_wide` (lowest rate first, then lowest lane count that
carries `intel_dp_link_required`), restricted to the 8b/10b rates IntelXeLink has tables for (≤ HBR3) and to the
sink's DPCD max rate/lanes. Two deliberate departures, both what the GOP does on pipe A: **24 bpp (8 bpc) always**
(Linux would pick 30 bpp on a 10-bit sink) and **full-range RGB** (Linux's "Broadcast RGB = auto" sends limited range
for CEA modes such as 1920x1080@60 and compresses in the pipe CSC; here MSA_MISC says full range, so the sink agrees).

### 2.3 DBUF slices and MBUS joining (skl_watermark.c)

Display 14 uses `adlp_allowed_dbufs` (`skl_compute_dbuf_slices` → `adlp_compute_dbuf_slices`); DBUF is 4096 blocks
in 4 slices of 1024 (`XE_LPDP_FEATURES` `.dbuf`). The table:

| active pipes | MBUS joined | pipe A slices | pipe B slices |
|---|---|---|---|
| A | yes (`adlp_check_mbus_joined`: first match) | S1-S4 | – |
| A + B | **no** | S1+S2 | S3+S4 |

* DDB per pipe (`skl_crtc_allocate_ddb`): the slice range, shared by weight (`intel_crtc_ddb_weight` = hdisplay) among
  pipes with the *same* slice set — A and B have disjoint sets, so each gets its full 2048. PLANE_BUF_CFG is relative
  to the MBUS bank (`mbus_ddb_offset`: S1/S2 → 0, S3/S4 → 2048), so **pipe B's entries are also 0..2047**.
* Per plane (`skl_crtc_allocate_plane_ddb`): the cursor gets a fixed block at the end (`skl_cursor_allocation`:
  max(32 with one pipe / 8 with two, the cursor's own minimum)), then the highest WM level whose `min_ddb_alloc` sum
  fits, then the rest by relative data rate (plane 1 alone: everything).
* Order with pipe A staying up (`intel_display.c:skl_commit_modeset_enables` + `intel_atomic_commit_tail`):
  PMDEMAND pre (max of old/new) → `intel_dbuf_pre_plane_update` (power the union of slices, `gen9_dbuf_slice_set`:
  DBUF_CTL_S* request bit 31, state bit 30 within 10 us) → pipe A's DDB/WM update (only if it shrinks) and a vblank →
  `intel_dbuf_mbus_post_ddb_update` for joined→unjoined: `intel_dbuf_mdclk_min_tracker_update` (DBUF_CTL_S*
  MIN_TRACKER_STATE_SERVICE = ratio - 1; display 14's MDCLK is CD2XCLK, ratio 2, x2 while joined: 3 → 1),
  `intel_mbus_dbox_update` (PIPE_MBUS_DBOX_CTL of every active pipe, `pipe_mbus_dbox_ctl`: A credit 12 joined / 8
  unjoined, B credit 0xA, I credit 2, B2B max 16 delay 1 regulate), `mbus_ctl_join_update` (MBUS_CTL: hashing 2x2,
  JOIN 0, pipe select A), wait a vblank on A → enable pipe B → `intel_dbuf_post_plane_update` → PMDEMAND post.
* **Pipe A is not reallocated** here. Linux would give it S1+S2 (0..2040 + cursor 2040..2047); IntelXeFB keeps the
  GOP's plane 1 allocation and its own 32-block cursor right after it (cursorInit requires both inside slice 1), which
  is valid in the unjoined layout and leaves pipe A's WMs untouched. If pipe A's DDB reaches past block 2047 the
  enable refuses ("pipe A's DDB is in slices 3-4") rather than half-shrinking it. The dry run reports which case the
  live machine is in.

### 2.4 Watermarks (skl_watermark.c)

* Latencies: `mtl_read_wm_latency` (MTL_LATENCY_LP0_LP1/LP2_LP3/LP4_LP5 0x45780-0x45788, 12:0 / 28:16) with
  `adjust_wm_latency` (read latency 6 us if level 0 reads 0; no 16 Gb DIMM WA on display 14). BMG is dGPU, so
  `HAS_HW_SAGV_WM` is false: **8 levels** (6 and 7 read latency 0 → disabled), and 0x70258/0x7025C are levels 6/7, not
  SAGV registers. SAGV block time from MTL_LATENCY_SAGV (0x4578C).
* Per level `skl_compute_plane_wm` (linear, 4 bytes/pixel, 512-byte blocks: `skl_compute_wm_params`, `skl_wm_method1/2`),
  transition WM (`skl_compute_transition_wm`, IPC on: `has_ipc`), SAGV WM0 (`tgl_compute_sagv_wm`: latency = block
  time + level 0) and its transition WM; `skl_wm_check_vblank` disables levels whose latency does not fit the vblank;
  `use_sagv_wm` (no HW SAGV WM, display ≥ 12, every enabled plane's SAGV WM0 enabled) puts SAGV WM0/trans into the
  level-0/TRANS registers (`skl_plane_wm_level`, `skl_plane_trans_wm`). Register encoding `skl_plane_wm_reg_val`
  (EN 31, lines 26:14, blocks 11:0).
* The cursor of pipe B stays off (WindowServer draws it); its DDB block is still reserved as Linux does.

### 2.5 CDCLK, PMDEMAND, bandwidth

* CDCLK never changes on BMG: `xe2hpd_cdclk_table` has one entry (652.8 MHz). `intel_pixel_rate_to_cdclk` with 2
  pixels per clock: each pipe's pixel rate ≤ 1305.6 MHz. `intel_bw_crtc_min_cdclk` (data rate x 10 / 512) is far below.
* PMDEMAND (`intel_pmdemand.c`): 0x45230 = QCLK_GV_BW 31:16 | VOLTAGE 14:12 | QCLK_GV_INDEX 11:8 | PIPES 7:6 | DBUFS 5:4 |
  PHYS 2:0; 0x45234 = REQ 31 | CDCLK MHz 30:20 | DDICLK MHz 18:8 | SCALERS 6:4 | PLLS 2:0. Two pipes: pipes 2,
  dbufs min(enabled slices, 3), phys 2 (every BMG PHY counts: none is TC), plls = phys + 1, ddiclk = max port clock,
  scalers 7, cdclk/voltage/QGV as they are. `intel_pmdemand_program_params`: wait REQ and DCPR_STATUS_1 bit 26 clear,
  write max(old, new) before the update, the new values after, set REQ, poll it clear (10 ms).
* Bandwidth (`intel_bw.c:xe2_hpd_get_bw_info`, `mtl_find_qgv_points`): derated BW per QGV point =
  min(53 GB/s, 70 % of channels x 4 bytes x dclk) from MTL_MEM_SS_INFO_GLOBAL / _QGV_POINT_*; two desktop monitors
  need a few GB/s, so this is a check that is printed, not a constraint in practice.

### 2.6 Power wells

`intel_display_power_map.c:xelpdp_power_wells_main`: PW_B is HSW_PWR_WELL_CTL2 (0x45404) index 6 (request bit 13,
state bit 12) with a fuse (`gen9_wait_for_power_well_fuses`: SKL_FUSE_STATUS 0x42000 bit 27 - (6 + 1) = 20). AUX power
is the AUX_CH_CTL power request bit (`xelpdp_aux_power_well_enable`), as probeEdid already does.

## 3. How macOS sees it

* Today: one `IntelXeFramebuffer` (matched on the PCI device, `IOFBDependentID` 0x4235383046420001, index 0). IOGraphics
  groups framebuffers into one controller by `IOFBDependentID` (`IOFBController::copyController`) and indexes them by
  `IOFBDependentIndex` (`IOFramebuffer::open`).
* The second output is a second IOFramebuffer instance: `IntelXeFramebufferPipe`, a subclass of IntelXeFramebuffer (so
  that NMXAccel, which watches `serviceMatching("IntelXeFramebuffer")` and slots heads by `IOFBDependentIndex`, sees it as
  head 1) that overrides every IOFramebuffer method itself and keeps its own small state. It is created by the primary
  (not by IOKit matching), attached to the same PCI device, with the same dependent ID and index 1. It answers
  `nvScanoutInfo` (CPU mapping + BAR2 address of its surface) for NMXAccel's copy path, has one mode (the sink's
  preferred valid EDID mode), the sink's EDID, a VBL timer on pipe B's frame counter, no hardware cursor, no VRR,
  no HiDPI, no flips.
* **Published at IntelXeFB's start, disconnected** (12 Nov; with the knob and a panel on pipe A), as Apple's drivers
  publish every framebuffer of a GPU at start and report a monitor as a connection change
  ([`DESIGN-multihead-accel.md`](DESIGN-multihead-accel.md) §2). It is then in `gStartedFramebuffers` before the first
  `IOFramebuffer::open`, so IOGraphics links it into head 0's controller (`openAllDependents`) and opens it with head 0;
  it exists before NMXAccel's arm and before WindowServer's first 0x980 (NMXAGDC lists it). IOGraphics' side, from the
  SDK headers (`IOKit/graphics/IOGraphicsTypes.h`, `IOFramebuffer.h`) and IOGraphics' `IOFramebuffer.cpp`:

  | what | disconnected | connected |
  |---|---|---|
  | `getAttributeForConnection(0, kConnectionCheckEnable / kConnectionEnable)` | 0: `updateOnline` says offline, no `IODisplayConnect`/`IODisplay` (`displaysOnline`), so no display for WindowServer (IOGraphicsLib `IOFBGetState`: online = `IODisplayForFramebuffer`) | 1 |
  | mode list (one mode) | ID 2, `appleTimingID` = `kIOTimingIDApple_0x0_0hz_Offline`: `updateOnline` takes it as `offlineMode`, `processConnectChange` sets it when the display goes (as IONDRVFramebuffer, whose `kConnectionCheckEnable` is `!searchOfflineMode()`) | ID 1, the sink's timing (detailed) |
  | size of that mode, `NVRMTiming` | the last sink's, or a placeholder CEA 1920x1080@60 before the first (`intelxeplan::offlineTiming`: NMXAGDC refuses K5 queries for a head without a sane timing) | the sink's |
  | `hasDDCConnect`, `kConnectionSupportsHLDDCSense` | false / unsupported | true with an EDID (`getDDCBlock`) |
  | aperture (`getApertureRange`) | none before the first sink; then the last surface | pipe B's surface |
  | `nvScanoutInfo` | `kIOReturnNotReady` | the surface |
  | `kConnectionChanged`, `kConnectionPostWake` | success: `connScan` already probed (and `processConnectChange` asks with a NULL value) | the same |
  | `IntelXeFBConnected` (property) | false | true |

  A change is reported through the connect interrupt (`registerForInterruptType(kIOFBConnectInterruptType)`, whose proc
  is IOFramebuffer's `connectChangeInterrupt`: it bumps the controller's `fConnectChange`, and IOGraphics' work runs
  `processConnectChange` on every framebuffer of the controller, so head 0 is re-probed too, as on a Mac when a monitor
  is plugged in). The interrupt is raised only for a real change (`intelxeplan::secondFbReport`: the connection changed,
  or a connected head got another sink); before IOGraphics' open there is no proc and the open reads the state.
* NMXAccel: with its multi-head knob (`nmxheads=1` / NVRAM `nmx-heads=1`) the boot arm gives head 1 a display pipe while
  it is disconnected (no framebuffer resource until its first mode change); knob off, the boot arm leaves it for
  `debug.nvaccelfb=3`, as it did for a late head ([`DESIGN-multihead-accel.md`](DESIGN-multihead-accel.md) §3, §5). If the
  start publish fails, the framebuffer is published when the pipe first lights (the old late path: composited after a
  WindowServer restart).
* Surface: a zeroed display buffer from IntelXe.kext (`intelxeDispVramAlloc`, contiguous, inside BAR2) mapped at a GGTT
  range IntelXe.kext hands out (`intelxeGgttAlloc`, flags 0).

## 4. Hot-plug of a second sink

The interrupt-driven hotplug (v10f) already decodes every SDE pin; `hpdWork` acts only on the panel's pin. With the
knob on, a long pulse on another pin re-runs the connector scan; a new DP sink lights pipe B (thread call, so the
hotplug work is not blocked by link training) and connects the second framebuffer (`configure` + `setConnected(true)`,
the connect interrupt), a lost one takes pipe B down and reports the framebuffer disconnected (kConnectionEnable 0 +
the connect interrupt); the framebuffer itself stays published. System sleep: pipe B goes down before the primary saves
its state (so the saved DBUF/MBUS/PMDEMAND are the single-pipe ones) and comes back after the primary's restore; the
framebuffer stays connected over the sleep (the monitor did not go), and after the wake the same sink is relit without
a report, another one is reported as a change, and a sink that is gone (or a pipe that will not light) is reported
disconnected. IntelXeFB's stop takes the pipe down and terminates the framebuffer, as before.

## 5. Out of scope

HDMI (native TMDS), MST, DSC, UHBR (128b/132b), 10 bpc, VRR/HiDPI/scaled modes/hardware cursor on pipe B, a GOP that
lit both pipes (only the first is adopted; pipe B is then left as the GOP set it and not touched), pipe A on a pipe
other than A with a second sink, shrinking pipe A's DDB, port sync / clone mode, audio.

## 6. Knob and dry run

* `-intelxefb_pipeb` or NVRAM `intelxefb-pipeb=1`: the second framebuffer published at start (disconnected), connector
  scan at start and on hotplug, pipe B lit for a DP sink and the framebuffer connected. Off (default): none of this code
  runs, no second framebuffer; pipe A's paths are unchanged.
* `debug.intelxefb_pipeb_mode` (string, any time): the hypothetical mode for the plan: empty = pipe A's own timing,
  `WxH@Hz` (CVT reduced blanking), `WxH@Hzn` (CVT normal blanking), `edid` (the scanned sink's choice); optional
  `:tc1`..`:tc4`/`:a` for the port.
* `debug.intelxefb_pipeb_plan` (string, read only, any time, no register writes, no AUX): pipe A recomputed from its
  live timing next to its live registers (M/N, linetime, DDB, WM levels, DBOX, MBUS), then everything the driver would
  program for pipe B (port/PHY/PLL table/link, timings, M/N, DDB split, WMs, DBUF/MBUS/DBOX/tracker, PMDEMAND before
  and after, power well, bandwidth/CDCLK checks) and the verdict. Its second line says whether the second framebuffer
  is published and connected.
* `debug.intelxefb_pipeb` (int, knob only): read = state, write 1 = light pipe B on the scanned sink now (connects the
  framebuffer), 0 = take it down (reports it disconnected).

## 7. One-monitor check: head 1 published, disconnected (nights)

The test machine has one monitor (on TC2). With both knobs on, head 1 must exist from the boot on, read as
disconnected everywhere, and leave the desktop alone: no phantom monitor, no extra desktop space, the main display
unchanged. New kexts installed (IntelXeFB.kext from `kernel/IntelXe/build.py`, NMXAccel.kext and NMXAGDC.kext from
`nullmoth/build/build-intelxe.sh kexts`), then:

```sh
sudo nvram intelxefb-pipeb=1 nmx-heads=1     # both knobs (boot-args -intelxefb_pipeb nmxheads=1 work the same)
sudo reboot                                  # wait for SSH / kern.boottime, log in at the login window
```

After the login (each command is short):

```sh
# 1. IntelXeFB: the second framebuffer is published, disconnected, nothing lit
sudo sysctl -n debug.intelxefb_pipeb_plan | sed -n 2p
#   knob -intelxefb_pipeb: on; second pipe: off; second framebuffer: published, disconnected
sudo sysctl -n debug.intelxefb_pipeb
#   0        (nothing lit; a negative errno only if a scan tried a sink)

# 2. ioreg: head 1 under the B580 with index 1, disconnected, the placeholder timing, no display under it
ioreg -r -c IntelXeFramebufferPipe -d 1 | grep -E '"(IOFBDependentIndex|IOFBDependentID|IntelXeFBConnected|IntelXeFBPipe)"'
#   "IOFBDependentID" = 4770781160257224705      (0x4235383046420001, head 0's)
#   "IOFBDependentIndex" = 1
#   "IntelXeFBConnected" = No
#   "IntelXeFBPipe" = 1
ioreg -r -c IntelXeFramebufferPipe -d 3 | grep -c IODisplayConnect      # 0: no display on head 1
ioreg -r -c IntelXeFramebuffer -d 3 | grep -c IODisplayConnect          # 1: both heads together, only head 0's monitor
ioreg -r -c IntelXeFramebufferPipe -d 2 | grep -c UserClient            # >= 1: WindowServer opened it (informational)

# 3. NMXAccel: head 1 has its own display pipe from the boot arm, idle
sudo sysctl -n debug.nmxaccel_heads
#   multi-head knob: 1 (...)
#   heads: published 0x3, registered 0x3, piped 0x3
#   display machine: 2 framebuffer(s), 2 pipe(s)       (with "pipe k -> head 0" and "pipe j -> head 1")
#   head 0: IntelXeFramebuffer ... -> armed (own display pipe)            (frames growing between two reads)
#   head 1: IntelXeFramebufferPipe id 0x... IOFBDependentIndex 1
#     monitor: none (disconnected: the head stays published, its pipe idles) (IntelXeFBConnected)
#     scan-out: does not describe itself (not lit, or disconnected)
#     caps: agdc-timing second-pipe-fb (copy only); cursor: software (WindowServer draws it)
#     frames 0; ...
#     -> armed (own display pipe), no monitor: idle until one is connected
#   late-head arm: nothing to register (published 0x3, registered 0x3) (retries 0)

# 4. NMXAGDC: WindowServer's first 0x980 already listed two heads
sudo sysctl -n debug.nmxagdc_heads
#   next 0x980 would list 2 head(s)
#   head 1: IntelXeFramebufferPipe id 0x..., in the live table, port 2
#     monitor: none (disconnected; the K5 timing below is IntelXeFB's placeholder or the last monitor's)
#     K5: link timing 1920x1080, 148500000 Hz pixel clock, 60.000 Hz; plane scaler 1920x1080 at 1:1 (0x10001)
sysctl -n debug.nvrmfb_agdc_maxfb            # 2
sysctl -n debug.nvrmfb_agdc_k5_link_mask     # 1 or 3: whether IOPresentment asked index 1 too (informational)

# 5. macOS: still exactly one display, the same main display
system_profiler SPDisplaysDataType | grep -c 'Resolution:'             # 1: one display
system_profiler SPDisplaysDataType | grep -E 'Resolution|Main Display|Online'
#   Resolution: 2560 x 1440 ... / Main Display: Yes / Online: Yes      (each once, as without the knobs)

# 6. the log of this boot
sudo dmesg | grep -E 'second framebuffer|multi-head|index 1|head 1 published'
#   IntelXeFB: second framebuffer published: pipe B, disconnected (placeholder mode), 1920x1080, 148500 kHz
#   IntelXeFB: second framebuffer: published as head 1, disconnected -- ...
#   NMXAccel:   index 1 is IntelXeFB's second framebuffer, published before the arm (no monitor): the boot arm gives it ...
#   NMXAccel:   sysctl: calling found_framebuffer(IntelXeFramebufferPipe index 1) ...  then "index 1 -> pipe ..." or
#               "index 1 already has pipe ... from the family's sweep"
#   NMX-agdc: auto: IntelXeFramebuffer head 1 published (no monitor): listed in the next 0x980 reply ...
```

Optional, the controller's power path with head 1 in it (about 10 s): `pmset displaysleepnow; sleep 5; caffeinate -u -t 2`
— the monitor wakes, head 0's frame count keeps growing, head 1 stays "no monitor", and the log shows
`second framebuffer: power 2 -> ...` lines and no `connect interrupt raised`.

Bad outcomes and what they mean: head 1 "registered, NO pipe of its own" (the family would not give a disconnected
framebuffer a pipe: a monitor plugged in later gets no frames); `system_profiler` listing a second display, or a second
desktop space (head 1 read as online); `debug.nvrmfb_agdc_maxfb` 1 (WindowServer asked before head 1 published). Back:
`sudo nvram -d intelxefb-pipeb; sudo nvram -d nmx-heads` and a reboot (or remove the boot-args).
