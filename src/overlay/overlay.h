#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

// In-process Dear ImGui overlay rendered inside the game's Present path on the game's
// present queue. This is the eyes-on tooling layer (P0.1) that every later phase
// (capture debug views, warp tuning, pacing stats) mounts its UI into.
//
// Toggle the full menu with the INSERT key. A small always-on status badge confirms the
// overlay is live even when the menu is hidden.
namespace Overlay {

// Called from the CreateSwapChain / CreateSwapChainForHwnd hooks with the command queue the
// swapchain presents on (in D3D12 that's the queue passed as the "device" argument). The
// overlay records its draw command list on this queue so it is ordered before the real Present.
void SetPresentQueue(ID3D12CommandQueue* queue);

// The present queue captured at swapchain creation, used by the warp pass too. May be null
// before the first swapchain is created.
ID3D12CommandQueue* GetPresentQueue();

// Called from the Present / Present1 hooks BEFORE the real Present. Lazily initializes ImGui
// and the D3D12 + Win32 backends from the live swapchain on first call, then renders the menu
// into the current back buffer.
void RenderOverlay(IDXGISwapChain* swapchain);

// True while the GAME has an active cursor clip — i.e. it's mouse-looking (gameplay). False when the
// game has released the cursor (inventory/pause/dialogue/main menu). The presenter uses this to
// auto-suppress the warp in menus. Reflects the game's own ClipCursor intent, not our overlay's.
bool GameHasCursorClip();

// Release ImGui + D3D12 objects and restore the window proc. Safe to call if never initialized.
void Shutdown();

} // namespace Overlay
