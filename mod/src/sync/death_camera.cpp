// The death camera on a partner far away (docs §3.47; 17.09 checklist item 15: "the camera should
// go to the partner, from afar too").
//
// The world is loaded around one position only: the map update exe+0x3BE060 asks exe+0x39AA10
// for the focus (the local player's vt[0x148], call at exe+0x3BE12F), the local player's floor
// hit (exe+0x312BA0, call at exe+0x3BE158) and its collision hit (exe+0x1CA7B0, call at
// exe+0x3BE170), and loads collision outward from there. A dead player's camera pointed at a
// partner 40 m or more away looked at ground that was never loaded, so up to 0.2.1 the camera
// stayed at the body. While this guest is down and its partner stands in the same map, those three
// calls -- from the map update only -- answer for the partner's character instead, and the
// FallDead camera request (id 5, exe+0x495FA0) is left out so the camera keeps following the
// partner rather than sinking to the body. Everything goes back the moment this player is up, a
// map apart, or out of the host's world. The host keeps the camera at its body: its enemies run on
// its own machine and could fall through collision unloaded around them.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kFocus        = 0x39AA10;   // (GMImp, float out[4]) -> out: the load focus
constexpr uint32_t kFloorHit     = 0x312BA0;   // (character) -> its floor hit, or 0
constexpr uint32_t kCollisionHit = 0x1CA7B0;   // (character) -> its collision hit, or 0
constexpr uint32_t kCamRequest   = 0x495FA0;   // (camera operator, request*): request id 5 = FallDead
constexpr uint32_t kFocusRet     = 0x3BE134;   // the map update's calls
constexpr uint32_t kFloorRet     = 0x3BE15D;
constexpr uint32_t kCollisionRet = 0x3BE175;
constexpr uint32_t kFallDead     = 5;

using FocusFn   = float*(__fastcall*)(void*, float*);
using ChrHitFn  = uintptr_t(__fastcall*)(uintptr_t);
using CamReqFn  = void(__fastcall*)(void*, const uint32_t*);
using ChrPosFn  = const float*(__fastcall*)(void*, float*);

FocusFn  g_focus     = nullptr;
ChrHitFn g_floor     = nullptr;
ChrHitFn g_collision = nullptr;
CamReqFn g_camReq    = nullptr;

std::atomic<bool>      g_enabled{ true };     // ini far_death_camera
std::atomic<uintptr_t> g_spectate{ 0 };       // the partner's character while spectating, else 0
std::atomic<uint32_t>  g_focusCalls{ 0 };
std::atomic<uint32_t>  g_requestsDropped{ 0 };

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool InGameImage(uintptr_t P) {
    static const uintptr_t Base = ExeBase();
    static const uintptr_t End = [] {
        const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
        return Base + Nt->OptionalHeader.SizeOfImage;
    }();
    return P >= Base && P < End;
}

// The spectated character, still one of the session's players; 0 otherwise.
uintptr_t Watched() {
    const uintptr_t Chr = g_spectate.load(std::memory_order_relaxed);
    if (!Chr) return 0;
    return IsSessionPlayer(Chr) ? Chr : 0;
}

bool PartnerPositionSafe(uintptr_t Chr, float* Out) {
    __try {
        const uintptr_t Vtbl = *reinterpret_cast<const uintptr_t*>(Chr);
        if (!InGameImage(Vtbl)) return false;
        const uintptr_t PosAt = *reinterpret_cast<const uintptr_t*>(Vtbl + 0x148);
        if (!InGameImage(PosAt)) return false;
        return reinterpret_cast<ChrPosFn>(PosAt)(reinterpret_cast<void*>(Chr), Out) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HitSafe(ChrHitFn Fn, uintptr_t Chr, uintptr_t* Out) {
    __try {
        *Out = Fn(Chr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float* __fastcall FocusDetour(void* Gm, float* Out) {
    float* Result = g_focus(Gm, Out);
    if (!Out || reinterpret_cast<uintptr_t>(_ReturnAddress()) != ExeBase() + kFocusRet) return Result;
    const uintptr_t Chr = Watched();
    if (!Chr) return Result;
    alignas(16) float Pos[4] = {};
    if (PartnerPositionSafe(Chr, Pos)) {
        Out[0] = Pos[0];
        Out[1] = Pos[1];
        Out[2] = Pos[2];
        g_focusCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return Result;
}

uintptr_t __fastcall FloorDetour(uintptr_t Chr) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == ExeBase() + kFloorRet) {
        const uintptr_t Partner = Watched();
        uintptr_t Hit = 0;
        if (Partner && HitSafe(g_floor, Partner, &Hit)) return Hit;
    }
    return g_floor(Chr);
}

uintptr_t __fastcall CollisionDetour(uintptr_t Chr) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == ExeBase() + kCollisionRet) {
        const uintptr_t Partner = Watched();
        uintptr_t Hit = 0;
        if (Partner && HitSafe(g_collision, Partner, &Hit)) return Hit;
    }
    return g_collision(Chr);
}

void __fastcall CamRequestDetour(void* Op, const uint32_t* Req) {
    if (Req && *Req == kFallDead && g_spectate.load(std::memory_order_relaxed)) {
        g_requestsDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_camReq(Op, Req);
}

template <typename T> bool HookAt(uint32_t Rva, void* Detour, T* Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour,
                                                      reinterpret_cast<void**>(Original))) {
        return true;
    }
    *Original = nullptr;
    LOG_WARNING("[CAM] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallFarDeathCamera(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed && Enabled) {
        Installed = true;
        HookAt(kFocus, reinterpret_cast<void*>(&FocusDetour), &g_focus, "the world's load focus");
        HookAt(kFloorHit, reinterpret_cast<void*>(&FloorDetour), &g_floor, "the focus character's floor hit");
        HookAt(kCollisionHit, reinterpret_cast<void*>(&CollisionDetour), &g_collision, "its collision hit");
        HookAt(kCamRequest, reinterpret_cast<void*>(&CamRequestDetour), &g_camReq, "the camera requests");
        if (!g_focus || !g_floor || !g_collision || !g_camReq) g_enabled.store(false);
    }
    LOG_INFO("[CAM] a guest down far from its partner: %s", g_enabled.load()
        ? "the world is loaded around the partner and the camera follows it"
        : "the camera stays with the body");
    return g_enabled.load();
}

bool StartFarSpectate(uintptr_t Partner) {
    if (!g_enabled.load() || !Partner || !IsSessionPlayer(Partner)) return false;
    g_focusCalls.store(0);
    g_requestsDropped.store(0);
    g_spectate.store(Partner);
    return true;
}

void StopFarSpectate(const char* Why) {
    if (!g_spectate.exchange(0)) return;
    LOG_INFO("[CAM] the world is loaded around me again (%s): %u focus passes around the partner, %u FallDead "
             "requests left out", Why, g_focusCalls.load(), g_requestsDropped.load());
}

bool FarSpectating() {
    return g_spectate.load() != 0;
}

} // namespace DS2Coop::Sync
