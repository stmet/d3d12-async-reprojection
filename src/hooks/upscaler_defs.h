#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Dummy class mimicking the NVSDK_NGX_Parameter virtual class.
// Virtual method indices must match MSVC compilation.
class NVSDK_NGX_Parameter {
public:
    virtual void Set(const char* name, void* value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, unsigned long long value) = 0;
    
    virtual int Get(const char* name, void** value) = 0;
    virtual int Get(const char* name, float* value) = 0;
    virtual int Get(const char* name, double* value) = 0;
    virtual int Get(const char* name, unsigned int* value) = 0;
    virtual int Get(const char* name, int* value) = 0;
    virtual int Get(const char* name, unsigned long long* value) = 0;
};

// FidelityFX SDK structures for FSR2 / FSR3

struct FfxFloatCoords2D { float x; float y; };
struct FfxDimensions2D { unsigned int width; unsigned int height; };

// --- FSR2 definitions ---
struct FfxResourceDescriptionFSR2 {
    unsigned int type;
    unsigned int format;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int mipCount;
    unsigned int flags;
};

struct FfxResourceFSR2 {
    void* resource;
    wchar_t name[64];
    FfxResourceDescriptionFSR2 description;
    int state;
    bool isDepth;
    unsigned char padding[7]; // Alignment padding to 8-byte boundary
    unsigned long long descriptorData;
};

// NOTE: the engine allocates the FULL FidelityFX dispatch description and passes a pointer to
// it. The project originally truncated this struct after motionVectors (the only fields it
// copied). The trailing fields below complete the real FSR2 layout so we can read
// motionVectorScale / renderSize / camera params. Reading them is memory-safe (the real struct
// is at least this large); correctness is validated by sanity-checking the logged values.
struct FfxFsr2DispatchDescription {
    void* commandList;
    FfxResourceFSR2 color;
    FfxResourceFSR2 depth;
    FfxResourceFSR2 motionVectors;
    FfxResourceFSR2 exposure;
    FfxResourceFSR2 reactive;
    FfxResourceFSR2 transparencyAndComposition;
    FfxResourceFSR2 output;
    FfxFloatCoords2D jitterOffset;
    FfxFloatCoords2D motionVectorScale;
    FfxDimensions2D renderSize;
    bool  enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool  reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
};

// --- FSR3 definitions ---
struct FfxResourceDescriptionFSR3 {
    unsigned int type;
    unsigned int format;
    union {
        unsigned int width;
        unsigned int size;
    };
    union {
        unsigned int height;
        unsigned int stride;
    };
    union {
        unsigned int depth;
        unsigned int alignment;
    };
    unsigned int mipCount;
    unsigned int flags;
    unsigned int usage;
};

struct FfxResourceFSR3 {
    void* resource;
    FfxResourceDescriptionFSR3 description;
    int state;
    wchar_t name[64];
};

// FSR 3.1 decoupled-upscaler dispatch layout (FfxFsr3UpscalerDispatchDescription), verified
// against the FidelityFX SDK header. CRITICAL: between transparencyAndComposition and output
// there are THREE extra resources — dilatedDepth, dilatedMotionVectors,
// reconstructedPrevNearestDepth — that earlier versions of this struct omitted. Without them
// every field after that point (output/jitter/motionVectorScale/renderSize/camera) is read 3
// FfxResource's too early, which is why color/depth/MV (the leading fields) captured correctly
// while the camera params came out zero. See FfxFsr2DispatchDescription note re: memory-safety.
struct FfxFsr3DispatchDescription {
    void* commandList;
    FfxResourceFSR3 color;
    FfxResourceFSR3 depth;
    FfxResourceFSR3 motionVectors;
    FfxResourceFSR3 exposure;
    FfxResourceFSR3 reactive;
    FfxResourceFSR3 transparencyAndComposition;
    FfxResourceFSR3 dilatedDepth;                   // 3.1 shared-resource outputs — required for
    FfxResourceFSR3 dilatedMotionVectors;           // correct alignment of everything below.
    FfxResourceFSR3 reconstructedPrevNearestDepth;
    FfxResourceFSR3 output;
    FfxFloatCoords2D jitterOffset;
    FfxFloatCoords2D motionVectorScale;
    FfxDimensions2D renderSize;
    // NOTE: no upscaleSize here. This game's build is the FSR 3.0-era decoupled-upscaler layout
    // (dilated shared resources present, but upscaleSize was only added in 3.1). Empirically
    // verified: with upscaleSize in place, cameraNear/Far/Fov read fov/viewSpaceToMeters/flags
    // (off by one 8-byte slot); removing it aligns near/far/fov onto their real values.
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

// FSR 3.0 FfxFrameGenerationConfig (FidelityFX-SDK host/ffx_interface.h), passed to
// ffxFsr3ConfigureFrameGeneration when the game enables Frame Generation. Layout mirrors the SDK so
// we can read it safely. The leading three pointers (swapChain + two callbacks) align HUDLessColor
// (which begins with a void* resource) to its real offset. HUDLessColor is the post-FX, SDR,
// pre-UI presentation color — the clean "world" layer for the hud-less reprojection pivot.
struct FfxFrameGenerationConfigFSR3 {
    void*           swapChain;                  // FfxSwapchain
    void*           presentCallback;            // FfxPresentCallbackFunc (game's UI composition)
    void*           frameGenerationCallback;    // FfxFrameGenerationDispatchFunc (the interpolation core)
    bool            frameGenerationEnabled;
    bool            allowAsyncWorkloads;
    FfxResourceFSR3 HUDLessColor;               // hud-less back buffer (may be empty -> UI via callback)
    unsigned int    flags;
    bool            onlyPresentInterpolated;
};

// FSR3 FfxFrameGenerationDispatchDescription (ffx_types.h). Passed to ffxFsr3DispatchFrameGeneration
// when the game generates interpolated frames itself (vs the FG swapchain owning present). presentColor
// is the source frame to interpolate from; outputs[] receive the generated frames. Hooking this both
// confirms FG is running and gives us the frames to reproject + the source to grab the hud-less from.
struct FfxFrameGenerationDispatchDescFSR3 {
    void*           commandList;
    FfxResourceFSR3 presentColor;
    FfxResourceFSR3 outputs[4];
    unsigned int    numInterpolatedFrames;
    bool            reset;
    int             backBufferTransferFunction;
    float           minMaxLuminance[2];
};

// --- FSR 3.1 / SDK v2 (ffx-api) definitions ---
typedef uint32_t ffxReturnCode_t;
typedef void* ffxContext;

#define FFX_API_RETURN_OK 0

#define FFX_API_EFFECT_MASK 0xffff0000u
#define FFX_API_EFFECT_ID_FRAMEGENERATION 0x00020000u
#define FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_DX12 0x00030000u

#define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION 0x00020002u
#define FFX_API_FRAME_GENERATION_CONFIG 0x0002000fu
#define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 0x00030002u

struct ffxApiHeader {
    uint64_t             type;
    struct ffxApiHeader* pNext;
};

typedef ffxApiHeader ffxConfigureDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

struct FfxApiResourceDescription {
    uint32_t     type;
    uint32_t     format;
    union {
        uint32_t width;
        uint32_t size;
    };
    union {
        uint32_t height;
        uint32_t stride;
    };
    union {
        uint32_t depth;
        uint32_t alignment;
    };
    uint32_t     mipCount;
    uint32_t     flags;
    uint32_t     usage;
};

struct FfxApiResource {
    void* resource;
    FfxApiResourceDescription description;
    uint32_t state;
};

struct FfxApiRect2D {
    int32_t left;
    int32_t top;
    int32_t width;
    int32_t height;
};

typedef ffxReturnCode_t (*FfxApiPresentCallbackFunc)(void* params, void* pUserCtx);
typedef ffxReturnCode_t (*FfxApiFrameGenerationDispatchFunc)(void* params, void* pUserCtx);

struct ffxConfigureDescFrameGeneration {
    ffxConfigureDescHeader            header;
    void*                             swapChain;
    FfxApiPresentCallbackFunc         presentCallback;
    void*                             presentCallbackUserContext;
    FfxApiFrameGenerationDispatchFunc frameGenerationCallback;
    void*                             frameGenerationCallbackUserContext;
    bool                              frameGenerationEnabled;
    bool                              allowAsyncWorkloads;
    FfxApiResource                    HUDLessColor;
    uint32_t                          flags;
    bool                              onlyPresentGenerated;
    FfxApiRect2D                      generationRect;
    uint64_t                          frameID;
};

struct FfxFrameGenerationConfig {
    ffxConfigureDescHeader            header;
    void*                             swapChain;
    FfxApiPresentCallbackFunc         presentCallback;
    void*                             presentCallbackContext;
    FfxApiFrameGenerationDispatchFunc frameGenerationCallback;
    void*                             frameGenerationCallbackContext;
    bool                              frameGenerationEnabled;
    bool                              allowAsyncWorkloads;
    bool                              allowAsyncPresent;
    FfxApiResource                    HUDLessColor;
};

struct ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 {
    ffxConfigureDescHeader header;
    FfxApiResource         uiResource;
    uint32_t               flags;
};

struct ffxDispatchDescFrameGeneration {
    ffxConfigureDescHeader header;
    void*                 commandList;
    FfxApiResource        presentColor;
    FfxApiResource        outputs[4];
    uint32_t              numGeneratedFrames;
    bool                  reset;
    uint32_t              backbufferTransferFunction;
    float                 minMaxLuminance[2];
    FfxApiRect2D          generationRect;
    uint64_t              frameID;
};


