# NoMoreAltF4 workflows

> These diagrams cover the Terminate path (the default). Seamless protection and the saving option, added in 1.0.0, are described in the [README](../README.md).

## Player workflow

This covers installing the mod, setting it up, and what happens during a mission.

```mermaid
flowchart TD
    A(["New install or game update"]) --> B["Install the ZHMModSDK release that matches your game build<br/>(3.290.1.0 needs SDK v4.2.x)"]
    B --> C["Copy NoMoreAltF4.dll into<br/>HITMAN 3\\Retail\\mods"]
    C --> D["Launch the game, press ~,<br/>tick NoMoreAltF4 under MODS"]
    D --> E["Configure in the mod menu:<br/>keep Terminate on Death/Failure ON<br/>pick an abort hotkey (F8 if you use QuickSave)<br/>optional: Allow Exit to Main Menu, Freelancer only"]
    E --> F{{"Play Freelancer / Elusive Target / Arcade"}}

    F --> G{"How did it go?"}
    G -->|mission complete| H["Game saves normally"]
    G -->|restart / replan / load| I["Allowed, the mod does nothing"]
    G -->|"spotted, lost SA,<br/>want out"| J["Press the abort hotkey"]
    G -->|died or failed| K["The mod detects the failure event"]

    J --> L["Game closes instantly"]
    K --> L
    L --> M["Relaunch from Steam"]
    M --> N(["Back to before the mission:<br/>carried gear, safehouse and campaign kept"])
    N --> F
    H --> F
    I --> F

    F -.->|crash or bug| O["Mod menu → Open log folder"]
    O --> P(["Attach NoMoreAltF4Logs\\session-*.log<br/>to the report (tokens already redacted)"])
```

## Developer and release workflow

This covers changing the code, testing it in-game, and shipping a version to GitHub and Nexus.

```mermaid
flowchart TD
    U(["Game update?"]) -->|yes| V["Wait for a matching ZHMModSDK release,<br/>bump ZHMMODSDK_VER in CMakeLists.txt,<br/>check the SDK diff for API changes"]
    U -->|no| A
    V --> A["Edit src/"]
    A --> B["Visual Studio rebuilds automatically"]
    B --> C["F5 with an *-Install preset"]
    C --> C1["Install step copies the previous raw log<br/>to logs/ (archive-game-log.cmake)"]
    C1 --> C2["Copies NoMoreAltF4.dll into the game,<br/>then launches HITMAN 3"]
    C2 --> D["Test in-game: Debug build, HTTP logging on<br/>(Freelancer death, restart/replan, hotkey, ET)"]
    D --> E{"ZHMModLoader.log and<br/>NoMoreAltF4Logs look right?"}
    E -->|no| A
    E -->|yes| F["Commit, push master"]
    F --> G["gh release create vX.Y.Z-alpha"]
    G --> H["CI (build.yml, on release: created)"]
    H --> H1["Build Debug + Release"]
    H1 --> H2["Attach zips to the GitHub release"]
    H1 --> H3["Upload to Nexus mod 1224<br/>(Debug and Release file groups)"]
    H2 --> I["Update by hand on Nexus:<br/>page version, summary/description, changelog"]
    H3 --> I
    I --> J(["Released"])
```
