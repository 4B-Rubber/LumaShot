#pragma once
// Windows.UI.Composition service for glass surfaces (card stack, later panel).
//
// One compositor per UI thread, created on first use and released with its
// last user; no thread, timer or loop of its own (the DispatcherQueue rides on
// the thread's existing message pump).
//
// Measured on 2026-10-17 (build/spike, docs/clipboard-card-stack.md 3.3):
//  - compositor + target: about +0.3 MiB private; 6 glass cards add ~0;
//  - drawing surfaces need a D2D device: hardware D3D11 costs about +41 MiB and
//    cannot be detached, WARP about +2.9 MiB after Trim, so surfaces use WARP.
//    Content is a few small tiles drawn once per history change; DWM still
//    composes them on the GPU.
//  - SetWindowRgn clips composition content, so glass windows that need shadows
//    stay click-through (layered + transparent) and take input elsewhere.
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <memory>
#include <wrl/client.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.h>
#include <windows.ui.composition.desktop.h>

namespace lumashot::ui {

class BackdropCompositor {
public:
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    using Compositor = ABI::Windows::UI::Composition::ICompositor;
    using GraphicsDevice = ABI::Windows::UI::Composition::ICompositionGraphicsDevice;
    using WindowTarget = ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget;

    // Shared per UI thread; nullptr when Windows.UI.Composition is unavailable.
    static std::shared_ptr<BackdropCompositor> Acquire();
    // HostBackdrop glass: Windows 11 (22000+), transparency effects on, no high
    // contrast. Callers use the opaque token fill otherwise.
    static bool GlassAvailable();

    BackdropCompositor(const BackdropCompositor&) = delete;
    BackdropCompositor& operator=(const BackdropCompositor&) = delete;
    ~BackdropCompositor();

    Compositor* Get() const { return compositor_.Get(); }
    // isTopmost=false: visuals sit below any redirected content.
    Ptr<WindowTarget> CreateTarget(HWND window) const;
    // WARP-backed graphics device for drawing surfaces, created on first use.
    GraphicsDevice* Graphics();
    bool HasGraphics() const { return static_cast<bool>(graphics_); }
    IDWriteFactory* Writer();
    // Returns WARP scratch memory after a batch of draws.
    void Trim();

private:
    BackdropCompositor() = default;
    bool Initialize();
    Ptr<ABI::Windows::System::IDispatcherQueueController> queue_;
    Ptr<Compositor> compositor_;
    Ptr<IUnknown> d3d_;
    Ptr<ID2D1Device> d2d_;
    Ptr<GraphicsDevice> graphics_;
    Ptr<IDWriteFactory> writer_;
};

}  // namespace lumashot::ui
