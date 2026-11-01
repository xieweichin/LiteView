#include "core/VectorRenderer.h"

#include <objidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <new>
#include <vector>

// GDI+ headers expect min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#if defined(__has_include)
#if __has_include(<d2d1_3.h>)
#include <d2d1_3.h>
#define LITEVIEW_HAS_SVG 1
#endif
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kMinFull = 1024.0;
constexpr double kMaxFull = 4096.0;

bool HasExtension(const std::wstring& path, const wchar_t* extension) {
    return _wcsicmp(PathFindExtensionW(path.c_str()), extension) == 0;
}

// Picks the rendered size from the picture's own size.
void PlanSize(double ownWidth, double ownHeight, UINT maxSide, UINT& width, UINT& height, UINT& fullWidth,
              UINT& fullHeight) {
    if (!(ownWidth > 0.0) || !(ownHeight > 0.0)) ownWidth = ownHeight = 512.0;
    const double longest = std::max(ownWidth, ownHeight);
    const double full = std::min(kMaxFull, std::max(kMinFull, longest * 2.0));
    const double side = (maxSide && maxSide < full) ? maxSide : full;
    fullWidth = std::max(1u, static_cast<UINT>(std::lround(ownWidth * full / longest)));
    fullHeight = std::max(1u, static_cast<UINT>(std::lround(ownHeight * full / longest)));
    width = std::max(1u, static_cast<UINT>(std::lround(ownWidth * side / longest)));
    height = std::max(1u, static_cast<UINT>(std::lround(ownHeight * side / longest)));
}

HRESULT CreateWicFactory(ComPtr<IWICImagingFactory>& factory) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
}

// ---- EMF / WMF -------------------------------------------------------------

bool StartGdiplus() {
    static std::once_flag once;
    static bool started = false;
    std::call_once(once, [] {
        // Kept running for the life of the process.
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        started = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    });
    return started;
}

HRESULT RenderMetafile(const std::wstring& path, UINT maxSide, ComPtr<IWICBitmapSource>& out, UINT& width,
                       UINT& height, UINT& fullWidth, UINT& fullHeight) {
    if (!StartGdiplus()) return E_FAIL;
    Gdiplus::Metafile metafile(path.c_str());
    if (metafile.GetLastStatus() != Gdiplus::Ok) return WINCODEC_ERR_BADIMAGE;
    const UINT ownWidth = metafile.GetWidth();
    const UINT ownHeight = metafile.GetHeight();
    if (ownWidth == 0 || ownHeight == 0) return WINCODEC_ERR_BADIMAGE;
    PlanSize(ownWidth, ownHeight, maxSide, width, height, fullWidth, fullHeight);

    Gdiplus::Bitmap canvas(static_cast<INT>(width), static_cast<INT>(height), PixelFormat32bppPARGB);
    if (canvas.GetLastStatus() != Gdiplus::Ok) return E_OUTOFMEMORY;
    {
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        if (graphics.DrawImage(&metafile, 0, 0, static_cast<INT>(width), static_cast<INT>(height)) != Gdiplus::Ok)
            return WINCODEC_ERR_BADIMAGE;
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    if (canvas.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) return E_FAIL;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CreateWicFactory(factory);
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(hr) && data.Stride > 0) {
        hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(data.Stride),
                                             static_cast<UINT>(data.Stride) * height, static_cast<BYTE*>(data.Scan0),
                                             bitmap.GetAddressOf());
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }
    canvas.UnlockBits(&data);
    if (FAILED(hr)) return hr;
    out = bitmap;
    return S_OK;
}

// ---- SVG -------------------------------------------------------------------

#ifdef LITEVIEW_HAS_SVG
HRESULT CreateTarget(ID2D1Factory* d2d, IWICBitmap* bitmap, ComPtr<ID2D1DeviceContext5>& context) {
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> target;
    HRESULT hr = d2d->CreateWicBitmapRenderTarget(bitmap, props, target.GetAddressOf());
    if (FAILED(hr)) return hr;
    return target.As(&context);  // fails before Windows 10 1703 (no SVG support)
}

HRESULT OpenStream(const std::wstring& path, ComPtr<IStream>& stream) {
    return SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                  nullptr, stream.ReleaseAndGetAddressOf());
}

HRESULT RenderSvg(const std::wstring& path, UINT maxSide, ComPtr<IWICBitmapSource>& out, UINT& width, UINT& height,
                  UINT& fullWidth, UINT& fullHeight) {
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CreateWicFactory(wic);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1Factory> d2d;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(d2d.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // First pass: parse to learn the picture's own size.
    double ownWidth = 0.0, ownHeight = 0.0;
    {
        ComPtr<IWICBitmap> probe;
        hr = wic->CreateBitmap(1, 1, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, probe.GetAddressOf());
        if (FAILED(hr)) return hr;
        ComPtr<ID2D1DeviceContext5> context;
        hr = CreateTarget(d2d.Get(), probe.Get(), context);
        if (FAILED(hr)) return hr;
        ComPtr<IStream> stream;
        hr = OpenStream(path, stream);
        if (FAILED(hr)) return hr;
        ComPtr<ID2D1SvgDocument> document;
        hr = context->CreateSvgDocument(stream.Get(), D2D1::SizeF(1.0f, 1.0f), document.GetAddressOf());
        if (FAILED(hr)) return hr;
        ComPtr<ID2D1SvgElement> root;
        document->GetRoot(root.GetAddressOf());
        if (!root) return WINCODEC_ERR_BADIMAGE;
        D2D1_SVG_LENGTH w{}, h{};
        D2D1_SVG_VIEWBOX box{};
        const bool hasBox = SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box,
                                                                sizeof(box))) &&
                            box.width > 0 && box.height > 0;
        const bool hasW = SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &w, sizeof(w))) &&
                          w.units == D2D1_SVG_LENGTH_UNITS_NUMBER && w.value > 0;
        const bool hasH = SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &h, sizeof(h))) &&
                          h.units == D2D1_SVG_LENGTH_UNITS_NUMBER && h.value > 0;
        if (hasW && hasH) {
            ownWidth = w.value;
            ownHeight = h.value;
        } else if (hasBox) {
            ownWidth = box.width;
            ownHeight = box.height;
            if (hasW) {
                ownWidth = w.value;
                ownHeight = w.value * box.height / box.width;
            } else if (hasH) {
                ownHeight = h.value;
                ownWidth = h.value * box.width / box.height;
            }
        } else {
            ownWidth = 300.0;  // the SVG default size
            ownHeight = 150.0;
        }
    }
    PlanSize(ownWidth, ownHeight, maxSide, width, height, fullWidth, fullHeight);

    // Second pass: draw at the chosen size.
    ComPtr<IWICBitmap> bitmap;
    hr = wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1DeviceContext5> context;
    hr = CreateTarget(d2d.Get(), bitmap.Get(), context);
    if (FAILED(hr)) return hr;
    ComPtr<IStream> stream;
    hr = OpenStream(path, stream);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1SvgDocument> document;
    hr = context->CreateSvgDocument(stream.Get(), D2D1::SizeF(static_cast<float>(width), static_cast<float>(height)),
                                    document.GetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1SvgElement> root;
    document->GetRoot(root.GetAddressOf());
    if (!root) return WINCODEC_ERR_BADIMAGE;
    // Scale to the viewport: give it a viewBox if it has none, and let the
    // root fill the viewport instead of its own width/height.
    D2D1_SVG_VIEWBOX box{};
    if (FAILED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box, sizeof(box))) ||
        !(box.width > 0) || !(box.height > 0)) {
        box = D2D1_SVG_VIEWBOX{0.0f, 0.0f, static_cast<float>(ownWidth), static_cast<float>(ownHeight)};
        root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box, sizeof(box));
    }
    const D2D1_SVG_LENGTH fill{100.0f, D2D1_SVG_LENGTH_UNITS_PERCENTAGE};
    root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &fill, sizeof(fill));
    root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &fill, sizeof(fill));
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0.0f));
    context->DrawSvgDocument(document.Get());
    hr = context->EndDraw();
    if (FAILED(hr)) return hr;
    out = bitmap;
    return S_OK;
}
#else
HRESULT RenderSvg(const std::wstring&, UINT, ComPtr<IWICBitmapSource>&, UINT&, UINT&, UINT&, UINT&) {
    return E_NOTIMPL;  // built without the Windows 10 1703 SDK headers
}
#endif

}  // namespace

bool IsVectorPath(const std::wstring& path) noexcept {
    return HasExtension(path, L".svg") || HasExtension(path, L".emf") || HasExtension(path, L".wmf");
}

HRESULT RenderVector(const std::wstring& path, UINT maxSide, ComPtr<IWICBitmapSource>& bitmap, UINT& width,
                     UINT& height, UINT& fullWidth, UINT& fullHeight) noexcept {
    bitmap.Reset();
    width = height = fullWidth = fullHeight = 0;
    try {
        if (HasExtension(path, L".svg")) return RenderSvg(path, maxSide, bitmap, width, height, fullWidth, fullHeight);
        return RenderMetafile(path, maxSide, bitmap, width, height, fullWidth, fullHeight);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
