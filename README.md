# NoMoreAltF4

A [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) plugin for Hitman: World of Assassination that automates the Alt-F4 exploit to prevent mission failure penalties. Specifically designed for **Freelancer** and **Elusive Targets** — the two modes where failure has permanent consequences.

## What It Does

In Freelancer mode, dying means losing your carried items, campaign progress, and safehouse inventory. In Elusive Targets, a single failure locks you out permanently. The classic workaround is to Alt-F4 before the game saves — this plugin automates that timing perfectly.

### Features

- **Auto-terminate on death/failure** — Detects the failure event the instant it is dispatched and kills the game process *before* it is saved to IOI servers. This is the only reliable, crash-safe protection: the failure is recorded by ordinary network requests that cannot be blocked without crashing or hanging the game, so terminating first is the one approach that always works. Protects carried items, safehouse items, campaign progress, and ET/Arcade attempts.
- **Smart event filtering** — Player-initiated actions like restarting, replanning, and loading saves are automatically ignored; `ContractFailed` is only treated as a real failure in the single-attempt modes this mod targets (Freelancer / Elusive Target / Arcade).
- **Allow Exit to Main Menu** (toggle, off by default) — Exiting to main menu during Freelancer or ET missions counts as a failure, so it's blocked unless you explicitly enable this option in the mod menu.
- **Manual abort hotkey** — Force-quit the mission at any time (spotted, lost Silent Assassin, etc.) with a single keypress. Configurable in the mod menu (None, F9, F10, F11, F12). Disabled by default to avoid conflicts with Nvidia overlay.
- **Freelancer detection** — Identifies Freelancer mode via scene path ("Evergreen" codename). Optional "Only Enable for Freelancer" toggle disables all protection in non-Freelancer missions.
- **Elusive Target protection** — Works for ET failures too (wrong kill method, objective failure) — not just player death.

### How It Works

The plugin **detects** a mission failure as early as possible and then **terminates the game** before the failure can be saved. Termination is the only protection, by design — see the warning below.

1. **Detection — `OnEventSent` hook (earliest point)** — When a failure event (`MissionFailed_Event`, `MissionWounded_Event`, `MildChess_MissionFailed`, or `ContractFailed` in Freelancer/ET/Arcade) is dispatched, the mod catches it here, *before* it is batched into any network request.
2. **Detection — IAT hook on WinHttpSendRequest (fallback)** — Reads HTTP POST bodies as a backup detector. It is strictly **observe-only**: it never alters or short-circuits the request (the game drives WinHTTP asynchronously, so faking a send result corrupts its state machine — faking success hangs the game, faking failure crashes it).
3. **`TerminateProcess`** — On detection, the game process is killed immediately, before the failure leaves the machine. This replicates a clean Alt-F4 and is the single reliable, crash-safe action.

> **Why termination is the only option:** the failure is recorded by ordinary `SaveEvents2` / `SaveAndSynchronizeEvents4` WinHTTP POSTs. Testing showed there is **no safe way to block or rewrite those requests in flight** — suppressing the WinHTTP call hangs or crashes the game, and dropping the higher-level event doesn't stop the POST. So the mod doesn't try: it kills the process first. If you turn auto-terminate off, **failures are no longer prevented.**

All per-mission state is reset on mission entry/exit so nothing leaks between missions.

## Compatibility

Built against **ZHMModSDK v4.0.2** and game build **3.260.0.0**. Always run the ZHMModSDK version that matches your installed game version — a mismatch resolves engine functions at stale offsets and can hang or crash the game.

## Why This Works

IO Interactive [officially restored](https://ioi.dk/hitman/patch-notes/2023/hitman-woa-august-patch-notes) the Alt-F4 exploit in the August 2023 patch. They can't distinguish it from legitimate crashes or power outages, so it's tolerated by design. This plugin just automates the timing.

## Installation

1. Download and install [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK).
2. Copy `NoMoreAltF4.dll` to the ZHMModSDK `mods` folder (e.g. `C:\Games\HITMAN 3\Retail\mods`).
3. Launch the game, press `~` (`^` on QWERTZ layouts) to open the mod menu, and enable **NoMoreAltF4**.

## Configuration

Open the ZHMModSDK overlay menu (`~` key) to access settings:

| Option | Default | Description |
|--------|---------|-------------|
| Terminate Game Process on Death/Failure | On | Kill the process the moment a failure is detected — the only reliable protection. **Leave this on.** |
| Only Enable for Freelancer | Off | Restrict all protection to Freelancer mode only |
| Allow Exit to Main Menu | Off | Allow exiting to main menu without blocking (risky in Freelancer/ET) |
| Manual abort hotkey | None | Hotkey to force-quit the mission manually (None, F9–F12) |
| Log HTTP requests | On | Log all IOI HTTP traffic for diagnostics |

## Building

### Steps

### 1. Clone this repository locally with all submodules.

You can either use `git clone --recurse-submodules` or run `git submodule update --init --recursive` after cloning.

### 2. Install Visual Studio (any edition).

Make sure you install the C++ and game development workloads.

### 3. Open the project in your IDE of choice.

See instructions for [Visual Studio](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-Visual-Studio-for-development) or [CLion](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-CLion-for-development).

See the [ZHMModSDK wiki](https://github.com/OrfeasZ/ZHMModSDK/wiki) for detailed IDE setup instructions.

## License

[GPL v3](LICENSE)

## See Also

- [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) — The mod SDK this plugin is built on
- [Freelancer Save Scum](https://www.nexusmods.com/hitman3/mods/1013) — Prior art, proves the concept via memory scanning
- [Rogueless Freelancer](https://www.nexusmods.com/hitman3/mods/470) — SMF entity patches (protects safehouse items only, not carried items)
