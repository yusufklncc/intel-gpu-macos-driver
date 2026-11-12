# Accelerating more than one display head (NMXAccel, NMXAGDC) — design

> Status (12 Nov 2026): implemented behind the multi-head knob (boot-arg `nmxheads=1`, NVRAM `nmx-heads=1`, or
> `debug.nmxaccel_heads_auto=1` at run time), default off, **untested with two heads**: the test machine has one monitor.
> With the knob off nothing new runs, except that the boot arm leaves IntelXeFB's second framebuffer (published at
> IntelXeFB's start with `-intelxefb_pipeb`) for `debug.nvaccelfb=3`, as it did a late head; with one head (no
> `-intelxefb_pipeb`) nothing new runs at all. The diagnostics `debug.nmxaccel_heads` and `debug.nmxagdc_heads`
> work with one monitor. The second head itself is IntelXeFB's ([`DESIGN-second-pipe.md`](DESIGN-second-pipe.md)).
> Since 12 Nov IntelXeFB publishes it at its start, disconnected (section 5, done); its one-monitor check is
> DESIGN-second-pipe.md §7.

## 1. How the accelerator binds to framebuffers today

* **One accelerator per GPU, several displays.** NMXAccel (`nullmoth/kexts/NVRM/accel/nvrm-accel.cpp`, an
  `IOGraphicsAccelerator2` on the B580's PCI device) has one display machine (`NVDisplayMachine`, an
  `IOAccelLegacyDisplayMachine`). The display machine owns one `IOAccelDisplayPipe` per IOFramebuffer it was handed with
  `found_framebuffer` (pipe table at display machine + 0x88, count at + 0x108; a pipe's framebuffer at pipe + 0x98).
  This is the shape nullmoth's NVIDIA driver was written for: `kNvMaxFB` = 5 heads (four NVIDIA heads and an Intel
  built-in panel), `populateAccelConfig` reports 5 framebuffers, and the Metal device is one per GPU, not per display.
* **Heads.** `fbPublished` (an `IntelXeFramebuffer` first-publish notification, so it also sees a head that publishes
  after NMXAccel started) slots each framebuffer by its `IOFBDependentIndex` into `fFBs[]`. Everything on the frame path
  is already per head: `pipeFramebuffer` maps a pipe back to its head by framebuffer pointer, `performTransaction`
  copies into *that* head's `nvScanoutInfo` surface, the async present state (`gNmxAs[head]`), the adaptive-sync window
  (`gNmxVrr[head]`) and the frame counters (`gCntIopOkHead[head]`) are per head.
* **The arm.** `nmxAutoArm` (boot1) waits for NMXAGDC's device control and for head 0 to go quiet, then writes levels
  1/3/4 through IntelXeFB's `intelxefbRunGated` (IOGraphics' system and controller gates, the order that does not
  deadlock against AppleGPUWrangler). Level 1 is `registerPendingLocked`: it walks indices 0, 1, 2, ... and calls
  `found_framebuffer` for every published head that has no pipe yet, stopping at the first index that has not published;
  then `display_mode_will_change`/`did_change` (the family enables the accelerator there). A head that publishes **after**
  that walk is only logged ("gets a pipe only when debug.nvaccelfb=3 is written again, and WindowServer composites it
  only after a WindowServer restart").

## 2. What IOGraphics, IOAccelerator and WindowServer expect of a multi-display GPU

* **IOGraphics** (SDK `IOKit/graphics/IOFramebuffer.h`, `IOGraphicsTypes.h`): the framebuffers of one controller share
  `kIOFBDependentIDKey` and are ordered by `kIOFBDependentIndexKey`; one IOFramebuffer stands for one CRTC/output. A
  display arriving or leaving is a *connection change* of an existing framebuffer: `kConnectionCheckEnable` /
  `kConnectionEnable` answered from the hardware, and `kIOFBConnectInterruptType` ("the display connection should be
  resensed") to make IOGraphics probe it again.
* **Apple's own drivers publish every framebuffer at start, connected or not.** On Intel Macs the `AppleIntelFramebuffer@0`
  .. `@N` nodes exist with nothing plugged in; AMD's controller publishes one framebuffer per display path the same way.
  The number of framebuffers on one GPU never changes at run time; what changes is their connection state. (An eGPU is
  not a counter-example: it is a new GPU with its own accelerator, AGDC and framebuffers.)
* **IOAcceleratorFamily2**: one accelerator per GPU; the display machine's start sweeps the GPU's framebuffers and makes
  a pipe for each (`IOAccelDisplayMachine::start`, `found_framebuffer`), and each pipe subscribes to its own
  framebuffer's notifications: `IOFramebuffer.h` reserves the notification group `kIOFBNotifyGroupID_AppleIOAccelDisplayPipe`
  (0x170), and `IOAccelDisplayPipe.h` has the receivers (`framebuffer_will_power_off`, `framebuffer_did_power_on`,
  `displayModeWillChange`/`DidChange`, `framebuffer_terminated`). So a head's mode changes, power and termination reach
  its pipe without NMXAccel's help once the pipe exists.
* **WindowServer / IOPresentment** builds its display pipes from AppleGraphicsDeviceControl command 0x980 (the list of
  framebuffers, NMXAGDC's reply) **when WindowServer starts**, then asks 0x921 (link configuration) and 0x711 (plane
  scaler, gamma, ... per framebuffer index). CoreDisplay logs "Unable to find display pipe for IOFB service" for a
  framebuffer that is not in that list, and never sends it a frame. nullmoth's field experience (the comment over
  `nvdaAgdcRescanForReply`): a monitor plugged in after boot was missing from the reply; with the rescan the reply is
  current, but only a WindowServer start asks for it again.
* **Consequence.** No restart is needed for a head that exists when WindowServer starts: plugging and unplugging its
  monitor is a connection change. A framebuffer that is *published* after WindowServer started can be armed on the
  accelerator at once (its pipe exists, frames would flow), but WindowServer composites it only after it rebuilds its
  display pipes (logout/login restarts WindowServer). That is the protocol's limit, not NMXAccel's; the way around it is
  to publish the head early (section 5).

## 3. What NMXAccel does with the knob on

* **Late heads** (`headsLate`, a thread call): a head (index ≥ 1) that publishes after the arm is registered 2 s later
  (never from inside the publish notification: that runs in the publisher's `registerService`, under IntelXeFB's pipe-B
  lock). Before: the head must go quiet (5 s) and describe its scan-out (`nvScanoutInfo`); otherwise it is retried every
  2 s for 30 s. Then, inside IOGraphics' gates through head 0's `intelxefbRunGated` (the second framebuffer is on the same
  controller), `registerPendingLocked` gives it its pipe. Mode `1` delivers the family-wide mode change the boot arm and
  `debug.nvaccelfb=3` deliver; mode `2` registers only and leaves the new pipe's mode changes to its own framebuffer
  notification (which of the two WindowServer wants is a two-monitor question). Never ungated: without the gate it is
  left for `debug.nvaccelfb=3`. The boot arm itself also kicks the late check once it is done, for a head that published
  while it ran.
* **Head 1 from the start** (12 Nov): with `-intelxefb_pipeb` IntelXeFB publishes its second framebuffer at its start,
  disconnected (`IntelXeFBConnected` false), so it is there for the boot arm. Knob on, the boot arm's walk registers it
  with head 0 (section 5); after the arm a disconnected head is registered without waiting for its scan-out (there is
  none to wait for). Knob off, the boot walk stops before an `IntelXeFramebufferPipe` (`nmxHeadBootSkips`, only while
  `nmxAutoArm`'s levels 1/3/4 run): it waits for `debug.nvaccelfb=3`, exactly as a late head did. (IOAcceleratorFamily's
  own sweep at NMXAccel's start may still give it a pipe if both heads published before NMXAccel started: that is the
  family's, not NMXAccel's, and `debug.nmxaccel_heads` shows it in the pipe table.)
* **The rule** is one pure function, `nmxHeadVerdict` in `nmx_heads.h`, shared by the late arm and the diagnostic and
  checked on the host (`tests/nmx_heads_test.cpp`, run by `build-intelxe.sh tests`): absent / armed (own pipe) /
  armed (own pipe) with no monitor (idle) / registered without a pipe / the pending boot arm takes it / the late arm
  takes it now / once its scan-out is up / manual (knob off: today's behaviour, also for the second framebuffer at the
  boot arm) / the accelerator is not armed at all (held, failed: nothing registers a head on its own) / blocked by a hole
  below it / terminated. Head 0 is never the late arm's. Inputs per head: published, registered, piped, scan-out,
  terminated, second-pipe framebuffer (`kNmxCapPipeFB`), and disconnected (`IntelXeFBConnected` false; absent means
  connected, as for head 0).
* **Termination**: a head ≥ 1 whose framebuffer terminates (IntelXeFB's stop takes the second framebuffer down) frees its
  slot and its published/registered/piped bits, so a new framebuffer at that index is registered again. Its reference is
  kept (a transaction in flight may hold the pointer), as `fbPublished` does for a displaced head 0. The pipe's own
  `framebuffer_terminated` is the family's.
* **Per-head capabilities**, read from the framebuffer's properties when it publishes (`nmxHeadCapsOf`); with the knob on a
  head ≥ 1 is never asked for what it lacks. Head 0 is never skipped, whatever its properties say.

| capability | head 0 (`IntelXeFramebuffer`) | head 1 (`IntelXeFramebufferPipe`) | what NMXAccel does for head 1 |
|---|---|---|---|
| flips (`IntelXeFBScanoutVersion`, `nmxScanout`) | yes | no | copy only: no flip, no "home" after a copy. Scan-out bindings are pinned and mapped by head 0's framebuffer at head 0's size, so they could never be head 1's anyway |
| pipe gamma (`IntelXeFBPipeGammaSize`, `nmxPipeGamma`) | yes | no | no table sent. NMXAGDC offers the 0x711 gamma capability only to a head with the property, so CoreDisplay applies gamma in its own GPU pass for head 1 |
| AGDC timing (`NVRMTiming`) | yes | yes (IntelXeFB hook, below) | NMXAGDC answers 0x921 and the plane scaler for index 1 |
| adaptive-sync stamps (`nmxVrrStamp`) | yes | no | asked once, refused, logged once (unchanged) |
| cursor | IntelXeFB's cursor plane (or software, `debug.intelxefb_hwcursor`) | software: `kIOHardwareCursorAttribute` 0 | nothing: WindowServer draws the cursor into head 1's composite |

* **Cursor and scaler per head.** The hardware cursor and its coordinate space (`fCurSpaceW/H` for a scaled HiDPI mode)
  belong to the framebuffer, and only head 0 has a cursor plane. The scaler state that matters to the accelerator is
  the plane-scaler capability NMXAGDC answers per head from that head's own `NVRMTiming`: the head's size at 1:1
  (0x10001, b580-hidpi finding 5), so WindowServer scales any scaled mode on the GPU and presents frames at the head's
  scan-out size. Per head, `debug.nmxaccel_heads` shows the last copied frame's source size against the scan-out size and
  counts mismatches (the "source 2560x1440 is not the scanout's" refusal), which is how a head whose scaling went to the
  pipe would show.
* **Display pipe capabilities.** `IOAccelDisplayPipeCapabilities` is one property of the accelerator (one device, one
  plane, two queued transactions); `IOAccelDisplayPipe::copyCapabilities` returns it for every pipe, which describes each
  pipe correctly. Whether IOPresentment wants one device entry per framebuffer instead is open (two monitors).

## 4. NMXAGDC: listing IntelXeFB's heads

* The 0x980 reply already lists every `IntelXeFramebuffer` under the B580 by `IOFBDependentIndex` (rescanned at most once
  a second when WindowServer asks, so a head published after boot is in the next reply), `portMask` bit 1 + i,
  `maxFramebuffers` = the number of heads without a hole. 0x921 and 0x711 are answered per index from that head's
  `NVRMTiming` (link port 1 + index), the gamma capability only for a head with `IntelXeFBPipeGammaSize`.
* A disconnected head is listed like any other (Apple's model: every framebuffer of the GPU is in the reply), with
  IntelXeFB's placeholder timing (CEA 1920x1080@60) or the last monitor's as its `NVRMTiming`, so 0x921/0x711 for its
  index are answered rather than refused (a refused link query was the single-head "0x80e" symptom). The dry run
  prints each head's monitor state.
* New: a head ≥ 1 publishing brings nothing up (the device control comes up from head 0; before, the second framebuffer
  logged "publishes no IntelXeFBAccelAllowed"), and the dry run `debug.nmxagdc_heads`: what the next 0x980 would list,
  scanned into a table of its own (the live table is not touched), each head's K5 answers (timing, plane scaler, gamma),
  whether it is in the live table yet (no: WindowServer has not asked since it published), and what IOPresentment asked.
* **IntelXeFB hook** (`kernel/IntelXeFB/IntelXeFB.cpp`, `IntelXeFramebufferPipe::configure`, a few lines): the second
  framebuffer publishes `NVRMTiming` (same layout as `publishTiming`: tag, pixel clock in Hz, h/v visible, sync start, sync
  end, total, refresh in mHz, sync polarities) and `NVRMFlipWaitUs` (a frame and a quarter). Without `NVRMTiming`,
  IOPresentment's 0x921 for index 1 was refused ("Unable to acquire capabilities ... 0x80e" was the single-head symptom
  of a missing link answer), and head 1's present wait fell back to 8 ms. Only under `-intelxefb_pipeb`: the second
  framebuffer exists nowhere else.

## 5. A second monitor without a WindowServer restart: head 1 published at start (done 12 Nov)

IntelXeFB publishes `IntelXeFramebufferPipe` at its start whenever `-intelxefb_pipeb` is on (and the panel is on pipe A),
disconnected: `kConnectionCheckEnable`/`kConnectionEnable` 0, the offline mode (`kIOTimingIDApple_0x0_0hz_Offline`) as
its one mode, a placeholder `NVRMTiming`, `nvScanoutInfo` answering not-ready, `IntelXeFBConnected` false. A sink
arriving or leaving is `configure` + `setConnected` plus the connect interrupt (IOGraphics' side, with the SDK header
and IOGraphics source references: [`DESIGN-second-pipe.md`](DESIGN-second-pipe.md) §3). That is Apple's model
(section 2):

* IOGraphics has both framebuffers in `gStartedFramebuffers` before its first open, so it links them as one
  controller's dependents and opens head 1 with head 0; head 1 is offline, so it gets no `IODisplay` and WindowServer
  shows no display for it.
* NMXAGDC's first 0x980 (WindowServer's start) lists both heads (`debug.nvrmfb_agdc_maxfb` 2), and K5 answers for
  index 1 from the placeholder timing.
* With the knob, NMXAccel's boot arm registers head 1 with head 0 (section 3): its display pipe exists before
  WindowServer starts. A head armed while disconnected gets no framebuffer resource (`initFramebufferResource` refuses a
  scan-out that does not describe itself, the family's own default) until its first mode change, which its pipe's
  framebuffer notification delivers once WindowServer sets the new monitor's mode.
* A monitor plugged in later is then a connection change WindowServer handles live; unplugging it reports the head
  disconnected and leaves it published. Knob off, NMXAccel leaves the head unregistered (`debug.nvaccelfb=3`), so a
  monitor is listed by WindowServer but gets frames only after that step.

Open (two monitors): whether IOPresentment re-reads the plane-scaler capability (0x711) when the monitor arrives or keeps
the placeholder's 1920x1080 from WindowServer's start; whether the family's `found_framebuffer` gives a disconnected
framebuffer a pipe (the one-monitor night check answers this one: head 1 "armed (own display pipe), no monitor" versus
"registered, NO pipe of its own"); mode 1 against mode 2 of the knob no longer matters for head 1 at boot (the boot
arm delivers the mode change itself).

## 6. Knobs and sysctls

| name | kind | default | meaning |
|---|---|---|---|
| `nmxheads=N` (boot-arg) / `nmx-heads` (NVRAM, "0".."2") | knob | 0 | 1: late heads registered with the mode change; 2: registered without it; 0: off |
| `debug.nmxaccel_heads_auto` | int, rw | the knob | the same at run time; writing 1 or 2 also runs the late check now |
| `debug.nmxaccel_heads` | string, ro, root | — | every head: class, registry id, index, scan-out, capabilities, cursor, frames, last source vs scan-out size, late registration, and the verdict; the display machine's pipe → head table; the late arm's last result. No register writes, no registration, no kernel pointers |
| `debug.nmxagdc_heads` | string, ro | — | NMXAGDC's dry run (section 4) |

## 7. Tests

* Host: `build-intelxe.sh tests` builds and runs `nmx_heads_test` (49 checks of the verdict, the boot-walk skip and the
  capability rule).
* One monitor (nights): `sudo sysctl -n debug.nmxaccel_heads` and `sudo sysctl -n debug.nmxagdc_heads` with the knob off
  (head 0 armed with its own pipe, head 1 absent, NMXAGDC would list one head with a 2560x1440 link timing); then
  `sudo sysctl -w debug.nmxaccel_heads_auto=1`, the same reads (knob 1, "nothing to register", head 0's frame count still
  growing, desktop unchanged), then `=0`. A boot with NVRAM `nmx-heads=1`: the same, and the log's `multi-head:` lines.
* One monitor with head 1 published at start (`intelxefb-pipeb=1` and `nmx-heads=1`): DESIGN-second-pipe.md §7 has the
  exact commands and expected output (head 1 "armed (own display pipe), no monitor", NMXAGDC listing two heads,
  `system_profiler` one display).
* Two monitors (open): `-intelxefb_pipeb` and `nmx-heads=1`; boot with one monitor, plug the second in after login:
  head 1's verdict goes "armed (own display pipe), no monitor" → "armed (own display pipe)", its frame count in
  `debug.nmxaccel_heads` grows, a second desktop appears without a logout (`debug.nvrmfb_agdc_maxfb` stays 2); unplug/replug
  of the second monitor (the head stays published, its scan-out answers not-ready while disconnected); both monitors at
  boot; a system sleep with both (no connection report at the wake for the same monitor); IntelXeFB's stop
  (termination frees the slot).
