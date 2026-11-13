# Freelancer campaign restore (one-shot tool, not part of the mod)

On 2026-11-12 a Freelancer showdown was left through the pause menu with a mod build that did
not catch that exit, and the campaign and the carried items were lost. This tool put the
campaign back on 2026-11-13. It worked: after a relaunch the server sent the saved campaign
unchanged (603 of 603 values). The tool was then removed from the mod. The exit itself is
caught since that day, so this should not be needed again.

It is kept here as a patch, so it can be rebuilt if a campaign is ever lost to a bug again.

## How it works

- **The campaign is one key/value store** on IOI's server ("CPD": carried items, merces,
  campaign step, safehouse decoration and so on, about 600 keys). Only the game client writes
  it, through `CpdSet` online events, which carry partial values that the server merges.
- **The server sends the whole store back** as `ContractProgressionData` in every Freelancer
  `GetForPlay2` response. So every session log with "Log HTTP requests" on holds snapshots of
  the campaign as it was at each mission or safehouse start.
- **The restore rides on a small `CpdSet` that the game sends itself** from the safehouse,
  for example when you flip a page of the themed weapon wall (`{"GearWall_ThematicIndex":N}`).
  The mod replaces that event's `Value` with the saved snapshot and sends the request as
  usual. When the server answers, or after 10 seconds, the mod closes the game, because the
  running game still holds the lost campaign in memory and would overwrite the restore with
  its next upload.
- **Only a 1-key numeric `CpdSet` from the safehouse is used** as the carrier, and only when
  it is the only one in the request. Departure or quit uploads, which carry many keys, are
  never used.

## Files

- `restore-tool.patch`: the restore, as a patch against the sources of 2026-11-13. It adds
  the menu section "Freelancer: restore a saved campaign", `RestoreFilter` (in
  `HookedSendRequest`), the server-answer check in the WinHTTP callback detour, a 10-second
  watchdog, `ebr::FindCpdSetValue` and its offline tests.
- `make_restore_file.py`: lists the campaign snapshots in a session log and writes one as
  `freelancer-restore.json`.

## Using it again

1. `git apply tools/freelancer-restore/restore-tool.patch`, then build. If the sources have
   moved on, apply it by hand: it is self-contained.
2. Find the last snapshot before the loss:
   `python tools/freelancer-restore/make_restore_file.py <session log>`.
   Each line shows the line number, contract, carried items, merces and campaign step.
3. Write it into the game's log folder:
   `python tools/freelancer-restore/make_restore_file.py <session log> <line> "<game>\NoMoreAltF4Logs\freelancer-restore.json"`.
4. Launch, check that the mod menu shows the saved campaign, and click "Arm restore" at the
   main menu.
5. Enter the safehouse and flip one page of the themed weapon wall. The game closes by
   itself.
6. Relaunch and check the safehouse. The session log's first Freelancer `GetForPlay2`
   response should equal the file.
7. Remove the patch again, and delete `freelancer-restore.json` from the game folder.

## Cautions

- **It writes to the player's online profile, and the mod cannot undo it.** Arming it by
  mistake later rolls the campaign back to the snapshot.
- **Do not play Freelancer between the loss and the restore.** Any campaign upload in
  between, such as starting a new campaign, changes keys the snapshot would have to undo.
- **Do not commit a player's `freelancer-restore.json` or session logs.** They belong in the
  git-ignored `logs` folder.
- **Changing the saved campaign alters IOI's data,** beyond what an Alt-F4 does.
