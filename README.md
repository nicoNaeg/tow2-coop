# tow2-coop: two-player co-op for The Outer Worlds 2 (work in progress)

Status: **v0.2.0-beta.1, a test build: co-op through Steam (friends find each other, no IP or port), or direct IP.
Tested with two copies on one PC only** (never across two PCs yet; the Steam link was checked on one copy). Two
independent copies of the game share one campaign owned by the host: the client joins by receiving the host's save,
each player controls their own character, sees the other (position, rotation, walk/idle animation, the player's look),
and the host owns NPC health and deaths, quest progress, map travel, door states and container contents. See `TASKS.md`
for the acceptance matrix with PASS / PARTIAL / NOT TESTED and evidence.

Download: the release zip (installer, loader, mod, this README). Each player needs their own copy of the game on Steam
(a Steam Families shared copy can't be played by two people at once). Unofficial fan mod, not affiliated with Obsidian
Entertainment or Xbox; it ships no game files. MIT license.

## How it plays (rules)

- **Join:** the client starts at the main menu and joins; the host saves its current campaign ("CoopHost") and hands it
  over; the client loads it into its own save folder `CoopHost_<host id>` (never over the client's own saves). The client
  plays a copy of the host's character (same look, stats and inventory at join time).
- **Players:** each player moves, fights and loots in their own world; the other player appears as a stand-in NPC body.
- **Combat:** NPC health and deaths follow the host; damage a client deals is sent to the host and applied there.
- **Quests and story:** host-owned. Quest starts, objective updates (addendums) and objective global variables reach the
  client live; quest completions/failures reach it through a save resync (a short reload on the client).
- **Travel:** host-led. When the host travels to another map, the client is handed the campaign again and follows.
- **Doors:** shared state; either player opens, closes or locks, the other world follows.
- **Containers:** shared contents; an item taken by one player disappears from that container for the other, the taker
  keeps it. Two players taking the same item at the same moment may both get it.
- **Disconnect:** a silent peer is dropped after 5 s (its stand-in disappears); a restarted client rejoins.

## Supported build (anything else: the mod stays off and the game runs unmodified)

| | |
|---|---|
| Store | Steam, app 1449110, build id 24225441, game version 1.2.0.1 (CL-2994920) |
| `TheOuterWorlds2-Win64-Shipping.exe` | SHA-256 `55ac756281828d00d93194ac80f99d13233415aff69360554d982a899f17c444` |
| Checked at run time | PE timestamp `0x89B6FE2C`, SizeOfImage `0x0B137000`, CheckSum `0x0ACBC2F0`, and the bytes at every hooked or called address |

## Parts

| File | What it does |
|---|---|
| `bin/dsound.dll` (`sandbox/dsound_proxy.c`) | Proxy loaded by the game. Forwards DirectSound; loads `tow2coop.dll` from a thread. **Test copies only** (`TOW2_SANDBOX_DIR` set): redirects Saved Games, LocalAppData, Documents to the sandbox, answers Steam stat, achievement and cloud writes locally, renames the single-instance mutex; stops the game (`0x5AFE00xx`) if it cannot. |
| `bin/core/tow2coop.dll` (`core/*.cpp`) | Build check, UE 5.4 reflection, hooks on `UGameEngine::Tick` and `ProcessEvent`; co-op: `coop.cpp` (UDP link, stand-in, NPC authority, doors, containers, resync), `story.cpp` (quests/globals), `join.cpp` (save hand-over over TCP); in test copies the hub's bridge protocol for tests. |
| `qa/` | Test tooling: gamepool config, deploy/prep scripts, bridge client (`tow2.py`), input (`gamewin.py`), tests (`test_npc_sync.py`, `test_melee.py`, `test_story.py`, `test_resync.py`, `test_interact.py`, `test_join.py`, `test_rejoin.py`), soak (`soak_run.ps1`). |

## Build

Needs llvm-mingw (portable, no install) and the coop-anything hub next to this folder (`tools/bridge/include`).

    powershell -NoProfile -File build.ps1        # proxy, fake Steam, core, then the proxy tests (all must PASS)

## Run two copies on one PC (testing)

Test copies are hard-linked, isolated installs made by the hub's `tools/gamepool` (config `qa/tow2.gamepool.json`).
One command runs the whole regression on a pair: `powershell -NoProfile -File qa/run_suite.ps1 -Out <folder>`
(`qa/soak_run.ps1 -Minutes 30` for the soak, `qa/run_key.ps1` for the shared key). The tests load a save copied from
your own game into each copy's sandbox (`qa/prep_instance.py --saves`); nothing is written to the real save folder.
Co-op settings come from the environment (else from `tow2coop.ini`, see below): `TOW2_COOP_ROLE=host|client` (or gamepool's `GAMEPOOL_ROLE`),
`TOW2_COOP_PORT` (UDP, default 27960; the save hand-over uses TCP port + 1), `TOW2_COOP_PEER=ip:port` for a client on
another PC, `TOW2_COOP_BIND` for the host (test copies: 127.0.0.1). Two copies on one 8 GB GPU need the VRAM budget
that `qa/prep_instance.py` writes (`Engine.ini`) and 960x540.

## Install for players (never tested across two PCs yet)

Each player runs the installer on their own PC, with the game closed (it finds the game through Steam; `-GameDir`
overrides, `-Find` only shows the folder). The two players must be **Steam friends**, each with their own copy:

    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Role host
    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Role client [-Friend "<host's Steam name>"]

The host starts the game and loads their campaign; the friend starts the game and waits at the main menu: about a
minute later the friend receives the host's campaign and loads it. **Over Steam** (the default) there is no address, no
open port and no firewall prompt: the game's own Steam session carries everything through Valve's relay, encrypted.
The players find each other through Steam rich presence (the host shows `tow2coop=host`, the friend shows
`tow2coop=join:<host>`); `-Friend` names the host when several friends host, or when the host's game details are
private. Only Steam friends are accepted.

**Direct IP** instead (`-Transport udp`, or a client given `-Peer`): the host's run prints a key, the friend runs
`install.ps1 -Role client -Peer <host address> -Key <key>`, and the host's PC must accept UDP 27960 and TCP 27961
(forward both ports or use a VPN such as Tailscale or ZeroTier). Only a player with the key becomes the peer, and the
campaign goes only to that player's address.

The installer places `dsound.dll` (loader), `tow2coop.dll` (the mod) and `tow2coop.ini` (settings, section `[coop]`:
`role`, `transport`, `host`, `peer`, `port`, `key`, `bind`) in `<game dir>\Arkansas\Binaries\Win64\`, refuses to
replace another mod's `dsound.dll`, and changes nothing of the game. When something goes wrong, `tow2coop.log` next to
the mod says what the link did (who was found, session state, hand-over). `qa/test_install.ps1` checks the installer
on a fake game folder; `qa/test_steam_self.py` checks the Steam hand-over on one copy (the copy sends its campaign to
itself over Steam, compares every byte and loads it).

## Remove

    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Uninstall

It deletes `dsound.dll`, `tow2coop.dll`, `tow2coop.ini` and `tow2coop.log` (only a `dsound.dll` that is the mod's). The
mod writes nothing else in the game folder; a client keeps the received campaign as the save `CoopHost_<host id>`, which
can be deleted from the game's Load menu. Steam "Verify integrity of game files" does not remove extra files.

## Known issues

- Keep the host's game window un-minimized while a friend joins or follows a map change: the game finishes a save only
  while its window is drawn (a minimized copy's save never completes, so the hand-over fails). Switching to another
  window without minimizing is fine.

- Loading a save within the first ~15 s after start crashes the game (engine `LoadMap`); the client waits 60 s of
  frames before loading a received campaign. Players loading from the menu by hand are not affected.
- The remote player is a stand-in NPC wearing the player's look as it was when the client joined (face, head, armor
  set and helmet); armor changed later is not shown to the other player.
- NPC AI still runs in each world; the host corrects positions over 3 m and owns health and death; enemies do not
  attack the remote player's stand-in.
- Quest completions reach the client by a reload (resync), not live.
- Damage messages are not acknowledged (fine on loopback/LAN, can be lost on a lossy link).
