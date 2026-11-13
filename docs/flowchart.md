# How NoMoreAltF4 works

> These diagrams cover the Terminate path (the default). Seamless protection and the saving option, added in 1.0.0, are described in the [README](../README.md).

There are four ways a kill can be triggered:

- **Abort hotkey.** The player presses the abort key.
- **Outgoing event, the primary path.** The game dispatches a failure event, which is seen in `OnEventSent` before it is batched into an HTTP request.
- **HTTP POST, the fallback path.** A failure event is found in a `SaveEvents2` body inside the `WinHttpSendRequest` hook, before the original send runs.
- **Server event, a guard.** While protection is active, failure confirmations coming back from the server are dropped.

Every kill path ends in `KillProcess`, which terminates the game before the failure reaches IOI's servers.

```mermaid
flowchart TD
    Start(["Game launches, SDK loads NoMoreAltF4"]) --> Init["<b>Init</b><br/>pin DLL, read INI settings,<br/>start LogMirror"]
    Init --> Eng["<b>OnEngineInitialized</b><br/>add 4 SDK detours +<br/>WinHttpSendRequest IAT hook"]
    Eng --> Run{{"Game running"}}

    Run --> Frame["Every frame<br/>(OnDrawUI)"]
    Run --> Sent["Game dispatches an online event<br/>(OnEventSent, primary)"]
    Run --> Post["Game POSTs to IOI<br/>(WinHttpSendRequest, fallback)"]
    Run --> Recv["Server event arrives<br/>(OnEventReceived)"]

    %% Per-frame
    Frame --> Hot{"Abort hotkey pressed<br/>without Alt/Win?"}
    Hot -->|yes| Gate1{"ShouldProtect?"}
    Gate1 -->|yes| Kill
    Gate1 -->|no| Upkeep
    Hot -->|no| Upkeep["Mission boundary: reset per-mission flags<br/>Release valve: clear a stale block after 8 s"]

    %% HTTP POST
    Post --> Scan["Scan body<br/>Evergreen events: Freelancer<br/>ContractType arcade/elusive: ET / Arcade"]
    Scan --> Gate2{"ShouldProtect?"}
    Gate2 -->|no| Pass
    Gate2 -->|yes| Fail

    %% OnEventSent
    Sent --> Blocked{"Network block<br/>already raised?"}
    Blocked -->|yes| DropOut["Drop the event"]
    Blocked -->|no| Gate3{"ShouldProtect?"}
    Gate3 -->|no| Pass
    Gate3 -->|yes| Fail

    Fail{"Failure event?<br/>MissionFailed / MissionWounded /<br/>MildChess_MissionFailed, or<br/>ContractFailed in Freelancer/ET/Arcade"}
    Fail -->|no| Pass["Let it through<br/>(original call runs)"]
    Fail -->|"ContractFailed only"| Manual{"Restart / replan / load,<br/>or exit to menu with<br/>'Allow Exit to Main Menu' on?"}
    Fail -->|death event| Auto
    Manual -->|yes| Pass
    Manual -->|no| Auto{"Terminate on<br/>Death/Failure ON?"}
    Auto -->|yes| Kill
    Auto -->|no| Warn["Log a warning:<br/>the failure WILL be saved"]

    %% Server events
    Recv --> SrvChk{"Failure / SegmentClosing event,<br/>and block raised or death seen?"}
    SrvChk -->|yes| DropIn["Drop the server event"]
    SrvChk -->|no| Pass

    %% Kill
    Kill["<b>KillProcess</b>"] --> K1["Raise network block<br/>(drop any further events)"]
    K1 --> K2["Flush SDK logs +<br/>LogMirror.SyncTail"]
    K2 --> K3(["TerminateProcess<br/>same effect as Alt-F4:<br/>the failure never reaches IOI"])

    classDef kill fill:#FCEBEB,stroke:#A32D2D,color:#501313
    classDef safe fill:#E1F5EE,stroke:#0F6E56,color:#04342C
    class Kill,K1,K2,K3 kill
    class Pass safe
```

`ShouldProtect` is true unless **Freelancer only** is on and the current mission is not Freelancer.

Unloading the mod at runtime takes a separate path. The destructor sets the IAT hook to passthrough, swaps the import table to a persistent jump stub, waits up to 2 s for in-flight calls to finish, and stops LogMirror. The DLL stays pinned so the game never runs code from freed memory.
