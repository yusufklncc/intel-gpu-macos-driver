# Game mode in IntelXe Manager

> Design note with its date (2026-11-13, branch `osguard`; the kernel lease of section 2 on branch `slpcboost` the
> same day): it records the plan and the state of that day, not the current status. The macOS update guard, built on the same branch: [`DESIGN-os-update-guard.md`](DESIGN-os-update-guard.md).

## 1. What macOS already does

macOS 14 and later turn **Game Mode** on by themselves when a game runs full screen. A game is an app whose
`Info.plist` puts it in the games category (`LSApplicationCategoryType` `public.app-category.games` or one of the
`public.app-category.*-games`); a game can opt out with `GCSupportsGameMode` false. Game Mode gives the game priority
on the CPU and GPU, lowers what background tasks may use, and doubles the Bluetooth sampling rate of controllers and
headphones. There is no public API to read whether it is on or to turn it on for another app.

Its GPU part works through Apple's own GPU drivers' scheduling. Nothing of it reaches a third-party accelerator: on
this stack Game Mode is CPU-side only, and it knows nothing of the Arc's clock. What this stack can add is the clock.

## 2. The GPU's lowest clock

The GuC's SLPC picks the GT clock between a floor and a ceiling. IntelXe has the knobs, in release builds:

- `debug.intelxe.slpc_min` and `debug.intelxe.slpc_max` (MHz, read-write, root): the floor and the ceiling SLPC is
  given (`kernel/IntelXe/xe_core.cpp` `slpcParams`; the floor defaults to 1200 MHz, Wa_14022085890, the ceiling to
  RP0, 2850 MHz on the B580; both are clamped to RPn..RP0);
- `debug.intelxe.step=17` sends them to the GuC now (`slpcStep`; step 17 is in the release list of `stepAllowed`,
  with the other steps "that set what Linux exposes at runtime"). It sends the floor, the ceiling, ignore-efficient,
  the strategies and the power profile, all from their current values.

The Power and app page's *GPU clock* writes the same floor to NVRAM for the next start (`intelxe-gpu-min-mhz`); game
mode changes it only in the running kernel. Choices: 2000 MHz, 2400 MHz (the default; the same as the *Performance*
setting), or the card's highest (3000, clamped to RP0 by `slpcParams`). It is never set above the user's ceiling
(`slpc_max`, the *Quiet* setting's 1800 MHz): `slpcParams` does not order the two, so the helper caps the floor.
A floor at or below the current one changes nothing.

What it should buy: frames that do not wait for the clock to rise after a short idle (frame-time spikes when a game
alternates light and heavy frames). **Not measured yet** (section 7). The *Fast GPU response for apps* switch (the
GuC low-latency hint, a file the Metal driver reads at an app's start) is a different knob, per queue; game mode
does not touch it: it is a persistent file, and its test notes a GPU fault after a long idle.

### The kernel lease: `debug.intelxe.slpc_boost` (built 2026-11-13, branch `slpcboost`)

The helper exists because the floor needs root and must not outlive the app. The kernel now offers a floor that needs
neither (`kernel/IntelXe/xe_slpcboost.h`, the glue in `xe_core.cpp` next to `slpcParams`):

- `debug.intelxe.slpc_boost` (int, `CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY`): a write of N (MHz) asks for a floor of
  N for **10 s** after the write; the next write renews it (and may change N); **0** ends it at once. A read returns the
  lease's floor as the GuC gets it (clamped, 0 = no lease). `debug.intelxe.slpc_boost_ms` (read-only, anybody): ms left.
- The GuC gets the higher of the lease's floor and the administrator's (`slpc_min`): a lease never lowers the floor.
  It is sent as Linux v7.2 `xe_guc_pc.c` `pc_set_min_freq` sends a minimum (`IGNORE_EFFICIENT_FREQUENCY` = min < RPe,
  then `GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ`), through `IntelXe::slpcMinSet`, which `slpcParams` (step 17, bring-up, GT
  reset, resume) uses too; `slpcParams` includes an active lease, so a GT reset during one keeps it.
- A kernel timer (thread call) ends the lease and sends `slpc_min` again, so a client that dies cannot leave it up.
- Counters: `debug.intelxe.stats`, line "slpc boost" (lease, ms left, the GuC's minimum, `slpc_min`, ceiling; grants,
  renewals, releases, expired, ended by sleep; refused for the rate and for a GT not ready; clamped; sends);
  `debug.intelxe.dump` has `slpc_boost` and `slpc_boost_ms`.

**Security bounds.** Any user may write it, so its effect is bounded to a power cost within the administrator's
settings:

| Bound | How |
|---|---|
| One lease for the GPU | a single state, not one per requester; the latest write wins (another user's write replaces the floor and the end) and a release by any user ends it for everyone; a client that renews every 5 s takes it again at its next renewal. Two users asking at once therefore get the later one's floor, never less than `slpc_min` and never more than the ceiling |
| Never above the ceiling | clamped to [RPn, `slpc_max` or RP0] (`slpcParams`' ceiling), again at every use, so lowering `slpc_max` during a lease lowers it too |
| Never below the administrator's floor | the GuC gets max(lease, `slpc_min`) |
| Rate limit | 10 requests (N > 0) per second, whoever makes them; `EBUSY` beyond (a renewal every few seconds needs one). A release (0) is always taken and does not count: it can only lower the clock (fixed 13 Nov after the night run, where a release right after the `EBUSY` was refused too and the lease ran its 10 s) |
| Not while the GT sleeps or resets | `EAGAIN` while suspended, in a GT reset, wedged or without SLPC (as `xe_guc_pc_set_min_freq_locked` answers `-EAGAIN`); a release is taken all the same, and a send the GT could not take is retried every 100 ms until the reset is over |
| Ends on sleep | `xeSuspend` ends it before the GuC's soft reset; the resume sends `slpc_min` |
| Bounded in time | 10 s after the last write; a timer, not the client, ends it |

Reads take no lock (a read must not wait for a bring-up); writes, the timer and the H2G run under `gDevLock`. Host
test: `kernel/IntelXe/tests/slpcboost_test.cpp` (bounds and clamp, grant, renew, the latest write wins, expiry,
release, the 5 s renewal pattern, the rate limit and a 1 kHz flood, a release after `EBUSY`, two users, a GT not
ready and the retry, sleep, `slpc_max` lowered during a lease).

**The app.** When the kext has the sysctl and the Time Machine option is off, game mode starts no helper and asks for
no password: the app writes the floor when a game comes to the front, every 5 s while it stays there, and 0 when it
leaves, when game mode is turned off and when the app quits (writes go through a serial queue off the main thread).
`EAGAIN` and `EBUSY` wait for the next renewal; any other error stops game mode with a message. After a sleep the next
renewal takes a new lease. With an older kext, or with the Time Machine option (`tmutil` needs root), the helper below
is used as before.

## 3. Background work

| Candidate | Possible without disabling it system-wide? | Resumes? | Used |
|---|---|---|---|
| Spotlight: `mdutil -i off /` | no: it turns indexing off for the volume and is kept in the volume's index settings; an app that dies leaves it off | only when turned on again | no |
| Spotlight: `taskpolicy -b -p <pid>` on `mds_stores` (darwin background) | yes, per process, until the process ends | `-B` | no: `mds_stores` already runs at background priority, `mdworker_shared` processes are short-lived (a new one is not affected), and macOS's Game Mode already lowers background work |
| Time Machine: `tmutil disable` | no: persistent | only when enabled again | no |
| Time Machine: `tmutil stopbackup` | yes: cancels the backup that is running, nothing else | backupd runs the next scheduled backup as usual (hourly); nothing to undo | yes, opt-in: while a game is in front the helper stops a backup that is running, checking every 10 s |

## 4. How it works

- **Opt-in, per session.** The Power and app page's *Game mode* switch is off at every start of the app; its
  choices (the clock, Time Machine, the list of added games) are kept, the switch is not. With the kernel lease (section
  2) that is all: no helper, no password. Otherwise turning it on asks for the administrator password once (`run()` with `osascript`, as the app's other changes) and starts
  `Resources/gamemode-helper.sh --run APP_PID UID FLOOR STOP_BACKUPS` as root in the background.
- **The helper** reads `debug.intelxe.slpc_min` (refuses to start without it), makes the pipe
  `/var/run/intelxe-gamemode/ctl` (mode 0600, owned by the user), writes its state next to it, and waits for one
  word per line: `game` sets the floor (and stops a Time Machine backup if asked), `desktop` puts the original floor
  back, `quit` ends. It also ends when the app's process is gone (checked every 2 s), on TERM or INT, and puts the
  floor back first (an EXIT trap). Arguments are fixed when it starts, through the password dialog; the pipe carries
  only those three words, so another process of the same user can at most switch between the two values the user
  approved. One helper at a time.
- **The app** follows the frontmost app (`NSWorkspace.didActivateApplicationNotification`). A game is an app in the
  games category or one the user added (bundle ID, or the executable's name for programs without a bundle: `java`
  for Minecraft). It sends `game` or `desktop` when that changes, and `quit` when game mode is turned off or the app
  quits. Full screen is not required (macOS's own Game Mode requires it; the clock helps a windowed game as much).
- **Undo.** Off: the switch, the menu bar item, quitting the app. Nothing is written to disk but the pipe and state
  in `/var/run`, which macOS empties at each start, as the kernel forgets the floor. The one case that leaves the
  floor raised is a helper killed with `kill -9` or crashed in a game: the app finds its state (a dead pid, phase
  `game`) and offers *Put it back* (`--restore`); a new helper restores it first too; a restart of macOS does as well.
- **The menu bar item** (opt-in, Monitor page): a line with the state ("Game mode: Minecraft in front"), *Turn game
  mode on…/off*, and *Count <app> as a game* for the last app in front that is not the Manager. Its icon turns into a
  game controller while a game is in front.

## 5. Files

`kernel/IntelXe/xe_slpcboost.h` and `xe_core.cpp` (the lease), `kernel/IntelXe/tests/slpcboost_test.cpp`,
`app/Resources/gamemode-helper.sh`, `app/Sources/GameMode.swift` (controller, Power and app section, menu lines),
`app/Sources/MenuBar.swift`, `app/Sources/main.swift` (the menu bar icon), `app/Sources/Settings.swift`,
`tools/report/collect-macos.sh` (the helper's state when one runs), `app/tests/test-gamemode-helper.sh`.

## 6. Tests

- `app/tests/test-gamemode-helper.sh` (about 30 s, no root, `sysctl` and `tmutil` stubbed in a throwaway root): bad
  arguments, no driver, refusal outside the test root without root; the pipe's mode; `game`, a repeated `game`,
  `desktop`, an unknown word; one helper at a time; the floor put back on `quit`, when the app process ends, on
  TERM; `kill -9` (the floor stays, `--status` says not running, `--restore` puts it back, a new helper restores
  first); the cap at the user's ceiling; a floor below the current one writes nothing; Time Machine left alone
  without the option and on the desktop, stopped in a game, and a backup that starts during the game stopped too.
- `IntelXeManager --selftest`: the games category, the list keys, the helper command's quoting, the state file; when
  the lease is used instead of the helper, the lease value in a game and on the desktop, which write errors pass, and
  no write without the sysctl.
- `kernel/IntelXe/tests/slpcboost_test.cpp`: the lease (section 2).

## 7. What needs the test machine (the night run)

- The lease as a user (no sudo): `sysctl -w debug.intelxe.slpc_boost=2400`, then `debug.intelxe.slpc_boost`,
  `slpc_boost_ms` and the stats line; the actual clock at idle (`debug.intelxe.sensors`) at or above 2400 MHz during
  the lease and back to the 1200 MHz floor 10 s after the last write (expiry) or at once after a write of 0 (release);
  a write of 3000 reads back 2850 (RP0) and, with `slpc_max=1800` set by root, 1800 (clamp); eleven writes in a second
  give `EBUSY` on the last, and a write of 0 right after it succeeds and reads back 0 (night of 13 Nov: refused before
  the fix); a sleep and wake end the lease (stats "ended by sleep"); `gt_reset=1` during a lease keeps it. The app
  with this kext: no password dialog, the floor in a game, released on the desktop.

- The helper with the real kext: `sysctl -w debug.intelxe.slpc_min=2400` and `step=17` while Metal clients run, and
  the clock read back (`debug.intelxe.sensors`, the Monitor page) in a game and after it.
- The effect: the Minecraft benchmark's frame times (average and 1 % low) with game mode off and on, and idle power
  in the game's menu (the floor costs power when the game waits).
- The password dialog, the menu bar lines, the frontmost-app switching, Time Machine with a real backup.
