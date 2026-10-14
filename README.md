# GameOptimizer

A Windows 11 desktop app (native Win32 GUI) that:

- **Scans your system** to determine which optimizations and tracking settings are actually
  active right now — nothing is remembered between runs, every state shown is read live.
- Toggles gaming/performance-oriented Windows settings.
- Disables Windows 11 tracking, telemetry and diagnostics, comprehensively, in one click.
- Shows a **dashboard** of system information and a live enabled/disabled summary of every option.
- Monitors known low-value background services and lets you disable/re-enable them.
- Monitors running processes with live CPU% / memory, flagging common background/bloat apps.
- **Reserves CPU cores for Valorant** by confining background processes to a couple of logical
  processors, without ever touching the game process itself.
- Saves the current set of running processes/services as a named **profile** to re-apply later.

Every tweak here is safe to use with kernel-anticheat games like **Valorant** — see
[Anti-cheat compatibility](#anti-cheat-compatibility).

It's still a single native `.exe` with no runtime dependencies. It idles at near-zero CPU, and
the live meters and process list only sample while their page is on screen.

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

This produces `build\gameopt.exe`. `mingw32-make clean` removes build artifacts. Header
dependencies are tracked, so editing a header rebuilds exactly what includes it.

> **If `gcc` fails with "The requested operation requires elevation":** a compatibility flag
> (`RUNASADMIN`) is set on that one `gcc.exe`. The identical target-prefixed driver isn't
> affected, and the Makefile routes `windres` through the same compiler:
>
> ```bash
> mingw32-make CC=x86_64-w64-mingw32-gcc
> ```

If you're using MSVC instead, compile all files in `src\*.c` with `/I include`, compile
`resources\app.rc` with `rc.exe`, and link against
`gdiplus.lib dwmapi.lib uxtheme.lib powrprof.lib advapi32.lib user32.lib gdi32.lib shell32.lib
comctl32.lib psapi.lib kernel32.lib` with `/SUBSYSTEM:WINDOWS`.

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
`HKEY_LOCAL_MACHINE` or Windows services (marked with an **Admin** badge) require running as
Administrator. Without elevation those switches show a lock, and a banner at the top offers a
one-click **Restart as administrator**. The banner doesn't block startup, and you can dismiss it
and keep using the non-admin tweaks.

**Keyboard:**

| Keys | Action |
|---|---|
| `Ctrl+1`…`Ctrl+6`, `Ctrl+Tab` / `Ctrl+Shift+Tab` | Switch pages |
| `Ctrl+F` | Search the current page |
| `F5` | Re-scan / refresh the current page |
| `Space` or `Enter` | Toggle the selected tweak, or collapse/expand a group |
| `Esc` (in a search box) | Clear the search |
| `Del` | End the selected process / delete the selected profile |
| `Shift+F10` or the menu key | Context menu for the selected row |

## The interface

- **Sidebar navigation** with live badges: the optimization score, `29/33`-style counts per
  category, running low-value services, and the number of saved profiles.
- **Nothing freezes.** Every slow operation runs on a background thread with a progress bar in
  the status bar. That covers PowerShell, `bcdedit`, service stops, TRIM, temp cleanup and the
  startup scan, so the window stays responsive the whole time. The old build blocked the UI for
  each of these, and the process list stalled it for 300 ms every refresh.
- **Undo.** After any tweak or service change the status bar says exactly what happened and
  offers **Undo**, which puts back precisely the settings that changed. That includes side
  effects, like one power-plan tweak switching the other off.
- **Restart tracking.** HAGS, MPO, dynamic tick, the TSC timer and NIC interrupt moderation only
  take effect after a reboot. When one differs from its state at launch, those rows show
  *Restart pending* and a banner lists what's waiting, with a **Restart now** button (it always
  asks first).
- **Search and filters everywhere.** Filter tweaks by On / Off / Needs restart / Needs admin,
  services by Running / Disabled, and processes by known background apps / pinned.
- **Grouped tweaks.** Performance and Privacy are split into collapsible groups (Power & CPU,
  Network, AI & voice, …), each with its own "x of y on" count and a detail card for the
  selected tweak.
- **Clear confirmations.** Destructive actions use themed dialogs that state the exact count
  ("Turn on 4", "Close 7"), list what will be affected, and note what needs admin or a restart.
- **Light and dark themes.** It follows the Windows setting by default; click the theme button
  at the bottom of the sidebar to pin Dark or Light. The title bar, scrollbars and context menus
  follow along.
- **High-DPI aware.** Fonts, spacing and icons scale per monitor (the old build hard-coded pixel
  sizes, so it was tiny at 150%). On small or high-scaling screens the dashboard reflows and
  scrolls, and lists give up their detail pane before they clip.
- **Remembers your window.** Size, position, last page and theme are restored on the next
  launch.

## How state detection works

Every option has an `apply()`, a `revert()`, and an **`is_active()`** that queries the real system:
a registry read, a service start-type query, a scheduled-task query, or the active power scheme.
On startup, on **Re-scan** (`F5`), and after every change, the app re-runs `is_active()` for all
options in the background and displays that.

This means the display never drifts from reality. If you change one of these settings through
Windows Settings, another tool, or a Windows update reverts it, GameOptimizer shows the true
current state the next time you look. It also means a tweak that silently fails (usually from
missing Administrator rights) snaps back to OFF instead of falsely reporting success.

There is no saved tweak state. `%LOCALAPPDATA%\GameOptimizer\` holds saved process/service
profiles and `config.ini`, which stores only window placement, the last page and the theme.

## Pages

| Page | What it does |
|---|---|
| **Dashboard** | Optimization score ring, live CPU and memory graphs, system info, one-click tiles (Boost performance / Block tracking / Focus on Valorant / Revert everything), maintenance actions, and **Copy report** (a plain-text summary of the PC and every tweak's state, for pasting anywhere) |
| **Performance** | Grouped, searchable switches for the performance tweaks below; click a switch (or press Space) to toggle |
| **Privacy** | The same for the tracking/telemetry tweaks below |
| **Services** | Known low-value services with friendly names, live state and start type; multi-select to disable or re-enable several at once |
| **Processes** | Every running process with live CPU/memory/cores, sortable columns, known-bloat flags, Valorant status and CPU core reservation |
| **Profiles** | Save/apply/delete process+service snapshots, with a preview of what each keeps running |

### Maintenance actions (Dashboard)

One-shot actions that aren't on/off settings: **SSD TRIM** (`Optimize-Volume -ReTrim`),
**flush DNS cache**, and **clean temp files** (reports how much space it freed). TRIM and
cleanup ask for confirmation first, and all three run in the background.

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
| Force the invariant TSC timer | `bcdedit /set useplatformclock no` — undoes HPET being forced on by OEM images and old tweak guides (needs a **reboot**) | Yes |
| Disable automatic driver search | `DriverSearching!SearchOrderConfig=0` | Yes |
| Launch Valorant at High CPU priority | IFEO `PerfOptions!CpuPriorityClass=3` + `IoPriority=3` for `VALORANT-Win64-Shipping.exe` | Yes |
| Disable CPU core parking | `CPMINCORES=100` on the active plan — no core has to be woken mid-frame | Yes |
| Disable CPU power throttling | `PowerThrottling!PowerThrottlingOff=1` — stops threads Windows decides are "background" being clocked down | Yes |
| Disable fullscreen optimizations | `GameDVR_FSEBehaviorMode=2` + the FSE honor flags — real exclusive fullscreen instead of borderless emulation | No |
| Disable mouse acceleration | `MouseSpeed`/`MouseThreshold1`/`MouseThreshold2=0`, pushed live via `SPI_SETMOUSE` | No |
| Disable Sticky/Filter/Toggle Keys shortcuts | Clears bit `0x04` of each `Flags` value — the feature stays in Settings, the popup stops stealing focus | No |
| Disable Multi-Plane Overlay (MPO) | `Dwm!OverlayTestMode=5` — the standard fix for DWM overlay flicker/stutter (needs a **reboot**) | Yes |
| Disable toast notifications | `PushNotifications!ToastEnabled=0` | No |
| Disable Widgets / News and Interests | `Dsh!AllowNewsAndInterests=0` — stops the Widgets host process | Yes |
| Disable Automatic Maintenance | `Schedule\Maintenance!MaintenanceDisabled=1` — no defrag/scan/update bursts mid-match | Yes |
| Disable Program Compatibility Assistant | Stops+disables `PcaSvc`, which inspects every process launch | Yes |
| Disable Edge startup boost & background mode | `BackgroundModeEnabled`/`StartupBoostEnabled=0` — Edge stops staying resident with no window open | Yes |

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
| Disable cloud content search & history | `IsMSACloudSearchEnabled`/`IsAADCloudSearchEnabled`/`IsDeviceSearchHistoryEnabled=0` | No |
| Disable app launch tracking | `Start_TrackProgs=0` — Windows stops recording which apps you open | No |
| Disable voice activation / wake words | `AgentActivationEnabled=0`, including on the lock screen | No |
| Stop websites reading your language list | `HttpAcceptLanguageOptOut=1` — removes a fingerprinting signal | No |
| Disable Microsoft Edge telemetry | `MetricsReportingEnabled`, `SendSiteInfoToImproveServices`, `PersonalizationReportingEnabled`, `UserFeedbackAllowed`, `DiagnosticData` all `0` | Yes |
| Disable Wi-Fi Sense / open hotspot auto-connect | `AutoConnectAllowedOEM=0` + the `WiFiSense*` feature flags | Yes |

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

The same rule shapes how the Valorant-specific features work. The tool never reads, writes or
injects into the game's memory, and it never modifies the game's own affinity:

- **Core reservation** moves *background* processes out of the way and leaves the game alone.
- **Launch priority** goes through Image File Execution Options → `PerfOptions`, which is the
  documented way to ask the Windows loader to create a process at a non-default priority. That
  is not the IFEO `Debugger` value: nothing is attached, substituted or injected.
- Raising the priority of an already-running Valorant is attempted with `SetPriorityClass` — the
  same API behind Task Manager's "Set priority" menu. Vanguard usually denies the handle, and
  when it does the tool reports the denial rather than working around it.

## Service monitor

Lists services commonly unneeded for a gaming setup (SysMain, WSearch, DiagTrack, Xbox Live
services, Fax, RemoteRegistry, etc.) with their Windows display names, current run state and
start type. Select one or several and disable+stop or re-enable them. Re-enabling restores the
start type a service had before this session disabled it, so an *Automatic* service comes back
as Automatic instead of Manual. **Undo** reverses the last change, including restarting services
that were running.

## Process monitor

Lists every running process with PID, CPU% (with an inline bar), working-set memory and allowed
core count. It refreshes every 2 seconds while the page is visible; **Live** pauses it. CPU% is
measured between refreshes rather than by sleeping, so the window never stalls, and your scroll
position and selection survive each refresh. Click a column header to sort.

Known low-value background apps (OneDrive, Teams, Widgets, Game Bar helper, Spotify, etc.) are
flagged. You can end a selected process (`Del`) or close all flagged ones at once, and the right-
click menu adds **Open file location**, **Copy name** and **Copy PID**. Rows Windows won't let a
standard user read are marked *Limited access* instead of showing misleading zeros.

### Reserving CPU cores for Valorant

**Focus CPU on Valorant** confines every ordinary background process to the first couple of
logical processors, leaving the rest of the CPU with nothing else scheduled on it. On a 16-thread
machine that means background work gets 2 cores and Valorant effectively owns the other 14.

The important part is what it *doesn't* do: **Valorant's own affinity is never modified.** The
game keeps the full core mask, and the scheduler puts its threads on the cores where nothing
else is allowed to run. This is deliberate — a kernel anti-cheat will refuse a write handle to
the game process anyway, so the technique works by moving everything *else* out of the way.

- Background work is parked on the **lowest** cores, because core 0 and its SMT sibling already
  absorb most driver DPC/ISR work and are the least attractive cores for a game.
- The reserved set scales with the machine: roughly one core per eight, minimum 2, and always at
  least 2 cores left over. Below 4 logical processors the feature declines to run.
- Left untouched: core Windows/session processes, Windows Defender, **Vanguard and the Riot
  client**, GPU and audio driver hosts, and OBS.
- Nothing is written to disk. Affinity lives in the process, so it resets when that process
  restarts, and **Release CPU cores** puts everything back immediately. A process counts as
  *pinned* only if it carries exactly the focus mask. Apps that restrict their own affinity
  (some games and launchers do) are neither counted nor touched by Release.
- Processes started *after* a focus pass are unaffected — run it again once everything's open.

The app also tries to raise the running Valorant process to **High** priority through
`SetPriorityClass`, the same documented call Task Manager uses. If Vanguard denies that handle
the app says so plainly and points you at the **Launch Valorant at High CPU priority** tweak on
the Performance page, which asks the Windows loader to start the game at High priority instead —
applied at launch, before the anti-cheat locks the process down.

> Without Administrator rights many background processes can't be opened for modification; the
> result dialog reports how many were skipped.

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

The Profiles page shows when each profile was saved and exactly which apps and services it keeps
running. Saving over an existing name asks before replacing it.

## Notes

- HAGS, dynamic tick, the TSC timer pin, MPO and NIC interrupt moderation require a reboot
  before they take effect.
- Boot-configuration state (dynamic tick, TSC timer) is read with a single `bcdedit` query per
  refresh pass, shared between both tweaks — each of those queries costs a PowerShell launch, and
  one per tweak would visibly stall the UI.
- "Disable CPU core parking" reverts by *removing* its per-scheme override rather than writing a
  hardcoded default, because the stock `CPMINCORES` value differs between power plans and
  Windows builds. Removing it puts the plan back on whatever Windows ships.
- "Force the invariant TSC timer" is already the effective default on most systems; it exists to
  undo `useplatformclock yes` left behind by OEM images and older tweak guides, and to pin it so
  nothing sets it again.
- `AllowTelemetry` cannot go below "Basic" on Windows 11 Home/Pro — level 0 only fully applies on
  Enterprise/Education/Server editions. The setting is still written and detected correctly.
- The "Disable telemetry Scheduled Tasks" state is detected from the Microsoft Compatibility
  Appraiser task as a stand-in for the group, since querying all ten via `schtasks.exe` on every
  refresh would visibly stall the UI. Applying/reverting always covers all ten.
- This tool changes system and per-user registry settings and service configuration. Everything
  it does is reversible through the same UI.
