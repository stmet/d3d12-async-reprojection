#pragma once
#include <cstdint>

// Minimal subset of the AMD FidelityFX ffx-api (amd_fidelityfx_dx12.dll) needed to read the upscale
// dispatch's depth / motion-vector / camera inputs. Layouts taken verbatim from the FidelityFX-SDK
// public headers (ffx_api.h / ffx_upscale.h) — do NOT reorder; the byte layout must match the DLL.

typedef uint32_t ffxReturnCode_t;
typedef void*    ffxContext;

// ffx_api.h: every desc begins with this header. `type` identifies the desc; `pNext` chains extras.
struct ffxApiHeader {
    uint64_t            type;
    struct ffxApiHeader* pNext;
};
typedef ffxApiHeader ffxDispatchDescHeader;

struct FfxApiResourceDescription {
    uint32_t type;
    uint32_t format;
    union { uint32_t width;  uint32_t size;      };
    union { uint32_t height; uint32_t stride;    };
    union { uint32_t depth;  uint32_t alignment; };
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct FfxApiResource {
    void*                    resource;     // the underlying ID3D12Resource*
    FfxApiResourceDescription description;
    uint32_t                 state;
};

struct FfxApiFloatCoords2D { float x; float y; };
struct FfxApiDimensions2D  { uint32_t width; uint32_t height; };

// ffx_upscale.h: ffxDispatchDescUpscale, dispatched via ffxDispatch().
#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE 0x00010001u
struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void*                 commandList;
    FfxApiResource        color;
    FfxApiResource        depth;
    FfxApiResource        motionVectors;
    FfxApiResource        exposure;
    FfxApiResource        reactive;
    FfxApiResource        transparencyAndComposition;
    FfxApiResource        output;
    FfxApiFloatCoords2D   jitterOffset;
    FfxApiFloatCoords2D   motionVectorScale;
    FfxApiDimensions2D    renderSize;
    FfxApiDimensions2D    upscaleSize;
    bool                  enableSharpening;
    float                 sharpness;
    float                 frameTimeDelta;
    float                 preExposure;
    bool                  reset;
    float                 cameraNear;
    float                 cameraFar;
    float                 cameraFovAngleVertical;
    float                 viewSpaceToMetersFactor;
    uint32_t              flags;
};
