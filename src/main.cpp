// RobloxShadeHost: redraws the Roblox window in a D3D11 swapchain of its own, so ReShade can be
// installed on this exe instead of Roblox. Roblox is only observed from outside, through window
// enumeration and Windows.Graphics.Capture. Nothing is opened, read or loaded into its process.

#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "overlay.h"
#include "roblox_window.h"
#include "state.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <tlhelp32.h>

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring exeName;
};

std::vector<WindowInfo> GetVisibleWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
            wchar_t title[256];
            GetWindowTextW(hwnd, title, sizeof(title)/sizeof(wchar_t));
            if (wcslen(title) > 0) {
                DWORD pid;
                GetWindowThreadProcessId(hwnd, &pid);
                HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                std::wstring exeName = L"Unknown";
                if (hSnapshot != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W pe;
                    pe.dwSize = sizeof(PROCESSENTRY32W);
                    if (Process32FirstW(hSnapshot, &pe)) {
                        do {
                            if (pe.th32ProcessID == pid) {
                                exeName = pe.szExeFile;
                                break;
                            }
                        } while (Process32NextW(hSnapshot, &pe));
                    }
                    CloseHandle(hSnapshot);
                }
                
                auto& vec = *reinterpret_cast<std::vector<WindowInfo>*>(param);
                vec.push_back({hwnd, title, exeName});
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

State g;

namespace
{
int Run()
{
    auto config = LoadConfig();

    while(true) {
        std::wcout << L"Current target: " << config.targetExecutable << L"\n";
        std::wcout << L"Press Enter to keep, type 'list' to view open games, or type new executable: ";
        std::wstring newExe;
        std::getline(std::wcin, newExe);
        if (newExe.empty()) {
            break;
        } else if (newExe == L"list" || newExe == L"?") {
            auto windows = GetVisibleWindows();
            std::wcout << L"\n--- Open Windows ---\n";
            for (size_t i = 0; i < windows.size(); ++i) {
                std::wcout << L"[" << i + 1 << L"] " << windows[i].exeName << L" - " << windows[i].title << L"\n";
            }
            std::wcout << L"Enter the number to select, or 0 to cancel: ";
            std::wstring numStr;
            std::getline(std::wcin, numStr);
            try {
                int idx = std::stoi(numStr) - 1;
                if (idx >= 0 && idx < windows.size()) {
                    config.targetExecutable = windows[idx].exeName;
                    std::wcout << L"Selected: " << config.targetExecutable << L"\n\n";
                    break;
                }
            } catch (...) {}
            std::wcout << L"\n";
        } else {
            config.targetExecutable = newExe;
            break;
        }
    }

    std::wcout << L"Enable Depth AI for 3D shaders? (Y/N, current " << (config.enableDepth ? L"Y" : L"N") << L"): ";
    std::wstring depthAns;
    std::getline(std::wcin, depthAns);
    if (!depthAns.empty())
    {
        if (depthAns == L"y" || depthAns == L"Y" || depthAns == L"1")
            config.enableDepth = true;
        else if (depthAns == L"n" || depthAns == L"N" || depthAns == L"0")
            config.enableDepth = false;
    }
    
    std::wcout << L"\n";
    SaveConfig(config);

    if (!GraphicsCaptureSession::IsSupported())
    {
        std::puts("Windows Graphics Capture is not supported on this system.");
        return 1;
    }

    CreateOverlayWindows();

    if (!RegisterHotKey(g.overlay, kEditModeHotkey, config.input.modifiers, config.input.key))
    {
        const std::wstring error = L"Could not register " + g.inputHotkey + L". It may be reserved by Windows or in use by another program. "
                                   L"Choose another ToggleKey in RobloxShadeHost.ini and restart.";
        MessageBoxW(nullptr, error.c_str(), L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (config.overlay.key && !RegisterHotKey(g.overlay, kOverlayToggleHotkey, config.overlay.modifiers, config.overlay.key))
    {
        const std::wstring error = L"Could not register " + g.overlayHotkey + L". It may be reserved by Windows or in use by another program. "
                                   L"Choose another OverlayToggleKey in RobloxShadeHost.ini and restart.";
        MessageBoxW(nullptr, error.c_str(), L"RobloxShadeHost", MB_OK | MB_ICONERROR);
        return 1;
    }

    g.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateDevice();
    if (config.enableDepth) InitDepth();

    std::printf("Install ReShade on this exe (DirectX 10/11/12).\n"
              "Waiting for %ls...\n", config.targetExecutable.c_str());
    std::printf("%ls: toggle input capture. ReShade keeps its own menu and effect shortcuts.\n", g.inputHotkey.c_str());
    if (config.overlay.key)
        std::printf("%ls: toggle overlay and frame capture.\n", g.overlayHotkey.c_str());

    ULONGLONG nextSearch = 0;
    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                if (config.enableDepth) ShutdownDepth();
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g.captureEnabled && g.target)
            StopCapture();

        if (g.target && !IsWindow(g.target))
        {
            StopCapture();
            std::printf("%ls closed. Waiting for %ls...\n", config.targetExecutable.c_str(), config.targetExecutable.c_str());
        }

        if (g.captureEnabled && !g.target && GetTickCount64() >= nextSearch)
        {
            nextSearch = GetTickCount64() + 500;
            if (HWND target = FindTargetWindow(config.targetExecutable))
            {
                try
                {
                    StartCapture(target);
                }
                catch (const winrt::hresult_error& e)
                {
                    std::printf("Could not capture %ls: %ls\n", config.targetExecutable.c_str(), e.message().c_str());
                }
            }
        }

        UpdateOverlay();

        if (g.target)
        {
            // Only the newest frame matters. Rendering every queued frame would add latency.
            while (auto frame = g.pool.TryGetNextFrame())
                g.latestFrame = frame;

            if (g.latestFrame)
            {
                SizeInt32 size = g.latestFrame.ContentSize();
                if ((size.Width != g.poolSize.Width || size.Height != g.poolSize.Height) && size.Width > 0 && size.Height > 0)
                {
                    g.latestFrame = nullptr;
                    g.poolSize = size;
                    g.pool.Recreate(g.captureDevice, kPixelFormat, 2, size);
                    std::printf("%ls resized to %dx%d\n", config.targetExecutable.c_str(), size.Width, size.Height);
                }
            }

            // Also re-presents on timeout, so the ReShade menu stays responsive if Roblox stops drawing.
            if (g.overlayVisible && g.latestFrame)
                PresentLatestFrame();
        }

        MsgWaitForMultipleObjects(1, &g.frameEvent, FALSE, g.overlayVisible ? 16 : 250, QS_ALLINPUT);
    }
}
} // namespace

int main()
{
    std::puts(R"(  ____       _     _            ____  _               _      _   _           _
 |  _ \ ___ | |__ | | _____  __/ ___|| |__   __ _  __| | ___| | | | ___  ___| |_
 | |_) / _ \| '_ \| |/ _ \ \/ /\___ \| '_ \ / _` |/ _` |/ _ \ |_| |/ _ \/ __| __|
 |  _ < (_) | |_) | | (_) >  <  ___) | | | | (_| | (_| |  __/  _  | (_) \__ \ |_
 |_| \_\___/|_.__/|_|\___/_/\_\|____/|_| |_|\__,_|\__,_|\___|_| |_|\___/|___/\__|
)");
    std::printf("v%s\n\n", ROBLOX_SHADE_HOST_VERSION);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
        return Run();
    }
    catch (const winrt::hresult_error& e)
    {
        std::printf("Error 0x%08X: %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return 1;
    }
}
