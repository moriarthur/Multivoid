// coop/game/custom_content.h -- the game's Custom content setting, force-enabled for this peer while
// a session runs.
//
// VotV gates its custom content (the TV's local video list, custom posters, rugs, flags, the 3D
// printer's Assets models) behind a setting whose toggle appears only after the tutorial. A co-op
// session skips the tutorial and the main menu the toggle lives in, so without help every peer's
// custom content stays at its default, off, and the TV answers every play with the game's own
// 'Custom Content is disabled!' toast. The seam is the gate's own question: lib_C::isCustom, the
// function the checking UIs call, watched at the script-body gate and its answer forced on in the
// post phase, config-gated. The hook is the deterministic half; the underlying flag's live home is
// not pinned yet, so nothing writes it -- a build that renames isCustom goes dormant, logged, until
// its new name is taught. MTA precedent: a game rule a client cannot reach the toggle for is forced
// at the rule's reader, not patched into a save (reference/mtasa-blue, the minimized-map rule).

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::custom_content {

// Registers the gate watch when the flag wants it. The per-tick retry pump (subsystems::Install),
// so a watch the gate refused registers again on a later tick. Game thread.
void Install(coop::net::Session* session);

// Settles the watch and says once whether it went live. Game thread, once per pump tick.
void Tick();

// The session ended: the watch is process-lifetime, the armed gate is not.
void OnDisconnect();

}  // namespace coop::custom_content
