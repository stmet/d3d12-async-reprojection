#include "warp_renderer.h"
#include "../common/logger.h"
#include "../input/mouse_tracker.h"

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler")
#include <cmath>

namespace {

constexpr UINT kFrames = 3;

bool                        s_init   = false;
bool                        s_failed = false;
WarpParams                  s_params;

ID3D12Device*               s_device   = nullptr;
ID3D12RootSignature*        s_rootSig  = nullptr;
ID3D12PipelineState*        s_pso      = nullptr;
ID3D12DescriptorHeap*       s_srvHeap  = nullptr;   // 1 SRV (warp source)
ID3D12DescriptorHeap*       s_rtvHeap  = nullptr;   // 1 RTV (current backbuffer)
ID3D12Resource*             s_warpSrc  = nullptr;   // copy of the backbuffer we sample from

ID3D12CommandAllocator*     s_alloc[kFrames] = {};
ID3D12GraphicsCommandList*  s_list   = nullptr;
ID3D12Fence*                s_fence  = nullptr;
HANDLE                      s_fenceEvent = nullptr;
UINT64                      s_fenceVal = 0;
UINT64                      s_frameFence[kFrames] = {};
UINT                        s_frameIdx = 0;

UINT                        s_w = 0, s_h = 0;
DXGI_FORMAT                 s_fmt = DXGI_FORMAT_UNKNOWN;
uint64_t                    s_lastPresentQpc = 0;
UINT                        s_srvInc = 0;          // CBV_SRV_UAV descriptor stride

// ---- mode 2 reprojection pipeline (color + depth + MV) ----
ID3D12RootSignature*        s_rpRoot   = nullptr;
ID3D12PipelineState*        s_rpPso    = nullptr;
ID3D12DescriptorHeap*       s_rpSrvHeap = nullptr;  // kFrames * 3 SRVs (color, depth, mv per slot)
bool                        s_rpFailed = false;

const char* kReprojectShader =
"Texture2D    gColor : register(t0);\n"
"Texture2D    gDepth : register(t1);\n"
"Texture2D    gMV    : register(t2);\n"
"Texture2D    gFull  : register(t3);\n"
"SamplerState gLin   : register(s0);\n"
"SamplerState gPt    : register(s1);\n"
"cbuffer P : register(b0) {\n"
"    float2 gWarp;           // rotational mouse offset (UV)\n"
"    float2 gMvScale;        // signed MV->UV gain per axis\n"
"    float  gMvFactor;       // async extrapolation (game-frames ahead)\n"
"    float  gMvThreshold;    // moving-object residual threshold\n"
"    float  gDepthEdgeThresh;\n"
"    uint   gDepthEdge;\n"
"    uint   gMode;           // 0 rotational, 2 hybrid, 3 per-pixel MV, 4 perspective rotational\n"
"    uint   gEnable;\n"
"    float  gNearCut;        // skip object reproj where reversed-Z depth above this (near field/weapon)\n"
"    float  gCamRejectK;     // adaptive camera-motion rejection strength\n"
"    float  gTanHalfV;       // tan(fovV/2) for ray reconstruction (mode 4)\n"
"    float  gAspect;         // display width/height (mode 4)\n"
"    float  gYaw;            // fresh camera yaw delta, radians (mode 4)\n"
"    float  gPitch;          // fresh camera pitch delta, radians (mode 4)\n"
"    uint   gWeaponLock;     // 1 = keep near-field (weapon + its optics) screen-locked (mode 4)\n"
"    float  gWeaponDilate;   // UV radius to fill weapon-mask holes (scope lens at world depth) (mode 4)\n"
"    uint   gHudMask;        // 1 = keep HUD regions screen-locked (crosshair + edges)\n"
"    float  gHudCenterR;     // crosshair lock radius (UV, circular about screen center)\n"
"    float  gHudEdge;        // edge-HUD lock inset (UV from each screen edge; minimap/ammo/health)\n"
"    uint   gUseHudless;\n"
"    float  gUiThreshold;    // |present-hudless| (max channel) -> UI; mask ramps thr..2*thr\n"
"    uint   gUiErode;        // UI-mask erosion radius (px); rejects film-grain false positives\n"
"    uint   gDebugView;      // 1 = output the UI mask as grayscale (tuning)\n"
"    float  gEdgeFade;       // disocclusion fade: UV width over which out-of-frame samples ramp to black (0=off)\n"
"    float  gMaskDilate;     // weapon-lock near-mask dilation radius (UV); grows the gun mask to cover the\n"
"                            // soft render-res depth silhouette edge (kills the ghost outline)\n"
"};\n"
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VSOut VSMain(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    o.uv  = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"    return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_Target {\n"
"    float2 suv = i.uv;\n"
"    if (gEnable != 0) {\n"
"      if (gMode == 4) {\n"
"        // PERSPECTIVE rotational reprojection (depth-independent -> cannot fold). Reconstruct this\n"
"        // pixel's view ray from the FOV, rotate it by the fresh mouse delta to find where that ray\n"
"        // pointed in the frozen frame, and project back to UV. Reduces to a uniform shift at the\n"
"        // screen center but correctly curves toward the edges (which a flat UV shift gets wrong).\n"
"        float th = gTanHalfV;\n"
"        float tw = gTanHalfV * gAspect;\n"
"        float2 ndc = float2(i.uv.x * 2.0f - 1.0f, 1.0f - i.uv.y * 2.0f);\n"
"        float3 d = float3(ndc.x * tw, ndc.y * th, 1.0f);\n"
"        float cy = cos(gYaw),   sy = sin(gYaw);\n"
"        float cp = cos(gPitch), sp = sin(gPitch);\n"
"        float3 dp = float3(d.x, cp * d.y - sp * d.z, sp * d.y + cp * d.z);     // Rx(pitch)\n"
"        float3 f  = float3(cy * dp.x + sy * dp.z, dp.y, -sy * dp.x + cy * dp.z); // Ry(yaw)\n"
"        if (f.z <= 1e-4f) return float4(0,0,0,1);   // rotated behind camera -> border\n"
"        suv = float2((f.x / f.z / tw + 1.0f) * 0.5f, (1.0f - f.y / f.z / th) * 0.5f);\n"
"        // Head-locked layer: the first-person weapon + everything bolted to it (sights, red-dot) is\n"
"        // camera-locked at the near plane (reversed-Z -> high depth). Keep it un-warped so the gun\n"
"        // and its optics stay put as a unit while the world (far depth, incl. NPC/world markers)\n"
"        // reprojects around them. This is the VR weapon-layer trick.\n"
"        if (gWeaponLock != 0) {\n"
"            // Dilated near test: the captured depth is render-res (softer than the display-res color),\n"
"            // so the gun's silhouette edge reads ambiguous and would warp into a ghost OUTLINE. Treat a\n"
"            // pixel as gun if it OR a close neighbor is near-field, growing the mask to swallow the edge.\n"
"            float dHere = gDepth.SampleLevel(gPt, i.uv, 0).r;\n"
"            float dMax = dHere;\n"
"            if (gMaskDilate > 0.0f) {\n"
"                float2 md = float2(gMaskDilate / gAspect, gMaskDilate);\n"
"                dMax = max(dMax, gDepth.SampleLevel(gPt, i.uv + float2( md.x, 0), 0).r);\n"
"                dMax = max(dMax, gDepth.SampleLevel(gPt, i.uv + float2(-md.x, 0), 0).r);\n"
"                dMax = max(dMax, gDepth.SampleLevel(gPt, i.uv + float2( 0, md.y), 0).r);\n"
"                dMax = max(dMax, gDepth.SampleLevel(gPt, i.uv + float2( 0,-md.y), 0).r);\n"
"                dMax = max(dMax, gDepth.SampleLevel(gPt, i.uv + md, 0).r);\n"
"                dMax = max(dMax, gDepth.SampleLevel(gPt, i.uv - md, 0).r);\n"
"            }\n"
"            bool nearHere = dMax > gNearCut;\n"
"            if (!nearHere && gWeaponDilate > 0.0f) {\n"
"                // Fill holes inside the weapon silhouette. The scope lens/optic display renders at\n"
"                // WORLD depth, so bare depth misclassifies it as world and warps it loose from the\n"
"                // gun. If this far pixel is enclosed by near-field weapon along 2+ axes, it is a hole\n"
"                // in the gun (the lens) -> lock it with the gun.\n"
"                float2 r = float2(gWeaponDilate / gAspect, gWeaponDilate);\n"
"                float2 dg = r * 0.70710678f;\n"
"                int enc = 0;\n"
"                if (gDepth.SampleLevel(gPt, i.uv + float2( r.x, 0), 0).r > gNearCut && gDepth.SampleLevel(gPt, i.uv + float2(-r.x, 0), 0).r > gNearCut) enc++;\n"
"                if (gDepth.SampleLevel(gPt, i.uv + float2( 0, r.y), 0).r > gNearCut && gDepth.SampleLevel(gPt, i.uv + float2( 0,-r.y), 0).r > gNearCut) enc++;\n"
"                if (gDepth.SampleLevel(gPt, i.uv + float2( dg.x, dg.y), 0).r > gNearCut && gDepth.SampleLevel(gPt, i.uv + float2(-dg.x,-dg.y), 0).r > gNearCut) enc++;\n"
"                if (gDepth.SampleLevel(gPt, i.uv + float2( dg.x,-dg.y), 0).r > gNearCut && gDepth.SampleLevel(gPt, i.uv + float2(-dg.x, dg.y), 0).r > gNearCut) enc++;\n"
"                if (enc >= 2) nearHere = true;\n"
"            }\n"
"            if (nearHere) {\n"
"                suv = i.uv;                                   // gun (or its lens): stay screen-locked\n"
"            } else {\n"
"                // World pixel: if its warped gather source lands on the gun, sampling it would paint\n"
"                // a SECOND, moving copy of the weapon (the gun is baked into this single buffer, not a\n"
"                // separate layer). Reject -> sample the un-warped world here instead. Costs a thin\n"
"                // un-warped band around the gun, but kills the ghost weapon.\n"
"                // Dilate the SOURCE test the same way as the destination mask: the captured depth is\n"
"                // render-res, so the gun's silhouette reads as world along its soft edge; a single-point\n"
"                // test lets that gun-edge COLOR leak through as a thin copy that trails the turn. Treat\n"
"                // the source as gun if it OR a close neighbor is near-field.\n"
"                float dSrc = gDepth.SampleLevel(gPt, suv, 0).r;\n"
"                if (gMaskDilate > 0.0f) {\n"
"                    float2 md = float2(gMaskDilate / gAspect, gMaskDilate);\n"
"                    dSrc = max(dSrc, gDepth.SampleLevel(gPt, suv + float2( md.x, 0), 0).r);\n"
"                    dSrc = max(dSrc, gDepth.SampleLevel(gPt, suv + float2(-md.x, 0), 0).r);\n"
"                    dSrc = max(dSrc, gDepth.SampleLevel(gPt, suv + float2( 0, md.y), 0).r);\n"
"                    dSrc = max(dSrc, gDepth.SampleLevel(gPt, suv + float2( 0,-md.y), 0).r);\n"
"                    dSrc = max(dSrc, gDepth.SampleLevel(gPt, suv + md, 0).r);\n"
"                    dSrc = max(dSrc, gDepth.SampleLevel(gPt, suv - md, 0).r);\n"
"                }\n"
"                if (dSrc > gNearCut) suv = i.uv;\n"
"            }\n"
"        }\n"
"      } else if (gMode == 3) {\n"
"        // TRUE reprojection. Every pixel is reprojected by its OWN motion vector, which already\n"
"        // encodes the depth-correct geometric screen motion the engine computed (camera rotation +\n"
"        // translation/parallax + object motion). No corner cancellation -> no translation shaking,\n"
"        // and the camera-locked weapon (MV~0) simply stays put -> no ghost.\n"
"        float2 mv = gMV.SampleLevel(gPt, i.uv, 0).rg * gMvScale;\n"
"        float2 suvA = i.uv - mv * gMvFactor;            // forward-extrapolate this surface\n"
"        // Add only the RESIDUAL of fresh mouse rotation the game's MV hasn't seen yet: the MV\n"
"        // extrapolation already applied the game's last-frame rotation (~uniform = corner average),\n"
"        // so subtract that and add the fresh mouse term to avoid double-counting camera rotation.\n"
"        float2 g0 = gMV.SampleLevel(gPt, float2(0.04f, 0.04f), 0).rg;\n"
"        float2 g1 = gMV.SampleLevel(gPt, float2(0.96f, 0.04f), 0).rg;\n"
"        float2 g2 = gMV.SampleLevel(gPt, float2(0.04f, 0.96f), 0).rg;\n"
"        float2 g3 = gMV.SampleLevel(gPt, float2(0.96f, 0.96f), 0).rg;\n"
"        float2 cornerMV = (g0 + g1 + g2 + g3) * 0.25f * gMvScale;\n"
"        float2 residualRot = gWarp + cornerMV * gMvFactor;\n"
"        suv = saturate(suvA + residualRot);\n"
"      } else {\n"
"        // Mouse handles the CAMERA (responsive, late-latched on the CPU side).\n"
"        suv = saturate(i.uv + gWarp);\n"
"        if (gMode == 2) {\n"
"            // MV handles moving OBJECTS only: subtract the camera's global MV (estimated from the\n"
"            // frame corners) so static world cancels, reproject only the residual.\n"
"            float dHere = gDepth.SampleLevel(gLin, i.uv, 0).r;\n"
"            // Skip the near field entirely (reversed-Z: near plane -> high value). The first-person\n"
"            // weapon is camera-locked (MV ~ 0) so it has a huge residual vs the world and would be\n"
"            // falsely reprojected into a ghost/stencil of itself. Leave it on the pure camera warp.\n"
"            if (dHere <= gNearCut) {\n"
"                float2 localVel = gMV.SampleLevel(gPt, i.uv, 0).rg;\n"
"                float2 g0 = gMV.SampleLevel(gPt, float2(0.04f, 0.04f), 0).rg;\n"
"                float2 g1 = gMV.SampleLevel(gPt, float2(0.96f, 0.04f), 0).rg;\n"
"                float2 g2 = gMV.SampleLevel(gPt, float2(0.04f, 0.96f), 0).rg;\n"
"                float2 g3 = gMV.SampleLevel(gPt, float2(0.96f, 0.96f), 0).rg;\n"
"                float2 globalVel = (g0 + g1 + g2 + g3) * 0.25f;\n"
"                float2 residual = localVel - globalVel;\n"
"                // Under camera TRANSLATION the corner estimate ignores parallax, so static near\n"
"                // geometry shows a large false residual. Raise the 'is a moving object' bar in\n"
"                // proportion to camera speed (gCamRejectK) to suppress that shaking.\n"
"                float camMag = length(globalVel);\n"
"                float thr = max(gMvThreshold, camMag * gCamRejectK);\n"
"                if (dot(residual, residual) > thr * thr) {\n"
"                    float2 disp = residual * gMvScale * gMvFactor;\n"
"                    float dl = length(disp);\n"
"                    if (dl > 0.06f) disp *= 0.06f / dl;   // clamp wild MV (disocclusion) from popping\n"
"                    float2 src = saturate(suv - disp);\n"
"                    float w = 1.0f;\n"
"                    if (gDepthEdge != 0) {\n"
"                        // Reproject only where the gather source is the SAME surface; at a moving-\n"
"                        // object edge the source lands on background and raw reversed-Z depth jumps.\n"
"                        float dThere = gDepth.SampleLevel(gLin, src, 0).r;\n"
"                        w = 1.0f - saturate((abs(dHere - dThere) - gDepthEdgeThresh) / max(gDepthEdgeThresh, 1e-5f));\n"
"                    }\n"
"                    suv = lerp(suv, src, w);\n"
"                }\n"
"            }\n"
"        }\n"
"      }\n"
"    }\n"
"    // HUD lock (applies last — UI is composited on top of everything). Keep screen-anchored UI from\n"
"    // warping: a center disc for the crosshair, and a border inset for the edge HUD (minimap, ammo,\n"
"    // health). These have no usable depth, so we mask them by screen region. Gives a fixed reference\n"
"    // to judge whether the warped world/weapon line up under the crosshair.\n"
"    if (gHudMask != 0) {\n"
"        float2 c = i.uv - 0.5f; c.x *= gAspect;\n"
"        bool inCenter = dot(c, c) < gHudCenterR * gHudCenterR;\n"
"        bool inEdge   = i.uv.x < gHudEdge || i.uv.x > 1.0f - gHudEdge ||\n"
"                        i.uv.y < gHudEdge || i.uv.y > 1.0f - gHudEdge;\n"
"        if (inCenter || inEdge) suv = i.uv;\n"
"    }\n"
"    if (gEnable == 0) {\n"
"        if (gUseHudless != 0) {\n"
"            return gFull.Sample(gLin, i.uv);\n"
"        } else {\n"
"            return gColor.Sample(gLin, i.uv);\n"
"        }\n"
"    }\n"
"    if (gUseHudless != 0) {\n"
"        // Decoupled HUD compositor. The warp's scene source is the FINAL present buffer (gFull:\n"
"        // all post-processing + film grain), with UI holes filled from the hud-less buffer (gColor)\n"
"        // so nothing UI-shaped gets dragged by the warp. The REAL UI is then re-applied UNWARPED as\n"
"        // a screen-space layer. UI detection is a single-tap difference on the ALIGNED (unwarped)\n"
"        // present/hudless pair -- it is never fused into the per-pixel warp classification, which is\n"
"        // what tore object silhouettes apart in the old neighborhood-search compositor.\n"
"        // Scene at the warped gather location (present, with UI hole-filled by hudless):\n"
"        float3 pW = gFull.SampleLevel(gLin, suv, 0).rgb;\n"
"        float3 hW = gColor.SampleLevel(gLin, suv, 0).rgb;\n"
"        float3 dW = abs(pW - hW);\n"
"        float  mW = smoothstep(gUiThreshold, gUiThreshold * 2.0f, max(dW.r, max(dW.g, dW.b)));\n"
"        float3 warpedScene = lerp(pW, hW, mW);\n"
"        // UI layer at THIS screen pixel (unwarped), re-composited on top:\n"
"        // Re-composite mask at this screen pixel, ERODED over a small radius. A lone high-delta\n"
"        // film-grain pixel surrounded by background is pulled to 0 by the min, so the unwarped\n"
"        // present never ghosts into the warped background (the doubling/blur under motion that\n"
"        // scaled with warp strength). Solid UI interiors stay 1; UI shrinks by ~gUiErode px.\n"
"        float texW, texH; gColor.GetDimensions(texW, texH);\n"
"        float2 inv = float2(1.0f / texW, 1.0f / texH);\n"
"        int R = (int)gUiErode;\n"
"        float mH = 1.0f;\n"
"        [loop] for (int dy = -R; dy <= R; ++dy) {\n"
"            [loop] for (int dx = -R; dx <= R; ++dx) {\n"
"                float2 uv = i.uv + float2(dx, dy) * inv;\n"
"                float3 dN = abs(gFull.SampleLevel(gLin, uv, 0).rgb - gColor.SampleLevel(gLin, uv, 0).rgb);\n"
"                mH = min(mH, smoothstep(gUiThreshold, gUiThreshold * 2.0f, max(dN.r, max(dN.g, dN.b))));\n"
"            }\n"
"        }\n"
"        float3 pH = gFull.SampleLevel(gLin, i.uv, 0).rgb;\n"
"        if (gDebugView != 0) return float4(mH, mH, mH, 1.0f);\n"
"        return float4(lerp(warpedScene, pH, mH), 1.0f);\n"
"    }\n"
"    float4 outc = gColor.Sample(gLin, suv);\n"
"    // Disocclusion feather: where the warp sampled past the frame edge (suv outside [0,1]) the clamp\n"
"    // sampler would smear the border pixel. Instead ramp those pixels to black over gEdgeFade UV, so a\n"
"    // fast flick shows a soft darkened margin rather than a streaked smear.\n"
"    if (gEdgeFade > 0.0f) {\n"
"        float2 over = max(max(-suv, suv - 1.0f), 0.0f);\n"
"        float outAmt = max(over.x, over.y);\n"
"        outc.rgb *= saturate(1.0f - outAmt / gEdgeFade);\n"
"    }\n"
"    return outc;\n"
"}\n";

const char* kShader =
"Texture2D    gColor : register(t0);\n"
"SamplerState gSamp  : register(s0);\n"
"cbuffer Params : register(b0) { float2 gWarp; float2 pad; };\n"
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VSOut VSMain(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    o.uv  = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"    return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_Target {\n"
"    float2 uv = saturate(i.uv + gWarp);\n"
"    return gColor.Sample(gSamp, uv);\n"
"}\n";

void ReleaseResources() {
    for (UINT i = 0; i < kFrames; ++i) { if (s_alloc[i]) { s_alloc[i]->Release(); s_alloc[i] = nullptr; } }
    if (s_list)     { s_list->Release();     s_list = nullptr; }
    if (s_fence)    { s_fence->Release();    s_fence = nullptr; }
    if (s_fenceEvent) { CloseHandle(s_fenceEvent); s_fenceEvent = nullptr; }
    if (s_warpSrc)  { s_warpSrc->Release();  s_warpSrc = nullptr; }
    if (s_srvHeap)  { s_srvHeap->Release();  s_srvHeap = nullptr; }
    if (s_rtvHeap)  { s_rtvHeap->Release();  s_rtvHeap = nullptr; }
    if (s_pso)      { s_pso->Release();      s_pso = nullptr; }
    if (s_rootSig)  { s_rootSig->Release();  s_rootSig = nullptr; }
    if (s_rpSrvHeap){ s_rpSrvHeap->Release();s_rpSrvHeap = nullptr; }
    if (s_rpPso)    { s_rpPso->Release();    s_rpPso = nullptr; }
    if (s_rpRoot)   { s_rpRoot->Release();   s_rpRoot = nullptr; }
    s_rpFailed = false;
}

bool BuildPipeline() {
    // Root signature: SRV table (t0) + 2 root constants (b0) + static linear-clamp sampler.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 4; // float2 gWarp + float2 pad
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        LOG_ERROR("Warp: SerializeRootSignature failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        if (sig) sig->Release(); if (err) err->Release();
        return false;
    }
    HRESULT hr = s_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s_rootSig));
    sig->Release(); if (err) err->Release();
    if (FAILED(hr)) { LOG_ERROR("Warp: CreateRootSignature failed 0x%X", hr); return false; }

    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* e1 = nullptr; ID3DBlob* e2 = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &e1)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &e2))) {
        LOG_ERROR("Warp: shader compile failed: %s %s",
                  e1 ? (char*)e1->GetBufferPointer() : "", e2 ? (char*)e2->GetBufferPointer() : "");
        if (vs) vs->Release(); if (ps) ps->Release(); if (e1) e1->Release(); if (e2) e2->Release();
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = s_rootSig;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = s_fmt;
    pso.SampleDesc.Count = 1;
    hr = s_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&s_pso));
    vs->Release(); ps->Release(); if (e1) e1->Release(); if (e2) e2->Release();
    if (FAILED(hr)) { LOG_ERROR("Warp: CreateGraphicsPipelineState failed 0x%X", hr); return false; }
    return true;
}

bool BuildSizedResources(UINT w, UINT h, DXGI_FORMAT fmt) {
    if (s_warpSrc) { s_warpSrc->Release(); s_warpSrc = nullptr; }

    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(s_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s_warpSrc)))) {
        LOG_ERROR("Warp: failed to create warp-source texture %ux%u fmt=%u", w, h, (unsigned)fmt);
        return false;
    }
    // SRV for the warp source.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    s_device->CreateShaderResourceView(s_warpSrc, &sd, s_srvHeap->GetCPUDescriptorHandleForHeapStart());
    s_w = w; s_h = h; s_fmt = fmt;
    return true;
}

bool EnsureInit(ID3D12Resource* backbuffer) {
    if (s_failed) return false;
    D3D12_RESOURCE_DESC bb = backbuffer->GetDesc();

    if (!s_init) {
        if (FAILED(backbuffer->GetDevice(IID_PPV_ARGS(&s_device)))) { s_failed = true; return false; }
        s_fmt = bb.Format;
        if (!BuildPipeline()) { s_failed = true; return false; }

        // Slot 0: in-place scratch SRV (Render). Slots 1..kFrames: rotating source SRV (WarpInto).
        D3D12_DESCRIPTOR_HEAP_DESC sh = {};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 1 + kFrames;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        s_srvInc = s_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 1;
        if (FAILED(s_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&s_srvHeap))) ||
            FAILED(s_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&s_rtvHeap)))) { s_failed = true; return false; }

        for (UINT i = 0; i < kFrames; ++i)
            if (FAILED(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc[i])))) { s_failed = true; return false; }
        if (FAILED(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc[0], nullptr, IID_PPV_ARGS(&s_list)))) { s_failed = true; return false; }
        s_list->Close();
        s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence));
        s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!BuildSizedResources((UINT)bb.Width, bb.Height, bb.Format)) { s_failed = true; return false; }
        s_init = true;
        LOG_INFO("Warp: initialized (%llux%u fmt=%u)", bb.Width, bb.Height, (unsigned)bb.Format);
        return true;
    }

    // Re-create the warp source on a resolution/format change.
    if ((UINT)bb.Width != s_w || bb.Height != s_h || bb.Format != s_fmt) {
        // Drain GPU before swapping the source texture.
        s_fence->Signal(0);
        s_fenceVal = 0;
        for (UINT i = 0; i < kFrames; ++i) s_frameFence[i] = 0;
        if (!BuildSizedResources((UINT)bb.Width, bb.Height, bb.Format)) { s_failed = true; return false; }
    }
    return true;
}

inline void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

// Builds the mode-2 reprojection pipeline (3-SRV table + 12 root constants + linear/point samplers).
// Lazy, one-shot; assumes EnsureInit already created s_device and set s_fmt (the dest RTV format).
bool BuildReprojectPipeline() {
    if (s_rpPso) return true;
    if (s_rpFailed) return false;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 4;            // t0 color, t1 depth, t2 mv, t3 full
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 27;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samps[2] = {};
    samps[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samps[0].AddressU = samps[0].AddressV = samps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[0].ShaderRegister = 0;
    samps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samps[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samps[1].AddressU = samps[1].AddressV = samps[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samps[1].ShaderRegister = 1;
    samps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samps;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        LOG_ERROR("Reproject: SerializeRootSignature failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        if (sig) sig->Release(); if (err) err->Release(); s_rpFailed = true; return false;
    }
    HRESULT hr = s_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s_rpRoot));
    sig->Release(); if (err) err->Release();
    if (FAILED(hr)) { LOG_ERROR("Reproject: CreateRootSignature failed 0x%X", hr); s_rpFailed = true; return false; }

    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* e1 = nullptr; ID3DBlob* e2 = nullptr;
    if (FAILED(D3DCompile(kReprojectShader, strlen(kReprojectShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &e1)) ||
        FAILED(D3DCompile(kReprojectShader, strlen(kReprojectShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &e2))) {
        LOG_ERROR("Reproject: shader compile failed: %s %s",
                  e1 ? (char*)e1->GetBufferPointer() : "", e2 ? (char*)e2->GetBufferPointer() : "");
        if (vs) vs->Release(); if (ps) ps->Release(); if (e1) e1->Release(); if (e2) e2->Release();
        s_rpFailed = true; return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = s_rpRoot;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = s_fmt;
    pso.SampleDesc.Count = 1;
    hr = s_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&s_rpPso));
    vs->Release(); ps->Release(); if (e1) e1->Release(); if (e2) e2->Release();
    if (FAILED(hr)) { LOG_ERROR("Reproject: CreateGraphicsPipelineState failed 0x%X", hr); s_rpFailed = true; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kFrames * 4;     // color/depth/mv/full per in-flight slot
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&s_rpSrvHeap)))) {
        LOG_ERROR("Reproject: CreateDescriptorHeap failed"); s_rpFailed = true; return false;
    }
    LOG_INFO("Reproject: pipeline built (RTV fmt=%u)", (unsigned)s_fmt);
    return true;
}

// Late-latch window start. Submit-relative (async, frameSubmitQpc != 0): measure the mouse-motion
// window from the displayed frame's own submit time, so the warp accumulates with the frame's true
// age across re-presents — smooth present-rate extrapolation instead of stepped game-rate motion (the
// "drunk/rubberband" feel). Present-relative (frameSubmitQpc == 0, sync path): legacy behavior.
// trimMs biases the window start (negative = reach further back = warp harder / less latency).
static uint64_t WarpBaseQpc(uint64_t frameSubmitQpc, uint64_t now) {
    int64_t trim = MouseTracker::MsToQpc(s_params.trimMs);
    if (frameSubmitQpc) return (uint64_t)((int64_t)frameSubmitQpc + trim);
    return s_lastPresentQpc ? (uint64_t)((int64_t)s_lastPresentQpc + trim) : now;
}

} // namespace

WarpRenderer& WarpRenderer::Instance() { static WarpRenderer w; return w; }
WarpParams&   WarpRenderer::Params()   { return s_params; }

void WarpRenderer::Render(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer) {
    if (!s_params.enable || !queue || !backbuffer) return;
    if (!EnsureInit(backbuffer)) return;

    // Late-latch: warp by the mouse delta accrued between this frame's input-sample time (estimated
    // as the previous present + trim) and now.
    uint64_t now = MouseTracker::NowQpc();
    uint64_t base = s_lastPresentQpc ? (uint64_t)((int64_t)s_lastPresentQpc + MouseTracker::MsToQpc(s_params.trimMs)) : now;
    long long cx, cy, bx, by;
    MouseTracker::GetAccAt(now, cx, cy);
    MouseTracker::GetAccAt(base, bx, by);
    s_lastPresentQpc = now;

    float warpU = (float)(cx - bx) * s_params.gain * s_params.sign / (float)s_w;
    float warpV = (float)(cy - by) * s_params.gain * s_params.sign / (float)s_h;
    s_params.lastU = warpU; s_params.lastV = warpV;
    float consts[4] = { warpU, warpV, 0.0f, 0.0f };

    UINT slot = s_frameIdx % kFrames;
    if (s_fence->GetCompletedValue() < s_frameFence[slot]) {
        s_fence->SetEventOnCompletion(s_frameFence[slot], s_fenceEvent);
        WaitForSingleObject(s_fenceEvent, INFINITE);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    s_device->CreateRenderTargetView(backbuffer, nullptr, rtv);

    s_alloc[slot]->Reset();
    s_list->Reset(s_alloc[slot], s_pso);

    // backbuffer (PRESENT) -> COPY_SOURCE ; warpSrc (COMMON) -> COPY_DEST ; copy ; flip states.
    Barrier(s_list, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(s_list, s_warpSrc,  D3D12_RESOURCE_STATE_COMMON,  D3D12_RESOURCE_STATE_COPY_DEST);
    s_list->CopyResource(s_warpSrc, backbuffer);
    Barrier(s_list, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(s_list, s_warpSrc,  D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_VIEWPORT vp = { 0, 0, (float)s_w, (float)s_h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)s_w, (LONG)s_h };
    s_list->RSSetViewports(1, &vp);
    s_list->RSSetScissorRects(1, &sc);
    s_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    s_list->SetGraphicsRootSignature(s_rootSig);
    ID3D12DescriptorHeap* heaps[] = { s_srvHeap };
    s_list->SetDescriptorHeaps(1, heaps);
    s_list->SetGraphicsRootDescriptorTable(0, s_srvHeap->GetGPUDescriptorHandleForHeapStart());
    s_list->SetGraphicsRoot32BitConstants(1, 4, consts, 0);
    s_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_list->DrawInstanced(3, 1, 0, 0);

    Barrier(s_list, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(s_list, s_warpSrc,  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    s_list->Close();
    ID3D12CommandList* lists[] = { s_list };
    queue->ExecuteCommandLists(1, lists);
    s_frameFence[slot] = ++s_fenceVal;
    queue->Signal(s_fence, s_fenceVal);
    s_frameIdx++;
}

void WarpRenderer::WarpInto(ID3D12CommandQueue* queue,
                            ID3D12Resource* source, D3D12_RESOURCE_STATES srcState,
                            ID3D12Resource* dest,   D3D12_RESOURCE_STATES destState,
                            uint64_t frameSubmitQpc) {
    if (!queue || !source || !dest) return;
    if (!EnsureInit(dest)) return;   // pipeline + heaps sized to the real backbuffer (dest)

    // Late-latch the warp offset exactly like the in-place path. When the warp is disabled (or
    // suppressed because the game is in a cursor-free menu) we emit a zero offset so the presenter
    // still produces a clean resample (passthrough).
    float warpU = 0.0f, warpV = 0.0f;
    uint64_t now = MouseTracker::NowQpc();
    if (s_params.enable && !s_params.runtimeSuppress) {
        uint64_t base = WarpBaseQpc(frameSubmitQpc, now);
        long long cx, cy, bx, by;
        MouseTracker::GetAccAt(now, cx, cy);
        MouseTracker::GetAccAt(base, bx, by);
        warpU = (float)(cx - bx) * s_params.gain * s_params.sign / (float)s_w;
        warpV = (float)(cy - by) * s_params.gain * s_params.sign / (float)s_h;
    }
    s_lastPresentQpc = now;
    s_params.lastU = warpU; s_params.lastV = warpV;
    float consts[4] = { warpU, warpV, 0.0f, 0.0f };

    UINT slot = s_frameIdx % kFrames;
    if (s_fence->GetCompletedValue() < s_frameFence[slot]) {
        s_fence->SetEventOnCompletion(s_frameFence[slot], s_fenceEvent);
        WaitForSingleObject(s_fenceEvent, INFINITE);
    }

    // Source SRV into this slot's descriptor (slots 1..kFrames).
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = s_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = s_srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvCpu.ptr += (SIZE_T)(1 + slot) * s_srvInc;
    srvGpu.ptr += (UINT64)(1 + slot) * s_srvInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = s_fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    s_device->CreateShaderResourceView(source, &sd, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    s_device->CreateRenderTargetView(dest, nullptr, rtv);

    s_alloc[slot]->Reset();
    s_list->Reset(s_alloc[slot], s_pso);

    Barrier(s_list, dest,   destState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // source (the replacement buffer) is ALLOW_SIMULTANEOUS_ACCESS — read as an SRV from COMMON, no barrier.

    D3D12_VIEWPORT vp = { 0, 0, (float)s_w, (float)s_h, 0.0f, 1.0f };
    D3D12_RECT scr = { 0, 0, (LONG)s_w, (LONG)s_h };
    s_list->RSSetViewports(1, &vp);
    s_list->RSSetScissorRects(1, &scr);
    s_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    s_list->SetGraphicsRootSignature(s_rootSig);
    ID3D12DescriptorHeap* heaps[] = { s_srvHeap };
    s_list->SetDescriptorHeaps(1, heaps);
    s_list->SetGraphicsRootDescriptorTable(0, srvGpu);
    s_list->SetGraphicsRoot32BitConstants(1, 4, consts, 0);
    s_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_list->DrawInstanced(3, 1, 0, 0);

    Barrier(s_list, dest,   D3D12_RESOURCE_STATE_RENDER_TARGET, destState);

    s_list->Close();
    ID3D12CommandList* lists[] = { s_list };
    queue->ExecuteCommandLists(1, lists);
    s_frameFence[slot] = ++s_fenceVal;
    queue->Signal(s_fence, s_fenceVal);
    s_frameIdx++;
}

void WarpRenderer::ReprojectInto(ID3D12CommandQueue* queue,
                                 ID3D12Resource* color, D3D12_RESOURCE_STATES srcState,
                                 ID3D12Resource* dest,  D3D12_RESOURCE_STATES destState,
                                 ID3D12Resource* depth, DXGI_FORMAT depthSrvFmt,
                                 ID3D12Resource* mv,    DXGI_FORMAT mvSrvFmt,
                                 float mvFactor, float fovV,
                                 ID3D12Resource* hud,   D3D12_RESOURCE_STATES hudState,
                                 uint64_t frameSubmitQpc) {
    if (!queue || !color || !dest) return;
    // Modes 2/3 consume depth+MV; without them there is nothing to reproject — fall back to the
    // uniform-shift path. Mode 4 (perspective rotational) is depth-independent and needs neither.
    bool needsGeom = (s_params.mode == 2 || s_params.mode == 3);
    if (needsGeom && (!depth || !mv)) { WarpInto(queue, color, srcState, dest, destState, frameSubmitQpc); return; }
    if (!EnsureInit(dest)) return;
    if (!BuildReprojectPipeline()) { WarpInto(queue, color, srcState, dest, destState, frameSubmitQpc); return; }

    // FOV geometry first — the angular gain model needs it to back-fill the UV readout, and the shader
    // needs it (gTanHalfV/gAspect) to reconstruct + rotate view rays. Use the manual FOV (mode 4 lean),
    // falling back to ~59 deg vertical (1.034 rad) if it is unset.
    float aspect    = s_h ? (float)s_w / (float)s_h : 1.777f;
    float tanHalfV  = tanf((fovV > 0.01f ? fovV : 1.034f) * 0.5f);
    float tanHalfH  = tanHalfV * aspect;

    // Late-latch the camera term. Two models:
    //  - ANGULAR (lean default): counts -> yaw/pitch in radians via sensDegPer1000. FOV-correct; the
    //    on-screen shift falls out of the perspective projection below, automatically larger when zoomed.
    //  - LEGACY UV: counts -> a flat UV shift via `gain` (FOV-agnostic), then convert to yaw/pitch so
    //    the center matches the uniform shift exactly while the edges curve. Used by modes 0/2/3 too.
    float warpU = 0.0f, warpV = 0.0f, yaw = 0.0f, pitch = 0.0f;
    uint64_t now = MouseTracker::NowQpc();
    bool effEnable = s_params.enable && !s_params.runtimeSuppress;
    if (effEnable) {
        uint64_t base = WarpBaseQpc(frameSubmitQpc, now);
        long long cx, cy, bx, by;
        MouseTracker::GetAccAt(now, cx, cy);
        MouseTracker::GetAccAt(base, bx, by);
        long long dx = cx - bx, dy = cy - by;
        if (s_params.angularGain) {
            const float radPerCount = s_params.sensDegPer1000 * (3.14159265f / 180.0f) / 1000.0f;
            yaw   = (float)dx * radPerCount * s_params.sign;
            pitch = (float)dy * radPerCount * s_params.sign * s_params.pitchRatio;
            // Back-fill the center-equivalent UV shift for the HUD readout (lastU/lastV) and the
            // modes-0/2/3 gWarp term. Inverse of the shader's yaw = 2*tanHalfH*warpU.
            warpU = (tanHalfH > 1e-5f) ? yaw   / (2.0f * tanHalfH) : 0.0f;
            warpV = (tanHalfV > 1e-5f) ? pitch / (2.0f * tanHalfV) : 0.0f;
        } else {
            warpU = (float)dx * s_params.gain * s_params.sign / (float)s_w;
            warpV = (float)dy * s_params.gain * s_params.sign / (float)s_h;
            yaw   = 2.0f * tanHalfH * warpU;
            pitch = 2.0f * tanHalfV * warpV;
        }
    }
    // Bound the rotation so a fast flick can't open a huge disocclusion band (the eye is motion-blurred
    // during a hard flick, so the slightly-incomplete correction isn't perceptible).
    if (s_params.maxWarpDeg > 0.0f) {
        float maxRad = s_params.maxWarpDeg * (3.14159265f / 180.0f);
        if (yaw   >  maxRad) yaw   =  maxRad;  if (yaw   < -maxRad) yaw   = -maxRad;
        if (pitch >  maxRad) pitch =  maxRad;  if (pitch < -maxRad) pitch = -maxRad;
    }
    s_lastPresentQpc = now;
    s_params.lastU = warpU; s_params.lastV = warpV; s_params.lastMvFactor = mvFactor;

    // ADS vs hip weapon-lock profile (the optic needs different lock settings when aiming).
    const bool  ads             = s_params.adsActive;
    const float effNearCut      = ads ? s_params.adsNearCut    : s_params.nearDepthCut;
    const float effWeaponDilate = ads ? s_params.adsWeaponDilate : s_params.weaponDilate;
    const float effMaskDilate   = ads ? s_params.adsMaskDilate : s_params.maskDilate;

    struct RPConsts {
        float warpU, warpV;
        float mvScaleX, mvScaleY;
        float mvFactor;
        float mvThreshold;
        float depthEdgeThresh;
        UINT  depthEdge;
        UINT  mode;
        UINT  enable;
        float nearCut, camRejectK;
        float tanHalfV, aspect, yaw, pitch;
        UINT  weaponLock;
        float weaponDilate;
        UINT  hudMask;
        float hudCenterR, hudEdge;
        UINT  useHudless;
        float uiThreshold;
        UINT  uiErode;
        UINT  debugView;
        float edgeFade;
        float maskDilate;
    } consts = {
        warpU, warpV,
        s_params.mvScale * s_params.sign, s_params.mvScale * s_params.sign,
        mvFactor,
        s_params.mvThreshold,
        s_params.depthEdgeThresh,
        s_params.depthEdge ? 1u : 0u,
        (UINT)s_params.mode,
        effEnable ? 1u : 0u,
        effNearCut, s_params.camRejectK,
        tanHalfV, aspect, yaw, pitch,
        (s_params.weaponLock && depth) ? 1u : 0u,  // only when real depth is bound
        effWeaponDilate,
        s_params.hudMask ? 1u : 0u,
        s_params.hudCenterR, s_params.hudEdge,
        hud ? 1u : 0u,
        s_params.uiThreshold,
        (UINT)(s_params.uiErode < 0 ? 0 : s_params.uiErode),
        s_params.debugMask ? 1u : 0u,
        s_params.edgeFade,
        effMaskDilate
    };

    UINT slot = s_frameIdx % kFrames;
    if (s_fence->GetCompletedValue() < s_frameFence[slot]) {
        s_fence->SetEventOnCompletion(s_frameFence[slot], s_fenceEvent);
        WaitForSingleObject(s_fenceEvent, INFINITE);
    }

    // Four contiguous SRVs for this slot: color (s_fmt), depth (depthSrvFmt), mv (mvSrvFmt), full (s_fmt).
    UINT bd = slot * 4;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_rpSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_rpSrvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)bd * s_srvInc;
    gpu.ptr += (UINT64)bd * s_srvInc;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    // Mode 4 ignores depth/MV; if the capture is missing, bind color as a harmless stand-in so the
    // descriptor table is fully populated (color is already being made shader-readable below).
    ID3D12Resource* depthRes = depth ? depth : color;  DXGI_FORMAT depthFmt = depth ? depthSrvFmt : s_fmt;
    ID3D12Resource* mvRes    = mv    ? mv    : color;  DXGI_FORMAT mvFmt    = mv    ? mvSrvFmt    : s_fmt;
    ID3D12Resource* hudRes   = hud   ? hud   : color;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    sv.Format = s_fmt;      s_device->CreateShaderResourceView(color,    &sv, h); h.ptr += s_srvInc;
    sv.Format = depthFmt;   s_device->CreateShaderResourceView(depthRes, &sv, h); h.ptr += s_srvInc;
    sv.Format = mvFmt;      s_device->CreateShaderResourceView(mvRes,    &sv, h); h.ptr += s_srvInc;
    sv.Format = s_fmt;      s_device->CreateShaderResourceView(hudRes,   &sv, h);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    s_device->CreateRenderTargetView(dest, nullptr, rtv);

    s_alloc[slot]->Reset();
    s_list->Reset(s_alloc[slot], s_rpPso);

    Barrier(s_list, dest,  destState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // color (the replacement buffer) is ALLOW_SIMULTANEOUS_ACCESS: read as an SRV directly from COMMON,
    // no transition barrier (and no cross-queue state-tracking conflict with the game's queue).
    if (hud && hud != color) {
        Barrier(s_list, hud, hudState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // depth + mv are simultaneous-access COMMON (the capture leaves them shader-readable) — no barrier.

    D3D12_VIEWPORT vp = { 0, 0, (float)s_w, (float)s_h, 0.0f, 1.0f };
    D3D12_RECT scr = { 0, 0, (LONG)s_w, (LONG)s_h };
    s_list->RSSetViewports(1, &vp);
    s_list->RSSetScissorRects(1, &scr);
    s_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    s_list->SetGraphicsRootSignature(s_rpRoot);
    ID3D12DescriptorHeap* heaps[] = { s_rpSrvHeap };
    s_list->SetDescriptorHeaps(1, heaps);
    s_list->SetGraphicsRootDescriptorTable(0, gpu);
    s_list->SetGraphicsRoot32BitConstants(1, 27, &consts, 0);
    s_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_list->DrawInstanced(3, 1, 0, 0);

    if (hud && hud != color) {
        Barrier(s_list, hud, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, hudState);
    }
    Barrier(s_list, dest,  D3D12_RESOURCE_STATE_RENDER_TARGET, destState);

    s_list->Close();
    ID3D12CommandList* lists[] = { s_list };
    queue->ExecuteCommandLists(1, lists);
    s_frameFence[slot] = ++s_fenceVal;
    queue->Signal(s_fence, s_fenceVal);
    s_frameIdx++;
}

void WarpRenderer::Shutdown() {
    if (s_fence && s_fenceEvent) {
        s_fence->Signal(++s_fenceVal);
        if (s_fence->GetCompletedValue() < s_fenceVal) {
            s_fence->SetEventOnCompletion(s_fenceVal, s_fenceEvent);
            WaitForSingleObject(s_fenceEvent, INFINITE);
        }
    }
    ReleaseResources();
    if (s_device) { s_device->Release(); s_device = nullptr; }
    s_init = false;
}
