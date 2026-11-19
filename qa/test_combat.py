"""Co-op combat on a running pair (copy 0 host, copy 1 client, the test save loaded): the Ursopod group ~200 m from the
start (three level NPCs, one encounter).

A. The host hides in a crevice 8 m from them, the client steps into the open in front of one: the client's world sees
   the client first (out of a fight the stand-ins are neutral to all: each player is only seen in their own world);
   the host's world must fight too (the client's hint), the host's world deciding (both are near). Both stand-ins go
   on the players' team.
B. The host steps into the open too, 20 s: in the client's world each NPC fights whom the host's world says (the client,
   the host's stand-in, or the same companion); both players get fought at some point; both take hits from them in
   their own world; the two copies of each fighting NPC stay close.
C. The host walks 150 m away: the client's world decides the NPCs near it (its AI is the one awake), the host's copies
   follow it; the host comes back: the host's world decides again.
E. The client says it is dead (test switch): in the host's world its stand-in leaves and the NPCs turn to the host
   or the companions; alive again: its stand-in is back, in the fight.
D. The host kills them and every other NPC in a fight near the players: dead in both worlds, nothing left fighting or
   pinned; the stand-ins out of the fight (untargetable) again.
The players have pseudo god mode (they take hits, they don't die) and no fall damage during the test; god mode again
after. Usage: python -I test_combat.py [ABCD]"""
import math
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

FAILS = 0
NAMES = {0: "nobody", 1: "host", 2: "client", 3: "other"}


def check(ok, what, detail=""):
    global FAILS
    print(f"{'PASS' if ok else 'FAIL'} {what} {detail}", flush=True)
    FAILS += not ok
    return ok


def safe(f, d=None):
    try:
        return f()
    except Exception as e:  # noqa: BLE001
        return d if d is not None else "err " + str(e)[:160]


def ev(inst, expr):
    return call("eval", {"expr": expr}, inst=inst)["value"]


def uc(inst, ref, fn, args=None):
    return call("ue.call", {"ref": ref, "fn": fn, "args": args or {}}, inst=inst)


def loc(inst, ref):
    return ev(inst, ref + ".K2_GetActorLocation()")


def dist(a, b):
    return math.dist((a["X"], a["Y"], a["Z"]), (b["X"], b["Y"], b["Z"])) / 100


def health(inst, ref="pawn"):
    return uc(inst, ref + ".HealthComponent", "GetHealth")["ReturnValue"]


def fights(inst):
    return {f["npc"]: f for f in call("coop.fights", {}, inst=inst)}


def teleport(inst, to, face=None):
    uc(inst, "pawn", "K2_SetActorLocation", {"NewLocation": to, "bSweep": False, "bTeleport": True})
    if face:
        yaw = math.degrees(math.atan2(face["Y"] - to["Y"], face["X"] - to["X"]))
        uc(inst, "pc", "SetControlRotation", {"NewRotation": {"Pitch": -5, "Yaw": yaw, "Roll": 0}})


def find_group():
    """The Ursopods nearest the host: {short name: (host ref, client ref)}."""
    me = loc(0, "pawn")
    out = []
    for c in call("ue.find", {"class": "Character", "limit": 600}, inst=0):
        a, cls, path = c.split(" ", 2)
        if "_UAID_" not in path or not cls.startswith("Bear_T3_"):
            continue
        if safe(lambda: health(0, a), 0) <= 0:
            continue
        b = call("ue.find", {"path": path}, inst=1)
        if not b:
            continue
        out.append((dist(loc(0, a), me), path.split(".")[-1], a, b[0].split()[0]))
    out.sort()
    return {name: (a, b) for _, name, a, b in out[:3]}


def players(mode):
    for i in (0, 1):
        hc = "pawn.HealthComponent"
        if mode == "test":
            uc(i, hc, "SetGod", {"bEnabled": False})
            uc(i, hc, "SetPseudoGod", {"bEnabled": True})
            call("ue.set", {"path": hc + ".bImmuneToFallDamage", "value": True}, inst=i)  # probe 90: a fall killed the host
        else:
            uc(i, hc, "SetPseudoGod", {"bEnabled": False})
            uc(i, hc, "SetGod", {"bEnabled": True})
            mx = uc(i, hc, "GetMaxHealth")["ReturnValue"]
            uc(i, hc, "SetHealth", {"Health": mx})


HOST_PID = 0


def shots():  # both windows, for the evidence (a check failed)
    if not HOST_PID:
        return
    from gamewin import Game
    out = Path(r"C:\coop-anything\local\the-outer-worlds-2\evidence") / f"combat-{time.strftime('%H%M%S')}-host.png"
    safe(lambda: Game(HOST_PID).shot(str(out)))
    print("info: screenshot", out)


def unpause(when):
    """The host's game paused by a screen the fights opened (a flaw offer wants Escape held): photograph it, hold
    Escape (reject), as a player would."""
    for k in range(3):
        if not call("coop.status", {}, inst=0).get("hostPaused"):
            return
        shots()
        print(f"info: the host's game is paused {when}: Escape held")
        if not HOST_PID:
            return
        from gamewin import Game
        safe(lambda: Game(HOST_PID).key("ESCAPE", hold_s=2.0))
        time.sleep(1.0)


def main(parts):
    group = find_group()
    if not check(len(group) >= 2, "the Ursopod group is there in both worlds", ", ".join(group)):
        return 1
    names = list(group)
    s1h, s1c = group[names[0]]
    start = loc(0, "pawn")
    s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
    check(s0.get("combatFights", -1) == 0 and not fights(0) and not fights(1), "nobody fights at the start",
          f"host fights {s0.get('combatFights')}, lists {len(fights(0))}/{len(fights(1))}")
    check(s0.get("standinUntargetable") is True and s1.get("standinUntargetable") is True, "out of a fight both stand-ins are untargetable by AI",
          f"host's {s0.get('standinUntargetable')} ({s0.get('standinSpell')}), client's {s1.get('standinUntargetable')} ({s1.get('standinSpell')})")
    players("test")
    try:
        p = loc(0, s1h)
        ang = math.atan2(start["Y"] - p["Y"], start["X"] - p["X"])
        crevice = {"X": p["X"] + 800 * math.cos(ang), "Y": p["Y"] + 800 * math.sin(ang), "Z": p["Z"] + 20}  # probe 90: safe, hidden
        pc = loc(1, s1c)
        fwd = ev(1, s1c + ".GetActorForwardVector()")
        front = {"X": pc["X"] + 700 * fwd["X"], "Y": pc["Y"] + 700 * fwd["Y"], "Z": pc["Z"] + 20}       # probe 90: safe, in the open
        if "A" in parts:
            unpause("before A")
            teleport(0, crevice, p)
            time.sleep(2.5)
            check(not [f for f in fights(0).values() if f["here"]], "the hidden host is not fought yet", str(list(fights(0).values()))[:200])
            teleport(1, front, pc)
            t0 = time.time()
            seen_c = seen_h = None
            while time.time() - t0 < 20 and (seen_h is None or seen_c is None):
                fc, fh = fights(1), fights(0)
                if seen_c is None and any(f["here"] == 2 and f["npc"] in group for f in fc.values()):
                    seen_c = time.time() - t0
                if seen_h is None and any(f["here"] and f["npc"] in group for f in fh.values()):
                    seen_h = time.time() - t0
                time.sleep(0.25)
            s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
            check(seen_c is not None and (seen_h is None or seen_c <= seen_h + 0.3), "the client was seen in its own world first",
                  f"client's world after {seen_c} s, host's world after {seen_h} s")
            check(seen_h is not None and seen_c is not None and seen_h - seen_c < 3.0, "the host's world fights too (seen by either: a fight for both)",
                  f"{(seen_h or 0) - (seen_c or 0):.1f} s later; hints sent {s1.get('combatHintsSent')}, in {s0.get('combatHintsIn')}")
            check(s0.get("combatHintsIn", 0) >= 1, "it came from the client's hint (the host's world could not see the stand-in)",
                  f"hints in {s0.get('combatHintsIn')}")
            time.sleep(1.0)
            s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
            check(s0.get("standinFighting") and s1.get("standinFighting") and s0.get("standinUntargetable") is False and s1.get("standinUntargetable") is False,
                  "both stand-ins are on the players' team and targetable in the fight",
                  f"host's {s0.get('standinFighting')}/{s0.get('standinUntargetable')}, client's {s1.get('standinFighting')}/{s1.get('standinUntargetable')}")
            owners = {f["owner"] for f in fights(1).values()}
            check(owners == {"host"}, "with both players near, the host's world decides", str(owners))
        if "B" in parts:
            unpause("before B")
            side = {"X": front["X"] + 400 * -fwd["Y"], "Y": front["Y"] + 400 * fwd["X"], "Z": front["Z"]}
            teleport(0, side, pc)
            hp0, hp1 = health(0), health(1)
            st0 = call("coop.status", {}, inst=1)
            snaps0, fsnaps0 = st0.get("npcSnapped", 0), st0.get("npcFightSnapped", 0)
            dmg0 = st0.get("dmgSent", 0)  # the client does not attack in B: what it reports is someone else's damage
            agree = total = 0
            others = []
            words, drift, misses = set(), [], []
            last_hit = {0: set(), 1: set()}
            t0 = time.time()
            while time.time() - t0 < 20:
                fh, fc = fights(0), fights(1)
                for name, f in fh.items():
                    if f["owner"] != "host" or f["word"] == 0:
                        continue
                    if name not in group:  # another NPC in a fight (run 98: an OA soldier); its copy may sleep far from the client
                        c = fc.get(name)
                        if len(others) < 4:
                            others.append(f"{name[:24]} says {NAMES[f['word']]}, client's copy {NAMES[c['here']] if c else 'nobody'}"
                                          f" at {c.get('dist', -1) if c else -1:.0f} m, other here {c.get('otherHere') if c else None}")
                        continue
                    words.add(f["word"])
                    total += 1
                    c = fc.get(name)
                    if c and c["here"] == f["word"]:
                        agree += 1
                    elif len(misses) < 6:
                        misses.append(f"{name[:22]} host says {NAMES[f['word']]}, client's copy fights {NAMES[c['here']] if c else 'nobody'}")
                    if name in group:
                        a, b = group[name]
                        d = safe(lambda: dist(loc(0, a), loc(1, b)), None)
                        if isinstance(d, float):
                            drift.append(d)
                for i in (0, 1):
                    who = safe(lambda: str(uc(i, "pawn.HealthComponent", "GetLastHitInstigator")["ReturnValue"]), "")
                    if who and who != "None":
                        last_hit[i].add(who.split()[1] if len(who.split()) > 1 else who)
                time.sleep(0.5)
            ratio = agree / total if total else 0
            check(total >= 20 and ratio >= 0.85, "the client's copies fight whom the host's world says",
                  f"{agree}/{total} samples ({ratio:.0%}); {'; '.join(misses)}")
            check({1, 2} <= words, "the enemies went for both players", "words seen: " + ", ".join(NAMES[w] for w in sorted(words)))
            dh, dc = hp0 - health(0), hp1 - health(1)
            check(dh > 0 and any("Bear_" in w for w in last_hit[0]), "the host took hits in the host's world", f"{dh:.0f} health, hit by {sorted(last_hit[0])}")
            check(dc > 0 and any("Bear_" in w for w in last_hit[1]), "the client took hits in the client's world", f"{dc:.0f} health, hit by {sorted(last_hit[1])}")
            if drift:
                med = statistics.median(drift)
                st = call("coop.status", {}, inst=1)
                snaps, fsnaps = st.get("npcSnapped", 0) - snaps0, st.get("npcFightSnapped", 0) - fsnaps0
                check(med < 3.0, "the two copies of each NPC stay close in the fight", f"median {med:.1f} m, max {max(drift):.1f} m over {len(drift)} samples")
                check(fsnaps <= 4, "few visible snaps of fighting NPCs in 20 s (eased instead)", f"{fsnaps} snaps of fighting NPCs ({snaps} of all NPCs), eased {st.get('npcEased')}")
                check(st.get("dmgSent", 0) - dmg0 <= 2, "the client reported no damage it did not deal (it does not attack in B)",
                      f"{st.get('dmgSent', 0) - dmg0} reports; hits not its own seen {st.get('dmgNotMine', 0)} (run 118 before the fix: 106 reports)")
            if others:
                print("info: other fights: " + "; ".join(others))
            s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
            print(f"info: client pins {s1.get('combatPins')}, detections {s1.get('combatDetections')}; host hints in {s0.get('combatHintsIn')}, detections {s0.get('combatDetections')}")
        if "C" in parts:
            unpause("before C")
            far = {"X": start["X"], "Y": start["Y"], "Z": start["Z"] + 20}
            teleport(0, far)
            t0 = time.time()
            took = None
            while time.time() - t0 < 8:
                fc = fights(1)
                if fc and all(f["owner"] == "client" for f in fc.values() if f["npc"] in group):
                    took = time.time() - t0
                    break
                time.sleep(0.25)
            check(took is not None, "the host 150+ m away: the client's world decides the NPCs near it", f"after {took} s" if took is not None else str(list(fights(1).values()))[:240])
            time.sleep(2)
            fh = fights(0)
            pinned = [n for n, f in fh.items() if n in group and f["pinned"]]
            words = {n: NAMES[f["word"]] for n, f in fh.items() if n in group}
            check(bool(pinned) or not any(w in ("host", "client") for w in words.values()), "the host's copies follow the client's world",
                  f"pinned {len(pinned)}, words {words}, records in {call('coop.status', {}, inst=0).get('combatRecsIn')}")
            teleport(0, side, pc)
            t0 = time.time()
            back = None
            while time.time() - t0 < 8:
                fc = fights(1)
                if all(f["owner"] == "host" for f in fc.values() if f["npc"] in group):
                    back = time.time() - t0
                    break
                time.sleep(0.25)
            check(back is not None, "the host back: the host's world decides again", f"after {back} s; handoffs {call('coop.status', {}, inst=0).get('combatHandoffs')}")
        if "E" in parts:
            unpause("before E")
            call("coop.set", {"fakeDead": True}, inst=1)
            t0 = time.time()
            left = None
            while time.time() - t0 < 6:
                s0 = call("coop.status", {}, inst=0)
                on_it = [n for n, f in fights(0).items() if n in group and f["here"] == 2]
                if s0.get("peerDead") and s0.get("puppet") == "none" and not on_it:
                    left = time.time() - t0
                    break
                time.sleep(0.25)
            check(left is not None, "the client dead: its stand-in leaves the host's world",
                  f"after {left} s" if left is not None else f"peerDead {s0.get('peerDead')}, stand-in {s0.get('puppet')}, on it {on_it}")
            t0 = time.time()
            while time.time() - t0 < 5:  # the AI picks another target by itself once its target is gone
                fh = {n: f for n, f in fights(0).items() if n in group}
                words = {NAMES[f["word"]] for f in fh.values()}
                if "client" not in words:
                    break
                time.sleep(0.25)
            s0 = call("coop.status", {}, inst=0)
            check("client" not in words, "the enemies turned to the living player",
                  f"after {time.time() - t0:.1f} s, words now: " + ", ".join(sorted(words)) + f"; stand-in {s0.get('puppet')[:14]}, client decides {s0.get('combatOwned')};"
                  + "; ".join(f" {n[:22]} {f['owner']} word {NAMES[f['word']]} here {NAMES[f['here']]} pinned {f['pinned']} at {f.get('dist', -1):.0f} m" for n, f in fh.items()))
            call("coop.set", {"fakeDead": False}, inst=1)
            t0 = time.time()
            back = None
            while time.time() - t0 < 6:
                s0 = call("coop.status", {}, inst=0)
                if not s0.get("peerDead") and s0.get("puppet") != "none" and s0.get("standinFighting") and s0.get("standinUntargetable") is False:
                    back = time.time() - t0
                    break
                time.sleep(0.25)
            check(back is not None, "alive again: its stand-in is back, in the fight", f"after {back} s; {s0.get('puppet')[:40]}, spell {s0.get('standinSpell')}")
            time.sleep(1.5)
            s0 = call("coop.status", {}, inst=0)
            time.sleep(1.5)
            unpause("after E")
            s0 = call("coop.status", {}, inst=0)
            check(s0.get("npcsSentAgo", 99) < 1.0 and not s0.get("hostPaused"), "the host's game runs on after the stand-in came back",
                  f"NPCs sent {s0.get('npcsSentAgo', -1):.1f} s ago; host paused {s0.get('hostPaused')}, settled {s0.get('settled')}, peer {s0.get('peer')}, error {s0.get('error')!r}")
        if "D" in parts:
            unpause("before D")
            for name, (a, b) in group.items():
                safe(lambda: uc(0, a + ".HealthComponent", "Suicide", {"bPreventRevival": True}))
            others = {}
            for _ in range(3):  # every other NPC in a fight near the players (run 101: an OA soldier kept the party fighting)
                for c in call("ue.find", {"class": "Character", "limit": 600}, inst=0):
                    a, cls, path = c.split(" ", 2)
                    short = path.split(".")[-1]
                    if short in others or short in group or "_UAID_" not in path:
                        continue
                    if safe(lambda: uc(0, a, "IsInCombat")["ReturnValue"], False) is True and safe(lambda: dist(loc(0, a), loc(0, "pawn")), 999) < 80:
                        others[short] = a
                        safe(lambda: uc(0, a + ".HealthComponent", "Suicide", {"bPreventRevival": True}))
                time.sleep(1.0)
            print("info: other NPCs in a fight, killed: " + ", ".join(n[:30] for n in others))
            t0 = time.time()
            dead = False
            while time.time() - t0 < 8 and not dead:
                dead = all(safe(lambda: health(1, b), 1) <= 0 for a, b in group.values())
                if not dead:
                    unpause("after the kills")  # a paused host sends no deaths
                time.sleep(0.25)
            check(dead, "the host killed them: dead in the client's world too", f"{time.time() - t0:.1f} s")
            time.sleep(2.5)
            time.sleep(2.0)
            unpause("after the kills")
            time.sleep(1.0)
            left = [f for i in (0, 1) for f in fights(i).values() if f["npc"] in group and (f["here"] or f["pinned"])]
            check(not left, "nothing left fighting or pinned", str(left)[:200])
            t0 = time.time()
            calm = False
            while time.time() - t0 < 20 and not calm:
                s0, s1 = call("coop.status", {}, inst=0), call("coop.status", {}, inst=1)
                calm = not s0.get("standinFighting") and not s1.get("standinFighting")
                time.sleep(0.5)
            calm = calm and s0.get("standinUntargetable") is True and s1.get("standinUntargetable") is True
            check(calm, "after the fight the stand-ins are out of it and untargetable again",
                  f"{time.time() - t0:.1f} s; host's {s0.get('standinFighting')}/{s0.get('standinUntargetable')} ({s0.get('standinSpell')}),"
                  f" client's {s1.get('standinFighting')}/{s1.get('standinUntargetable')} ({s1.get('standinSpell')}); party fight {s0.get('partyFight')}/{s1.get('partyFight')};"
                  f" stand-ins {s0.get('puppet')[:14]}/{s1.get('puppet')[:14]}, dead {s0.get('meDead')}/{s1.get('meDead')}, NPCs sent {s0.get('npcsSentAgo', -1):.1f} s ago;"
                  f" host paused {s0.get('hostPaused')}, settled {s0.get('settled')}, peer {s0.get('peer')}, world age {s0.get('worldAge', -1):.0f} s, error {s0.get('error')!r}")
    finally:
        safe(lambda: players("normal"))
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return FAILS


if __name__ == "__main__":
    HOST_PID = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "ABCED"))
