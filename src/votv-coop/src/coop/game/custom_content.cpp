// coop/game/custom_content.cpp -- see coop/game/custom_content.h.

#include "coop/game/custom_content.h"

#include "coop/config/config.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>

namespace coop::custom_content {
namespace {

namespace sg = ue_wrap::script_gate;

constexpr int kIsCustomTag = 0x43554343;  // 'CUCC'

std::atomic<coop::net::Session*> g_session{nullptr};

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg      g_reg = Reg::Pending;
bool     g_saidLive = false;

uint64_t g_forced = 0;       // answers flipped on, for the settle line
bool     g_resultSaid = false;

// The post phase: the body ran and answered from the saved flag; whatever it said, the answer the
// session wants is yes. `result` is the bool's one-byte frame storage (the reflection layer's bool
// frame convention); a null result storage is a call whose answer nobody reads -- nothing to flip.
void OnIsCustomPost(const sg::Call& call) {
    if (call.result) {
        *static_cast<uint8_t*>(call.result) = 1;
        ++g_forced;
    } else if (!g_resultSaid) {
        g_resultSaid = true;
        // Fires with no result storage are normal for a discarded return; said once, counted apart.
        UE_LOGI("custom_content: an isCustom call carried no result storage (discarded return)");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    // The product switch reads once per process, on the first tick of the first session (the
    // tv_sync rule): the registry's layered read is not a per-tick cost.
    static const bool s_wanted =
        coop::config::ResolveFlag(coop::config_registry::rows::game_custom_content);
    if (!s_wanted) return;

    if (g_reg == Reg::Pending) {
        // lib_C::isCustom, by class and name: every caller the game reaches it through goes through
        // the same body, whatever route it enters by.
        if (sg::WatchClassName(L"lib_C", L"isCustom", kIsCustomTag, nullptr, &OnIsCustomPost))
            g_reg = Reg::Registered;
        else if (sg::ClassNameWatchSettled(L"lib_C", L"isCustom", kIsCustomTag))
            g_reg = Reg::Refused;
    }
    if (g_reg == Reg::Registered && !g_saidLive) {
        if (sg::ClassNameWatchLive(L"lib_C", L"isCustom", kIsCustomTag)) {
            g_saidLive = true;
            UE_LOGI("custom_content: lib_C::isCustom watched -- the session's custom content answers "
                    "yes");
        }
    }
    if (g_reg == Reg::Refused && !g_saidLive) {
        g_saidLive = true;
        UE_LOGW("custom_content: lib_C::isCustom did not resolve -- this game build gates custom "
                "content by another name, and the TV stays gated until it is taught");
    }
}

void OnDisconnect() {
    // The watch is process-lifetime and inert without a gate holder; only the tally resets.
    g_forced = 0;
    g_resultSaid = false;
}

}  // namespace coop::custom_content
