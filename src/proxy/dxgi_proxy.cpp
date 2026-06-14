#include "dxgi_proxy.h"
#include "../common/logger.h"
#include "../hooks/hook_manager.h"
#include <process.h>

DXGIFunctions g_RealDXGI;
HMODULE g_DllModule = nullptr;

bool DXGIFunctions::Load() {
    if (realDll) return true;

    wchar_t systemPath[MAX_PATH];
    GetSystemDirectoryW(systemPath, MAX_PATH);
    wcscat_s(systemPath, L"\\dxgi.dll");

    realDll = LoadLibraryW(systemPath);
    if (!realDll) {
        return false;
    }

    CreateDXGIFactory = GetProcAddress(realDll, "CreateDXGIFactory");
    CreateDXGIFactory1 = GetProcAddress(realDll, "CreateDXGIFactory1");
    CreateDXGIFactory2 = GetProcAddress(realDll, "CreateDXGIFactory2");
    DXGIDeclareAdapterRemovalSupport = GetProcAddress(realDll, "DXGIDeclareAdapterRemovalSupport");
    DXGIGetDebugInterface1 = GetProcAddress(realDll, "DXGIGetDebugInterface1");

    ApplyCompatResolutionQuirking = GetProcAddress(realDll, "ApplyCompatResolutionQuirking");
    CompatString = GetProcAddress(realDll, "CompatString");
    CompatValue = GetProcAddress(realDll, "CompatValue");
    DXGID3D10CreateDevice = GetProcAddress(realDll, "DXGID3D10CreateDevice");
    DXGID3D10CreateLayeredDevice = GetProcAddress(realDll, "DXGID3D10CreateLayeredDevice");
    DXGID3D10GetLayeredDeviceSize = GetProcAddress(realDll, "DXGID3D10GetLayeredDeviceSize");
    DXGID3D10RegisterLayers = GetProcAddress(realDll, "DXGID3D10RegisterLayers");
    DXGID3D10ETWRundown = GetProcAddress(realDll, "DXGID3D10ETWRundown");
    DXGIDumpJournal = GetProcAddress(realDll, "DXGIDumpJournal");
    DXGIReportAdapterConfiguration = GetProcAddress(realDll, "DXGIReportAdapterConfiguration");
    PIXBeginCapture = GetProcAddress(realDll, "PIXBeginCapture");
    PIXEndCapture = GetProcAddress(realDll, "PIXEndCapture");
    PIXGetCaptureState = GetProcAddress(realDll, "PIXGetCaptureState");
    SetAppCompatStringPointer = GetProcAddress(realDll, "SetAppCompatStringPointer");
    UpdateHMDEmulationStatus = GetProcAddress(realDll, "UpdateHMDEmulationStatus");

    return true;
}

#define DEFINE_FORWARD(name) \
    extern "C" void WINAPI name() { \
        if (!g_RealDXGI.realDll) g_RealDXGI.Load(); \
        typedef void (WINAPI *PFN)(); \
        ((PFN)g_RealDXGI.name)(); \
    }

DEFINE_FORWARD(ApplyCompatResolutionQuirking)
DEFINE_FORWARD(CompatString)
DEFINE_FORWARD(CompatValue)
DEFINE_FORWARD(DXGID3D10CreateDevice)
DEFINE_FORWARD(DXGID3D10CreateLayeredDevice)
DEFINE_FORWARD(DXGID3D10GetLayeredDeviceSize)
DEFINE_FORWARD(DXGID3D10RegisterLayers)
DEFINE_FORWARD(DXGID3D10ETWRundown)
DEFINE_FORWARD(DXGIDumpJournal)
DEFINE_FORWARD(DXGIReportAdapterConfiguration)
DEFINE_FORWARD(PIXBeginCapture)
DEFINE_FORWARD(PIXEndCapture)
DEFINE_FORWARD(PIXGetCaptureState)
DEFINE_FORWARD(SetAppCompatStringPointer)
DEFINE_FORWARD(UpdateHMDEmulationStatus)

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    if (!g_RealDXGI.realDll) g_RealDXGI.Load();
    typedef HRESULT (WINAPI *PFN)(REFIID, void**);
    return ((PFN)g_RealDXGI.CreateDXGIFactory)(riid, ppFactory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    if (!g_RealDXGI.realDll) g_RealDXGI.Load();
    typedef HRESULT (WINAPI *PFN)(REFIID, void**);
    return ((PFN)g_RealDXGI.CreateDXGIFactory1)(riid, ppFactory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    if (!g_RealDXGI.realDll) g_RealDXGI.Load();
    typedef HRESULT (WINAPI *PFN)(UINT, REFIID, void**);
    return ((PFN)g_RealDXGI.CreateDXGIFactory2)(Flags, riid, ppFactory);
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    if (!g_RealDXGI.realDll) g_RealDXGI.Load();
    typedef HRESULT (WINAPI *PFN)();
    return ((PFN)g_RealDXGI.DXGIDeclareAdapterRemovalSupport)();
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    if (!g_RealDXGI.realDll) g_RealDXGI.Load();
    typedef HRESULT (WINAPI *PFN)(UINT, REFIID, void**);
    return ((PFN)g_RealDXGI.DXGIGetDebugInterface1)(Flags, riid, pDebug);
}

unsigned int __stdcall InitThread(void* pArguments) {
    Logger::Instance().Initialize(g_DllModule);
    LOG_INFO("Worker thread started. Initializing capture core...");

    if (!g_RealDXGI.Load()) {
        LOG_ERROR("Failed to load original system dxgi.dll!");
        return 1;
    }
    LOG_INFO("Original dxgi.dll loaded and export table parsed successfully.");

    if (HookManager::Instance().InstallHooks()) {
        LOG_INFO("Hooks installed successfully.");
    } else {
        LOG_ERROR("Failed to install hooks!");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_DllModule = hModule;
            DisableThreadLibraryCalls(hModule);
            _beginthreadex(NULL, 0, InitThread, NULL, 0, NULL);
            break;
        case DLL_PROCESS_DETACH:
            HookManager::Instance().UninstallHooks();
            break;
    }
    return TRUE;
}
