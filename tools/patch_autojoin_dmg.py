"""One-off source patch for core/coop.cpp: automatic join from the main menu, acknowledged damage events."""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")


def rep(old, new):
    global s
    assert old in s, old[:80]
    s = s.replace(old, new, 1)


rep("kResync = 8, kResyncRequest = 9, kInteract = 10;", "kResync = 8, kResyncRequest = 9, kInteract = 10, kDamageAck = 11;")
# Damage carries an event id
rep('''struct Damage {
    Header h;
    uint64_t id;
    float amount;
};''', '''struct Damage {
    Header h;
    uint32_t eid;  // client-unique; the host acknowledges it and applies each eid once
    uint64_t id;
    float amount;
};''')
# state
rep('''    bool watches_set = false;''', '''    bool watches_set = false;
    struct PendingDamage { Damage d; double next; int tries; };
    std::vector<PendingDamage> dmg_pending;   // client: until the host acknowledges
    std::vector<uint32_t> dmg_seen;           // host: eids already applied
    uint32_t next_dmg_eid = 1;
    double start_time = 0, next_join_request = 0;
    uint64_t dmg_resent = 0, join_requests = 0;''')
# client_report_damage: queue reliably
rep('''        Damage d{};
        fill_header(d.h, kDamage, sizeof d);
        d.id = kv.first, d.amount = n.pending_damage;
        send_raw(&d, sizeof d);
        n.pending_damage = 0;
        g.dmg_sent++;''', '''        Damage d{};
        d.eid = (g.me << 16) ^ g.next_dmg_eid++;
        d.id = kv.first, d.amount = n.pending_damage;
        g.dmg_pending.push_back({d, 0.0, 0});
        n.pending_damage = 0;
        g.dmg_sent++;''')
# resend loop + ack handling: add helper after client_report_damage definition end (before apply_damage)
rep('''void apply_damage(const Damage& d) {''', '''void resend_damage() {
    for (size_t i = 0; i < g.dmg_pending.size();) {
        auto& p = g.dmg_pending[i];
        if (g.now >= p.next) {
            if (p.tries >= 25) { g.dmg_pending.erase(g.dmg_pending.begin() + i); continue; }  // 5 s: the host is gone
            fill_header(p.d.h, kDamage, sizeof p.d);
            send_raw(&p.d, sizeof p.d);
            if (p.tries) g.dmg_resent++;
            p.tries++, p.next = g.now + 0.2;
        }
        i++;
    }
}

void apply_damage(const Damage& d) {''')
# host: dedupe + ack in receive
rep('''        if (h.type == kDamage && g.host) {
            if (n != static_cast<int>(sizeof(Damage))) { g.bad++; continue; }
            Damage d;
            memcpy(&d, buf, sizeof d);
            g.recv++;
            apply_damage(d);
            continue;
        }''', '''        if (h.type == kDamage && g.host) {
            if (n != static_cast<int>(sizeof(Damage))) { g.bad++; continue; }
            Damage d;
            memcpy(&d, buf, sizeof d);
            g.recv++;
            if (std::find(g.dmg_seen.begin(), g.dmg_seen.end(), d.eid) == g.dmg_seen.end()) {
                apply_damage(d);
                g.dmg_seen.push_back(d.eid);
                if (g.dmg_seen.size() > 1024) g.dmg_seen.erase(g.dmg_seen.begin(), g.dmg_seen.begin() + 512);
            }
            send_typed(kDamageAck, &d.eid, 4);
            continue;
        }
        if (h.type == kDamageAck && !g.host) {
            uint32_t eid;
            if (n != static_cast<int>(sizeof(Header)) + 4) { g.bad++; continue; }
            memcpy(&eid, buf + sizeof(Header), 4);
            for (size_t i = 0; i < g.dmg_pending.size(); i++)
                if (g.dmg_pending[i].d.eid == eid) { g.dmg_pending.erase(g.dmg_pending.begin() + i); break; }
            continue;
        }''')
# the stale-sequence filter would drop a resent damage copy whose header seq is old: resends get fresh seqs (fill_header) - ok.
# frame: resend + auto-join request
rep('''        if (!g.host) {
            client_report_damage();''', '''        if (!g.host) {
            client_report_damage();
            resend_damage();''')
rep('''    if (g.host && g.resync_sends > 0 && g.now >= g.next_resync_send) {''', '''    // Automatic join: a client at the main menu (front end up for a minute: an earlier load crashes the engine) asks
    // the host for its campaign every 5 s until a hand-over starts.
    if (!g.host && !gameplay && g.now - g.start_time > 60.0 && g.now >= g.next_join_request && g.now >= g.fetch_quiet_until + 90.0) {
        uint32_t reason = 2;
        send_typed(kResyncRequest, &reason, 4);
        g.next_join_request = g.now + 5.0;
        g.join_requests++;
    }
    if (g.host && g.resync_sends > 0 && g.now >= g.next_resync_send) {''')
# host: reason-aware resync request (the join request arrives while the client is not in gameplay)
rep('''        if (h.type == kResyncRequest && g.host) {
            g.resync_requests++;
            request_resync("client could not apply a quest outcome");
            continue;
        }''', '''        if (h.type == kResyncRequest && g.host) {
            uint32_t reason = 1;
            if (n == static_cast<int>(sizeof(Header)) + 4) memcpy(&reason, buf + sizeof(Header), 4);
            g.resync_requests++;
            request_resync(reason == 2 ? "client joins from the main menu" : "client could not apply a quest outcome");
            continue;
        }''')
# start time
rep('''    g.on = true;
    why = std::string(g.host''', '''    g.on = true;
    g.start_time = clock_s();
    why = std::string(g.host''')
rep('''.set("drops", static_cast<double>(g.drops))''', '''.set("drops", static_cast<double>(g.drops)).set("dmgResent", static_cast<double>(g.dmg_resent)).set("dmgPending", static_cast<double>(g.dmg_pending.size())).set("joinRequests", static_cast<double>(g.join_requests))''')
p.write_text(s, encoding="utf-8", newline="\n")
print("patched", p)
