# tow2-coop — The Outer Worlds 2 co-op mod

**Experimental two-player co-op for The Outer Worlds 2 (Windows / Steam).** Play a shared campaign with separate characters through Steam or a direct IP connection.

## Download & play

1. **Download `tow2coop-setup.exe`** from the project's latest release. Both players must install **the same version** (currently **v0.6.0**) and own separate Steam copies of the game.
2. **Close the game and run the installer on both PCs.** It detects the Steam installation; choose **Change...** if needed, then **Install**. The installer is unsigned, so Windows SmartScreen may display a warning.
3. **Host:** Launch the game and, on the main menu, choose **Let's play together (co-op)** → **I'll host** (Steam friends), **I'll host: anyone can join** (public lobby), or **I'll host a new game**. Continue your campaign, or start a new one.
4. **Friend:** From the same co-op menu, choose **I'll join a friend's game**, then select the host. You can also try Steam's **Join game** option from the friends list.
5. **Play together.** The friend receives the host's campaign and joins with their own character. Joining can take about a minute.

**Steam play needs no IP address, port forwarding, or manual network setup.** Each player needs their own Steam copy; one Steam Families shared license cannot be used by both simultaneously.

> **Compatibility:** Only the Steam game **v1.2.0.1**, build **24225441** (app **1449110**), is supported. Other builds leave the mod disabled. Use matching mod versions on both PCs.

## What works

- **Shared adventure:** Host-owned campaign, quests, story progress, dialogue, doors, switches, containers, loose loot, and quest items.
- **Separate characters:** Each player has their own appearance, equipment, inventory, skills, perks, and level. Earned XP is largely shared.
- **Co-op combat:** Players see each other move, equip weapons, attack, and fight enemies together; enemy health and deaths follow the host.
- **Travel and reloads:** Either player can trigger travel; host save reloads propagate to the friend.
- **Dialogue:** The player who starts a conversation chooses for both; each game plays the conversation.
- **Item trading:** Look at the other player and press **E** (or controller **X**) to open the transfer screen.

## Important rules

- **Only the host saves the campaign.** The friend's personal saves remain separate, and the mod keeps their character and gear between sessions.
- On first join, the friend starts with their own character's appearance and progression, but **without their previous items or money**. They keep items acquired during co-op.
- To **start a new campaign together**, the host selects **I'll host a new game**. Both players create their characters before joining the same story.
- While joined, the friend cannot load their own campaign. Return to the main menu to play separately.
- Menus do **not pause** gameplay during co-op.

## Known limitations

**This is a test build, not a finished multiplayer mode.** Most recent testing was performed with two game instances on one PC; cross-account testing of public Steam lobbies and Steam **Join game** is still pending.

- Joining or resyncing waits if the host is in combat, dialogue, or another state where the game cannot save.
- On reload or death, the friend's recent character progress may roll back to the last private save (up to roughly **3 minutes**, sometimes longer if saving was blocked).
- Enemy positions or animations can differ slightly between the two games.
- Some dialogue consequences, quest notifications, or quest items may not synchronize perfectly.
- Simultaneous looting can occasionally duplicate an item.

For detailed test results and remaining work, see [`TASKS.md`](TASKS.md).

## Other installation options

**Release ZIP:** Extract it and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
```

**Direct IP:** Supported as an alternative to Steam. The host shares the connection key; the client configures the host address and key. The host must allow **UDP 27960** and **TCP 27961**, or use a VPN such as Tailscale or ZeroTier. See the installer options in `install.ps1`.

**Update:** Close the game and run the new installer; no uninstall needed.

**Uninstall:** Run `tow2coop-setup.exe` and select **Uninstall**, or use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Uninstall
```

**Troubleshooting:** Check `tow2coop.log` in `Arkansas\Binaries\Win64\` inside the game folder. Confirm both players use v0.6.0 and the exact supported game build.

---

Unofficial fan-made mod; not affiliated with Obsidian Entertainment or Xbox. Contains no game files. **MIT licensed.**
