#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Async raw-mouse tracker. Fed from WM_INPUT in the overlay's window hook (the game's own window
// thread), read by the warp at present time on the render thread — a lock-free timestamped ring,
// so the warp can late-latch the mouse position as it was at any past QPC. This is what makes the
// reprojection asynchronous from the game's input sampling: we warp the rendered frame toward the
// freshest mouse delta, not the (older) delta the game rendered with.
namespace MouseTracker {

// Parse a WM_INPUT raw-mouse packet and append it to the ring. Safe to call from the window thread.
void OnRawInput(LPARAM lParam);

// Accumulated mouse position (in raw counts) as it was at/just-before targetQpc. Falls back to the
// newest sample when targetQpc is in the future (the common "now" case).
void GetAccAt(uint64_t targetQpc, long long& ax, long long& ay);

uint64_t NowQpc();
int64_t  MsToQpc(double ms);

} // namespace MouseTracker
