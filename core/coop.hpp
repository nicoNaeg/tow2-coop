#pragma once
#include <string>
#include <coopbridge/json.h>

#include <atomic>
#include <cstdint>

namespace coop {
// The game's UPlayerInfoComponent::AddXP(XP, EXPGainReason, TArray* details): hooked in main.cpp (party XP).
using AddXpFn = bool (*)(void* self, int32_t xp, uint8_t reason, void* details);
extern AddXpFn add_xp_original, add_xp_entry;  // the original (no hook) and the hooked entry (as the game calls it)
bool xp_blocked(void* self, int32_t xp, uint8_t reason);  // game thread, before an XP gain: drop it?
void on_xp(void* self, int32_t xp, uint8_t reason);   // game thread, after a gain: share it?
coopbridge::json::Value xp_test(int xp, int reason);  // test bridge: an XP gain through the hooked AddXP
// The game's UIndianaUI::OpenItemTransfer(AActor* other): its two-list transfer screen between the player's inventory
// and the other actor's (found in main.cpp; nullptr when this build differs). Giving items goes through it.
using OpenTransferFn = void* (*)(void* hud, void* other);
extern OpenTransferFn open_transfer_fn;
coopbridge::json::Value give_open();  // test bridge, game thread: open the transfer screen on the other player's stand-in
// The game's mod installs (found in main.cpp; nullptr when this build differs): UWeapon::EquipModByClass(const
// TSubclassOf<UWeaponMod>&, bool bIgnoreSlotModdability), and the armor one EquipArmorModByClassDebug calls.
using WeaponModFn = bool (*)(void* weapon, void* const* mod_class, bool ignore_moddability);
using ArmorModFn = void (*)(void* armor, void* const* mod_class, bool, bool, bool, bool);
extern WeaponModFn weapon_equip_mod;
extern ArmorModFn armor_equip_mod;
coopbridge::json::Value mods_test(const coopbridge::json::Value& p);  // test bridge, game thread: {item, add?} -> mods
// The game's SaveGameManager::LoadGame (hooked in main.cpp), game thread. on_load_request: -1 = load as asked, else the
// ELoadGameResult to return without loading. on_load_started: after the game took the request (result: ELoadGameResult).
int on_load_request(const std::wstring& save_name);
void on_load_started(const std::wstring& save_name, uint8_t result);
bool close_screens();  // game thread: true while a screen the mod opened is still open (it is asked to close)
// UFunctions whose ProcessEvent calls the hook hands to observe() (door state changes, container item removals).
extern std::atomic<int> watch_count;
extern void* watch[8];
void observe(void* object, void* function, const void* parms);  // any thread; only copies, frame() does the work
// Shared conversations: while the other player leads the conversation we play, our own response and skip input is
// dropped in ProcessEvent (any thread; conv_block is the cheap test).
extern std::atomic<bool> conv_block;
bool conv_swallow(void* object, void* function);
// The game's damage function (hooked in main.cpp): true = drop it, it comes from the stand-in (its shots, its swings).
bool standin_damage(void* causer, void* instigator);
extern bool damage_hooked;
bool start(std::string& why);  // reads TOW2_COOP_* from the environment; false = co-op stays off
void frame(float dt);          // game thread, once per frame
coopbridge::json::Value status();
coopbridge::json::Value set_options(const coopbridge::json::Value& p);  // {story,doors,npcs,scan,combat...: bool}
coopbridge::json::Value fights();  // test bridge, game thread: NPCs fighting or pinned in this world
coopbridge::json::Value spawned(const coopbridge::json::Value& p);  // test bridge, game thread: spawn one / the shared list
coopbridge::json::Value steam_probe();
coopbridge::json::Value lobby_probe();  // test: an invisible Steam lobby made, searched, left (off the game thread)
coopbridge::json::Value face_debug();  // game thread: the look we send, the one we got, the last error (test bridge)  // any thread, blocks up to ~15 s: Steam link self-test without another player
}  // namespace coop
