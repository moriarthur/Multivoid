// coop/interactables/tv_sync.h -- television playback, mirrored: an open, play, pause or stop on any
// TV drives every peer's copy of that TV.
//
// The seam is the ENGINE's own MediaPlayer natives (OpenFile/OpenUrl/Play/Pause/Close), post-hooked
// the audio-seam way (deck_play_sync): every open path the game has funnels through them -- the
// offline list's uicomp_videoSlot drives the claimed player directly, and the TV's own openLink does
// the same for a URL -- so the lane needs no Blueprint verb of the TV family and survives a recook
// that renames the family's functions. The author resolves the player to a TV through the family's
// mediaPlayer claim and sends the edge; a receiver finds the same TV by its save Key and drives the
// same native verb on its own copy, the game's own media pipeline repainting screen and sound. Presser
// authored, the deck's shape: anyone's edge plays everywhere, the host relays it, and a receiver's own
// organic edge is theirs. The video file is local, so a peer without the file fails the open natively
// (the game's own 'Video error' toast) and loses nothing else. Late join: nothing to replay yet -- a
// joiner's TVs stay dark until the next edge; the last-open replay is the written next step.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct TvPlayEventPayload;
}  // namespace coop::net

namespace coop::tv_sync {

// Registers the lazy seam installer on the session. The per-tick pump (subsystems::Install).
void Install(coop::net::Session* session);

// Installs the MediaPlayer seams once they resolve; flushes the organic-edge ring to the wire;
// the seam-fire evidence line. Game thread, once per pump tick.
void Tick();

// An edge from the wire, its format already checked by the dispatcher. Role and trust gates live
// here: a client applies a host's edge, the host applies and relays a client's edge, both rate
// bounded per sender. Game thread.
void OnTvPlay(const coop::net::TvPlayEventPayload& p, uint8_t senderSlot);

// The session ended: ring, generations, per-TV state, evidence counters, one summary line. The
// native hooks are process-lifetime forwarders; the armed gate silences them outside a session.
void OnDisconnect();

}  // namespace coop::tv_sync
