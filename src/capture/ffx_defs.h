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

// ============================================================================================
// NATIVE FSR3 SDK path (FSR 3.0 — what Cyberpunk uses). Dispatched via ffxFsr3ContextDispatchUpscale
// / ffxFsr3UpscalerContextDispatch in ffx_fsr3*_x64.dll, NOT the ffx-api. Layout verified against the
// FidelityFX-SDK + the project's prior empirical fix: the three dilated* resources must be present and
// there is NO upscaleSize, which aligns cameraNear/Far/Fov onto their real values for the 3.0 build.
// Depth/MV are the leading fields and capture regardless of any tail misalignment. Do NOT reorder.
// ============================================================================================
struct FfxFloatCoords2D { float x; float y; };
struct FfxDimensions2D  { unsigned int width; unsigned int height; };

struct FfxResourceDescriptionFSR3 {
    unsigned int type; unsigned int format;
    union { unsigned int width;  unsigned int size;      };
    union { unsigned int height; unsigned int stride;    };
    union { unsigned int depth;  unsigned int alignment; };
    unsigned int mipCount; unsigned int flags; unsigned int usage;
};

struct FfxResourceFSR3 {
    void* resource;
    FfxResourceDescriptionFSR3 description;
    int   state;
    wchar_t name[64];
};

struct FfxFsr3DispatchDescription {
    void*           commandList;
    FfxResourceFSR3 color;
    FfxResourceFSR3 depth;
    FfxResourceFSR3 motionVectors;
    FfxResourceFSR3 exposure;
    FfxResourceFSR3 reactive;
    FfxResourceFSR3 transparencyAndComposition;
    FfxResourceFSR3 dilatedDepth;
    FfxResourceFSR3 dilatedMotionVectors;
    FfxResourceFSR3 reconstructedPrevNearestDepth;
    FfxResourceFSR3 output;
    FfxFloatCoords2D jitterOffset;
    FfxFloatCoords2D motionVectorScale;
    FfxDimensions2D  renderSize;
    FfxDimensions2D  upscaleSize;   // present in the current FSR3 SDK layout (CP2077) — aligns cam params
    bool  enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool  reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    unsigned int flags;
};
