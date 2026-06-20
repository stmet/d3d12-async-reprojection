#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <unordered_set>
#include <mutex>

// Minimal lifetime tracker for the handful of upscaler-input resources we intercept
// (color/depth/MV). Its only job is to fire HookManager::OnResourceDestroyed when one of
// those resources is COM-destroyed, so the intercepted pointers get nulled before they
// dangle. It deliberately does NOT track every resource/barrier/descriptor the game
// creates — that tracking fed a candidate-scoring heuristic that is no longer used, and
// hooking ResourceBarrier/OMSetRenderTargets/Create*View serialized the game's parallel
// command-recording threads on a global lock for data nothing consumed.
class ResourceTracker {
public:
    static ResourceTracker& Instance();

    // Attach a destruction callback to an intercepted resource. Idempotent and cheap, so
    // it is safe to call every frame with the same pointer — only the first call for a
    // given resource installs the callback. Touches the lock only for intercepted
    // resources (a few per frame), never on the game's hot paths.
    void TrackForDestruction(ID3D12Resource* resource);

    // Invoked by the per-resource destruction callback when the COM object is released.
    void UnregisterResource(ID3D12Resource* resource);

private:
    ResourceTracker() = default;
    ~ResourceTracker() = default;

    std::unordered_set<ID3D12Resource*> m_tracked;
    std::mutex m_mutex;
};
