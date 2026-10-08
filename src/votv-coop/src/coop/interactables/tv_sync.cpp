// coop/interactables/tv_sync.cpp -- see coop/interactables/tv_sync.h.

#include "coop/interactables/tv_sync.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/tv.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

namespace coop::tv_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace TV = ue_wrap::tv;
namespace P  = ::ue_wrap::profile;

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_hooksInstalled = false;

// Armed only while the session runs AND is wired (the deck's shape) -- single-player media edges
// never author.
std::atomic<bool> g_armed{false};

// The wire-apply bracket depth: >0 while this lane's own reflected call of a MediaPlayer verb is on
// the stack, whose post hook must not author an edge for what is an application. Atomic per the
// "sometimes a task-graph worker" detour note (deck_play_sync).
std::atomic<int> g_wireDepth{0};
struct ScopedWireApply {
    ScopedWireApply()  { g_wireDepth.fetch_add(1, std::memory_order_relaxed); }
    ~ScopedWireApply() { g_wireDepth.fetch_sub(1, std::memory_order_relaxed); }
};

// The playback generations. An open mints; play, pause and stop act on the generation they belong
// to, and an edge for a generation this peer no longer holds is stale (the deck's guard, per TV:
// the base has many televisions, the deck had one).
uint32_t g_seenGen = 0;
struct AppliedTv {
    wchar_t  key[32] = {};  // the TV's save Key text, NUL-terminated
    uint32_t gen = 0;       // the generation currently live on this peer's copy, 0 = none
};
constexpr int kAppliedCap = 16;
AppliedTv g_applied[kAppliedCap];

uint32_t AppliedGenFor(const std::wstring& key) {
    for (const AppliedTv& a : g_applied)
        if (a.key[0] && key == a.key) return a.gen;
    return 0;
}

void SetAppliedGen(const std::wstring& key, uint32_t gen) {
    AppliedTv* slot = nullptr;
    for (AppliedTv& a : g_applied) {
        if (!slot && !a.key[0]) slot = &a;
        if (key == a.key) { slot = &a; break; }
    }
    if (!slot) return;  // more distinct TVs than the table holds: the extra stays unguarded
    std::wmemset(slot->key, 0, 32);
    std::wcsncpy(slot->key, key.c_str(), 31);
    slot->gen = gen;
}

// The detour -> Tick handoff ring (the deck's contract: the post hook runs deep inside engine
// dispatch -- no locks, no engine calls, no sends; it classifies, extracts and queues only).
struct RingEntry {
    uint8_t  op;         // kTvPlay*
    bool     isUrl;
    uint32_t gen;        // open: the minted generation; the rest: the generation they act on
    wchar_t  key[32] = {};  // the TV's save Key text
    wchar_t  src[160] = {}; // open: the media reference (a basename of the file, or the URL whole)
};
constexpr int kRingCap = 12;
RingEntry g_ring[kRingCap];
int  g_ringN = 0;
bool g_ringOverflowWarned = false;
bool g_truncSaid = false;

// Evidence, the deck's cadence: always counted, at most one line a minute, and only when nonzero.
std::atomic<uint64_t> g_fires{0};
std::atomic<uint64_t> g_authored{0};
uint64_t g_appliedEdges = 0, g_appliedOpens = 0, g_droppedNoTv = 0, g_droppedNoPlayer = 0,
         g_droppedStale = 0, g_droppedRate = 0, g_droppedShort = 0;
std::chrono::steady_clock::time_point g_nextCounterLog{};

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A sender's opens apply at a bounded rate: a human clicks play a few times a minute, so the bound
// is only felt by a flooding peer.
constexpr float    kOpenBurst     = 4.0f;
constexpr float    kOpenPerSecond = 2.0f;
struct OpenBucket {
    float    tokens = kOpenBurst;
    uint64_t lastMs = 0;
};
OpenBucket g_openRate[coop::net::kMaxPeers];

bool TakeOpenToken(uint8_t slot) {
    OpenBucket& b = g_openRate[slot];
    const uint64_t now = NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kOpenPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kOpenBurst) b.tokens = kOpenBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

// The basename of an open path: everything after the last separator. The game opens the offline list
// by the full path on this machine; the wire carries only the file name, each peer re-anchoring it on
// its own Assets\tv. A URL never passes here.
void BasenameInto(const std::wstring& path, wchar_t* out, int cap) {
    const size_t cut = path.find_last_of(L"\\/");
    std::wstring name = (cut == std::wstring::npos) ? path : path.substr(cut + 1);
    if ((int)name.size() > cap - 1) {
        name.resize(cap - 1);
        if (!g_truncSaid) {
            g_truncSaid = true;
            UE_LOGW("tv_sync: a media reference longer than %d characters was cut -- it cannot match "
                    "a peer's file anyway", cap - 1);
        }
    }
    std::wmemcpy(out, name.data(), name.size());
    out[name.size()] = 0;
}

// The frame's first parameter, as the post hook sees the call: OpenFile/OpenUrl take exactly one
// FString. Its offset comes from the live UFunction, its bytes read as the FString header.
bool ReadFirstStringParam(void* fn, void* locals, std::wstring& out) {
    if (!fn || !locals) return false;
    const auto params = R::FunctionParams(fn);
    for (const auto& p : params) {
        if (!(p.flags & P::cpf::Parm) || p.offset < 0) continue;
        if (p.size < 16) return false;  // not a FString-shaped first parameter: let it be, logged
        struct FStringHead { wchar_t* data; int32_t num; int32_t max; };
        FStringHead head{};
        std::memcpy(&head, static_cast<uint8_t*>(locals) + p.offset, sizeof(head));
        if (!head.data || head.num <= 0 || head.num > (1 << 20)) return false;
        out.assign(head.data, static_cast<size_t>(head.num));
        return true;
    }
    return false;
}

// Write `s` into the frame's first parameter as an engine-minted FString header.
bool SetFirstStringParam(ue_wrap::ParamFrame& f, void* fn, const std::wstring& s) {
    const auto params = R::FunctionParams(fn);
    for (const auto& p : params) {
        if (!(p.flags & P::cpf::Parm) || p.offset < 0 || p.size < 16) continue;
        alignas(16) uint8_t hdr[16]{};
        if (!ue_wrap::fstring_utils::MintFString(s, hdr)) return false;
        return f.SetRaw(p.name.c_str(), hdr, 16);
    }
    return false;
}

// ---- the five seam callbacks (GT, deep inside engine dispatch) ---------------------------------

void OnEdge(void* context, void* /*sourceObject*/, void* /*result*/) {
    g_fires.fetch_add(1, std::memory_order_relaxed);
    if (g_wireDepth.load(std::memory_order_relaxed) > 0) return;
    if (!g_armed.load(std::memory_order_relaxed)) return;

    // A television's claimed player, and only that: the radio and the other media devices claim
    // players the family does not hold, and their edges are not television edges.
    void* tv = TV::TvForMediaPlayer(context);
    if (!tv) return;

    const ue_wrap::ufunction_hook::CallerFrame fr = ue_wrap::ufunction_hook::CurrentCallerFrame();
    uint8_t op = 0;
    bool isUrl = false;
    std::wstring src;
    if (fr.function == TV::OpenFileFn()) {
        op = net::kTvPlayOpen;
        if (!ReadFirstStringParam(TV::OpenFileFn(), fr.locals, src)) {
            UE_LOGW("tv_sync: an OpenFile edge on a TV's player carried no readable path -- dropped");
            return;
        }
        wchar_t base[160];
        BasenameInto(src, base, 160);
        src = base;
    } else if (fr.function == TV::OpenUrlFn()) {
        op = net::kTvPlayOpen;
        isUrl = true;
        if (!ReadFirstStringParam(TV::OpenUrlFn(), fr.locals, src)) {
            UE_LOGW("tv_sync: an OpenUrl edge on a TV's player carried no readable URL -- dropped");
            return;
        }
    } else if (fr.function == TV::PlayFn()) {
        op = net::kTvPlayResume;
    } else if (fr.function == TV::PauseFn()) {
        op = net::kTvPlayPause;
    } else if (fr.function == TV::CloseFn()) {
        op = net::kTvPlayStop;
    } else {
        return;  // not one of this lane's five: nothing to judge
    }

    const std::wstring key = TV::KeyString(tv);
    if (key.empty() || key == L"None") return;  // a family member the lane cannot address

    if (g_ringN >= kRingCap) {
        if (!g_ringOverflowWarned) {
            g_ringOverflowWarned = true;
            UE_LOGW("tv_sync: ring overflow -- dropping (log-once)");
        }
        return;
    }

    RingEntry e{};
    e.op = op;
    e.isUrl = isUrl;
    if (op == net::kTvPlayOpen) {
        const uint32_t gen = ++g_seenGen;
        SetAppliedGen(key, gen);
        e.gen = gen;
    } else {
        const uint32_t gen = AppliedGenFor(key);
        if (gen == 0) {
            // Nothing of this TV's is live on this peer (a stop with nothing playing: the idle
            // close an import or a power toggle makes) -- no session to speak for, nothing to send.
            return;
        }
        e.gen = gen;
    }
    std::wcsncpy(e.key, key.c_str(), 31);
    std::wcsncpy(e.src, src.c_str(), 159);
    g_ring[g_ringN++] = e;
    g_authored.fetch_add(1, std::memory_order_relaxed);
}

void SendEdge(coop::net::Session& s, const RingEntry& e) {
    coop::net::TvPlayEventPayload p{};
    p.op = e.op;
    p.flags = e.isUrl ? net::kTvPlayIsUrl : 0;
    coop::net::WireKeyFromString(e.key, p.tvKey);
    p.gen = e.gen;
    const std::string utf8 = coop::text::ToUtf8(e.src);
    p.srcLen = static_cast<uint16_t>(utf8.size() > sizeof(p.src) ? sizeof(p.src) : utf8.size());
    if (!utf8.empty()) std::memcpy(p.src, utf8.data(), p.srcLen);
    if (s.role() == coop::net::Role::Host)
        s.SendReliable(coop::net::ReliableKind::TvPlayEvent, &p, sizeof(p));
    else
        s.SendReliableToSlot(0, coop::net::ReliableKind::TvPlayEvent, &p, sizeof(p));
}

// The apply-side Assets anchor: lib_C::GetAssetFolder on the library's default object, \tv appended.
// Resolved once and kept; a build without the function leaves file opens undrivable on this peer
// (said once), URLs unaffected. The TV being applied to serves as __WorldContext when the function
// carries one.
bool AssetsTvFolder(void* tv, std::wstring& out) {
    static std::wstring s_folder;
    static bool s_tried = false;
    static bool s_ok = false;
    if (s_tried) {
        out = s_folder;
        return s_ok;
    }
    void* cls = R::FindClass(L"lib_C");
    void* cdo = cls ? R::FindClassDefaultObject(L"lib_C") : nullptr;
    void* fn = cls ? R::FindFunction(cls, L"GetAssetFolder") : nullptr;
    if (!cdo || !fn) {
        s_tried = true;
        UE_LOGW("tv_sync: lib_C::GetAssetFolder did not resolve -- file opens cannot be applied on "
                "this peer (URLs still sync)");
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    for (const auto& prm : R::FunctionParams(fn)) {
        if (prm.name == L"__WorldContext") f.Set<void*>(L"__WorldContext", tv);
    }
    if (!ue_wrap::Call(cdo, f)) {
        s_tried = true;
        UE_LOGW("tv_sync: GetAssetFolder call failed -- file opens cannot be applied here");
        return false;
    }
    // The result may sit in the return slot or an out param named by the node; take the first
    // candidate whose sixteen bytes read as a sane FString header.
    struct FStringHead { wchar_t* data; int32_t num; int32_t max; };
    const wchar_t* candidates[] = { L"return", L"ReturnValue", L"path" };
    for (const wchar_t* name : candidates) {
        const int32_t off = f.ParamOffset(name);
        if (off < 0) continue;
        FStringHead head{};
        f.GetRaw(name, &head, sizeof(head));
        if (head.data && head.num >= 0 && head.num < (1 << 20)) {
            std::wstring folder(head.data, static_cast<size_t>(head.num));
            if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
                folder += L'\\';
            folder += L"tv";
            s_folder = folder;
            s_tried = true;
            s_ok = true;
            UE_LOGI("tv_sync: Assets anchor resolved: '%ls'", s_folder.c_str());
            break;
        }
    }
    if (!s_tried) s_tried = true;
    out = s_folder;
    return s_ok;
}

// One edge applied to this peer's copy of the TV, under the wire guard. False for a malformed or
// unresolvable edge; a native open failure (no such file here) is the game's own toast.
bool ApplyEdge(const std::wstring& key, const coop::net::TvPlayEventPayload& p,
               const std::wstring& src) {
    void* tv = TV::TvByKey(key);
    if (!tv) {
        ++g_droppedNoTv;
        return false;
    }
    void* player = TV::MediaPlayerOf(tv);
    if (!player) {
        // This peer's copy of the TV has claimed no channel -- its interface was never opened. One
        // dark TV, self-heals at the next edge; the claim is the game's own.
        ++g_droppedNoPlayer;
        return false;
    }

    ScopedWireApply guard;
    switch (p.op) {
    case net::kTvPlayOpen: {
        if (p.flags & net::kTvPlayIsUrl) {
            ue_wrap::ParamFrame f(TV::OpenUrlFn());
            if (!f.valid() || !SetFirstStringParam(f, TV::OpenUrlFn(), src)) return false;
            return ue_wrap::Call(player, f);
        }
        std::wstring folder;
        if (!AssetsTvFolder(tv, folder)) return false;
        const std::wstring path = folder + L"\\" + src;
        ue_wrap::ParamFrame f(TV::OpenFileFn());
        if (!f.valid() || !SetFirstStringParam(f, TV::OpenFileFn(), path)) return false;
        return ue_wrap::Call(player, f);
    }
    case net::kTvPlayStop: {
        ue_wrap::ParamFrame f(TV::CloseFn());
        return f.valid() && ue_wrap::Call(player, f);
    }
    case net::kTvPlayResume: {
        ue_wrap::ParamFrame f(TV::PlayFn());
        return f.valid() && ue_wrap::Call(player, f);
    }
    case net::kTvPlayPause: {
        ue_wrap::ParamFrame f(TV::PauseFn());
        return f.valid() && ue_wrap::Call(player, f);
    }
    default:
        return false;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) { g_armed.store(false, std::memory_order_relaxed); return; }
    // The product switch reads once, on the first tick of the first session: the config registry's
    // layered read is not a per-tick cost, and a lane arming mid-session would desync the ring.
    static const bool s_enabled = coop::config::ResolveFlag(coop::config_registry::rows::game_tv_sync);
    g_armed.store(s->connected() && s_enabled, std::memory_order_relaxed);

    // Lazy latched seam install: the five natives resolve with the engine class, the family's
    // classes with their levels.
    if (!g_hooksInstalled) {
        if (!TV::EnsureResolved() || !TV::OpenFileFn()) return;
        const bool a = ue_wrap::ufunction_hook::InstallPostHook(TV::OpenFileFn(), &OnEdge);
        const bool b = ue_wrap::ufunction_hook::InstallPostHook(TV::OpenUrlFn(), &OnEdge);
        const bool c = ue_wrap::ufunction_hook::InstallPostHook(TV::PlayFn(), &OnEdge);
        const bool d = ue_wrap::ufunction_hook::InstallPostHook(TV::PauseFn(), &OnEdge);
        const bool e = ue_wrap::ufunction_hook::InstallPostHook(TV::CloseFn(), &OnEdge);
        g_hooksInstalled = true;  // installs are idempotent + process-lifetime
        UE_LOGI("tv_sync: seams installed (family classes=%llu file=%d url=%d play=%d pause=%d "
                "close=%d)",
                (unsigned long long)TV::ResolvedClassCount(), a ? 1 : 0, b ? 1 : 0, c ? 1 : 0,
                d ? 1 : 0, e ? 1 : 0);
    }

    TV::EnsureResolved();  // keep the family fresh (throttled inside)

    // Flush the detour ring -> wire.
    if (g_ringN > 0 && s->connected()) {
        for (int i = 0; i < g_ringN; ++i) SendEdge(*s, g_ring[i]);
        g_ringN = 0;
    }

    // Seam-fire evidence, on a 60 s cadence when nonzero.
    const auto now = std::chrono::steady_clock::now();
    if (g_nextCounterLog == std::chrono::steady_clock::time_point{})
        g_nextCounterLog = now + std::chrono::seconds(60);
    if (now >= g_nextCounterLog) {
        g_nextCounterLog = now + std::chrono::seconds(60);
        const uint64_t fi = g_fires.exchange(0, std::memory_order_relaxed);
        const uint64_t au = g_authored.exchange(0, std::memory_order_relaxed);
        if (fi || au || g_appliedEdges || g_droppedNoTv || g_droppedNoPlayer || g_droppedStale ||
            g_droppedRate)
            UE_LOGI("tv_sync: counters /60s: fires=%llu authored=%llu applied=%llu (opens=%llu) "
                    "dropped: noTv=%llu noPlayer=%llu stale=%llu rate=%llu short=%llu",
                    (unsigned long long)fi, (unsigned long long)au, g_appliedEdges, g_appliedOpens,
                    g_droppedNoTv, g_droppedNoPlayer, g_droppedStale, g_droppedRate, g_droppedShort);
        g_appliedEdges = g_appliedOpens = g_droppedNoTv = g_droppedNoPlayer = g_droppedStale =
            g_droppedRate = g_droppedShort = 0;
    }
}

void OnTvPlay(const coop::net::TvPlayEventPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.op > net::kTvPlayPause) {
        UE_LOGW("tv_sync: op=%u out of range (from slot %u) -- dropping", p.op, senderSlot);
        return;
    }
    const std::wstring key = coop::net::StringFromWireKey(p.tvKey);
    if (key.empty()) { ++g_droppedShort; return; }

    // The generations: max-merge so this peer's next mint stays above anything seen; a transport
    // edge for a generation no longer live here is stale.
    if (p.gen > g_seenGen) g_seenGen = p.gen;

    std::wstring src;
    if (p.op == net::kTvPlayOpen) {
        if (p.srcLen == 0 || p.srcLen > sizeof(p.src)) { ++g_droppedShort; return; }
        // The receive boundary: strict, whole-field. A repaired name is a file nobody has.
        if (!coop::text::FromUtf8Strict(p.src, p.srcLen, &src)) {
            ++g_droppedShort;
            UE_LOGW("tv_sync: an open's media reference was not well-formed UTF-8 (from slot %u) "
                    "-- dropping whole", static_cast<unsigned>(senderSlot));
            return;
        }
        if (!TakeOpenToken(senderSlot)) { ++g_droppedRate; return; }
    } else {
        if (AppliedGenFor(key) == 0 || p.gen != AppliedGenFor(key)) {
            ++g_droppedStale;
            return;
        }
    }

    if (ApplyEdge(key, p, src)) {
        ++g_appliedEdges;
        if (p.op == net::kTvPlayOpen) {
            ++g_appliedOpens;
            SetAppliedGen(key, p.gen);
        } else if (p.op == net::kTvPlayStop) {
            SetAppliedGen(key, 0);
        }
    }
}

void OnDisconnect() {
    g_seenGen = 0;
    for (AppliedTv& a : g_applied) { a.key[0] = 0; a.gen = 0; }
    g_ringN = 0;
    g_ringOverflowWarned = false;
    g_wireDepth.store(0, std::memory_order_relaxed);
    g_armed.store(false, std::memory_order_relaxed);
    // The native hooks are process-lifetime forwarders; the armed gate silences them.
}

}  // namespace coop::tv_sync
