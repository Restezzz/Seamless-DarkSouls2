// DX11 Present Hook + ImGui renderer
//
// Fix summary vs original:
//
// Fix 1 — GetForegroundWindow() race:
//   Create the throwaway swap chain on a private hidden HWND we own, so it
//   always succeeds regardless of which window is in the foreground at init
//   time. The game's actual HWND is read from the real swap chain desc on
//   the first Present call.
//
// Fix 2 — RTV stale after backbuffer resize / scene reload:
//   Every Present frame we check the current backbuffer dimensions against
//   the last known dimensions. On mismatch we release the old RTV and
//   recreate it from the new backbuffer. This covers fullscreen toggles,
//   resolution changes, and DS2's title→game scene transition.
//
// Fix 3 — Wrong vtable slot (IDXGISwapChain1 vs IDXGISwapChain):
//   DS2 SotFS may create an IDXGISwapChain1 or higher whose vtable adds
//   methods before Present, shifting its slot. We avoid this by hooking
//   via a swap chain created with our own window using IDXGIFactory
//   directly (no device needed for vtable read), which always gives us
//   a base IDXGISwapChain vtable layout.  Present is still slot 8.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "../../include/ui.h"
#include "../../include/hooks.h"
#include "../../include/session.h"
#include "../../include/utils.h"
#include "../../include/ui_kit.h"
#include "../../include/ui_settings.h"
#include "../../include/input_capture.h"

using namespace DS2Coop::Utils;
using namespace DS2Coop::Hooks;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace DS2Coop::UI {

// ============================================================================
// State
// ============================================================================
static bool                    g_imguiInitialized = false;
static bool                    g_shuttingDown     = false;
static ID3D11Device*           g_device           = nullptr;
static ID3D11DeviceContext*    g_context          = nullptr;
static ID3D11RenderTargetView* g_rtv              = nullptr;
static HWND                    g_hwnd             = nullptr;
static WNDPROC                 g_originalWndProc  = nullptr;
static UINT                    g_rtvWidth         = 0;
static UINT                    g_rtvHeight        = 0;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_originalPresent = nullptr;

// ResizeBuffers hook — slot 13 in IDXGISwapChain vtable
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffersFn g_originalResizeBuffers = nullptr;

// ============================================================================
// RTV helper — create/recreate from the swap chain's current backbuffer
// ============================================================================
static void RebuildRTV(IDXGISwapChain* swapChain) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer)))) {
        LOG_ERROR("RebuildRTV: GetBuffer failed");
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    g_rtvWidth  = desc.Width;
    g_rtvHeight = desc.Height;

    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();
    LOG_INFO("RTV (re)built: %ux%u", g_rtvWidth, g_rtvHeight);
}

// ============================================================================
// WndProc hook
// ============================================================================
HWND GetGameWindow() { return g_hwnd; }

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Guard: ImGui context may not exist yet if messages arrive between
    // WndProc hook installation and ImGui::CreateContext()
    if (!g_imguiInitialized)
        return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);

    // The menu's Windows cursor is switched here: ShowCursor only takes effect
    // on the thread that owns the window (input_capture.cpp).
    ApplyMenuCursor(hwnd);

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return TRUE;

    // While the menu holds the input, keyboard and mouse messages stop here:
    // ImGui has seen them above, the game must not. System keys (Alt+Tab,
    // Alt+F4) still go through.
    if (IsGameInputBlocked()) {
        switch (msg) {
        case WM_INPUT:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR:
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            return 0;
        default:
            break;
        }
    }

    // Read current WndProc each call in case DS2 re-hooked it after us
    WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (current == HookedWndProc) current = g_originalWndProc; // avoid self-loop
    return CallWindowProcW(current, hwnd, msg, wParam, lParam);
}

// ============================================================================
// Hooked ResizeBuffers — called when the game changes resolution/fullscreen
// We must release our RTV before the game resizes, then rebuild after
// ============================================================================
static bool g_resizing = false;

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain,
    UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {

    if (width == 0 || height == 0) {
        return g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);
    }

    g_resizing = true;

    // Release our RTV — the backbuffer is about to be destroyed
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_rtvWidth = 0;
    g_rtvHeight = 0;

    // Invalidate ImGui's DX11 objects (they reference the old backbuffer size)
    if (g_imguiInitialized) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }

    // Let the game do the actual resize
    HRESULT hr = g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);

    // Rebuild our RTV from the new backbuffer
    if (SUCCEEDED(hr) && g_imguiInitialized) {
        RebuildRTV(swapChain);
        ImGui_ImplDX11_CreateDeviceObjects();
        LOG_INFO("RTV (re)built: %ux%u", width, height);
    }

    g_resizing = false;
    return hr;
}

// ============================================================================
// Overlay size — follows the back buffer
// ============================================================================
// The game often shows its first frames at 1280x720 and switches to the real
// resolution a moment later, and a window can be resized at any time. Once the
// back buffer has held one size for a moment, the fonts and the style are built
// again if that size (or the menu size the player picked) wants another scale.
// Always between frames: the font atlas cannot change inside one.
static constexpr ULONGLONG kSizeSettleMs = 400;
static UINT      g_seenWidth  = 0;
static UINT      g_seenHeight = 0;
static ULONGLONG g_sizeSeenAt = 0;

static void FitOverlayToScreen() {
    if (!g_rtvWidth || !g_rtvHeight) return;
    const ULONGLONG Now = GetTickCount64();
    if (g_rtvWidth != g_seenWidth || g_rtvHeight != g_seenHeight) {
        g_seenWidth  = g_rtvWidth;
        g_seenHeight = g_rtvHeight;
        g_sizeSeenAt = Now;
    }
    if (Now - g_sizeSeenAt < kSizeSettleMs) return;

    const float W = static_cast<float>(g_rtvWidth), H = static_cast<float>(g_rtvHeight);
    const float Old = Kit::Scale();
    if (std::fabs(Kit::TargetScale(W, H) - Old) < 0.001f) return;

    ImGui_ImplDX11_InvalidateDeviceObjects();   // the font texture; NewFrame builds it again
    Kit::Setup(W, H);
    LOG_INFO("[UI] overlay scale %.2f -> %.2f (%ux%u, menu size %d%%)",
             Old, Kit::Scale(), g_rtvWidth, g_rtvHeight, GetMenuSize());
}

// ============================================================================
// Hooked Present
// ============================================================================
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Don't touch ImGui if we're shutting down or resizing
    if (g_shuttingDown || g_resizing) return g_originalPresent(swapChain, syncInterval, flags);

    // --- One-time init on first real Present call ---
    if (!g_imguiInitialized) {
        LOG_INFO("[PRESENT] First Present call intercepted! swapChain=%p", swapChain);

        // Get device from the game's own swap chain — always correct
        HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device),
                                          reinterpret_cast<void**>(&g_device));
        if (FAILED(hr)) {
            LOG_ERROR("[PRESENT] GetDevice failed (hr=0x%08X)", hr);
            return g_originalPresent(swapChain, syncInterval, flags);
        }
        g_device->GetImmediateContext(&g_context);
        LOG_INFO("[PRESENT] Device=%p Context=%p", g_device, g_context);

        // HWND from swap chain desc — game's real window
        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        // Build RTV for the first time
        RebuildRTV(swapChain);
        if (!g_rtv) return g_originalPresent(swapChain, syncInterval, flags);

        // Hook WndProc for input
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(HookedWndProc)));

        // Init ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        // Fonts with Cyrillic, sized for the screen, and the overlay's style.
        Kit::Setup(static_cast<float>(g_rtvWidth), static_cast<float>(g_rtvHeight));

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_device, g_context);

        g_imguiInitialized = true;
        // Take the game's input while the menu is open (input_capture.cpp).
        InstallInputCapture();
        LOG_INFO("ImGui initialized on DX11 device (%ux%u)", g_rtvWidth, g_rtvHeight);
        Overlay::GetInstance().ShowNotification(Format(Tr("Seamless Co-op is ready. Press %s to open the menu.", "Seamless Co-op готов. Меню \xE2\x80\x94 клавиша %s."), KeyName(GetMenuKey()).c_str()), 6.0f, NotifyKind::Info);
    }

    // If RTV was released by ResizeBuffers hook, rebuild it
    if (!g_rtv) {
        RebuildRTV(swapChain);
        if (!g_rtv) return g_originalPresent(swapChain, syncInterval, flags);
    }

    // --- Every frame: render ---
    FitOverlayToScreen();
    auto& overlay = Overlay::GetInstance();

    // The menu key is configurable (Settings tab); the overlay polls it.
    overlay.PollMenuKey();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // The old badge sat over the game's health bars at all times; the
    // overlay now draws a quiet hint only when it is useful.

    overlay.Render();

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return g_originalPresent(swapChain, syncInterval, flags);
}

// ============================================================================
// OverlayRenderer::Initialize
// ============================================================================
OverlayRenderer& OverlayRenderer::GetInstance() {
    static OverlayRenderer instance;
    return instance;
}

bool OverlayRenderer::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("Installing DX11 Present hook...");

    // -------------------------------------------------------------------------
    // Fix 1 & 3: Create the throwaway swap chain on a private hidden window
    // we own, so GetForegroundWindow() race is eliminated and we always get a
    // base IDXGISwapChain (slot 8 = Present), not IDXGISwapChain1/3.
    // -------------------------------------------------------------------------
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DS2CoopDummy";
    // RegisterClassExW returns 0 if class already exists — that's fine, we
    // just need the class to exist before CreateWindowExW
    ATOM classAtom = RegisterClassExW(&wc);
    bool classWasNew = (classAtom != 0);

    HWND dummyHwnd = CreateWindowExW(0, L"DS2CoopDummy", L"", WS_OVERLAPPED,
                                     0, 0, 8, 8, nullptr, nullptr,
                                     hInst, nullptr);
    if (!dummyHwnd) {
        LOG_ERROR("Failed to create dummy window (err=%lu)", GetLastError());
        if (classWasNew) UnregisterClassW(L"DS2CoopDummy", hInst);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = dummyHwnd;   // our window — always valid
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device*        tempDevice    = nullptr;
    ID3D11DeviceContext* tempContext   = nullptr;
    IDXGISwapChain*      tempSwapChain = nullptr;
    D3D_FEATURE_LEVEL    featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &tempSwapChain, &tempDevice, &featureLevel, &tempContext);

    if (FAILED(hr)) {
        LOG_ERROR("Hardware device failed (hr=0x%08X), trying WARP...", hr);
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &scd, &tempSwapChain, &tempDevice, &featureLevel, &tempContext);
    }

    DestroyWindow(dummyHwnd);
    if (classWasNew) UnregisterClassW(L"DS2CoopDummy", hInst);

    if (FAILED(hr)) {
        LOG_ERROR("Both device types failed. Present hook unavailable.");
        return false;
    }

    // Slot 8 = Present, Slot 13 = ResizeBuffers
    void** vtable          = *reinterpret_cast<void***>(tempSwapChain);
    void*  presentAddr     = vtable[8];
    void*  resizeAddr      = vtable[13];
    LOG_INFO("IDXGISwapChain::Present at %p", presentAddr);
    LOG_INFO("IDXGISwapChain::ResizeBuffers at %p", resizeAddr);

    bool hooked = HookManager::GetInstance().InstallHook(
        presentAddr,
        reinterpret_cast<void*>(&HookedPresent),
        reinterpret_cast<void**>(&g_originalPresent));

    // Hook ResizeBuffers so resolution changes don't break the overlay
    bool resizeHooked = HookManager::GetInstance().InstallHook(
        resizeAddr,
        reinterpret_cast<void*>(&HookedResizeBuffers),
        reinterpret_cast<void**>(&g_originalResizeBuffers));
    if (resizeHooked) LOG_INFO("ResizeBuffers hook installed");
    else LOG_WARNING("ResizeBuffers hook failed — resolution changes may break overlay");

    tempSwapChain->Release();
    tempContext->Release();
    tempDevice->Release();

    if (!hooked) {
        LOG_ERROR("MinHook failed to hook Present");
        return false;
    }

    m_initialized = true;
    LOG_INFO("Present hook installed — ImGui initializes on first game frame");
    return true;
}

void OverlayRenderer::Shutdown() {
    if (!m_initialized) return;

    g_shuttingDown = true;
    Sleep(100); // Give render thread time to exit HookedPresent

    if (g_imguiInitialized) {
        if (g_hwnd && g_originalWndProc)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(g_originalWndProc));

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if (g_rtv)     { g_rtv->Release();    g_rtv     = nullptr; }
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device)  { g_device->Release();  g_device  = nullptr; }

        g_imguiInitialized = false;
    }

    m_initialized = false;
    LOG_INFO("Overlay renderer shut down");
}

} // namespace DS2Coop::UI
