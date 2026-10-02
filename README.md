# fansctl

[![License: AGPL v3](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/askender/fansctl)](https://github.com/askender/fansctl/releases)
[![Platform](https://img.shields.io/badge/platform-Apple%20Silicon-lightgrey.svg)](#known-smc-facts)

[中文文档](README.zh-CN.md)

Fan and temperature control for Apple Silicon Macs, field-tested on a
MacBook Pro 16" M1 Max (MacBookPro18,4). **One binary, two faces**: run it
with no arguments and it becomes a menu-bar app (forks to the background,
never occupies your terminal); pass a command and it is a CLI. Zero
third-party dependencies — links only system frameworks
(IOKit / CoreFoundation / AppKit / Security).

The menu-bar UI is bilingual (English / 中文): it follows your system
language and can be switched at any time from the menu. CLI output and
logs are Chinese for now. Version: `fansctl version` (currently 1.3.0).

## Build & install

```sh
make                # build ./fansctl
make install        # install to ~/.local/bin (rm-then-cp: AMFI quirk)
make install-root   # install setuid-root helper to /usr/local/bin/fansctl-root (one password)
make test           # smoke test (read-only, never touches fan state)
make restart        # restart the menu bar (relaunched by the LaunchAgent)
```

**Invariant**: after changing privileged logic (`__apply`/`__smart`/`__hold`),
`make` + `make install` must be followed by `make install-root` to sync the
helper; pure UI/read-only changes do not need it.

## CLI

```
fansctl               start the menu-bar app (forks to background)
fansctl fans          list fan RPMs (read-only)
fansctl status        current/target RPMs and mode (read-only)
fansctl temps         all temperature sensors (read-only)
fansctl power         power survey (read-only): system draw, input, adapter, USB, top processes
fansctl dump          dump all SMC keys (exploration)
fansctl watch [sec]   refresh loop (default 2 s)
sudo fansctl set <rpm> [N]   set RPM (no N = all fans)
sudo fansctl max [N]         full speed
sudo fansctl auto [N]        back to automatic
sudo fansctl smart [lo hi]   smart curve (default 40~80°C; Ctrl+C or smart stop exits and restores)
sudo fansctl smart stop      stop smart mode (incl. instances started from the menu bar)
sudo fansctl hold [°C]       thermostat mode, PI closed loop on the hottest sensor (default 70°C,
                             mutually exclusive with smart)
sudo fansctl hold stop       stop thermostat mode (incl. menu-bar instances)
```

## Menu bar

- Status item: `hottest°C|max rpm (krpm)`, refreshed every 2 s
- First row: hottest sensor / curve % or thermostat target + system power draw
  (SMC key `PSTR`, same source as ioreg SystemPowerIn; auto-hidden on machines
  without it). While charging, the battery component (Amperage×Voltage) is
  subtracted and shown separately, e.g. `34W+10W chg` = 34 W system + 10 W
  going into the battery
- **Max / Auto / Smart / Thermostat** — four mutually exclusive states with
  checkmarks; clicking any control first takes over (stops a running
  Smart/Thermostat daemon), then applies
- **Smart thresholds** submenu: 60~95 (default) / 55~95 / 45~85 / 40~80,
  persisted in NSUserDefaults, hot-updatable while running
  (SIGUSR1 + pidfile, control never interrupted)
- **Thermostat target** submenu: 65 / 70 (default) / 75 / 80°C, likewise
  hot-updatable
- **Language** submenu: 中文 / English — defaults to the system language,
  switching takes effect immediately and persists
- The dropdown reopens automatically after an action; the timer keeps
  refreshing while the menu is open (NSRunLoopCommonModes)
- Quit (⌘Q): if no daemon is running and fans are in manual mode, auto is
  restored via the helper before quitting (no orphan RPM); quitting the UI
  never kills a running daemon
- Single-instance guard: `/tmp/fansctl.bar.pid`

## Smart mode

Control input = the hottest of all temperature sensors (conservative).
T ≤ low threshold → hand control back to the system (fans may stop);
between thresholds → linear interpolation from the baseline to full speed;
≥ high threshold → full speed; 1°C hysteresis prevents flapping.
Baseline = the system's target RPM at the moment of takeover; if the fan was
already in manual mode (stale session), it falls back to the minimum RPM.
State pidfile `/tmp/fansctl.smart.pid` = `pid low high`, shared between CLI
and menu bar. SIGTERM exits gracefully (restores auto); log:
`/tmp/fansctl.smart.log` (includes the pid that sent the stop signal, useful
for debugging unexpected exits).

Robustness:

- **Wake handoff** — a stretched tick (continuous clock
  `mach_continuous_time`, which keeps running during sleep) detects a system
  wake: control is handed back for 5 s while sensors stabilize, then retaken
  on a fresh baseline
- **MODE_REASSERT** — every tick verifies manual ownership and the target
  RPM; if an external tool or the system overwrote them (switched back to
  auto, replaced the target), fansctl retakes control or rewrites the target
  immediately
- **Write backoff** — when the SMC is busy (0x82, thermal controller
  occupied), retries with exponential backoff for ~1.5 s instead of hammering
- **smart stop waits for cleanup** — returns only after the daemon has
  restored auto and cleared its pidfile, so the caller's immediately
  following set is not overridden; reports failure honestly on timeout
- SMC read failures reopen the connection and retry 3 times (the connection
  can be briefly invalid right after wake)

## Thermostat mode

The closed-loop counterpart to Smart mode (an open-loop curve): give it a
target temperature and a PI controller holds the hottest sensor there.

- Velocity-form PI: ΔRPM = 25×Δerror + 8×error − 150×temperature trend
  (per second), increment clamped to ±250 rpm — no integral windup, the
  trend term damps overshoot
- Dead band: holds RPM while |error| < 0.3°C and the temperature is steady
- Hands back to the system 3°C below target (fans may stop), retakes within
  1.5°C (hysteresis)
- Fully mutually exclusive with Smart mode: starting either takes over the
  other's control first
- State pidfile `/tmp/fansctl.hold.pid` = `pid target°C`; `hold stop` waits
  for cleanup; log `/tmp/fansctl.hold.log`; wake handoff and MODE_REASSERT
  identical to Smart mode

## Power survey

`fansctl power` prints the full power picture in one shot (read-only,
no root needed):

- **System power** — SMC key `PSTR` (second-level real-time, same source as
  the menu bar's first row); the charging component is split out while
  charging
- **Power input** — AppleSmartBattery telemetry
  `SystemVoltageIn × SystemCurrentIn`: the actual power entering the port.
  This telemetry refreshes roughly once a minute; trust PSTR for short-term
  dynamics
- **Adapter** — rated watts and PD-negotiated voltage from `AdapterDetails`
- **USB devices** — each device's declared 5V current draw (IOUSBLib reads
  bMaxPower from the config descriptor; USB2 units are 2 mA, USB3+ are
  8 mA); self-powered devices are marked
- **Top power processes** — sorted by instantaneous CPU% (two samples
  0.4 s apart), plus average% since launch (cumulative ÷ lifetime — a
  statistical value immune to short-task spikes), cumulative CPU time,
  memory/%MEM/PID. High now + low average = a transient spike
  (thumbnail/window rendering); both high = a real drain. macOS limitation:
  an unprivileged process can only read its own user's processes (system
  processes like WindowServer need `sudo fansctl power`); per-process GPU
  usage has no public API

Limitations: macOS exposes no public API for the *actual* power drawn by
peripherals or for PD negotiation toward them; the USB side only has
declared values (for ground truth, use an external power meter).

## Passwordless control & security model

`/usr/local/bin/fansctl-root` (root:wheel 4755) is a copy of this binary;
the menu bar runs privileged actions through it without a password.
**Whitelist only**: `__apply max|auto|set`, `__smart lo hi | stop | thresh`,
`__hold °C | stop | target`, `smart stop`. Every argument is range-checked;
there is no shell and no environment expansion — the helper cannot be used
as a generic privilege-escalation vector. If the helper is not installed,
the menu bar falls back to an `__ask` subprocess using
AuthorizationExecuteWithPrivileges (a password prompt each time).

## Launch at login

`~/Library/LaunchAgents/local.fansctl.bar.plist` (a copy lives in the repo):

```sh
# first, replace /Users/USERNAME in the plist with your username
cp local.fansctl.bar.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.fansctl.bar.plist
```

RunAtLoad + automatic restart on crash (SuccessfulExit=false); quitting from
the menu does not respawn it.

**Boot-time safety net** (`make install-restore`, one password): the
system-level LaunchDaemon `/Library/LaunchDaemons/local.fansctl.restore.plist`
runs `fansctl-root __apply auto` once as root at every boot — if the SMC was
left in manual mode after a crash or power loss, fans are restored to auto
immediately instead of being stuck at their last RPM.

## Known SMC facts (MacBook Pro 16" M1 Max, macOS 15)

- SMCKeyData must be 80 bytes (Apple Silicon), selector 2
- FNum=2; Mn=1200, Mx=5779/6241; mode keys F0md/F1md (ui8, 1=manual),
  target keys F0Tg/F1Tg (`flt ` = f32 little-endian)
- No Ftst and no FS! key — no unlock procedure needed
- On Apple Silicon, fans can stop completely when cold: Ac/Tg = 0 is a real
  state, not a bug

## License

AGPL-3.0-or-later (see [LICENSE](LICENSE)). Anyone may freely use, modify
and redistribute this project; products based on it (including deployments
as network services that distribute no binaries) must open-source their
derivative code under AGPL-3.0. **Commercial use is allowed; closed-source
commercial use violates the license** — if you use it in a commercial
product, the author would appreciate hearing from you.

## Acknowledgements

Three mechanisms were inspired by
[TomEageer/fanctl](https://github.com/TomEageer/fanctl) (a Python
implementation, MIT-licensed), independently re-implemented in C here:

- Boot-time restore (LaunchDaemon)
- Robustness: wake handoff / mode reassert (MODE_REASSERT) / write backoff
- Thermostat mode: PI closed loop on a target temperature (with trend damping)

Thanks to the original author for open-sourcing their work.
