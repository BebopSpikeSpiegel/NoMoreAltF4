# NoMoreAltF4

A [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) plugin for HITMAN: World of Assassination that keeps a mission failure from being recorded. It is made for the modes where failure has lasting consequences: **Freelancer**, **Elusive Targets** and **Elusive Target Arcade**.

**[中文说明见下方](#中文说明)**

## What's new in 1.0.0

- **Seamless protection.** A failure no longer has to close the game. The failed attempt is hidden from IOI's server while the game keeps running: you see the normal failure screen, go back to the menu and start again.
- **Saving in no-save missions.** The greyed-out Save button works again in story missions that switch saving off (such as Holiday Hoarders) and in escalations.
- The abort key now waits while a mission is loading, and tells you when it is ignored.
- A Freelancer pause-menu exit is now protected. Earlier versions missed it.

## Features

### Terminate on failure (default, on)

When a failure is detected, the game process is killed before the failure is saved to IOI's servers. This is the automated Alt-F4, and it is the fallback for everything else in the mod. It protects carried Freelancer gear, the campaign, and Elusive Target / Arcade attempts.

- Restart, Replan and loading a save are your own choice. They are recognised and always pass through.
- In Freelancer, leaving a mission through the pause menu costs the gear you carried, so it is treated as a failure whatever the other options say.

### Seamless protection (optional, off by default)

With Seamless on, a death, a failed objective or a pause-menu exit is contained instead of closing the game:

1. The mod spots the failure event before it is uploaded.
2. Every event of that attempt is rewritten into a harmless one of the same size in the outgoing request. The request itself is always really sent.
3. The server never learns that the attempt failed. It later closes the attempt as an abandoned session, which is what happens after an Alt-F4.

What you do: at the failure screen choose **exit to menu**, then start the mission again. Do not use Restart or Replan from the failure screen.

If anything looks wrong (a request the mod cannot read or rewrite safely), the mod closes the game as before. That is why "Terminate Game Process on Death/Failure" must stay on.

Two boxes:

| Box | Covers | Status |
|-----|--------|--------|
| Seamless protection (story, Arcade, Elusive Targets) | Story and other contracts, Elusive Target Arcade, Elusive Targets | Tested in game, see below |
| Freelancer (experimental, use cheap gear) | Freelancer missions | Experimental |

**Freelancer Seamless** works differently after the failure: the mission end screen cannot load, so the game shows an error dialog after a short wait. Choose **Go to menu** (not Retry) and enter Freelancer again. The safehouse then reloads your campaign from the server with your gear and Merces as they were at mission start. Until that has happened, the mod closes the game before anything could overwrite the campaign.

### Saving in no-save story missions and escalations (optional, off by default)

The server switches saving off for some contracts, and the game greys out the Save button. With this option on, saving is switched back on for story-type missions such as Holiday Hoarders and for escalations. It applies from the next mission load. Freelancer, Elusive Targets and Arcade are never changed.

In an escalation, only load saves made in the level you are playing.

### Manual abort key

Force-quit a mission with one key (None, F8 to F12; off by default). Presses with Alt or Win held are ignored. While a mission is still loading, the abort is queued and fires once the mission has started.

## What was tested

All on game 3.290.1.0 with ZHMModSDK v4.2.1.

| Case | Result |
|------|--------|
| Terminate on a real Elusive Target failure | Attempt kept |
| Seamless, story missions | Deaths and exits contained |
| Seamless, Elusive Target Arcade | Deaths, an objective failure and an exit contained; the same level was offered again |
| Seamless, live Elusive Target | 2 deaths and 2 exits contained; the target stayed playable and was then completed normally |
| Freelancer Seamless | 4 deaths and 1 pause-menu exit contained; campaign unchanged |
| Saving in Holiday Hoarders | Save and load accepted by the server; mission completed normally |

Not tested yet: a Freelancer showdown with Seamless, an objective failure on a live Elusive Target, and save/load in an escalation.

## Risks

- Seamless changes data sent to IOI's servers. Terminate only stops the game, as an Alt-F4 does. Use Seamless at your own risk.
- A game update can change what the game sends. After an update, try Seamless in a story mission first, where nothing is at stake.
- Never close the game during a loading screen while a mission is starting. During development, one such close left the account in a state where finished missions were no longer closed properly (no playstyle rating, XP counted twice). It cleared after the game had been closed for a few hours. The abort key waits during loading for this reason.

## Compatibility

Built against **ZHMModSDK v4.2.1** and tested on game build **3.290.1.0**. Always run the ZHMModSDK version that matches your game version: a mismatch can hang or crash the game.

> **QuickSave mod (SDK v4.2+):** the SDK bundles a QuickSave mod that uses **F9 to quick-load**. If you enable it, do not use F9 as the abort key. Pick F8 or F10 to F12.

Disabling the mod at runtime is safe, but its DLL stays loaded until the game exits. After re-enabling it in the same run, restart the game before relying on the saving option.

## Why terminating works

IO Interactive [officially restored](https://ioi.dk/hitman/patch-notes/2023/hitman-woa-august-patch-notes) the Alt-F4 exploit in the August 2023 patch, because it cannot be told apart from a crash or a power cut. The Terminate mode automates that timing. Seamless aims for the same server-side result, an abandoned session, without closing the game.

## Installation

1. Install [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) v4.2.x.
2. Copy `NoMoreAltF4.dll` to the ZHMModSDK `mods` folder (for example `C:\Games\HITMAN 3\Retail\mods`).
3. Launch the game, press `~` (`^` on QWERTZ layouts) to open the mod menu, and enable **NoMoreAltF4**.

## Configuration

Open the ZHMModSDK overlay menu (`~` key):

| Option | Default | Description |
|--------|---------|-------------|
| Terminate Game Process on Death/Failure | On | Close the game when a failure is detected. **Leave this on**: Seamless depends on it as its fallback. |
| Only Enable for Freelancer | Off | Switch all protection off outside Freelancer |
| Allow Exit to Main Menu | Off | Let a pause-menu exit through unchanged. Freelancer ignores this option. |
| Allow saving in no-save story missions and escalations | Off | Re-enable the Save button there |
| Seamless protection (story, Arcade, Elusive Targets) | Off | Contain a failure without closing the game |
| Freelancer (experimental, use cheap gear) | Off | The same for Freelancer missions |
| Manual abort hotkey | None | Force-quit key (None, F8 to F12) |
| Log HTTP requests | On | Log IOI traffic for diagnostics |
| Archive session logs | On | Keep a copy of every session's log |

## Logs

ZHMModSDK wipes `ZHMModLoader.log` every time the game starts. With **Archive session logs** on, NoMoreAltF4 copies each session into `NoMoreAltF4Logs\session-YYYYMMDD-HHMMSS.log` next to it (the last 30 are kept). Login tokens are removed from these copies. They still contain your profile id and gameplay data, so read a log before posting it publicly.

For developers: launching from Visual Studio with an `*-Install` preset also copies the previous raw `ZHMModLoader.log` into the project's `logs\` folder (`cmake/archive-game-log.cmake`). Those raw copies are not redacted and are git-ignored.

## Building

1. Clone this repository with all submodules (`git clone --recurse-submodules`).
2. Install Visual Studio with the C++ and game development workloads.
3. Open the project; see the ZHMModSDK instructions for [Visual Studio](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-Visual-Studio-for-development) or [CLion](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-CLion-for-development).

`tests/ebr_replay.cpp` is a standalone test of the event rewriter. It replays the request bodies captured in session logs; the command to compile it is at the top of the file.

## 中文说明

NoMoreAltF4 是《杀手：暗杀世界》的 ZHMModSDK 插件，用来防止任务失败被记录到官方服务器，主要面向自由职业者、行踪不定的目标和街机模式。

**1.0.0 新功能**

- **无缝保护（Seamless）**：失败后不用再关游戏。失败的那一局不会上传到服务器，游戏继续运行。看到失败画面后选“返回到菜单”，再重新开始即可。不要在失败画面直接点重新开始或重新计划。
- **解锁存档**：《假日偷窃狂》这类被官方关闭存档的剧情关卡，以及升级合约，保存按钮恢复可用，存档和读档都正常。自由职业者、行踪不定的目标和街机模式不受影响。

**使用方法**

1. 安装 ZHMModSDK v4.2.x，把 `NoMoreAltF4.dll` 放进 `Retail\mods` 文件夹。
2. 游戏内按 `~` 打开菜单，启用 NoMoreAltF4。
3. 无缝保护：勾选 `Seamless protection (story, Arcade, Elusive Targets)`。
4. 解锁存档：勾选 `Allow saving in no-save story missions and escalations`，下一次载入任务时生效。
5. `Terminate Game Process on Death/Failure` 请保持开启：无缝保护遇到异常时会靠它自动关游戏保底。

**已实测（游戏 3.290.1.0）**：剧情关卡、街机模式、真实的行踪不定的目标（阵亡 2 次、退出 2 次都被拦下，目标仍可游玩，之后正常完成）。自由职业者的无缝保护是实验功能，默认关闭；失败后结算画面会报错，请选“前往菜单”，再重新进入自由职业者。

**注意**

- 无缝保护会改写发往官方服务器的数据，使用风险自负。
- 游戏更新后，请先在剧情关卡里试一次。
- 任务载入过程中不要强行关闭游戏。

## License

[GPL v3](LICENSE)

## See Also

- [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK), the mod SDK this plugin is built on
- [Freelancer Save Scum](https://www.nexusmods.com/hitman3/mods/1013), prior art
- [Rogueless Freelancer](https://www.nexusmods.com/hitman3/mods/470), SMF entity patches (protects safehouse items only)
