#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct DXGIFunctions {
    HMODULE realDll = nullptr;

    FARPROC CreateDXGIFactory = nullptr;
    FARPROC CreateDXGIFactory1 = nullptr;
    FARPROC CreateDXGIFactory2 = nullptr;
    FARPROC DXGIDeclareAdapterRemovalSupport = nullptr;
    FARPROC DXGIGetDebugInterface1 = nullptr;

    FARPROC ApplyCompatResolutionQuirking = nullptr;
    FARPROC CompatString = nullptr;
    FARPROC CompatValue = nullptr;
    FARPROC DXGID3D10CreateDevice = nullptr;
    FARPROC DXGID3D10CreateLayeredDevice = nullptr;
    FARPROC DXGID3D10GetLayeredDeviceSize = nullptr;
    FARPROC DXGID3D10RegisterLayers = nullptr;
    FARPROC DXGID3D10ETWRundown = nullptr;
    FARPROC DXGIDumpJournal = nullptr;
    FARPROC DXGIReportAdapterConfiguration = nullptr;
    FARPROC PIXBeginCapture = nullptr;
    FARPROC PIXEndCapture = nullptr;
    FARPROC PIXGetCaptureState = nullptr;
    FARPROC SetAppCompatStringPointer = nullptr;
    FARPROC UpdateHMDEmulationStatus = nullptr;

    bool Load();
};

extern DXGIFunctions g_RealDXGI;
