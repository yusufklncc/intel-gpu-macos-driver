# Game mode in IntelXe Manager

> Design note with its date (2026-11-13, branch `osguard`): it records the plan and the state of that day, not the
> current status. The macOS update guard, built on the same branch: [`DESIGN-os-update-guard.md`](DESIGN-os-update-guard.md).

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

### A kernel knob for later (not implemented; a separate decision)

The helper exists because the floor needs root and must not outlive the app. A kernel sysctl could do both:
`debug.intelxe.slpc_boost` (int, `CTLFLAG_RW | CTLFLAG_ANYBODY`): a write of N asks for a floor of N MHz, clamped to
[the configured floor, `slpc_max` or RP0], held for `debug.intelxe.slpc_boost_lease_ms` (default 10000) after the
last write and dropped by a kernel timer, so a client that dies cannot leave it up; 0 drops it at once; a read
returns the effective floor and the lease left. Any user could then raise the clock within the user's own ceiling
for ten seconds at a time (a power cost, no security one). With it the app would write the sysctl every few seconds
while a game is in front and need no root helper and no stale-state handling. The seam: the app speaks to the
helper in three words (`game`, `desktop`, `quit`), and only the helper's `set_min` touches the clock; both are the
places such a knob would replace. `kernel/` is not changed on this branch.

## 3. Background work

| Candidate | Possible without disabling it system-wide? | Resumes? | Used |
|---|---|---|---|
| Spotlight: `mdutil -i off /` | no: it turns indexing off for the volume and is kept in the volume's index settings; an app that dies leaves it off | only when turned on again | no |
| Spotlight: `taskpolicy -b -p <pid>` on `mds_stores` (darwin background) | yes, per process, until the process ends | `-B` | no: `mds_stores` already runs at background priority, `mdworker_shared` processes are short-lived (a new one is not affected), and macOS's Game Mode already lowers background work |
| Time Machine: `tmutil disable` | no: persistent | only when enabled again | no |
| Time Machine: `tmutil stopbackup` | yes: cancels the backup that is running, nothing else | backupd runs the next scheduled backup as usual (hourly); nothing to undo | yes, opt-in: while a game is in front the helper stops a backup that is running, checking every 10 s |

## 4. How it works

- **Opt-in, per session.** The Power and app page's *Game mode* switch is off at every start of the app; its
  choices (the clock, Time Machine, the list of added games) are kept, the switch is not. Turning it on asks for the
  administrator password once (`run()` with `osascript`, as the app's other changes) and starts
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
- `IntelXeManager --selftest`: the games category, the list keys, the helper command's quoting, the state file.

## 7. What needs the test machine (the night run)

- The helper with the real kext: `sysctl -w debug.intelxe.slpc_min=2400` and `step=17` while Metal clients run, and
  the clock read back (`debug.intelxe.sensors`, the Monitor page) in a game and after it.
- The effect: the Minecraft benchmark's frame times (average and 1 % low) with game mode off and on, and idle power
  in the game's menu (the floor costs power when the game waits).
- The password dialog, the menu bar lines, the frontmost-app switching, Time Machine with a real backup.
