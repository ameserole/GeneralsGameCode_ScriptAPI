# GeneralsGameCode with a Script API

This is a fork of [TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode) that adds a **script API** to *Command & Conquer: Generals – Zero Hour*: a WebSocket server inside the game through which an external program (a Python bot, or an AI agent driving one) plays one player, live, under exactly the same limits as a human.

The upstream project's README is kept unchanged in [README-upstream.md](README-upstream.md); everything there about the code base, building and contributing still applies.

## Purpose

- **Bots that play fair.** A script sees what a human player would see and acts only through the same network messages a mouse click sends, so a bot can't do anything a person at the keyboard couldn't.
- **Bot against bot, watched by humans.** Two scripted players play a normal LAN game; people watch from a LAN observer slot or from the replay afterwards.
- **AI-agent tournaments.** AI agents write and tune bots, play each other live and improve between games. The tooling for that (Python client, launcher, tournament runner) lives in the parent project, outside this repository.

All changes are behind a build option that is **off by default**, so a normal build of this fork is the upstream game. With the option on, the API stays inactive unless the game is started with a `-scriptApi*` flag.

## What changed

### 1. A build option
`RTS_BUILD_OPTION_SCRIPT_API` (CMake, default `OFF`, not available for VS6 builds) defines `RTS_SCRIPT_API=1` on `core_config`, so every target sees the same layout, and adds two fetched dependencies: [IXWebSocket](https://github.com/machinezone/IXWebSocket) (`cmake/ixwebsocket.cmake`) and [nlohmann/json](https://github.com/nlohmann/json) (`cmake/nlohmann_json.cmake`). Every engine change is wrapped in `#if RTS_SCRIPT_API` (and `RTS_ZEROHOUR` where the code is shared with Generals).

### 2. The server and protocol library: `Core/Libraries/Source/ScriptApi/` (`core_scriptapi`)
Game-independent code, unit-tested without the game:
- **`ScriptApiServer`**: the WebSocket server on `127.0.0.1`. Network I/O runs on background threads; the game thread exchanges messages through thread-safe inbound and outbound queues and never blocks on the network. IXWebSocket is hidden behind a pimpl so game code never includes it (its headers clash with the game's winsock use).
  - **One controller and up to three watchers.** `ws://127.0.0.1:PORT/` is the controller (the bot); a new controller replaces the old one (close code 4001). `ws://127.0.0.1:PORT/watch` is a watcher: it gets every event and query and may give orders, but can't use session commands; a fourth watcher is refused (close code 4003).
  - Slow clients are dropped (close code 4002) when their queue or socket buffer grows too large.
- **Protocol, router and commands**: request parsing, replies and events, error codes, per-command access (controller-only commands), order tracking (`command_executed`), the actions-per-minute limiter, money committed to orders still on their way, replay-name checks.
- **World and game-state helpers**: the visibility rule (`isVisibleToPlayer`), the memory of fogged structures, snapshot timers, enemy-visibility tracking, damage aggregation, skirmish and LAN request parsing.

### 3. The in-game subsystem: `GeneralsMD/Code/GameEngine/{Include,Source}/ScriptApi/`
`TheScriptApi` is created by `GameEngine` when a `-scriptApi*` flag is given and updated once per frame, **before** `TheMessageStream->propagateMessages()`, so script orders enter the same pipeline as a player's input.

| File | What it does |
|---|---|
| `ScriptApi.cpp` | Lifecycle, per-frame update, connection handling, `hello`, `ping`, `exit` |
| `ScriptApiSkirmish.cpp` | Skirmish options, `start_skirmish`, `quit_game`, `save_replay` |
| `ScriptApiLanLobby.cpp` | LAN lobby: open, host, join, slots, accept, start, leave, surrender (drives the real LAN menu screens) |
| `ScriptApiWorldView.cpp` | Queries (`game_info`, `catalog`, `faction_catalog`, `map`, `state`, `player_stats`), fog-filtered snapshots, game events |
| `ScriptApiActions.cpp`, `ScriptApiAbilities.cpp` | Orders: move, attack, build, train, sell, upgrades, sciences, abilities, garrison, repair and more, each checked like a UI click |
| `ScriptApiInput.cpp` | The input lock that keeps humans from playing a bot's player |

### 4. Read-only hooks in existing engine code
Small guarded calls that report events to the API and never change game state or use the logic's random number generator:
- `GameLogic.cpp`: loading started and game state changes.
- `GameLogicDispatch.cpp`: each executed network message (for `command_executed` and committed money).
- `Player.cpp`: units created, structures started and completed, upgrades, sciences.
- `Object.cpp`, `ActiveBody.cpp`: deaths, removals and damage.
- `Eva.cpp`: announcer messages.
- `Win32GameEngine.cpp`: a script-controlled window keeps running while minimized.
- `GlobalData`, `CommandLine.cpp`: the new command-line flags.
- `ControlBar.h`, `ProductionPrerequisite.h`: small accessors for checks the UI makes.

### 5. Bug fixes found along the way
Marked `TheSuperHackers @bugfix`; candidates for small, separate upstream PRs:
- **`CommandLine.cpp`:** `-ignoreAsserts` was lost at shutdown, so the leak report raised an assertion dialog after the game data was gone.
- **`GameMemory.cpp`:** a data race in the debug memory totals.
- **`GameEngine.cpp`:** the LAN lobby state leaked when quitting to the desktop from a LAN screen.

## Fair-play guarantees

- **Orders are clicks.** Every order becomes the same `GameMessage` the UI would send, after the UI's checks:
  - the command button exists and is enabled, including the science a general's power needs
  - `canMakeUnit` (money, queue, limits)
  - a legal build location
  - a visible target
- **Fog of war.** Other players' objects are reported only when the local player could see them (or as immobile fog "ghosts"). Enemy economy, production and pathfinding data are never read.
- **No influence on the simulation.** Hooks are read-only. Replays of API games re-simulate without a CRC mismatch, which the parent project's tests check.
- **Humans can't take over.** `-scriptApiInput view` (the default) lets a person move the camera in a bot's window, but not give orders.
- **Rate limit.** `-scriptApiApm N` caps accepted orders per minute of game time (`RATE_LIMITED` beyond it).
- **No cheats in normal builds.** Commands that reveal hidden state or bypass rules (`debug_*`, `set_input_mode`) exist only in debug builds started with `-scriptApiDebug`.

## Building

Follow [README-upstream.md](README-upstream.md) and the upstream build docs for the toolchain (Visual Studio 2022, CMake, Ninja), then turn the option on:

```powershell
cmake --preset win32-debug -DRTS_BUILD_OPTION_SCRIPT_API=ON -DRTS_DEBUG_MULTI_INSTANCE=ON -DRTS_BUILD_OPTION_TESTS=ON
cmake --build build/win32-debug --target z_generals scriptapi_googletest
build\win32-debug\Tests\Google\ScriptApi\Debug\scriptapi_googletest.exe
```

- **`RTS_DEBUG_MULTI_INSTANCE=ON`** lets several game instances run on one PC, which a LAN game between two bots (plus an observer) needs. Instance N binds `127.0.0.(N+1)`.
- **`scriptapi_googletest`** holds the unit tests for the game-independent library.
- The game still needs the Zero Hour game data, as upstream. Debug builds read the install path from `HKCU\Software\Electronic Arts\EA Games\Generals\InstallPath`.

In the parent project, `scripts\build.ps1 -Tests -Deploy` does all of this and copies the game next to the game data.

## Running

```powershell
generalszh.exe -win -nologo -noshellmap -scriptApiPort 5000
```

| Flag | Meaning |
|---|---|
| `-scriptApiPort N` | Turn the API on, listening on `127.0.0.1:N` |
| `-scriptApiInput view\|locked\|full` | What a human can do in this window: camera only (default), nothing, or everything |
| `-scriptApiApm N` | At most N accepted orders per minute of game time (default 0: no cap) |
| `-scriptApiDebug` | Enable debug-only test commands (debug builds only) |
| `-ignoreAsserts` | Log the debug build's assertions instead of stopping on a dialog (useful for unattended games) |

## Protocol in brief

JSON text messages over WebSocket.

- **Request:** `{"id": 7, "cmd": "train", "factory": 1234, "template": "ChinaTankBattleMaster", "count": 2}`
- **Reply:** `{"type": "reply", "reply_to": 7, "ok": true, ...}`, or `{"type": "reply", "reply_to": 7, "ok": false, "error": "INSUFFICIENT_FUNDS", "message": "..."}`
- **Event:** `{"type": "event", "event": "construction_complete", "frame": 4200, ...}`

**Command groups:**
- **Session:** `hello` (protocol version, role, game state, result once a game is over), `ping`, `exit`.
- **Menus:** `skirmish_options`, `start_skirmish`, `quit_game`, `save_replay`, and the `lan_*` commands.
- **Queries:** `game_info`, `catalog`, `faction_catalog`, `template_info`, `map`, `state` (a snapshot), `player_stats`, `set_snapshot_interval`.
- **Orders:**
  - movement and combat: `move`, `attack_move`, `attack`, `force_attack`, `guard`, `stop`
  - production: `build`, `train`, `cancel_train`, `upgrade`, `purchase_science`, `sell`
  - abilities and units: `use_ability`, `enter`, `exit_container`, `evacuate`, `dock`, `repair`, `resume_construction`, `set_rally_point`, and more

  Each order reply is followed by `command_executed` once the game logic carries it out.

**Events:**
- **Your own player:** units, construction, upgrades, sciences, money, power, rank, damage and losses, `build_failed`, announcer messages.
- **Other players:** `enemy_spotted`, `enemy_lost_from_view`, `enemy_destroyed`, `structure_discovered`, all fog-filtered.
- **Game state:** `game_state`, `game_over` (result, winners, every player's score-screen stats).
- **Periodic snapshots.**

The full protocol, design and history are in the parent project (`PLAN-script-api.md`, `Plans/`), together with the Python client.

## License

Same as upstream: GPL-3.0, see [LICENSE.md](LICENSE.md). New files carry the project's GPL header.
