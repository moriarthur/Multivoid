// ue_wrap/devices/tv.h -- the television family: the prop_tv2 base and its placement variants, each
// TV's save Key and its claimed media player, and the native MediaPlayer verbs a playback edge rides.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::tv {

// Resolve the family's classes, per-class offsets and the engine MediaPlayer class and verbs, by
// name, throttled (a class a level has not streamed in stays out until it loads). Idempotent; a
// class whose Key or mediaPlayer does not resolve is left out and said once. Game thread.
bool EnsureResolved();

// How many family classes resolved, for the evidence line.
uint64_t ResolvedClassCount();

// The actor's save Key as text ('' when this class resolved without one -- such a class is out of
// the lane's reach, and the lane never sends it).
std::wstring KeyString(void* tv);

// The TV's claimed media player (its mediaPlayer ObjectProperty), or null when the TV has none
// claimed -- a TV whose interface was never opened claims nothing.
void* MediaPlayerOf(void* tv);

// The family member whose mediaPlayer is this instance, or null: the radio and the other media
// devices claim players the family does not hold, and their edges are not television edges.
void* TvForMediaPlayer(void* player);

// Resolve a TV by its save Key text, walking the family's live instances. Few TVs, called per
// received edge, never per frame. Game thread.
void* TvByKey(const std::wstring& key);

// The engine MediaPlayer class's native verbs, resolved by name once. Null until resolved.
void* OpenFileFn();
void* OpenUrlFn();
void* PlayFn();
void* PauseFn();
void* CloseFn();

}  // namespace ue_wrap::tv
