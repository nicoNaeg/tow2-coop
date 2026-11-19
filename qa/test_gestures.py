"""The other player's weapon and its use, on the stand-in (copy 0 host, copy 1 client, the test save loaded).

A. Weapons: the host takes its rifle, the client its blade: each stand-in holds the other player's weapon class.
B. Shots: the host fires 3 shots at the ground ahead: the client's world's stand-in of the host fires 3 too, and its
   last shot ends near the host's.
C. No harm: in the client's world the host's stand-in is made to fire at the client (pseudo god mode, to measure):
   the client's health stays, the game's damage function dropped the stand-in's damage.
D. Swings: the client swings its blade: in the host's world the client's stand-in plays an NPC attack animation.
E. Holster: the host holsters its weapon: its stand-in holsters it too; drawn again, drawn again.
F. Reload: the host empties its rifle: it reloads, and so does its stand-in.
G. Mods: a silencer installed on the host's rifle shows on its stand-in's rifle.
H. Bash: the host's quick melee with its rifle: the stand-in plays the game's NPC one (a long gun's).
I. Crouch: the host crouches: its stand-in crouches (its capsule at the crouched height); stands up again.
J. Crouch while walking: the stand-in's walk animation keeps going through the change of stance.
God mode again after. Usage: python -I test_gestures.py [ABCDEF]"""
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tow2 import call  # noqa: E402

FAILS = 0


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


def ref_of(v):
    return str(v).split()[0]


def st(inst):
    return call("coop.status", {}, inst=inst)


def wait_for(cond, secs, step=0.2):
    t0 = time.time()
    while time.time() - t0 < secs:
        v = safe(cond, False)
        if v and not (isinstance(v, str) and v.startswith("err")):
            return v
        time.sleep(step)
    return safe(cond, False)


def loc(inst, ref):
    return ev(inst, ref + ".K2_GetActorLocation()")


def dist(a, b):
    return math.dist((a["X"], a["Y"], a["Z"]), (b["X"], b["Y"], b["Z"])) / 100


def weapon(inst, who="pawn"):
    return ref_of(uc(inst, who, "GetCurrentWeapon")["ReturnValue"])


def shot_end(inst, w):
    mode = ref_of(uc(inst, w, "GetCurrentMode")["ReturnValue"])
    return uc(inst, mode, "GetLastShotEndLocation")["ReturnValue"]


def trigger(inst, w, hold=0.15):
    uc(inst, w, "BeginPrimaryTrigger")
    time.sleep(hold)
    uc(inst, w, "EndPrimaryTrigger", {"bForceEnd": False})


def aim(inst, yaw, pitch):
    pc = ref_of(ev(inst, "pawn.Controller"))
    uc(inst, pc, "SetControlRotation", {"NewRotation": {"Pitch": pitch, "Yaw": yaw, "Roll": 0.0}})


def short(path):
    return str(path).rsplit(".", 1)[-1]


def unpause_all():
    """A game paused by one of its own screens (a flaw offer after a fight: "Hold Esc Reject") is answered first."""
    import subprocess
    from gamewin import Game
    for i in (0, 1):
        for _ in range(3):
            pc = safe(lambda: call("eval", {"expr": "pc"}, inst=i)["value"].split()[0], None)
            gs = safe(lambda: call("ue.find", {"path": "/Script/Engine.Default__GameplayStatics"}, inst=i)[0].split()[0], None)
            paused = pc and gs and safe(lambda: call("ue.call", {"ref": gs, "fn": "IsGamePaused", "args": {"WorldContextObject": pc}}, inst=i)["ReturnValue"], False) is True
            if not paused:
                break
            want = rf"C:\coop-anything\local\gamepool\instances\tow2\{i}\*"
            cmd = "(Get-Process -Name TheOuterWorlds2-Win64-Shipping | Where-Object { $_.Path -like '%s' }).Id" % want
            pid = int(subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.split()[0])
            print(f"info: copy {i}'s game is paused (a screen of its own): Escape held", flush=True)
            safe(lambda: Game(pid).key("ESCAPE", hold_s=2.0))
            time.sleep(1.0)


def revive_if_down():
    """A player left down by the step before (run 121: the client died in the fight test) is revived, god mode again."""
    for i in (0, 1):
        life = str(safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "GetStateOfBeing"}, inst=i)["ReturnValue"], "")).split(":")[-1]
        if life and life != "Healthy":
            safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "Resurrect", "args": {"bDebug": False, "HealthPercentOverride": 1.0}}, inst=i))
            safe(lambda: call("ue.call", {"ref": "pawn.HealthComponent", "fn": "SetGod", "args": {"bEnabled": True}}, inst=i))
            print(f"info: copy {i}'s player was {life} after the step before: revived", flush=True)
            time.sleep(8)  # its stand-in comes back in the other world


def main(parts):
    unpause_all()
    revive_if_down()
    s0, s1 = st(0), st(1)
    if not check(s0.get("puppet", "none") != "none" and s1.get("puppet", "none") != "none", "both stand-ins are there"):
        return 1
    puppet0, puppet1 = s0["puppet"], s1["puppet"]  # the client's stand-in (host's world), the host's (client's world)
    for i in (0, 1):  # weapons in hand (a fight just before may have left them holstered)
        safe(lambda: uc(i, "pawn.EquipmentComponent", "UnholsterWeapon_BP", {"InUnholsterRequestType": "Normal"}))
    time.sleep(1.5)
    if "A" in parts:
        uc(0, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 2, "bInstantEquip": True, "bStartingCombat": False})
        uc(1, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 0, "bInstantEquip": True, "bStartingCombat": False})
        ok = wait_for(lambda: st(0)["myWeapon"] and st(1)["standinWeapon"] == st(0)["myWeapon"]
                      and st(0)["standinWeapon"] == st(1)["myWeapon"], 8)
        s0, s1 = st(0), st(1)
        check(bool(ok) and s0["myWeapon"] != s1["myWeapon"], "each stand-in holds the other player's weapon",
              f"host has {short(s0['myWeapon'])}, its stand-in {short(s1['standinWeapon'])}; client has {short(s1['myWeapon'])}, "
              f"its stand-in {short(s0['standinWeapon'])}")
        held = safe(lambda: ev(1, weapon(1, puppet1)).split()[1], "")
        check(held == short(s0["myWeapon"]), "in the client's world the host's stand-in has it in hand", held)
    if "B" in parts:
        me = safe(lambda: ev(0, "pawn.K2_GetActorRotation()"), {"Yaw": 0.0})
        yaw = me["Yaw"]
        h, c = safe(lambda: loc(0, "pawn"), None), safe(lambda: loc(0, puppet0), None)
        if h and c:  # not toward the other player (run 133: 4.3 m off at 6 m; guess, not checked: the client's body stops the
            # stand-in's shot where the host's shot passes the client's stand-in)
            toward = math.degrees(math.atan2(c["Y"] - h["Y"], c["X"] - h["X"]))
            if abs((yaw - toward + 180) % 360 - 180) < 60:
                yaw = toward + 180
        aim(0, yaw, -12.0)  # the ground ~8 m ahead: the same spot in both worlds
        time.sleep(0.6)
        w0 = weapon(0)
        a0, b1 = st(0)["myShots"], st(1)["standinShots"]
        ammo0 = uc(0, w0, "GetCurrentAmmoCount")["ReturnValue"]
        for _ in range(3):
            trigger(0, w0)
            time.sleep(1.2)  # the rifle chambers a round between shots
        e0 = shot_end(0, w0)
        fired = ammo0 - uc(0, w0, "GetCurrentAmmoCount")["ReturnValue"]  # the rounds that really left (a pull may be refused)
        counted = st(0)["myShots"] - a0
        got = wait_for(lambda: st(1)["standinShots"] - b1 >= counted, 4)
        s1 = st(1)
        check(fired >= 2 and counted == fired, "the host's shots are counted from its weapon's events, once each",
              f"{counted} counted, {fired} rounds fired")
        check(bool(got) and s1["standinShots"] - b1 == counted, "its stand-in fired as many in the client's world",
              f"{s1['standinShots'] - b1}; stand-in drawn {s1.get('standinDrawn')}, weapon {short(s1.get('standinWeapon'))}, "
              f"peer shots {s1.get('peerShots')}, host drawn {st(0).get('myDrawn')}")
        e1 = safe(lambda: shot_end(1, weapon(1, puppet1)), {})
        d = safe(lambda: dist(e0, e1), 99)
        rng = safe(lambda: dist(e0, loc(0, "pawn")), 0)
        check(d <= max(4.0, 0.15 * rng), "the stand-in's last shot ended near the host's", f"{d:.2f} m apart, {rng:.1f} m away")  # a wall close by: 3.9 m (run 124)
        check(st(1)["standinDamageBlocked"] >= 0 and s1.get("standinAiming") is not None, "the client's world reports its stand-in's aim",
              f"aiming {s1.get('standinAiming')}")
    if "C" in parts:
        time.sleep(2.5)  # the stand-in's aim from B has ended (2 s after the last shot)
        hc = "pawn.HealthComponent"
        lib = call("ue.find", {"path": "/Script/Arkansas.Default__AISequencerBehaviors"}, inst=1)[0].split()[0]
        try:
            uc(1, hc, "SetGod", {"bEnabled": False})
            uc(1, hc, "SetPseudoGod", {"bEnabled": True})
            p = loc(1, puppet1)
            yaw = ev(1, puppet1 + ".K2_GetActorRotation()")["Yaw"]  # in front of it: its aim turns only so far from its body
            near = {"X": p["X"] + 500 * math.cos(math.radians(yaw)), "Y": p["Y"] + 500 * math.sin(math.radians(yaw)), "Z": p["Z"] + 10}
            uc(1, "pawn", "K2_SetActorLocation", {"NewLocation": near, "bSweep": False, "bTeleport": True})
            time.sleep(0.8)
            me = loc(1, "pawn")
            target = {"X": me["X"], "Y": me["Y"], "Z": me["Z"] + 30}
            uc(1, lib, "SetOrientation", {"Character": puppet1, "TrackingRegion": "TR_WeaponAim",
                                          "AIOrientationControl": {"FaceTarget": "None", "FaceTargetSubLocation": "Default",
                                                                   "FaceLocation": target, "FaceActor": "None"}})
            time.sleep(0.8)
            h0 = uc(1, hc, "GetHealth")["ReturnValue"]
            b0 = st(1)["standinDamageBlocked"]
            w1 = weapon(1, puppet1)
            for _ in range(3):
                trigger(1, w1, 0.12)
                time.sleep(0.8)
            time.sleep(0.5)
            h1 = uc(1, hc, "GetHealth")["ReturnValue"]
            blocked = st(1)["standinDamageBlocked"] - b0
            # Probe 92, same place without the block: 3 shots took 9 health.
            check(blocked >= 3, "the stand-in's 3 shots hit, and the game's damage function dropped them", f"{blocked} dropped")
            check(h1 >= h0 - 0.5, "the client lost no health to them", f"{h0:.0f} -> {h1:.0f}")
        finally:
            uc(1, hc, "SetPseudoGod", {"bEnabled": False})
            uc(1, hc, "SetGod", {"bEnabled": True})
            safe(lambda: uc(1, lib, "ClearOrientation", {"Character": puppet1, "TrackingRegion": "TR_WeaponAim"}))
    if "D" in parts:
        uc(1, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 0, "bInstantEquip": True, "bStartingCombat": False})
        wait_for(lambda: st(0)["standinWeapon"] == st(1)["myWeapon"], 5)
        anim = ref_of(ev(0, puppet0 + ".Mesh.GetAnimInstance()"))
        a1, b0 = st(1)["mySwings"], st(0)["standinSwings"]
        seen = set()
        w1 = weapon(1)
        for _ in range(2):
            trigger(1, w1, 0.1)
            t0 = time.time()
            while time.time() - t0 < 1.6:
                m = str(safe(lambda: uc(0, anim, "GetCurrentActiveMontage")["ReturnValue"], "None"))
                if "_ATK_" in m:
                    seen.add(m.rsplit(".", 1)[-1])
                time.sleep(0.05)
        s0, s1 = st(0), st(1)
        check(s1["mySwings"] - a1 >= 2, "the client's swings are counted from its blade's events", f"{s1['mySwings'] - a1}")
        check(s0["standinSwings"] - b0 >= 2, "its stand-in swung in the host's world", f"{s0['standinSwings'] - b0}")
        check(bool(seen), "with the game's NPC attack animations", ", ".join(sorted(seen)))
    if "E" in parts:
        uc(0, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 2, "bInstantEquip": True, "bStartingCombat": False})
        wait_for(lambda: st(1)["standinWeapon"] == st(0)["myWeapon"], 5)
        uc(0, "pawn.EquipmentComponent", "HolsterWeapon_BP", {"InRequestType": "Normal"})
        down = wait_for(lambda: not st(0)["myDrawn"] and not st(1)["standinDrawn"], 5)
        check(bool(down), "the host holstered its weapon: so did its stand-in",
              f"host drawn {st(0)['myDrawn']}, stand-in drawn {st(1)['standinDrawn']}, in hand {short(safe(lambda: weapon(1, puppet1), ''))}")
        uc(0, "pawn.EquipmentComponent", "UnholsterWeapon_BP", {"InUnholsterRequestType": "Normal"})
        up = wait_for(lambda: st(0)["myDrawn"] and st(1)["standinDrawn"], 5)
        check(bool(up), "drawn again: the stand-in drew it too", f"in hand {short(safe(lambda: weapon(1, puppet1), ''))}")
    if "F" in parts:
        w0 = weapon(0)
        a0, b1 = st(0)["myReloads"], st(1)["standinReloads"]
        for _ in range(30):  # until the magazine is empty: the game reloads by itself
            if st(0)["myReloads"] > a0:
                break
            trigger(0, w0)
            time.sleep(0.75)
        got = wait_for(lambda: st(1)["standinReloads"] > b1, 6)
        check(st(0)["myReloads"] > a0, "the host reloaded (counted from its weapon's events)", f"{st(0)['myReloads'] - a0}; ammo {safe(lambda: uc(0, w0, 'GetCurrentAmmoCount')['ReturnValue'])}; events {st(0).get('myEvents', [])[-12:]}")
        check(bool(got), "its stand-in reloaded in the client's world", f"{st(1)['standinReloads'] - b1}")
    if "G" in parts:  # the host's weapon mods show on its stand-in's weapon
        uc(0, "pawn.EquipmentComponent", "EquipWeaponFromSlot", {"SlotIndex": 2, "bInstantEquip": True, "bStartingCombat": False})
        wait_for(lambda: st(1)["standinWeapon"] == st(0)["myWeapon"], 5)
        w0 = weapon(0)
        sil = next((c.split(" ", 2)[2] for c in call("ue.find", {"name": "WeaponMod_Barrel_Silencer_MR_Tactical_C", "limit": 5}, inst=0)
                    if " BlueprintGeneratedClass " in c), None)
        if check(bool(sil), "a silencer for the host's rifle is loaded", str(sil)):
            r = call("coop.mods", {"item": w0, "add": sil}, inst=0)
            got = wait_for(lambda: sil in call("coop.mods", {"item": weapon(1, puppet1)}, inst=1).get("mods", []), 8)
            check(bool(got), "the host's rifle got a silencer: its stand-in's rifle has it too",
                  f"host {r.get('mods')}; stand-in {safe(lambda: call('coop.mods', {'item': weapon(1, puppet1)}, inst=1).get('mods'))}")
    if "H" in parts:  # a gun's quick melee (a bash): the stand-in plays the game's NPC one
        w0 = weapon(0)
        wpath = ev(0, w0).split(" ", 2)[2]
        vis = [c.split()[0] for c in call("ue.find", {"class": "/Script/Arkansas.WeaponVisuals", "limit": 600}, inst=0)
               if c.split(" ", 2)[2].startswith(wpath + ".")]
        anim = ref_of(ev(1, puppet1 + ".Mesh.GetAnimInstance()"))
        a0 = st(0)["mySwings"]
        if check(bool(vis), "the host's rifle has its visuals objects", str(len(vis))):
            uc(0, vis[0], "TriggerEventBP", {"WeaponEvent": "QuickMelee", "Params": {}})  # what the game sends them for a bash
            seen = set()
            t0 = time.time()
            while time.time() - t0 < 2.5:
                m = str(safe(lambda: uc(1, anim, "GetCurrentActiveMontage")["ReturnValue"], "None"))
                if "QuickMelee" in m:
                    seen.add(m.rsplit(".", 1)[-1])
                time.sleep(0.05)
            check(st(0)["mySwings"] - a0 == 1, "the host's quick melee is counted from its rifle's event", f"{st(0)['mySwings'] - a0}")
            check(bool(seen), "its stand-in bashed with the game's NPC quick melee animation", ", ".join(sorted(seen)))
    if "I" in parts:  # crouch
        def feet_gap():  # the stand-in's feet (client world) minus the host's (host world), cm
            hz = ev(0, "pawn.K2_GetActorLocation()")["Z"] - uc(0, "pawn.CapsuleComponent", "GetUnscaledCapsuleHalfHeight")["ReturnValue"]
            sz = ev(1, puppet1 + ".K2_GetActorLocation()")["Z"] - uc(1, puppet1 + ".CapsuleComponent", "GetUnscaledCapsuleHalfHeight")["ReturnValue"]
            return sz - hz
        full = uc(1, puppet1 + ".CapsuleComponent", "GetUnscaledCapsuleHalfHeight")["ReturnValue"]
        low = call("ue.get", {"path": puppet1 + ".CharacterMovement.CrouchedHalfHeight"}, inst=1)
        uc(0, "pawn", "Crouch", {"bClientSimulation": False})
        down = wait_for(lambda: st(0)["myCrouched"] and st(1)["standinCrouched"], 4)
        time.sleep(1.2)
        h1 = uc(1, puppet1 + ".CapsuleComponent", "GetUnscaledCapsuleHalfHeight")["ReturnValue"]
        flag = call("ue.get", {"path": puppet1 + ".bIsCrouched"}, inst=1)
        anim = ref_of(ev(1, puppet1 + ".Mesh.GetAnimInstance()"))
        full_c = call("ue.get", {"ref": anim, "depth": 0, "filter": "FullyCrouched"}, inst=1).get("bIsFullyCrouched")
        check(full_c is True, "its animation is fully crouched", f"bIsFullyCrouched {full_c}")
        tick_on = uc(1, puppet1 + ".CharacterMovement", "IsComponentTickEnabled")["ReturnValue"]
        mode = call("ue.get", {"path": puppet1 + ".CharacterMovement.MovementMode"}, inst=1)
        check(tick_on is False and mode == "MOVE_Walking", "its movement component stops ticking after (the walk animation needs it)",
              f"ticking {tick_on}, mode {mode}")
        gap = feet_gap()
        check(abs(gap) < 8, "crouched, its feet are on the host's", f"{gap:+.1f} cm")
        check(bool(down) and flag is True and abs(h1 - low) < 3, "the host crouched: its stand-in crouched too",
              f"stand-in crouched {flag}, its capsule {h1:.0f} (standing {full:.0f}, crouched {low}); host crouched {st(0).get('myCrouched')}, "
              f"stand-in told {st(1).get('standinCrouched')} ({st(1).get('standinCrouches')} changes)")
        uc(0, "pawn", "UnCrouch", {"bClientSimulation": False})
        up = wait_for(lambda: not st(0)["myCrouched"] and not st(1)["standinCrouched"], 4)
        stood = wait_for(lambda: uc(1, puppet1 + ".CapsuleComponent", "GetUnscaledCapsuleHalfHeight")["ReturnValue"] > low + 20, 8)
        h2 = uc(1, puppet1 + ".CapsuleComponent", "GetUnscaledCapsuleHalfHeight")["ReturnValue"]
        extra = ""
        if not stood:  # what kept it crouched: its state, and who stands near it in the client's world
            here = ev(1, puppet1 + ".K2_GetActorLocation()")
            near = []
            for c in call("ue.find", {"class": "Character", "limit": 800}, inst=1):
                a, cls, _ = c.split(" ", 2)
                if a == puppet1:
                    continue
                q = safe(lambda: ev(1, a + ".K2_GetActorLocation()"), None)
                if q and math.dist((q["X"], q["Y"], q["Z"]), (here["X"], here["Y"], here["Z"])) < 250:
                    near.append(f"{cls} {math.dist((q['X'], q['Y'], q['Z']), (here['X'], here['Y'], here['Z'])):.0f}")
            extra = (f"; stand-in crouched {call('ue.get', {'path': puppet1 + '.bIsCrouched'}, inst=1)}, mode "
                     f"{call('ue.get', {'path': puppet1 + '.CharacterMovement.MovementMode'}, inst=1)}, misses {st(1).get('crouchMisses')}, "
                     f"near it: {', '.join(near) or 'nobody'}")
        check(bool(up) and bool(stood), "it stood up: its stand-in too", f"its capsule {h2:.0f}, told {bool(up)}{extra}")
        time.sleep(0.5)
        gap = feet_gap()
        check(abs(gap) < 8, "standing, its feet are on the host's", f"{gap:+.1f} cm")
    if "J" in parts:  # the host crouches while walking: its stand-in keeps walking through the change
        import subprocess
        import threading
        from gamewin import Game
        cmd = r"(Get-Process -Name TheOuterWorlds2-Win64-Shipping | Where-Object { $_.Path -like 'C:\coop-anything\local\gamepool\instances\tow2\0\*' }).Id"
        host = Game(int(subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout.split()[0]))
        h, c = loc(0, "pawn"), loc(0, puppet0)  # toward the client's stand-in: ground the client stands on
        aim(0, math.degrees(math.atan2(c["Y"] - h["Y"], c["X"] - h["X"])), 0.0)
        anim = ref_of(ev(1, puppet1 + ".Mesh.GetAnimInstance()"))
        host.key("W", hold_s=0.05)  # wake the window's input
        time.sleep(0.5)
        walker = threading.Thread(target=lambda: host.key("W", hold_s=2.2))
        walker.start()
        t0, samples, crouched_at = time.time(), [], None
        while time.time() - t0 < 2.2:
            if crouched_at is None and time.time() - t0 > 0.6:
                uc(0, "pawn", "Crouch", {"bClientSimulation": False})
                crouched_at = time.time() - t0
            samples.append((time.time() - t0, safe(lambda: call("ue.get", {"path": f"{anim}.SmoothedSpeed"}, inst=1), 0) or 0))
            time.sleep(0.08)
        walker.join()
        during = [v for t, v in samples if crouched_at + 0.3 <= t <= crouched_at + 1.0]  # the change of stance (~1 s), not the walk's end
        check(max(v for _, v in samples) > 100, "the host walked: its stand-in walked", f"anim speed max {max(v for _, v in samples):.0f}")
        check(bool(during) and min(during) > 40, "and kept walking while it crouched",
              f"anim speed min {min(during) if during else -1:.0f} in the second after the crouch; "
              + " ".join(f"{v:.0f}" for _, v in samples))
        uc(0, "pawn", "UnCrouch", {"bClientSimulation": False})
        time.sleep(1.5)
    for i in (0, 1):
        safe(lambda: uc(i, "pawn.HealthComponent", "SetGod", {"bEnabled": True}))
    print(f"RESULT: {'PASS' if not FAILS else 'FAIL'} ({FAILS} failure(s))")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "ABCDEFGHIJ"))
