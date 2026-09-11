// Draws the overlay offscreen at 1920x1080 over a stand-in game scene and saves
// each state as a BMP, so the menu can be looked at without starting the game.
// Every scene comes out in English and in Russian (en_*.bmp, ru_*.bmp); these
// are the README screenshots. Nothing real is shown: the host page keeps its
// addresses hidden and the players are made up.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <Windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"

#define private public
#include "../../include/ui.h"
#undef private
#include "../../include/ui_kit.h"
#include "../../include/ui_settings.h"

void PreviewSetSession(int mode);

using namespace DS2Coop::UI;

namespace {

constexpr int kW = 1920, kH = 1080;
constexpr const char* kLanguages[] = { "en", "ru" };
ID3D11Device*           g_dev = nullptr;
ID3D11DeviceContext*    g_ctx = nullptr;
ID3D11Texture2D*        g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void DrawFakeGame() {
    ImDrawList* L = ImGui::GetBackgroundDrawList();
    L->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(kW, kH * 0.62f), IM_COL32(40, 46, 58, 255),
                               IM_COL32(40, 46, 58, 255), IM_COL32(118, 100, 80, 255), IM_COL32(118, 100, 80, 255));
    L->AddRectFilledMultiColor(ImVec2(0, kH * 0.62f), ImVec2(kW, kH), IM_COL32(64, 56, 44, 255),
                               IM_COL32(64, 56, 44, 255), IM_COL32(24, 21, 18, 255), IM_COL32(24, 21, 18, 255));
    for (int i = 0; i < 6; i++)
        L->AddCircleFilled(ImVec2(kW * 0.28f, kH * 0.72f), 260.0f - i * 40.0f, IM_COL32(255, 150, 60, 14));
    L->AddRectFilled(ImVec2(1300, 380), ImVec2(1420, 820), IM_COL32(30, 28, 26, 255));   // a pillar
    // The game's HUD, to see what the overlay sits next to.
    L->AddRectFilled(ImVec2(70, 52), ImVec2(520, 64), IM_COL32(160, 36, 36, 255));
    L->AddRectFilled(ImVec2(70, 72), ImVec2(400, 80), IM_COL32(52, 140, 60, 255));
    L->AddText(ImVec2(1720, 1010), IM_COL32(230, 220, 200, 255), "Souls 12345");
}

void Frame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    DrawFakeGame();
    Overlay::GetInstance().Render();
    ImGui::Render();
    const float Clear[4] = { 0, 0, 0, 1 };
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, Clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Run(int frames) { for (int i = 0; i < frames; i++) Frame(); }

void Save(const std::string& name) {
    D3D11_TEXTURE2D_DESC Desc{};
    g_tex->GetDesc(&Desc);
    Desc.Usage = D3D11_USAGE_STAGING;
    Desc.BindFlags = 0;
    Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* Staging = nullptr;
    g_dev->CreateTexture2D(&Desc, nullptr, &Staging);
    g_ctx->CopyResource(Staging, g_tex);
    D3D11_MAPPED_SUBRESOURCE Map{};
    g_ctx->Map(Staging, 0, D3D11_MAP_READ, 0, &Map);

    const std::string Path = name + ".bmp";
    FILE* F = std::fopen(Path.c_str(), "wb");
    BITMAPFILEHEADER Fh{};
    BITMAPINFOHEADER Ih{};
    Ih.biSize = sizeof(Ih); Ih.biWidth = kW; Ih.biHeight = -kH; Ih.biPlanes = 1;
    Ih.biBitCount = 32; Ih.biCompression = BI_RGB; Ih.biSizeImage = kW * kH * 4;
    Fh.bfType = 0x4D42; Fh.bfOffBits = sizeof(Fh) + sizeof(Ih); Fh.bfSize = Fh.bfOffBits + Ih.biSizeImage;
    std::fwrite(&Fh, sizeof(Fh), 1, F);
    std::fwrite(&Ih, sizeof(Ih), 1, F);
    std::vector<uint8_t> Row(kW * 4);
    for (int y = 0; y < kH; y++) {
        const uint8_t* Src = static_cast<const uint8_t*>(Map.pData) + y * Map.RowPitch;
        for (int x = 0; x < kW; x++) {
            Row[x * 4 + 0] = Src[x * 4 + 2];
            Row[x * 4 + 1] = Src[x * 4 + 1];
            Row[x * 4 + 2] = Src[x * 4 + 0];
            Row[x * 4 + 3] = 255;
        }
        std::fwrite(Row.data(), 1, Row.size(), F);
    }
    std::fclose(F);
    g_ctx->Unmap(Staging, 0);
    Staging->Release();
    std::printf("saved %s\n", Path.c_str());
}

// The current state, once per language.
void Shoot(const char* scene, int frames) {
    for (const char* Lang : kLanguages) {
        InitUiSettings(Lang, "F1");
        Run(frames);
        Save(std::string(Lang) + "_" + scene);
    }
}

void ClearNotifications(Overlay& o) {
    std::lock_guard<std::mutex> Lock(o.m_notifMutex);
    o.m_notifications.clear();
}

} // namespace

int main() {
    WSADATA Wsa;
    WSAStartup(MAKEWORD(2, 2), &Wsa);

    D3D_FEATURE_LEVEL Level;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &g_dev, &Level, &g_ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &g_dev, &Level, &g_ctx))) {
        std::printf("no D3D11 device\n");
        return 1;
    }
    D3D11_TEXTURE2D_DESC Td{};
    Td.Width = kW; Td.Height = kH; Td.MipLevels = 1; Td.ArraySize = 1;
    Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; Td.SampleDesc.Count = 1;
    Td.Usage = D3D11_USAGE_DEFAULT; Td.BindFlags = D3D11_BIND_RENDER_TARGET;
    g_dev->CreateTexture2D(&Td, nullptr, &g_tex);
    g_dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    Kit::Setup(static_cast<float>(kH));
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Overlay& O = Overlay::GetInstance();

    // The menu, not in a lobby yet.
    InitUiSettings("en", "F1");
    PreviewSetSession(0);
    O.SetVisible(true);
    Run(60);   // let the window fade in
    Shoot("home", 5);

    // Hosting: a password typed, the addresses left hidden.
    O.m_page = Overlay::Page::Host;
    O.m_showAddresses = false;
    std::strcpy(O.m_inputPassword, "ember");
    Shoot("host", 20);

    // Joining: the host's VPN address and the password.
    O.m_page = Overlay::Page::Join;
    std::strcpy(O.m_inputIP, "26.12.34.56");
    std::strcpy(O.m_inputPassword, "ember");
    Shoot("join", 10);

    O.m_tab = 1;
    Shoot("settings", 20);

    // In a lobby: as the host with a guest, and as the guest in the host's world.
    O.m_tab = 0;
    O.m_page = Overlay::Page::Home;
    PreviewSetSession(1);
    Shoot("lobby_host", 20);
    PreviewSetSession(3);
    Shoot("lobby_guest", 20);

    // Menu closed: the hint in the corner and a few notifications.
    PreviewSetSession(0);
    O.SetVisible(false);
    for (const char* Lang : kLanguages) {
        InitUiSettings(Lang, "F1");
        ClearNotifications(O);
        Run(40);
        O.ShowNotification(Format(Tr("Seamless Co-op is ready. Press %s to open the menu.",
                                     "Seamless Co-op готов. Меню — клавиша %s."), "F1"), 6.0f, NotifyKind::Info);
        O.ShowNotification(Format(Tr("%s joined the lobby", "Игрок %s вошёл в лобби"), "Lucatiel"), 6.0f,
                           NotifyKind::Player);
        O.ShowNotification(Tr("Connected. The host will summon you in a moment.",
                              "Подключено. Хост сейчас вас призовёт."), 6.0f, NotifyKind::Success);
        Run(40);
        Save(std::string(Lang) + "_hud");
    }

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
