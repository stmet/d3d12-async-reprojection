#include "resource_tracker.h"
#include "../hooks/hook_manager.h"
#include "../common/logger.h"
#include <unknwn.h>

// Unique GUID for our destruction tracker
// {4D2A9250-9E1A-4C2E-8E0E-F6DF7D8B9B1E}
static const GUID DESTRUCTION_TRACKER_GUID =
{ 0x4d2a9250, 0x9e1a, 0x4c2e, { 0x8e, 0xe, 0xf6, 0xdf, 0x7d, 0x8b, 0x9b, 0x1e } };

// Lightweight COM object attached to a tracked resource via SetPrivateDataInterface.
// When the resource is destroyed it releases its private-data interfaces, dropping this
// object's refcount to zero, which lets us observe the destruction.
class ResourceDestructionTracker : public IUnknown {
public:
    ResourceDestructionTracker(ID3D12Resource* res) : m_refCount(1), m_resource(res) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) {
            ResourceTracker::Instance().UnregisterResource(m_resource);
            delete this;
            return 0;
        }
        return count;
    }

private:
    ULONG m_refCount;
    ID3D12Resource* m_resource;
};

ResourceTracker& ResourceTracker::Instance() {
    static ResourceTracker instance;
    return instance;
}

// SEH helper: contains NO C++ objects with destructors to avoid C2712 compilation error.
static bool TrackForDestructionSafeHelper(ID3D12Resource* resource, void* trackerCOM) {
    __try {
        // Safe check: try to read the vtable pointer to verify readability.
        void** vtable = *(void***)resource;
        if (!vtable) return false;

        HRESULT hr = resource->SetPrivateDataInterface(DESTRUCTION_TRACKER_GUID, (IUnknown*)trackerCOM);
        return SUCCEEDED(hr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ResourceTracker::TrackForDestruction(ID3D12Resource* resource) {
    if (!resource) return;

    // Check pointer alignment to prevent reading from unaligned garbage addresses.
    if (((uintptr_t)resource & 7) != 0) {
        LOG_WARN("TrackForDestruction: Rejected unaligned resource pointer 0x%p", resource);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Already have a callback installed for this resource — installing a second one
        // with the same GUID would release the first (firing UnregisterResource and
        // nulling the very pointer we are trying to keep), so bail out early.
        if (m_tracked.find(resource) != m_tracked.end()) return;
    }

    // Install the callback outside the lock: SetPrivateDataInterface AddRefs the tracker
    // and won't re-enter our code for a freshly created tracker, but keeping COM calls out
    // of the critical section avoids any lock-ordering surprises.
    ResourceDestructionTracker* tracker = new ResourceDestructionTracker(resource);
    bool ok = TrackForDestructionSafeHelper(resource, tracker);
    tracker->Release(); // Release initial reference (SetPrivateDataInterface increments refcount internally)

    if (ok) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tracked.insert(resource);
    } else {
        LOG_ERROR("TrackForDestruction: CRASH AVOIDED or registration failed for resource pointer 0x%p!", resource);
    }
}

void ResourceTracker::UnregisterResource(ID3D12Resource* resource) {
    if (!resource) return;

    // Null out any intercepted pointers referencing this resource before it dangles.
    HookManager::Instance().OnResourceDestroyed(resource);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_tracked.erase(resource);
}
