# NoMoreAltF4

A [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) plugin for Hitman: World of Assassination that automates the Alt-F4 exploit to prevent mission failure penalties. Specifically designed for **Freelancer** and **Elusive Targets** — the two modes where failure has permanent consequences.

## What It Does

In Freelancer mode, dying means losing your carried items, campaign progress, and safehouse inventory. In Elusive Targets, a single failure locks you out permanently. The classic workaround is to Alt-F4 before the game saves — this plugin automates that timing perfectly.

### Features

- **Auto-terminate on death/failure** — Detects `ContractFailed` events in outgoing network traffic and kills the game process before the failure is saved to IOI servers. Protects carried items, safehouse items, and campaign progress.
- **Block outgoing failure messages** — Intercepts and drops mission failure packets at multiple layers (game event system, HTTP POST body inspection via IAT hook) so the server never receives them.
- **Smart event filtering** — Player-initiated actions like restarting, replanning, and loading saves are automatically allowed through — only actual deaths and failures are blocked.
- **Allow Exit to Main Menu** (toggle, off by default) — Exiting to main menu during Freelancer or ET missions counts as a failure, so it's blocked unless you explicitly enable this option in the mod menu.
- **Manual abort hotkey** — Force-quit the mission at any time (spotted, lost Silent Assassin, etc.) with a single keypress. Configurable in the mod menu (None, F9, F10, F11, F12). Disabled by default to avoid conflicts with Nvidia overlay.
- **Freelancer detection** — Identifies Freelancer mode via scene path ("Evergreen" codename). Optional "Only Enable for Freelancer" toggle disables all protection in non-Freelancer missions.
- **Elusive Target protection** — Works for ET failures too (wrong kill method, objective failure) — not just player death.

### How It Works

The plugin uses a 4-layer interception strategy:

1. **ZAchievementManagerSimple hooks** — Blocks `SegmentClosing` and other server events when death is detected
2. **IAT hook on WinHttpSendRequest** — Reads HTTP POST bodies before TLS encryption, detects `ContractFailed` in the payload
3. **OnEventSent hook** — Drops outgoing online events at the game layer before any HTTP call is made
4. **TerminateProcess** — Nuclear option: kills the game process immediately, bypassing all save/cleanup routines

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
| Terminate Game Process on Death/Failure | On | Auto-kill the process when a failure event is detected |
| Block Outgoing Mission Failure Messages | On | Drop failure packets without terminating |
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

[MIT](LICENSE)

## See Also

- [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) — The mod SDK this plugin is built on
- [Freelancer Save Scum](https://www.nexusmods.com/hitman3/mods/1013) — Prior art, proves the concept via memory scanning
- [Rogueless Freelancer](https://www.nexusmods.com/hitman3/mods/470) — SMF entity patches (protects safehouse items only, not carried items)
