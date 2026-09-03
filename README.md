# GameOptimizer

A Windows 11 desktop app (native Win32 GUI) that:

- **Scans your system** to determine which optimizations and tracking settings are actually
  active right now — nothing is remembered between runs, every state shown is read live.
- Toggles gaming/performance-oriented Windows settings.
- Disables Windows 11 tracking, telemetry and diagnostics, comprehensively, in one click.
- Shows a **dashboard** of system information and a live enabled/disabled summary of every option.
- Monitors known low-value background services and lets you disable/re-enable them.
- Monitors running processes with live CPU% / memory, flagging common background/bloat apps.
- Saves the current set of running processes/services as a named **profile** to re-apply later.

Every tweak here is safe to use with kernel-anticheat games like **Valorant** — see
[Anti-cheat compatibility](#anti-cheat-compatibility).

## Building

Requires a C compiler for Windows (MinGW-w64 GCC or MSVC). This repo was built and tested with
MinGW-w64 GCC 16 (WinLibs, UCRT runtime), installed via:

```bash
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
```

Then, from this directory:

```bash
mingw32-make
```

This produces `build\gameopt.exe`. `mingw32-make clean` removes build artifacts.

If you're using MSVC instead, compile all files in `src\*.c` with `/I include`, compile
`resources\app.rc` with `rc.exe`, and link against
`powrprof.lib advapi32.lib user32.lib gdi32.lib shell32.lib comctl32.lib psapi.lib kernel32.lib`
with `/SUBSYSTEM:WINDOWS`.

> **Note on the build:** MinGW's spec file always links its own `default-manifest.o`, which
> collides with the application manifest in `app.rc` (`multiple non-default manifests`). The
> Makefile shadows it with an empty object of the same name on the `-B` search path so only ours
> is linked. The manifest is what gives the app themed common controls and per-monitor DPI
> awareness, so it can't simply be dropped.

## Running

```bash
build\gameopt.exe
```

Most tweaks write to `HKEY_CURRENT_USER` and work without elevation. Tweaks that touch
`HKEY_LOCAL_MACHINE` or Windows services (marked in the **Admin** column) require running as
Administrator. On startup the app detects if it isn't elevated and offers to relaunch itself via
UAC — you can decline and still use the non-admin tweaks.

**Keyboard:** `Ctrl+Tab` / `Ctrl+Shift+Tab` cycle pages, `Ctrl+1`…`Ctrl+6` jump to one directly.

## How state detection works

Every option has an `apply()`, a `revert()`, and an **`is_active()`** that queries the real system:
a registry read, a service start-type query, a scheduled-task query, or the active power scheme.
On startup, on every page switch, and after every change, the app re-runs `is_active()` for all
options and displays that.

This means the display never drifts from reality. If you change one of these settings through
Windows Settings, another tool, or a Windows update reverts it, GameOptimizer shows the true
current state the next time you look. It also means a tweak that silently fails (usually from
missing Administrator rights) snaps back to OFF instead of falsely reporting success.

There is no saved tweak state — `%LOCALAPPDATA%\GameOptimizer\` is used only for saved
process/service profiles.

## Pages

| Page | What it does |
|---|---|
| **Dashboard** | System info (CPU/GPU/RAM/OS/disk/uptime/power plan), a live ON/OFF list of every option, one-click "Disable All Windows Tracking" / "Apply All Performance Tweaks" / "Revert Everything", and maintenance actions |
| **Performance** | Checkbox list of the performance tweaks below; click a row to toggle it |
| **Privacy** | Checkbox list of the tracking/telemetry tweaks below |
| **Services** | Known low-value services with live state + start type; disable/re-enable each |
| **Processes** | Live process list (CPU%/memory, auto-refreshing), flags known bloat, kill individually or all flagged |
| **Profiles** | Save/apply/delete process+service snapshots |

### Maintenance actions (Dashboard)

One-shot actions that aren't on/off settings: **SSD TRIM** (`Optimize-Volume -ReTrim`),
**flush DNS cache**, and **clean temp files**. Each asks for confirmation first.

## What each tweak actually does

### Performance
| Tweak | Effect | Admin? |
|---|---|---|
| High Performance power plan | Switches active power scheme (revert → Balanced) | No |
| Ultimate Performance power plan | Unlocks the hidden scheme via `powercfg -duplicatescheme` and activates it | Yes |
| Aggressive processor boost mode | `PERFBOOSTMODE=2` on the active plan | Yes |
| Disable USB selective suspend | Stops USB power-gating (mouse/keyboard input hitches) | Yes |
| Windows Game Mode | `HKCU...\GameBar!AutoGameModeEnabled` | No |
| Disable Game Bar / Game DVR | Disables background capture overhead | Yes |
| Boost game scheduling priority | MMCSS `Games` task priority + `SystemResponsiveness=0` + `Win32PrioritySeparation=38` | Yes |
| Hardware-accelerated GPU scheduling | `HwSchMode=2` (needs a **reboot**) | Yes |
| Disable network throttling index | Removes the 10pkt/s non-QoS throttle | Yes |
| Disable Nagle's algorithm / delayed ACK | `TcpAckFrequency=1` + `TCPNoDelay=1` per adapter | Yes |
| Disable NIC power saving | `PnPCapabilities=24` per adapter — avoids packet-loss spikes | Yes |
| Disable NIC interrupt moderation | `*InterruptModeration=0` per adapter (needs a **reboot**) | Yes |
| Best-performance visual effects | Disables animations/transparency | No |
| Disable SysMain (Superfetch) | Stops+disables the service | Yes |
| Disable Windows Search indexing | Stops+disables `WSearch` | Yes |
| Disable UWP background apps | `BackgroundAccessApplications!GlobalUserDisabled` | No |
| Remove startup app launch delay | `Explorer\Serialize!StartupDelayInMSec=0` | Yes |
| Disable hibernation + Fast Startup | `powercfg /hibernate off` | Yes |
| Disable dynamic tick | `bcdedit /set disabledynamictick yes` (needs a **reboot**) | Yes |
| Disable automatic driver search | `DriverSearching!SearchOrderConfig=0` | Yes |

### Tracking / telemetry / privacy
| Tweak | Effect | Admin? |
|---|---|---|
| Disable DiagTrack | Stops+disables the Connected User Experiences and Telemetry service | Yes |
| Disable telemetry ETW autologger | `AutoLogger-Diagtrack-Listener!Start=0` — stops it restarting at boot | Yes |
| Minimize diagnostic data | `AllowTelemetry=0` (capped at "Basic" on Home/Pro by Windows itself) | Yes |
| Disable WAP Push routing | Stops+disables `dmwappushservice` | Yes |
| Disable Advertising ID | Turns off the per-user ad ID | Yes |
| Disable Cortana | Policy block via Windows Search | Yes |
| Disable Bing / web results in Start | `BingSearchEnabled=0` + `DisableWebSearch=1` — keeps typed queries local | No |
| Disable Windows Recall / AI data analysis | `WindowsAI!DisableAIDataAnalysis=1` | Yes |
| Disable Windows Copilot | `TurnOffWindowsCopilot=1` | Yes |
| Disable Activity History | Stops activity feed collection/upload | Yes |
| Disable clipboard history & cloud sync | `AllowClipboardHistory`/`AllowCrossDeviceClipboard=0` | Yes |
| Disable tailored experiences | Stops diagnostic-data-based personalization | No |
| Disable Start menu ads & suggestions | The `ContentDeliveryManager` cluster + `DisableWindowsConsumerFeatures` | Yes |
| Disable Windows Spotlight | Stops rotating lock-screen content from Microsoft | Yes |
| Disable feedback notifications | Stops periodic feedback prompts | No |
| Disable telemetry Scheduled Tasks | Disables CEIP/Compat Appraiser/WER queue tasks etc. | Yes |
| Disable CEIP | `SQMClient\Windows!CEIPEnable=0` | Yes |
| Disable inking & typing personalization | `RestrictImplicitTextCollection`/`InkCollection=1` | No |
| Disable handwriting data sharing | Blocks handwriting error reports and sample uploads | Yes |
| Disable online speech recognition | `OnlineSpeechPrivacy!HasAccepted=0` | No |
| Disable location tracking | Sets system location consent to Deny | Yes |
| Disable Find My Device | `FindMyDevice!AllowFindMyDevice=0` | Yes |
| Block app access to diagnostic info | `LetAppsGetDiagnosticInfo=2` | Yes |
| Disable settings sync | Stops settings/themes/passwords syncing to your MS account | Yes |
| Disable Delivery Optimization P2P | `DODownloadMode=0` — no update uploads to other PCs | Yes |
| Disable Remote Assistance | `fAllowToGetHelp=0` | Yes |
| Disable Windows Error Reporting | Stops `WerSvc`, disables crash uploads | Yes |

Every tweak is reversible from the same page (toggle it again), and "Revert Everything" on the
dashboard undoes all of them.

## Anti-cheat compatibility

Everything in this tool is deliberately limited to power, scheduler, network-stack and
telemetry settings. **Nothing here touches the trust surface that kernel anti-cheats check.**
Specifically, GameOptimizer will never disable:

- **Memory Integrity (HVCI) or Virtualization-Based Security** — Vanguard (Valorant) requires
  these on Windows 11 and refuses to start without them.
- **Secure Boot / TPM** — also a hard Vanguard requirement.
- **Windows Defender / real-time protection** — tampering with it is a common ban-risk heuristic.
- **Driver signature enforcement or test signing.**

Guides that promise big FPS wins from those settings will get you locked out of (or banned from)
the games this tool is meant to optimize for, so they are omitted entirely rather than offered
with a warning.

## Service monitor

Lists services commonly unneeded for a gaming setup (SysMain, WSearch, DiagTrack, Xbox Live
services, Fax, RemoteRegistry, etc.), shows current run state and start type, and lets you
disable+stop or re-enable each one.

## Process monitor

Snapshots running processes, samples CPU usage, and displays PID / name / CPU% / working-set
memory sorted by CPU, refreshing automatically every few seconds. Known low-value background apps
(OneDrive, Teams, Widgets, Game Bar helper, Spotify, etc.) are flagged; you can kill a selected
process or close all flagged ones at once.

> Without Administrator rights, CPU% and memory read as 0 for processes owned by other users or
> by the system — Windows denies the query. Run elevated for full numbers.

## Process/Service profiles

A profile is a snapshot of what's running at the moment you save it:

- **Save**: records the unique executable names of every running process, plus which of the
  "known unneeded services" are currently `Running`, into
  `%LOCALAPPDATA%\GameOptimizer\profiles\<name>.profile`.
- **Apply**: compares that snapshot to what's running *now*. Anything currently running that
  wasn't in the snapshot gets closed — processes via `TerminateProcess`, matched services via
  stop+disable. It shows a full preview (exact PIDs/names/services) and asks for confirmation
  before touching anything; it never starts things that are missing.
- **Protection**: a fixed list of core OS/session processes and security software (`csrss.exe`,
  `winlogon.exe`, `lsass.exe`, `svchost.exe`, `explorer.exe`, `dwm.exe`, Windows Defender, the
  optimizer itself, etc.) is never closed by a profile. Only the curated Service Monitor list is
  ever touched — arbitrary Windows services are left alone.

Typical use: launch your game + Discord + whatever else you want, save that as a "Gaming"
profile, then next time hit apply to close everything else (browser tabs, chat apps, launchers).

## Notes

- HAGS, dynamic tick and NIC interrupt moderation require a reboot before they take effect.
- `AllowTelemetry` cannot go below "Basic" on Windows 11 Home/Pro — level 0 only fully applies on
  Enterprise/Education/Server editions. The setting is still written and detected correctly.
- The "Disable telemetry Scheduled Tasks" state is detected from the Microsoft Compatibility
  Appraiser task as a stand-in for the group, since querying all ten via `schtasks.exe` on every
  refresh would visibly stall the UI. Applying/reverting always covers all ten.
- This tool changes system and per-user registry settings and service configuration. Everything
  it does is reversible through the same UI.
