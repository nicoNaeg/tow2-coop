# tow2-coop: two-player co-op for The Outer Worlds 2 (work in progress)

Status: **v0.5.4, a test build: co-op through Steam (friends, or public games; no IP or port), or direct IP.**
0.5.4: a quest item either player gets (a key, a pass, a sample) goes to both, and after joining the friend holds the
host's; a loose item one player picks up from the ground is gone in the other game too, and the host's save keeps it
gone; a switch or a radio one player uses takes the same state in the other game. 0.5.3: a choice the other player's character can't take for real (a skill check) is taken past its lock when the leader
picks it (0.5.1 did it only for a lock the test simulated; a real one restarted the conversation, a camera cut); a
quest set without its banner shows "Quest Completed" (or "Quest Failed") and its title, in the game's language.
Tested further: a door's wave of enemies, a fight against a squad of armed soldiers, ship travel, a new game together
up to the first planet. 0.5.2: a controller in the co-op menu (Y opens it, the D-pad moves, A picks, B goes back) and for giving items (X in
front of the other player); a hive's, a hatch's or a door's wave of enemies that starts in one game starts in the other
too (before, its enemies could stay hidden in their hive there for a minute); a quest that has to be set without the
game's banner shows its title in the game's notification. 0.5.1: the leader's pick is made in the other game even when that player's character couldn't take it (no restart),
and a conversation's payments are taken from the leader only. 0.5.0: the player who starts a conversation leads it, and the other player's game plays the same conversation (the
same lines, camera, voices and cinematics, the same choices on screen); only the leader chooses, and their pick is
made in both games. A new game together from the very beginning: the host hosts a New Game, the friend makes their
character in the game's character creator and joins the host's start. A quest one player completes is completed in
the other game the game's own way (its banner, journal and XP). 0.4.0: a Windows installer (`tow2coop-setup.exe`);
public games anyone can join, and Steam's "Join game" from the friends list; the game's menus no longer pause it in
co-op; the host's companions show where they are in both worlds; the friend's character is kept a few seconds after it
changes; doors, containers and XP are resent until confirmed. 0.3.0: host and join from the game's own main menu; the other
player's stand-in holds their weapon with its mods and uses it, and crouches with them; enemies the game spawns
during play are shared. The 0.2.0 betas gave each player their own character: the friend joins with nothing and keeps what they pick up, their own
look, level and XP (XP earned together is shared), and players give each other items through the other's stand-in. A
map change no longer waits for a save: when one player travels, the other starts the same travel at once and both
arrive together. Fights are shared: an enemy that spots either player starts the fight in both games, and the enemies
spread over both players. Tested on one PC with two copies (runs 70-111); the first real session across two PCs
(2026-11-17, beta.4) ran the earlier features. 0.3.0 to 0.5.1 were tested on one PC too (runs 112-170).
Use 0.5.4 on both PCs (another version on the other PC does not connect; the log says so). Two independent copies of
the game share one campaign owned by the host: the client joins by receiving the host's save, each player controls their
own character, sees the other (position, rotation, walk/idle animation, the player's look, armor and helmet changes),
and the host owns NPC health and deaths, quest progress, door states and container contents; either player can lead a
map change. See
`TASKS.md` for the acceptance matrix with PASS / PARTIAL / NOT TESTED and evidence.

Download: `tow2coop-setup.exe` (the installer, with the mod inside), or the release zip (the same files and a
script installer). Each player needs their own copy of the game on Steam
(a Steam Families shared copy can't be played by two people at once). Unofficial fan mod, not affiliated with Obsidian
Entertainment or Xbox; it ships no game files. MIT license.

## How it plays (rules)

- **Saves:** only the host saves the campaign. In a joined game the friend's saving is switched off (its autosave and
  quicksave slots are the friend's own campaign's); the friend's own games, loaded from the main menu, save normally.
  The mod keeps the friend's character itself (see Characters).
- **Host and join:** in the main menu, the Moon Man's choices have one more: *Let's play together (co-op).* Pick it
  (its number key or a click), then *I'll host* (your Steam friends can join your game), *I'll host a new game* (you
  both start from the very beginning) or *I'll join a friend's game* (the Steam friends who host are listed: pick
  one). The host then loads or continues their campaign as usual;
  the friend's game joins it: the host saves its current campaign ("CoopHost") and hands it over, and the friend's game
  loads it into its own save folder `CoopHost_<host id>` (never over the friend's own saves). The same choice shows
  who you play with, and stops hosting or joining.
- **Characters:** each player has their own. On the first join, the friend's character is the one of their own newest
  save (their look, level, skills, perks), with nothing in their pockets: no items, no money. From then on the friend
  keeps their own items (with ammo in the magazine, mods, durability), money, XP and level across every hand-over: before
  loading the campaign the host sends, the friend's mod saves its own character (a private save named "CoopSelf") and
  puts it into the host's campaign. The character is kept per host in `%LOCALAPPDATA%\tow2coop\characters\`.
- **A new game together:** the host picks *I'll host a new game*: their game starts the game's own New Game (the
  character creator, then the intro). The friend picks that host as usual (*Join ... (a new game: you make your
  character first)*): their own New Game starts, they make their character, and once the intro allows a save (after
  its opening conversation) the mod keeps that character and the friend joins the host's game with it. A friend who
  joins any host without a character of their own makes one the same way first.
- **Conversations:** the player who starts a conversation leads it. The other player is moved next to them and their
  game plays the same conversation: the same lines (the same one where the game draws a line at random), its camera,
  voices and cinematics, and the same choices on screen, but only the leader chooses (the other's keys and clicks on
  the choices do nothing); the leader's pick is made in both games, and when the leader's conversation ends, so does
  theirs. Both games run the conversation's own effects (items, quests, reputation), as each player's game does for
  them. No game pauses for a conversation (the base game doesn't).
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
  that wears the other player's own look, armor and helmet, and holds the same weapon with its mods (a silencer, a
  scope), drawn or holstered as they do, and crouches when they crouch.
  When they fire, the stand-in's weapon fires too (its animation, flash, sound and tracer), toward where their shot
  went; when they swing a melee weapon or bash with a gun, it plays the game's own attack animation; when they reload,
  it reloads. The
  stand-in's own shots and swings never hurt anyone: the other player's real hits arrive from their game.
- **Combat:** fights are shared. An enemy that sees either player starts the fight in both games, and in both it
  fights the same one: the host, the friend or a companion, as the game's own AI picks (it switches as it would), so
  the enemies spread over both players. Each player takes the hits of the enemies fighting them in their own game;
  the other player's stand-in takes the rest, unharmed. The host's game decides whom each enemy fights; for an enemy
  much nearer the friend than the host (the host more than 50 m away), the friend's game decides (far from a player,
  an enemy's AI sleeps in that player's game). Enemy health and deaths follow the host; the hits the friend deals are sent to
  the host (only theirs: their companions' hits are the host's game's own). Out of a fight each player is only seen in their own game, with their own stealth: the other player's
  stand-in is untargetable by the AI until a fight starts. A dead player's stand-in leaves the other game until they
  live again, and its enemies turn to the living player. Enemies the game spawns during play (a hive, a hatch, a door,
  a summon) are shared too: each game tells the other about its own; the other takes one of its own of the same kind
  nearby (both spawned it), or spawns a copy, and from then on it is shared like the others. A hive's or a hatch's
  enemies are the level's own (the same in both games), waiting in their spawner until their encounter's wave starts:
  a wave that starts in one game (its player came near, or the story) is started in the other with the game's own
  ActivateWave, so they come out in both.
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
- **Loose items** (on the ground, a table, a shelf): one player picks it up, it is gone in the other game too (now, or
  when that part of the map loads there); the host's save keeps it gone. Items dropped during play are each game's own.
- **Quest items** (keys, passes, samples): either player getting one gives it to the other too, whatever the source (the
  ground, a container, a gift, an NPC); after joining, the friend holds the host's (their own character came without
  the campaign's). The friend's game can drop a few of them again by itself while it catches up on the story after the
  join (in the tests, two leftovers of later quests out of 38); the host keeps them, and the mod's log names them.
- **Switches and radios:** one player uses one, the other game's takes the same state, the game's way (its own effects
  run there too).
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
(`qa/soak_run.ps1 -Minutes 30` for the soak, `qa/run_key.ps1` for the shared key); `qa/run_quick.ps1 -Out <folder>` is the
shorter one, and `-Menu` makes the players host and join through the main menu's co-op choice instead of by themselves. Add `-Warm` when other games ran
since the last session: a start with an emptied graphics-driver shader cache takes 7-17 GB instead of ~4.5 GB, and one
copy started alone and closed normally (`qa/warmup.ps1`) refills it. The tests load a save copied from
your own game into each copy's sandbox (`qa/prep_instance.py --saves`); nothing is written to the real save folder.
Co-op settings come from the environment (else from `tow2coop.ini`, see below): `TOW2_COOP_ROLE=host|client|auto` (or gamepool's
`GAMEPOOL_ROLE`; `auto`: picked in the main menu; `TOW2_COOP_MENU=1` keeps the copy's set side for direct IP and asks in the menu),
`TOW2_COOP_PORT` (UDP, default 27960; the save hand-over uses TCP port + 1), `TOW2_COOP_PEER=ip:port` for a client on
another PC, `TOW2_COOP_BIND` for the host (test copies: 127.0.0.1). Two copies on one 8 GB GPU need the VRAM budget
that `qa/prep_instance.py` writes (`Engine.ini`) and 960x540.

## Install for players

Each player runs **`tow2coop-setup.exe`** on their own PC, with the game closed: it finds the game through Steam
(*Change...* picks another folder), and *Install* (or *Update*) places the mod; *Uninstall* removes it. Windows may
warn about an unsigned program (*More info*, *Run anyway*). Each player needs their own copy of the game.

Both players install the same: who hosts and who joins is picked in the game's main menu. The script installer from
the zip does the same from a command line:

    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1

 (`-Role host` or
`-Role client [-Friend "<host's Steam name>"]` instead set it for good: the host always hosts, the friend always joins.)

To update, run the new version's installer the same way, with the game closed: it replaces the old files (no need to
uninstall first).

In the main menu, the host picks the co-op choice and *I'll host* (Steam friends) or *I'll host: anyone can join
(public game)*, then loads their campaign; the other player picks the co-op choice, *I'll join a friend's game* and
the host (friends who host and public games are listed), or uses *Join game* on the host in the Steam friends list (it
starts the game if needed; from a game of their own, they go back to the main menu first): about a minute later the friend receives the host's campaign and
loads it (with `-Role` set at install, no menu choice: the friend's game joins by itself). **Over Steam** (the default) there is no address, no
open port and no firewall prompt: the game's own Steam session carries everything through Valve's relay, encrypted.
The players find each other through Steam rich presence (the host shows `tow2coop=host`, the friend shows
`tow2coop=join:<host>`); `-Friend` names the host when several friends host, or when the host's game details are
private. Only Steam friends are accepted, and in a public game the players in its Steam lobby.

**Direct IP** instead (`-Transport udp`, or a client given `-Peer`): the host's run prints a key, the friend runs
`install.ps1 -Role client -Peer <host address> -Key <key>`, and the host's PC must accept UDP 27960 and TCP 27961
(forward both ports or use a VPN such as Tailscale or ZeroTier). Only a player with the key becomes the peer, and the
campaign goes only to that player's address.

The installer places `dsound.dll` (loader), `tow2coop.dll` (the mod) and `tow2coop.ini` (settings, section `[coop]`:
`role`, `transport`, `host`, `peer`, `port`, `key`, `bind`) in `<game dir>\Arkansas\Binaries\Win64\`, refuses to
replace another mod's `dsound.dll`, and changes nothing of the game. When something goes wrong, `tow2coop.log` next to
the mod says what the link did (who was found, session state, hand-over). `qa/test_setup.ps1` and
`qa/test_install.ps1` check the two installers on a fake game folder; `qa/test_steam_self.py` checks the Steam hand-over on one copy (the copy sends its campaign to
itself over Steam, compares every byte and loads it).

## Remove

    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Uninstall

It deletes `dsound.dll`, `tow2coop.dll`, `tow2coop.ini` and `tow2coop.log` (only a `dsound.dll` that is the mod's). The
mod writes nothing else in the game folder; a client keeps the received campaign as the save `CoopHost_<host id>`, which
can be deleted from the game's Load menu. Steam "Verify integrity of game files" does not remove extra files.

## Known issues

- While a hand-over save runs (a join, a resync, a reload), a minimized game is shown again behind the other windows
  for those few seconds (a minimized game never finishes a save), then minimized again.
- A join or a resync waits while the host can't save (combat, a conversation, a menu): it happens a few seconds after.

- Loading a save within the first ~15 s after start crashes the game (engine `LoadMap`); the client waits 60 s of
  frames before loading a received campaign. Players loading from the menu by hand are not affected.
- The remote player is a stand-in NPC that wears the other player's own look (face, hair, body) and armor and helmet;
  changes follow within a few seconds (a piece you don't wear yourself shows without its mods).
- Enemies run the game's own AI in both games, on the same target: their two copies can be 1-2 m apart (eased back
  together, snapped past 6 m).
- The friend's hits are told to the host by the game's "last hitter" of the enemy: a hit of the friend's in the same tenth
  of a second as a companion's on the same enemy is not counted.
- Spawned enemies were tested with Ursopods spawned the way a spawner does (by the test), and the game's own hives and
  floor hatches with mantisaur waves and a door (`Spawner_Human_Door_OnceEver`), started by the test the game's way
  (`Encounter.ActivateWave`). A wave started by the story or by the wave before it was not met in the test map (the
  mod passes on a wave's start whatever caused it). When both games spawn one of the same kind more than 15 m apart, each gets a copy of
  the other's (one more enemy).
- The co-op choice is in the main menu only (not in the pause menu: to start hosting, go back to the main menu). It is
  picked with the mouse, its number key or a controller (Y, the D-pad, A, B). The controller was tested with buttons
  the bots set through the mod (no controller plugged in), not with a real one. Public games and Steam's "Join game" were tested on
  one Steam account only (a public game made, searched for, left); a join between two accounts is still to try.
- In co-op the game's menus (pause, inventory, map, a flaw offer) no longer pause it: the world goes on, as in a
  multiplayer game.
- In a conversation the other player sees the leader's choices as their own game shows them: a choice their character
  can't take (a skill check, a flaw) shows as unavailable there, and is taken all the same when the leader picks it,
  without a restart. Tested with a real skill check (the light worker in the Order's headquarters, the host's skills
  high, the friend's at 0). Following a conversation, the friend's game counts the leader's choices as the friend's:
  it can offer the friend a flaw for them (for example Compulsive Liar after several lies). The leader's pick is told
  by the line it leads to: when two of the leader's responses lead to the same line, the friend's game may take the
  other one (the same line, no camera cut; that response's own effects, such as a skill check's XP, may differ).
- Each game runs a conversation's effects: an item the NPC gives goes to both players; a payment (money, an item handed
  over, a bounty, a purchase) is taken from the leader only.
- Co-op combat was tested against Ursopods (melee creatures), an Order soldier fighting nearby, and a squad of up to
  9 Protectorate soldiers (rifles, revolvers, a drone) the host attacked: up to 8 fought at once in both games, each
  soldier's two copies stayed within 6 m.
- The friend's character is kept by a private save the friend's game makes about 5 s after it changes (items, money,
  XP; at most one every 15 s), every 3 minutes, and before a hand-over that isn't a reload. On the host's reloads and
  after the friend's death the last kept one is used: what the friend gained in the last seconds is lost, more when the
  game could not save then (combat, a conversation), as the world goes back too.
- Giving items: E or a controller's X (its interact button) in front of the other player opens the screen; in the
  screen the game's own controls apply. The bots test the giving with the game's inventory functions, not with mouse
  clicks in the screen.
- Following a travel was tested with the game's travel into and out of an interior, with fast travel, and with ship
  travel (boarding the player ship, flying it to another planet's landing: the ship is then at the same place in both
  games), started by the bots through the game's travel functions rather than the ship's console.
- A quest one player completes is walked to the same end in the other game with the game's own quest steps (a step a
  second: the banner, the journal and the quest's XP come as usual). When that path ends the other way or gets stuck
  (more than 15 steps or 30 s), the quest's state is set directly: no banner then, but the game's notifications show
  "Quest Completed" (or "Quest Failed") and the quest's title. A quest the other game had not started is started there first, then walked.
- In a joined game the friend can't load one of their own saves (Continue, Load and Quickload bring back the host's
  current game): to play their own campaign, go back to the main menu first. After a death the friend comes back with
  the character as last kept (up to 3 minutes earlier). If the host is in a fight, that reload waits until the host's
  game can save again.
- Continue on the death screen was not clicked by the bots (it follows the real mouse): they call the game's LoadGame,
  which is what Continue does.
- Out of a fight the friend's world shows the host's companions where they are in the host's world (they follow the
  host); in a fight they have their own AI in each world again.
- A player without a character of their own (no save, nothing kept) is told in the join list to make one first, with a
  New Game of their own; joining from the very start of the host's new game waits until the host's game can save.
  Not yet tried from start to end.
- `tow2coop-setup.exe` is not signed: Windows SmartScreen may warn before it runs.