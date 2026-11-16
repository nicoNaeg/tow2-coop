// Minimal Unreal Engine 5.4 reflection access for The Outer Worlds 2 (Steam 1.2.0.1, CL-2994920).
// Reads the engine's own tables (object array, name pool, FField property chains) and calls UFunctions through
// UObject::ProcessEvent (vtable slot 79 on this build: the engine branch adds GetVersePath and CollectSaveOverrides).
// Layouts are the stock 5.4 ones; verify() checks them against live objects before anything else runs.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <coopbridge/json.h>

namespace ue {
using json = coopbridge::json::Value;
struct UObject;
using UClass = UObject;
using UStruct = UObject;
using UFunction = UObject;
struct FField;
using FProperty = FField;

struct FName { uint32_t index = 0, number = 0; };
struct FString { wchar_t* data = nullptr; int32_t num = 0, max = 0; };  // TArray<TCHAR>, num counts the NUL
template <class T> struct TArray { T* data; int32_t num, max; };

namespace off {
constexpr int obj_flags = 0x08, obj_index = 0x0C, obj_class = 0x10, obj_name = 0x18, obj_outer = 0x20;
constexpr int field_next = 0x28;
constexpr int struct_super = 0x40, struct_children = 0x48, struct_props = 0x50, struct_size = 0x58;
constexpr int func_flags = 0xB0, func_numparms = 0xB4, func_parmssize = 0xB6, func_retoffset = 0xB8;
constexpr int ff_class = 0x08, ff_owner = 0x10, ff_next = 0x20, ff_name = 0x28;
constexpr int prop_dim = 0x38, prop_size = 0x3C, prop_flags = 0x40, prop_offset = 0x4C, prop_extra = 0x78;
constexpr int enum_names = 0x40;
constexpr int pe_slot = 79;
}  // namespace off

constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400, CPF_ConstParm = 0x2, CPF_ReferenceParm = 0x8000000;

struct Rvas {
    uint32_t objarray, namepool, namepool_ready, fname_ctor, process_event;
};

// Setup and checks
bool init(uint8_t* base, const Rvas& r, std::string& err);  // addresses only, no reads of engine state
bool ready();                                               // name pool and object array filled in
bool verify(std::string& report);                           // layouts against live objects; false = do not run
extern void* g_process_event;                               // original ProcessEvent (trampoline once hooked)

// Names
std::string name(FName n);
std::string name_of(const UObject* o);
FName make_name(const std::string& s, bool add);  // FName{0,0} when not found and !add

// Objects
int num_objects();
UObject* object_at(int i);
bool alive(const UObject* o);           // in the object array, not being destroyed
bool readable(const void* p, size_t n);  // committed, readable memory (for addresses that come from outside)
UClass* class_of(const UObject* o);
UObject* outer_of(const UObject* o);
std::string path_of(const UObject* o);
std::string class_name(const UObject* o);  // class_of(o)'s short name
bool is_a(const UObject* o, const UClass* c);
bool is_cdo(const UObject* o);
UObject* find_path(const std::string& path);
UClass* find_class(const std::string& name_or_path);
std::vector<UObject*> instances(const UClass* c, bool include_cdo, size_t limit);
UObject* cdo(const UClass* c);

// Reflection
UStruct* super_of(const UStruct* s);
FField* first_prop(const UStruct* s);
FField* next_prop(const FField* f);
std::string field_name(const FField* f);
std::string field_type(const FField* f);
int prop_offset(const FProperty* p);
int prop_size(const FProperty* p);
uint64_t prop_flags(const FProperty* p);
FProperty* find_prop(const UStruct* s, const std::string& name);  // walks the super chain, case-insensitive
UFunction* find_func(const UStruct* s, const std::string& name);
int struct_size(const UStruct* s);
UStruct* struct_of(const FProperty* p);          // StructProperty -> its UScriptStruct
FProperty* array_inner(const FProperty* p);      // ArrayProperty -> its element property
json describe_struct(const UStruct* s, bool with_functions);

// Values
json read_value(const FProperty* p, const uint8_t* addr, int depth);
void write_value(const FProperty* p, uint8_t* addr, const json& v, std::vector<std::wstring>& keep);  // throws
json read_object(const UObject* o, int depth, const std::string& filter);

// Calls (game thread only)
void process_event(UObject* o, UFunction* f, void* parms);
json call(UObject* o, UFunction* f, const json& args);  // throws std::runtime_error on bad arguments

// References: "0x...", "/Path/To.Object" (then "#Prop.Sub"), or a root (engine, viewport, world, gi, lp, pc, pawn, gm, gs)
// followed by ".Prop", ".Arr[2]" or ".Fn()" (zero-argument call) steps.
UObject* resolve(const std::string& ref, std::string* err = nullptr);
json eval_path(const std::string& expr);                  // -> value
json set_path(const std::string& expr, const json& v);    // writes a property, returns the new value (throws)
UClass* function_class();
UClass* class_class();
UClass* scriptstruct_class();
UClass* enum_class();
json enum_values(const UObject* e);
std::string addr(const void* p);
}  // namespace ue
