// ue_wrap/devices/tv.cpp -- see ue_wrap/devices/tv.h. Per-class engine access for the television
// family, the appliance.cpp shape: a descriptor per placement variant, offsets and the claimed media
// player resolved from the live classes by name, a class that does not resolve left out and said once.
// The playback edges themselves ride the ENGINE's own MediaPlayer natives (OpenFile/OpenUrl/Play/
// Pause/Close), which every open path funnels through -- the offline list's uicomp_videoSlot and the
// TV's own verbs alike -- so the lane needs no Blueprint verb of the family at all.

#include "ue_wrap/devices/tv.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>

namespace ue_wrap::tv {
namespace {

namespace R = ue_wrap::reflection;

// One descriptor per family class. The Key the class inherits from Aactor_save_C addresses the TV on
// the wire; mediaPlayer is the ObjectProperty the TV's channel claim stores (prop_tv2_C's
// Take_Media_Channel writes it; uicomp_videoSlot drives that player for the offline list). Both
// resolve by name from the live class; a class missing either stays out.
struct Desc {
    const wchar_t* className;
    int32_t keyOff = -1;
    int32_t playerOff = -1;
    bool    resolved = false;
    bool    unusable = false;
    ue_wrap::CachedObjRef clsRef;
};

// The family as 0.9.0n ships it. A build that adds a variant leaves that variant out until its name
// joins this table, the appliance.cpp rule; the base first, so IsTv's descendant fallback has it.
Desc g_descs[] = {
    { L"prop_tv2_C" },
    { L"prop_tv_plasma_C" },
    { L"prop_tv2_floor_C" },
    { L"prop_tv2_floor1_C" },
    { L"prop_tv2_erie_C" },
    { L"prop_tv2_kerfur_C" },
    { L"prop_minitv_C" },
};
std::atomic<uint64_t> g_resolvedClasses{0};

std::atomic<uint64_t> g_nextTryMs{0};

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The class as this world holds it, re-looked-up when the kept one no longer holds (CachedObjRef).
void* ClassFor(Desc& d) {
    if (void* c = d.clsRef.Get()) return c;
    void* c = object_index::ClassByName(d.className);
    if (c) d.clsRef.Set(c);
    return c;
}

Desc* DescFor(void* obj) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return nullptr;
    for (Desc& d : g_descs)
        if (d.resolved && ClassFor(d) == cls) return &d;
    return nullptr;
}

// The engine MediaPlayer class and its five natives, resolved once. These are engine functions, not
// game Blueprints: their names are stable across game recooks, and OpenFile/OpenUrl take exactly one
// FString parameter, which the callers address by frame offset (the frame's first parameter) rather
// than by name, so even a parameter rename in the engine cannot move the lane.
struct Media {
    void* cls = nullptr;
    void* openFile = nullptr;
    void* openUrl = nullptr;
    void* play = nullptr;
    void* pause = nullptr;
    void* close = nullptr;
    bool  resolved = false;
    bool  unusable = false;
};
Media g_media;

}  // namespace

bool EnsureResolved() {
    const uint64_t now = NowMs();
    uint64_t next = g_nextTryMs.load(std::memory_order_relaxed);
    if (now < next) return g_resolvedClasses.load(std::memory_order_relaxed) > 0;
    while (!g_nextTryMs.compare_exchange_weak(next, now + 1000, std::memory_order_relaxed))
        ;

    uint64_t resolved = 0;
    for (Desc& d : g_descs) {
        if (d.resolved || d.unusable) {
            if (d.resolved) ++resolved;
            continue;
        }
        void* cls = ClassFor(d);
        if (!cls) continue;  // not streamed in yet; retried a second later
        const int32_t keyOff = R::FindPropertyOffset(cls, L"Key");
        const int32_t playerOff = R::FindPropertyOffset(cls, L"mediaPlayer");
        if (keyOff < 0 || playerOff < 0) {
            d.unusable = true;
            UE_LOGE("tv: %ls.%ls did not resolve by name -- this TV class is left out of the sync",
                    d.className, keyOff < 0 ? L"Key" : L"mediaPlayer");
            continue;
        }
        d.keyOff = keyOff;
        d.playerOff = playerOff;
        d.resolved = true;
        ++resolved;
        UE_LOGI("tv: resolved %ls Key@0x%04X mediaPlayer@0x%04X", d.className, keyOff, playerOff);
    }
    g_resolvedClasses.store(resolved, std::memory_order_relaxed);

    // The engine side, once. The five natives resolve or the lane says so once and stays dormant;
    // MediaAssets ships in the pak, so a miss here is a build that no longer plays media at all.
    if (!g_media.resolved && !g_media.unusable) {
        void* cls = object_index::ClassByName(L"MediaPlayer");
        if (cls) {
            g_media.cls      = cls;
            g_media.openFile = R::FindFunction(cls, L"OpenFile");
            g_media.openUrl  = R::FindFunction(cls, L"OpenUrl");
            g_media.play     = R::FindFunction(cls, L"Play");
            g_media.pause    = R::FindFunction(cls, L"Pause");
            g_media.close    = R::FindFunction(cls, L"Close");
            if (g_media.openFile && g_media.openUrl && g_media.play && g_media.pause && g_media.close) {
                g_media.resolved = true;
                UE_LOGI("tv: MediaPlayer natives resolved (OpenFile=%p OpenUrl=%p Play=%p Pause=%p Close=%p)",
                        g_media.openFile, g_media.openUrl, g_media.play, g_media.pause, g_media.close);
            } else {
                g_media.unusable = true;
                UE_LOGE("tv: a MediaPlayer native did not resolve (file=%p url=%p play=%p pause=%p close=%p) "
                        "-- the TV lane is OFF for this game build",
                        g_media.openFile, g_media.openUrl, g_media.play, g_media.pause, g_media.close);
            }
        }
    }
    return resolved > 0;
}

uint64_t ResolvedClassCount() { return g_resolvedClasses.load(std::memory_order_relaxed); }

std::wstring KeyString(void* tv) {
    const Desc* d = DescFor(tv);
    if (!d || d->keyOff < 0) return {};
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(tv) + d->keyOff);
    return R::ToString(key);
}

void* MediaPlayerOf(void* tv) {
    const Desc* d = DescFor(tv);
    if (!d || d->playerOff < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(tv) + d->playerOff);
}

void* TvForMediaPlayer(void* player) {
    if (!player) return nullptr;
    for (Desc& d : g_descs) {
        if (!d.resolved || d.playerOff < 0) continue;
        void* c = ClassFor(d);
        if (!c) continue;
        // The walk is per exact class, so every family member is asked for its own instances.
        struct Ctx { void* player; void* found; int32_t playerOff; } ctx{player, nullptr, d.playerOff};
        object_index::ForEachInstance(
            c,
            [](void* ctx, void* obj, int32_t) {
                auto* cctx = static_cast<Ctx*>(ctx);
                if (cctx->found) return;
                if (*reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + cctx->playerOff) ==
                    cctx->player)
                    cctx->found = obj;
            },
            &ctx);
        if (ctx.found) return ctx.found;
    }
    return nullptr;
}

void* TvByKey(const std::wstring& key) {
    if (key.empty()) return nullptr;
    for (Desc& d : g_descs) {
        if (!d.resolved || d.keyOff < 0) continue;
        void* c = ClassFor(d);
        if (!c) continue;
        struct Ctx { const std::wstring* key; void* found; int32_t keyOff; } ctx{&key, nullptr, d.keyOff};
        object_index::ForEachInstance(
            c,
            [](void* ctx, void* obj, int32_t) {
                auto* cctx = static_cast<Ctx*>(ctx);
                if (cctx->found) return;
                const R::FName& k = *reinterpret_cast<const R::FName*>(
                    reinterpret_cast<const char*>(obj) + cctx->keyOff);
                if (R::ToString(k) == *cctx->key) cctx->found = obj;
            },
            &ctx);
        if (ctx.found) return ctx.found;
    }
    return nullptr;
}

void* OpenFileFn()   { return g_media.resolved ? g_media.openFile : nullptr; }
void* OpenUrlFn()    { return g_media.resolved ? g_media.openUrl : nullptr; }
void* PlayFn()       { return g_media.resolved ? g_media.play : nullptr; }
void* PauseFn()      { return g_media.resolved ? g_media.pause : nullptr; }
void* CloseFn()      { return g_media.resolved ? g_media.close : nullptr; }

}  // namespace ue_wrap::tv
