// ==WindhawkMod==
// @id              alt-tab-anywhere
// @name            Alt+Tab Anywhere
// @description     Let's you decide which monitor Alt+Tab should show up on.
// @version         1.1
// @author          Visnes
// @github          https://github.com/visnes
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lversion
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Alt+Tab Anywhere

Pressing Alt+Tab shows the switcher on the primary display. This mod can show
it on the monitor where the cursor is, or on a specific monitor, and can limit
the list to the windows on that monitor.

Alt+Tab and Win+Alt+Tab have separate settings.

## Specific monitor

Enter the monitor as either:

- A number: `1`, `2`, `3`, ...
- A device name: `\\.\DISPLAY2` (or just `DISPLAY2`)

In the Advanced settings JSON, the value must be quoted (`"2"`, not `2`).

Monitor numbers follow the order Windows enumerates monitors in, which may not
match the numbers in Settings > System > Display, and can change after sleep,
reconnecting a monitor or a driver update. Device names are usually more
stable.

To see the number and device name of each monitor, enable debug logging for
this mod in Windhawk and check the log. The monitors are listed when the mod
loads and whenever its settings change.

If the specified monitor isn't found, the primary monitor is used. "Primary"
means whichever monitor Windows currently treats as primary, which other mods
can change.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- altTabLocation: cursor
  $name: Alt+Tab location
  $description: Which monitor the Alt+Tab switcher is shown on
  $options:
  - primary: Primary monitor
  - cursor: Monitor where the cursor is
  - specific: Specific monitor
- altTabWindows: cursorMonitor
  $name: Alt+Tab windows
  $description: Which windows are listed in the Alt+Tab switcher
  $options:
  - allMonitors: Windows on all monitors
  - cursorMonitor: Windows on the monitor where the cursor is
  - cursorMonitorExceptPrimary: >-
      Windows on the monitor where the cursor is, all windows if it's the
      primary monitor
  - specificMonitor: Windows on the specific monitor
- altTabMonitor: "1"
  $name: Alt+Tab specific monitor
  $description: >-
    Used by the "Specific monitor" options above. A monitor number (1, 2,
    3...) or a device name such as \\.\DISPLAY2. Falls back to the primary
    monitor if not found.
- winAltTabLocation: primary
  $name: Win+Alt+Tab location
  $description: Which monitor the switcher is shown on when Win is also held
  $options:
  - primary: Primary monitor
  - cursor: Monitor where the cursor is
  - specific: Specific monitor
- winAltTabWindows: allMonitors
  $name: Win+Alt+Tab windows
  $description: Which windows are listed when Win is also held
  $options:
  - allMonitors: Windows on all monitors
  - cursorMonitor: Windows on the monitor where the cursor is
  - cursorMonitorExceptPrimary: >-
      Windows on the monitor where the cursor is, all windows if it's the
      primary monitor
  - specificMonitor: Windows on the specific monitor
- winAltTabMonitor: "1"
  $name: Win+Alt+Tab specific monitor
  $description: >-
    Used by the Win+Alt+Tab "Specific monitor" options above. A monitor number
    (1, 2, 3...) or a device name such as \\.\DISPLAY2. Falls back to the
    primary monitor if not found.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <servprov.h>

#include <atomic>
#include <functional>

#include <winrt/base.h>

enum class Location {
    primary,
    cursor,
    specific,
};

enum class WindowsToShow {
    allMonitors,
    cursorMonitor,
    cursorMonitorExceptPrimary,
    specificMonitor,
};

// A monitor identified either by its 1-based enumeration number or by its
// device name (e.g. \\.\DISPLAY2). If number > 0, the number is used.
// If neither is set, the setting is invalid and the primary monitor is used.
struct MonitorSpec {
    int number;
    WCHAR deviceName[CCHDEVICENAME];
};

struct {
    Location altTabLocation;
    WindowsToShow altTabWindows;
    MonitorSpec altTabMonitor;
    Location winAltTabLocation;
    WindowsToShow winAltTabWindows;
    MonitorSpec winAltTabMonitor;
} g_settings;

enum class WinVersion {
    Unsupported,
    Win10,
    Win11,
};

WinVersion g_winVersion;

std::atomic<DWORD> g_threadIdForAltTabShowWindow;
std::atomic<DWORD> g_lastThreadIdForXamlAltTabViewHost_CreateInstance;
std::atomic<DWORD> g_threadIdForXamlAltTabViewHost_CreateInstance;
std::atomic<ULONGLONG> g_CreateInstance_TickCount;
// Monitor to filter windows by. nullptr means the cursor's monitor.
std::atomic<HMONITOR> g_filterMonitor;
constexpr ULONGLONG kDeltaThreshold = 200;

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE hModule, UINT* puPtrLen) {
    void* pFixedFileInfo = nullptr;
    UINT uPtrLen = 0;

    HRSRC hResource =
        FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
    if (hResource) {
        HGLOBAL hGlobal = LoadResource(hModule, hResource);
        if (hGlobal) {
            void* pData = LockResource(hGlobal);
            if (pData) {
                if (!VerQueryValue(pData, L"\\", &pFixedFileInfo, &uPtrLen) ||
                    uPtrLen == 0) {
                    pFixedFileInfo = nullptr;
                    uPtrLen = 0;
                }
            }
        }
    }

    if (puPtrLen) {
        *puPtrLen = uPtrLen;
    }

    return (VS_FIXEDFILEINFO*)pFixedFileInfo;
}

WinVersion GetWindowsVersion() {
    VS_FIXEDFILEINFO* fixedFileInfo = GetModuleVersionInfo(nullptr, nullptr);
    if (!fixedFileInfo) {
        return WinVersion::Unsupported;
    }

    WORD major = HIWORD(fixedFileInfo->dwFileVersionMS);
    WORD minor = LOWORD(fixedFileInfo->dwFileVersionMS);
    WORD build = HIWORD(fixedFileInfo->dwFileVersionLS);
    WORD qfe = LOWORD(fixedFileInfo->dwFileVersionLS);

    Wh_Log(L"Version: %u.%u.%u.%u", major, minor, build, qfe);

    switch (major) {
        case 10:
            if (build < 22000) {
                return WinVersion::Win10;
            } else {
                return WinVersion::Win11;
            }
            break;
    }

    return WinVersion::Unsupported;
}

bool IsWinKeyPressed() {
    return GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
}

////////////////////////////////////////////////////////////////////////////////
// Monitor lookup

HMONITOR GetPrimaryMonitor() {
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

bool DeviceNameMatches(PCWSTR actual, PCWSTR wanted) {
    if (_wcsicmp(actual, wanted) == 0) {
        return true;
    }

    // Allow "DISPLAY2" without the "\\.\" prefix.
    constexpr WCHAR kPrefix[] = L"\\\\.\\";
    constexpr size_t kPrefixLen = ARRAYSIZE(kPrefix) - 1;
    return _wcsnicmp(actual, kPrefix, kPrefixLen) == 0 &&
           _wcsicmp(actual + kPrefixLen, wanted) == 0;
}

struct FindMonitorData {
    const MonitorSpec* spec;
    int index;
    HMONITOR result;
};

BOOL CALLBACK FindMonitorEnumProc(HMONITOR hMon,
                                  HDC hdc,
                                  LPRECT rect,
                                  LPARAM lParam) {
    auto* data = reinterpret_cast<FindMonitorData*>(lParam);
    data->index++;

    if (data->spec->number > 0) {
        if (data->index == data->spec->number) {
            data->result = hMon;
            return FALSE;
        }
        return TRUE;
    }

    MONITORINFOEX monInfo{};
    monInfo.cbSize = sizeof(monInfo);
    if (GetMonitorInfo(hMon, &monInfo) &&
        DeviceNameMatches(monInfo.szDevice, data->spec->deviceName)) {
        data->result = hMon;
        return FALSE;
    }

    return TRUE;
}

// Returns nullptr if the monitor isn't found.
HMONITOR FindMonitor(const MonitorSpec& spec) {
    if (spec.number <= 0 && !spec.deviceName[0]) {
        return nullptr;
    }

    FindMonitorData data{&spec, 0, nullptr};
    EnumDisplayMonitors(nullptr, nullptr, FindMonitorEnumProc,
                        reinterpret_cast<LPARAM>(&data));
    return data.result;
}

BOOL CALLBACK LogMonitorsEnumProc(HMONITOR hMon,
                                  HDC hdc,
                                  LPRECT rect,
                                  LPARAM lParam) {
    int* index = reinterpret_cast<int*>(lParam);
    (*index)++;

    MONITORINFOEX monInfo{};
    monInfo.cbSize = sizeof(monInfo);
    if (!GetMonitorInfo(hMon, &monInfo)) {
        Wh_Log(L"Monitor %d: GetMonitorInfo failed", *index);
        return TRUE;
    }

    Wh_Log(L"Monitor %d: %s, %ldx%ld at (%ld, %ld)%s", *index,
           monInfo.szDevice, monInfo.rcMonitor.right - monInfo.rcMonitor.left,
           monInfo.rcMonitor.bottom - monInfo.rcMonitor.top,
           monInfo.rcMonitor.left, monInfo.rcMonitor.top,
           (monInfo.dwFlags & MONITORINFOF_PRIMARY) ? L" (primary)" : L"");
    return TRUE;
}

void LogMonitors() {
    int index = 0;
    EnumDisplayMonitors(nullptr, nullptr, LogMonitorsEnumProc,
                        reinterpret_cast<LPARAM>(&index));
}

////////////////////////////////////////////////////////////////////////////////

// Returns the monitor the switcher should be shown on, or nullptr to leave
// Alt+Tab's own choice (the primary monitor) unchanged.
HMONITOR GetAltTabTargetMonitor() {
    bool winKey = IsWinKeyPressed();
    auto location =
        winKey ? g_settings.winAltTabLocation : g_settings.altTabLocation;

    switch (location) {
        case Location::primary:
            return nullptr;

        case Location::cursor: {
            POINT pt;
            if (!GetCursorPos(&pt)) {
                return nullptr;
            }
            return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        }

        case Location::specific: {
            HMONITOR hMon = FindMonitor(winKey ? g_settings.winAltTabMonitor
                                               : g_settings.altTabMonitor);
            if (!hMon) {
                Wh_Log(L"Specified monitor not found, using primary");
            }
            return hMon;
        }
    }

    return nullptr;
}

// Used by the old Win10 (non-XAML) Alt+Tab, which is placed by rect.
bool HandleAltTabWindow(RECT* rect) {
    HMONITOR hMon = GetAltTabTargetMonitor();
    if (!hMon) {
        return false;
    }

    MONITORINFO monInfo;
    monInfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hMon, &monInfo)) {
        return false;
    }

    *rect = monInfo.rcWork;
    return true;
}

// Immersive monitor service, used to get the IImmersiveMonitor that the XAML
// Alt+Tab is placed and sized for. GUIDs and interface layout are taken from
// the upstream "Alt+Tab per monitor" mod (v1.2.1).

// {C2F03A33-21F5-47FA-B4BB-156362A2F239}
const CLSID CLSID_ImmersiveShell = {
    0xC2F03A33,
    0x21F5,
    0x47FA,
    {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39},
};

// {47094E3A-0CF2-430F-806F-CF9E4F0F12DD}
const GUID SID_IImmersiveMonitorService = {
    0x47094E3A,
    0x0CF2,
    0x430F,
    {0x80, 0x6F, 0xCF, 0x9E, 0x4F, 0x0F, 0x12, 0xDD},
};

// {4D4C1E64-E410-4FAA-BAFA-59CA069BFEC2}
const IID IID_IImmersiveMonitorManager = {
    0x4D4C1E64,
    0xE410,
    0x4FAA,
    {0xBA, 0xFA, 0x59, 0xCA, 0x06, 0x9B, 0xFE, 0xC2},
};

// The returned monitors are IImmersiveMonitor objects.
struct IImmersiveMonitorManager : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT index, IUnknown** monitor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFromHandle(HMONITOR monitorHandle,
                                                    IUnknown** monitor) = 0;
};

winrt::com_ptr<IUnknown> GetImmersiveMonitorFromHandle(HMONITOR monitorHandle) {
    winrt::com_ptr<IServiceProvider> serviceProvider;
    HRESULT hr =
        CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                         IID_PPV_ARGS(serviceProvider.put()));
    if (FAILED(hr)) {
        Wh_Log(L"CoCreateInstance failed: 0x%08X", hr);
        return nullptr;
    }

    winrt::com_ptr<IImmersiveMonitorManager> monitorManager;
    hr = serviceProvider->QueryService(SID_IImmersiveMonitorService,
                                       IID_IImmersiveMonitorManager,
                                       monitorManager.put_void());
    if (FAILED(hr)) {
        Wh_Log(L"QueryService failed: 0x%08X", hr);
        return nullptr;
    }

    winrt::com_ptr<IUnknown> monitor;
    hr = monitorManager->GetFromHandle(monitorHandle, monitor.put());
    if (FAILED(hr)) {
        Wh_Log(L"GetFromHandle failed: 0x%08X", hr);
        return nullptr;
    }

    return monitor;
}

void* CWin32ApplicationView_vtable;
void* CWinRTApplicationView_vtable;

using CWin32ApplicationView_v_GetNativeWindow_t =
    HRESULT(WINAPI*)(void* pThis, HWND* windowHandle);
CWin32ApplicationView_v_GetNativeWindow_t
    CWin32ApplicationView_v_GetNativeWindow;

using CWinRTApplicationView_v_GetNativeWindow_t =
    HRESULT(WINAPI*)(void* pThis, HWND* windowHandle);
CWinRTApplicationView_v_GetNativeWindow_t
    CWinRTApplicationView_v_GetNativeWindow;

HRESULT GetWindowHandleFromApplicationView(void* applicationView,
                                           HWND* windowHandle) {
    *windowHandle = nullptr;
    void* vtable = *(void**)applicationView;
    HRESULT hr = E_FAIL;

    if (vtable == CWin32ApplicationView_vtable) {
        hr = CWin32ApplicationView_v_GetNativeWindow(applicationView,
                                                     windowHandle);
    } else if (vtable == CWinRTApplicationView_vtable) {
        hr = CWinRTApplicationView_v_GetNativeWindow(applicationView,
                                                     windowHandle);
    }

    return hr;
}

bool IsWindowOnFilterMonitor(HWND windowHandle) {
    HMONITOR hMon = g_filterMonitor;

    if (!hMon) {
        POINT pt;
        if (!GetCursorPos(&pt)) {
            return false;
        }
        hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    auto hMonFromWindow =
        MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONEAREST);

    return hMon == hMonFromWindow;
}

using CVirtualDesktop_IsViewVisible_t = HRESULT(WINAPI*)(void* pThis,
                                                         void* applicationView,
                                                         BOOL* isVisible);
CVirtualDesktop_IsViewVisible_t CVirtualDesktop_IsViewVisible_Original;
HRESULT WINAPI CVirtualDesktop_IsViewVisible_Hook(void* pThis,
                                                  void* applicationView,
                                                  BOOL* isVisible) {
    Wh_Log(L">");
    auto ret = CVirtualDesktop_IsViewVisible_Original(pThis, applicationView,
                                                      isVisible);
    if (FAILED(ret)) {
        return ret;
    }

    if (g_threadIdForXamlAltTabViewHost_CreateInstance !=
        GetCurrentThreadId()) {
        // A focused window might be added after a short period. Filter windows
        // using our monitor rules if the alt tab window was just opened.
        // Otherwise, don't play with the filter anymore, as it's also used by
        // other components such as Win+Tab and the taskbar.
        if ((GetTickCount64() - g_CreateInstance_TickCount) > kDeltaThreshold ||
            g_lastThreadIdForXamlAltTabViewHost_CreateInstance !=
                GetCurrentThreadId()) {
            return ret;
        }
    }

    if (!*isVisible) {
        return ret;
    }

    HWND windowHandle;
    HRESULT hr =
        GetWindowHandleFromApplicationView(applicationView, &windowHandle);

    if (FAILED(hr) || !windowHandle) {
        return ret;
    }

    if (!IsWindowOnFilterMonitor(windowHandle)) {
        *isVisible = FALSE;
    }

    return ret;
}

using XamlAltTabViewHost_Show_t = HRESULT(WINAPI*)(void* pThis,
                                                   void* param1,
                                                   int param2,
                                                   void* param3);
XamlAltTabViewHost_Show_t XamlAltTabViewHost_Show_Original;
HRESULT WINAPI XamlAltTabViewHost_Show_Hook(void* pThis,
                                            void* monitor,
                                            int flags,
                                            void* applicationView) {
    Wh_Log(L">");

    // The switcher is placed and sized for the monitor passed to Show, so
    // substitute the target monitor. Falls back to the original on failure.
    winrt::com_ptr<IUnknown> targetMonitor;
    if (HMONITOR monitorHandle = GetAltTabTargetMonitor()) {
        targetMonitor = GetImmersiveMonitorFromHandle(monitorHandle);
    }

    return XamlAltTabViewHost_Show_Original(
        pThis, targetMonitor ? targetMonitor.get() : monitor, flags,
        applicationView);
}

using CAltTabViewHost_Show_t = HRESULT(WINAPI*)(void* pThis,
                                                void* param1,
                                                int param2,
                                                void* param3);
CAltTabViewHost_Show_t CAltTabViewHost_Show_Original;
HRESULT WINAPI CAltTabViewHost_Show_Hook(void* pThis,
                                         void* param1,
                                         int param2,
                                         void* param3) {
    Wh_Log(L">");
    g_threadIdForAltTabShowWindow = GetCurrentThreadId();
    HRESULT ret = CAltTabViewHost_Show_Original(pThis, param1, param2, param3);
    g_threadIdForAltTabShowWindow = 0;
    return ret;
}

using CMultitaskingViewFrame_CreateFrame_t = HRESULT(WINAPI*)(void* pThis,
                                                              RECT* rect,
                                                              void* param2);
CMultitaskingViewFrame_CreateFrame_t
    CMultitaskingViewFrame_CreateFrame_Original;
HRESULT WINAPI CMultitaskingViewFrame_CreateFrame_Hook(void* pThis,
                                                       RECT* rect,
                                                       void* param2) {
    if (g_threadIdForAltTabShowWindow != GetCurrentThreadId()) {
        return CMultitaskingViewFrame_CreateFrame_Original(pThis, rect, param2);
    }
    g_threadIdForAltTabShowWindow = 0;

    RECT newRect;
    if (!HandleAltTabWindow(&newRect)) {
        return CMultitaskingViewFrame_CreateFrame_Original(pThis, rect, param2);
    }

    HRESULT ret =
        CMultitaskingViewFrame_CreateFrame_Original(pThis, &newRect, param2);

    return ret;
}

HRESULT CreateInstanceHook(std::function<HRESULT()> original) {
    bool winKey = IsWinKeyPressed();
    auto windows =
        winKey ? g_settings.winAltTabWindows : g_settings.altTabWindows;

    if (windows == WindowsToShow::allMonitors) {
        return original();
    }

    if (windows == WindowsToShow::cursorMonitorExceptPrimary) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monInfo;
            monInfo.cbSize = sizeof(MONITORINFO);
            if (GetMonitorInfo(hMon, &monInfo) &&
                (monInfo.dwFlags & MONITORINFOF_PRIMARY)) {
                return original();
            }
        }
    }

    HMONITOR filterMonitor = nullptr;
    if (windows == WindowsToShow::specificMonitor) {
        filterMonitor = FindMonitor(winKey ? g_settings.winAltTabMonitor
                                           : g_settings.altTabMonitor);
        if (!filterMonitor) {
            Wh_Log(L"Specified monitor not found, filtering by primary");
            filterMonitor = GetPrimaryMonitor();
        }
    }
    g_filterMonitor = filterMonitor;

    g_threadIdForXamlAltTabViewHost_CreateInstance = GetCurrentThreadId();
    g_lastThreadIdForXamlAltTabViewHost_CreateInstance = GetCurrentThreadId();
    g_CreateInstance_TickCount = GetTickCount64();
    HRESULT ret = original();
    g_threadIdForXamlAltTabViewHost_CreateInstance = 0;
    return ret;
}

using XamlAltTabViewHost_CreateInstance_t = HRESULT(WINAPI*)(void* pThis,
                                                             void* param1,
                                                             void* param2,
                                                             void* param3);
XamlAltTabViewHost_CreateInstance_t XamlAltTabViewHost_CreateInstance_Original;
HRESULT WINAPI XamlAltTabViewHost_CreateInstance_Hook(void* pThis,
                                                      void* param1,
                                                      void* param2,
                                                      void* param3) {
    Wh_Log(L">");
    return CreateInstanceHook([=]() {
        return XamlAltTabViewHost_CreateInstance_Original(pThis, param1, param2,
                                                          param3);
    });
}

using CAltTabViewHost_CreateInstance_t = HRESULT(WINAPI*)(void* pThis,
                                                          void* param1,
                                                          void* param2,
                                                          void* param3,
                                                          void* param4,
                                                          void* param5,
                                                          void* param6,
                                                          void* param7,
                                                          void* param8,
                                                          void* param9,
                                                          void* param10,
                                                          void* param11);
CAltTabViewHost_CreateInstance_t CAltTabViewHost_CreateInstance_Original;
HRESULT WINAPI CAltTabViewHost_CreateInstance_Hook(void* pThis,
                                                   void* param1,
                                                   void* param2,
                                                   void* param3,
                                                   void* param4,
                                                   void* param5,
                                                   void* param6,
                                                   void* param7,
                                                   void* param8,
                                                   void* param9,
                                                   void* param10,
                                                   void* param11) {
    Wh_Log(L">");
    return CreateInstanceHook([=]() {
        return CAltTabViewHost_CreateInstance_Original(
            pThis, param1, param2, param3, param4, param5, param6, param7,
            param8, param9, param10, param11);
    });
}

using CAltTabViewHost_CreateInstance_Win11_t = HRESULT(WINAPI*)(void* pThis,
                                                                void* param1,
                                                                void* param2,
                                                                void* param3,
                                                                void* param4,
                                                                void* param5,
                                                                void* param6,
                                                                void* param7,
                                                                void* param8,
                                                                void* param9,
                                                                void* param10);
CAltTabViewHost_CreateInstance_Win11_t
    CAltTabViewHost_CreateInstance_Win11_Original;
HRESULT WINAPI CAltTabViewHost_CreateInstance_Win11_Hook(void* pThis,
                                                         void* param1,
                                                         void* param2,
                                                         void* param3,
                                                         void* param4,
                                                         void* param5,
                                                         void* param6,
                                                         void* param7,
                                                         void* param8,
                                                         void* param9,
                                                         void* param10) {
    Wh_Log(L">");
    return CreateInstanceHook([=]() {
        return CAltTabViewHost_CreateInstance_Win11_Original(
            pThis, param1, param2, param3, param4, param5, param6, param7,
            param8, param9, param10);
    });
}

MonitorSpec ParseMonitorSpec(PCWSTR value) {
    MonitorSpec spec{};

    // Trim surrounding spaces.
    while (*value == L' ' || *value == L'\t') {
        value++;
    }
    size_t len = wcslen(value);
    while (len > 0 && (value[len - 1] == L' ' || value[len - 1] == L'\t')) {
        len--;
    }

    if (len == 0 || len >= ARRAYSIZE(spec.deviceName)) {
        // Invalid; the primary monitor will be used.
        return spec;
    }

    bool allDigits = true;
    for (size_t i = 0; i < len; i++) {
        if (value[i] < L'0' || value[i] > L'9') {
            allDigits = false;
            break;
        }
    }

    if (allDigits) {
        spec.number = _wtoi(value);
    } else {
        wcsncpy_s(spec.deviceName, value, len);
    }

    return spec;
}

void LoadSettings() {
    auto loadLocation = [](PCWSTR name, Location defaultVal) {
        PCWSTR val = Wh_GetStringSetting(name);
        Location result = defaultVal;
        if (wcscmp(val, L"primary") == 0) {
            result = Location::primary;
        } else if (wcscmp(val, L"cursor") == 0) {
            result = Location::cursor;
        } else if (wcscmp(val, L"specific") == 0) {
            result = Location::specific;
        }
        Wh_FreeStringSetting(val);
        return result;
    };

    auto loadWindows = [](PCWSTR name, WindowsToShow defaultVal) {
        PCWSTR val = Wh_GetStringSetting(name);
        WindowsToShow result = defaultVal;
        if (wcscmp(val, L"allMonitors") == 0) {
            result = WindowsToShow::allMonitors;
        } else if (wcscmp(val, L"cursorMonitor") == 0) {
            result = WindowsToShow::cursorMonitor;
        } else if (wcscmp(val, L"cursorMonitorExceptPrimary") == 0) {
            result = WindowsToShow::cursorMonitorExceptPrimary;
        } else if (wcscmp(val, L"specificMonitor") == 0) {
            result = WindowsToShow::specificMonitor;
        }
        Wh_FreeStringSetting(val);
        return result;
    };

    auto loadMonitor = [](PCWSTR name) {
        PCWSTR val = Wh_GetStringSetting(name);
        MonitorSpec result = ParseMonitorSpec(val);
        Wh_FreeStringSetting(val);
        return result;
    };

    g_settings.altTabLocation =
        loadLocation(L"altTabLocation", Location::cursor);
    g_settings.altTabWindows =
        loadWindows(L"altTabWindows", WindowsToShow::cursorMonitor);
    g_settings.altTabMonitor = loadMonitor(L"altTabMonitor");
    g_settings.winAltTabLocation =
        loadLocation(L"winAltTabLocation", Location::primary);
    g_settings.winAltTabWindows =
        loadWindows(L"winAltTabWindows", WindowsToShow::allMonitors);
    g_settings.winAltTabMonitor = loadMonitor(L"winAltTabMonitor");

    LogMonitors();
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    g_winVersion = GetWindowsVersion();

    HMODULE twinuiPcshellModule = LoadLibrary(L"twinui.pcshell.dll");
    if (!twinuiPcshellModule) {
        Wh_Log(L"Couldn't load twinui.pcshell.dll");
        return FALSE;
    }

    if (g_winVersion == WinVersion::Win11) {
        // twinui.pcshell.dll
        WindhawkUtils::SYMBOL_HOOK twinuiPcshellSymbolHooks[] = {
            {
                {LR"(public: virtual long __cdecl CVirtualDesktop::IsViewVisible(struct IApplicationView *,int *))"},
                &CVirtualDesktop_IsViewVisible_Original,
                CVirtualDesktop_IsViewVisible_Hook,
            },
            {
                {LR"(const CWin32ApplicationView::`vftable'{for `IApplicationView'})"},
                &CWin32ApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWin32ApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWin32ApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(const CWinRTApplicationView::`vftable'{for `IApplicationView'})"},
                &CWinRTApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWinRTApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWinRTApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(public: virtual long __cdecl XamlAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"},
                &XamlAltTabViewHost_Show_Original,
                XamlAltTabViewHost_Show_Hook,
            },
            {
                {LR"(long __cdecl XamlAltTabViewHost_CreateInstance(struct XamlViewHostInitializeArgs const &,struct _GUID const &,void * *))"},
                &XamlAltTabViewHost_CreateInstance_Original,
                XamlAltTabViewHost_CreateInstance_Hook,
            },
            // For the old Win10 (non-XAML) Alt+Tab (can be enabled with
            // ExplorerPatcher):
            {
                {LR"(public: virtual long __cdecl CAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"},
                &CAltTabViewHost_Show_Original,
                CAltTabViewHost_Show_Hook,
                true,
            },
            {
                {LR"(public: virtual long __cdecl CMultitaskingViewFrame::CreateFrame(struct Geometry::CRect const &,enum MultitaskingViewZOrder))"},
                &CMultitaskingViewFrame_CreateFrame_Original,
                CMultitaskingViewFrame_CreateFrame_Hook,
                true,
            },
            {
                {LR"(long __cdecl CAltTabViewHost_CreateInstance(struct IMultitaskingData *,struct IMultitaskingViewManagerInternal *,struct IApplicationViewSwitcher *,struct IImmersiveAppCrusher *,struct IMultitaskingViewVisibilityServiceInternal *,struct IMultitaskingViewGestureState *,struct IApplicationViewCollection *,struct ITouchGestureSettings *,struct _GUID const &,void * *))"},
                &CAltTabViewHost_CreateInstance_Win11_Original,
                CAltTabViewHost_CreateInstance_Win11_Hook,
                true,
            },
        };

        if (!HookSymbols(twinuiPcshellModule, twinuiPcshellSymbolHooks,
                         ARRAYSIZE(twinuiPcshellSymbolHooks))) {
            Wh_Log(L"HookSymbols failed");
            return FALSE;
        }
    } else if (g_winVersion == WinVersion::Win10) {
        // twinui.pcshell.dll
        WindhawkUtils::SYMBOL_HOOK twinuiPcshellSymbolHooks[] = {
            {
                {LR"(public: virtual long __cdecl CVirtualDesktop::IsViewVisible(struct IApplicationView *,int *))"},
                &CVirtualDesktop_IsViewVisible_Original,
                CVirtualDesktop_IsViewVisible_Hook,
            },
            {
                {LR"(const CWin32ApplicationView::`vftable'{for `IApplicationView'})"},
                &CWin32ApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWin32ApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWin32ApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(const CWinRTApplicationView::`vftable'{for `IApplicationView'})"},
                &CWinRTApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWinRTApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWinRTApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(public: virtual long __cdecl CAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"},
                &CAltTabViewHost_Show_Original,
                CAltTabViewHost_Show_Hook,
            },
            {
                {LR"(public: virtual long __cdecl CMultitaskingViewFrame::CreateFrame(struct Geometry::CRect const &,enum MultitaskingViewZOrder))"},
                &CMultitaskingViewFrame_CreateFrame_Original,
                CMultitaskingViewFrame_CreateFrame_Hook,
            },
            {
                {LR"(long __cdecl CAltTabViewHost_CreateInstance(struct IMultitaskingData *,struct IMultitaskingViewManagerInternal *,struct IApplicationViewSwitcher *,struct IImmersiveAppCrusher *,struct IMultitaskingViewVisibilityServiceInternal *,struct IMultitaskingViewGestureState *,struct IApplicationViewCollection *,struct Windows::Internal::ComposableShell::Tabs::ITabController *,struct ITabViewManager *,struct _GUID const &,void * *))"},
                &CAltTabViewHost_CreateInstance_Original,
                CAltTabViewHost_CreateInstance_Hook,
            },
        };

        if (!HookSymbols(twinuiPcshellModule, twinuiPcshellSymbolHooks,
                         ARRAYSIZE(twinuiPcshellSymbolHooks))) {
            Wh_Log(L"HookSymbols failed");
            return FALSE;
        }
    } else {
        Wh_Log(L"Unsupported Windows version");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();
}