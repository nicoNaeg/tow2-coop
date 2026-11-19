# tow2-coop: two-player co-op for The Outer Worlds 2 (work in progress)

Status: **v0.2.0-beta.6, a test build: co-op through Steam (friends find each other, no IP or port), or direct IP.**
beta.6 gives each player their own character: the friend joins with nothing and keeps what they pick up, their own
look, level and XP (XP earned together is shared), and players give each other items through the other's stand-in. A
map change no longer waits for a save: when one player travels, the other starts the same travel at once and both
arrive together. Fights are shared: an enemy that spots either player starts the fight in both games, and the enemies
spread over both players. Tested on one PC with two copies (runs 70-111); the first real session across two PCs
(2026-11-17, beta.4) ran the earlier features.
Use beta.6 on both PCs (an older version on the other PC does not connect; the log says so). Two independent copies of
the game share one campaign owned by the host: the client joins by receiving the host's save, each player controls their
own character, sees the other (position, rotation, walk/idle animation, the player's look, armor and helmet changes),
and the host owns NPC health and deaths, quest progress, door states and container contents; either player can lead a
map change. See
`TASKS.md` for the acceptance matrix with PASS / PARTIAL / NOT TESTED and evidence.

Download: the release zip (installer, loader, mod, this README). Each player needs their own copy of the game on Steam
(a Steam Families shared copy can't be played by two people at once). Unofficial fan mod, not affiliated with Obsidian
Entertainment or Xbox; it ships no game files. MIT license.

## How it plays (rules)

- **Saves:** only the host saves the campaign. In a joined game the friend's saving is switched off (its autosave and
  quicksave slots are the friend's own campaign's); the friend's own games, loaded from the main menu, save normally.
  The mod keeps the friend's character itself (see Characters).
- **Join:** the client starts at the main menu and joins; the host saves its current campaign ("CoopHost") and hands it
  over; the client loads it into its own save folder `CoopHost_<host id>` (never over the client's own saves).
- **Characters:** each player has their own. On the first join, the friend's character is the one of their own newest
  save (their look, level, skills, perks), with nothing in their pockets: no items, no money. From then on the friend
  keeps their own items (with ammo in the magazine, mods, durability), money, XP and level across every hand-over: before
  loading the campaign the host sends, the friend's mod saves its own character (a private save named "CoopSelf") and
  puts it into the host's campaign. The character is kept per host in `%LOCALAPPDATA%\tow2coop\characters\`. To start
  fresh, create a character in a new game of your own first (its save becomes your newest save), then join.
- **Progression:** level, skills and perks are each player's own (they travel with the character). XP is party XP:
  a kill, a lock pick, a hack, a conversation or an interaction earns XP for both players (each with their own
  multipliers); quest XP comes from the host's campaign (the friend's game does not add its own); discovering a place
  or a collectible earns XP only for the player who finds it.
- **Giving items:** look at the other player's stand-in from a few meters and press E. The game's own transfer screen
  opens: your inventory on the left, an empty "Container" on the right. Move items into it with the mouse, as with any
  container: each item leaves at once and lands in the other player's inventory (with the game's item notification).
  A given weapon or armor arrives as a new copy with the same level, rarity and mods. If the other player's game
  doesn't confirm it within 20 s (gone, loading), the item comes back to you. (The game only lets
  players take from world containers, not put items in them, so the stand-in is the way to give.)
- **Players:** each player moves, fights and loots in their own world; the other player appears as a stand-in NPC body
  that wears the other player's own look, armor and helmet.
- **Combat:** fights are shared. An enemy that sees either player starts the fight in both games, and in both it
  fights the same one: the host, the friend or a companion, as the game's own AI picks (it switches as it would), so
  the enemies spread over both players. Each player takes the hits of the enemies fighting them in their own game;
  the other player's stand-in takes the rest, unharmed. The host's game decides whom each enemy fights; for an enemy
  much nearer the friend than the host (the host more than 50 m away), the friend's game decides (far from a player,
  an enemy's AI sleeps in that player's game). Enemy health and deaths follow the host; damage the friend deals is sent
  to the host. Out of a fight each player is only seen in their own game, with their own stealth: the other player's
  stand-in is untargetable by the AI until a fight starts. A dead player's stand-in leaves the other game until they
  live again, and its enemies turn to the living player.
- **Quests and story:** shared both ways. Quest starts, objective updates (addendums), the global variables quest
  objectives test, and quest outcomes (completed, failed) made in either player's game reach the other's live, without
  a reload: a conversation or an objective of the friend's counts for the host's campaign too.
- **Time:** one clock. The friend's day and night follow the host's (waiting, sleeping, ship travel); while the host's
  game is paused in a menu, the friend's clock waits for it, and the friend's NPCs keep moving on their own.
- **Travel:** when either player travels (the game's own travel, such as a door into an interior), the other player
  starts the same travel at once and both arrive together (fast travel too). The host doesn't follow while in combat; the friend is then
  brought back by a hand-over. When the host loads a save or reloads after a death, the friend loads the same save
  (see Death and reload).
- **Death and reload:** when the host's game loads a save (Continue after a death, Load, Quickload), the same save
  goes to the friend at once and both load it together; the world goes back in time and the friend's character goes
  back to its last kept state (at most 3 minutes old). When the friend dies, the host's current game is fetched
  right away; the friend's Continue (or Load) then loads it, with their last kept character, next to the host: in a
  joined game the friend's own game never loads one of its own saves. A trauma kit revive just keeps playing.
- **Doors:** shared state; either player opens, closes or locks, the other world follows.
- **Containers and dead NPCs:** shared contents; an item taken by one player disappears from that container or body
  for the other, the taker keeps it. Two players taking the same item at the same moment may both get it.
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
| `bin/core/tow2coop.dll` (`core/*.cpp`) | Build check, UE 5.4 reflection, hooks on `UGameEngine::Tick` and `ProcessEvent`; co-op: `coop.cpp` (UDP link, stand-in, NPC authority, doors, containers, resync, travel following, giving items), `story.cpp` (quests/globals), `join.cpp` (save hand-over over TCP); in test copies the hub's bridge protocol for tests. |
| `qa/` | Test tooling: gamepool config, deploy/prep scripts, bridge client (`tow2.py`), input (`gamewin.py`), tests (`test_autojoin.py`, `test_npc_sync.py`, `test_melee.py`, `test_story.py`, `test_interact.py`, `test_look.py`, `test_anim.py`, `test_resync.py`, `test_client_saves.py`, `test_resume.py`, `test_steam_late.py`, `test_join.py`, `test_rejoin.py`, `test_key.py`, `test_steam_self.py`, `test_install.ps1`), the whole regression (`run_suite.ps1`), a targeted one (`run_quick.ps1 [-Only <steps>]`), soak (`soak_run.ps1`), memory guard (`memguard.ps1`). |

## Build

Needs llvm-mingw (portable, no install) and the coop-anything hub next to this folder (`tools/bridge/include`).

    powershell -NoProfile -File build.ps1        # proxy, fake Steam, core, then the proxy tests (all must PASS)

## Run two copies on one PC (testing)

Test copies are hard-linked, isolated installs made by the hub's `tools/gamepool` (config `qa/tow2.gamepool.json`).
One command runs the whole regression on a pair: `powershell -NoProfile -File qa/run_suite.ps1 -Out <folder>`
(`qa/soak_run.ps1 -Minutes 30` for the soak, `qa/run_key.ps1` for the shared key). Add `-Warm` when other games ran
since the last session: a start with an emptied graphics-driver shader cache takes 7-17 GB instead of ~4.5 GB, and one
copy started alone and closed normally (`qa/warmup.ps1`) refills it. The tests load a save copied from
your own game into each copy's sandbox (`qa/prep_instance.py --saves`); nothing is written to the real save folder.
Co-op settings come from the environment (else from `tow2coop.ini`, see below): `TOW2_COOP_ROLE=host|client` (or gamepool's `GAMEPOOL_ROLE`),
`TOW2_COOP_PORT` (UDP, default 27960; the save hand-over uses TCP port + 1), `TOW2_COOP_PEER=ip:port` for a client on
another PC, `TOW2_COOP_BIND` for the host (test copies: 127.0.0.1). Two copies on one 8 GB GPU need the VRAM budget
that `qa/prep_instance.py` writes (`Engine.ini`) and 960x540.

## Install for players

Each player runs the installer on their own PC, with the game closed (it finds the game through Steam; `-GameDir`
overrides, `-Find` only shows the folder). The two players must be **Steam friends**, each with their own copy:

    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Role host
    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Role client [-Friend "<host's Steam name>"]

To update, run the new version's installer the same way, with the game closed: it replaces the old files (no need to
uninstall first).

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

- Keep the host's game window un-minimized while a friend joins or a hand-over runs (a quest resync, the host loading a
  save): the game finishes a save only while its window is drawn (a minimized copy's save never completes, so the
  hand-over fails). Switching to another window without minimizing is fine.
- A join or a resync waits while the host can't save (combat, a conversation, a menu): it happens a few seconds after.

- Loading a save within the first ~15 s after start crashes the game (engine `LoadMap`); the client waits 60 s of
  frames before loading a received campaign. Players loading from the menu by hand are not affected.
- The remote player is a stand-in NPC that wears the other player's own look (face, hair, body) and armor and helmet;
  changes follow within a few seconds (a piece you don't wear yourself shows without its mods). It doesn't show the
  other player's weapon or attacks: you see them move, not shoot.
- Enemies run the game's own AI in both games, on the same target: their two copies can be 1-2 m apart (eased back
  together, snapped past 6 m). The stand-in never crouches: in a fight, enemies see it standing.
- Enemies that the game spawns during play (not placed in the map) are not shared: each game has its own.
- While the host's game is paused in a fight (a menu, or one of the game's own screens such as a flaw offer), the
  host's enemies wait; the friend's keep fighting the way the host's game last said.
- Co-op combat was tested against Ursopods (melee creatures) and an Order soldier fighting nearby; a large fight with
  many armed humans was not tried yet.
- The friend's character is kept by a private save the friend's game makes every 3 minutes and before a hand-over
  that isn't a reload (a map change the friend could not follow). On the host's reloads and after the friend's death,
  the last kept one is used: what the friend gained since then (up to 3 minutes) is lost, as the world goes back too.
  When the game can't save at the moment (combat, a conversation), the last kept one is used as well.
- Giving items: keyboard and mouse only (E opens the screen; a gamepad's interact button doesn't). The bots test the
  giving with the game's inventory functions, not with mouse clicks in the screen.
- Following a travel was tested with the game's travel into and out of an interior and with fast travel; ship travel
  was not tried yet.
- Quest outcomes made in the friend's game are set on the host's quest directly when the host's own quest graph has
  not got there by itself: the host's game then shows no "quest completed" message and gives no quest XP for it.
- In a joined game the friend can't load one of their own saves (Continue, Load and Quickload bring back the host's
  current game): to play their own campaign, go back to the main menu first. After a death the friend comes back with
  the character as last kept (up to 3 minutes earlier). If the host is in a fight, that reload waits until the host's
  game can save again.
- Continue on the death screen was not clicked by the bots (it follows the real mouse): they call the game's LoadGame,
  which is what Continue does.
- Door and container changes are sent three times without acknowledgement; on a very lossy link one can be missed.
  (Damage the client deals is resent until the host acknowledges it; quest updates are acknowledged too.)
