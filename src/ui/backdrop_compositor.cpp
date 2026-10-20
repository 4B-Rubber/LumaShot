#include "ui/backdrop_compositor.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <cstring>
#include <roapi.h>
#include <windows.ui.composition.interop.h>
#include <wrl/wrappers/corewrappers.h>
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace lumashot::ui {
namespace {
std::weak_ptr<BackdropCompositor>& Cached() {
    static thread_local std::weak_ptr<BackdropCompositor> cached;
    return cached;
}
DWORD WindowsBuild() {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{sizeof(info)};
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!address) return 0;
    RtlGetVersion get{};
    static_assert(sizeof(get) == sizeof(address));
    memcpy(&get, &address, sizeof(get));
    return get(&info) == 0 ? info.dwBuildNumber : 0;
}
}  // namespace

std::shared_ptr<BackdropCompositor> BackdropCompositor::Acquire() {
    auto& cached = Cached();
    if (auto existing = cached.lock()) return existing;
    std::shared_ptr<BackdropCompositor> created(new BackdropCompositor());
    if (!created->Initialize()) return {};
    cached = created;
    return created;
}

bool BackdropCompositor::GlassAvailable() {
    if (WindowsBuild() < 22000) return false;
    DWORD transparency = 1, size = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                               (contrast.dwFlags & HCF_HIGHCONTRASTON);
    return transparency != 0 && !high_contrast;
}

bool BackdropCompositor::Initialize() {
    // The app thread already runs COM as STA; DQTAT_COM_NONE leaves it alone.
    DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    if (FAILED(CreateDispatcherQueueController(
            options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(queue_.GetAddressOf()))))
        return false;
    Ptr<IInspectable> instance;
    if (FAILED(RoActivateInstance(
            Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Composition_Compositor).Get(), &instance)))
        return false;
    return SUCCEEDED(instance.As(&compositor_));
}

BackdropCompositor::~BackdropCompositor() {
    graphics_.Reset();
    d2d_.Reset();
    d3d_.Reset();
    compositor_.Reset();
    // Shutting the queue down drains nothing of ours; release is enough.
    queue_.Reset();
}

BackdropCompositor::Ptr<BackdropCompositor::WindowTarget> BackdropCompositor::CreateTarget(HWND window) const {
    Ptr<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop> interop;
    Ptr<WindowTarget> target;
    if (FAILED(compositor_.As(&interop)) || FAILED(interop->CreateDesktopWindowTarget(window, FALSE, &target))) return {};
    return target;
}

BackdropCompositor::GraphicsDevice* BackdropCompositor::Graphics() {
    if (graphics_) return graphics_.Get();
    Ptr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        return nullptr;
    Ptr<ID2D1Factory1> factory;
    Ptr<IDXGIDevice> dxgi;
    Ptr<ID2D1Device> d2d;
    Ptr<ABI::Windows::UI::Composition::ICompositorInterop> interop;
    Ptr<GraphicsDevice> graphics;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(factory.GetAddressOf()))) ||
        FAILED(device.As(&dxgi)) || FAILED(factory->CreateDevice(dxgi.Get(), &d2d)) || FAILED(compositor_.As(&interop)) ||
        FAILED(interop->CreateGraphicsDevice(d2d.Get(), &graphics)))
        return nullptr;
    d3d_ = device;
    d2d_ = d2d;
    graphics_ = graphics;
    return graphics_.Get();
}

IDWriteFactory* BackdropCompositor::Writer() {
    if (!writer_)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(writer_.GetAddressOf()));
    return writer_.Get();
}

void BackdropCompositor::Trim() {
    // D2D caches (glyph runs, gradients, scratch targets) first, then the WARP
    // device's own pools. Call after the draw batch was committed.
    if (d2d_) d2d_->ClearResources(0);
    Ptr<ID3D11Device> device;
    Ptr<IDXGIDevice3> dxgi;
    if (!d3d_ || FAILED(d3d_.As(&device)) || FAILED(d3d_.As(&dxgi))) return;
    Ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->ClearState();
    context->Flush();
    dxgi->Trim();
}

}  // namespace lumashot::ui
