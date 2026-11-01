#include "core/ImageDecoder.h"

#include "core/Metadata.h"
#include "core/PdfRenderer.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

UINT Scaled(UINT dimension, UINT longest, UINT limit) {
    return std::max(1u, static_cast<UINT>((static_cast<std::uint64_t>(dimension) * limit +
                                           longest / 2) / longest));
}

HRESULT ApplyTransform(IWICImagingFactory* factory, WICBitmapTransformOptions option,
                       ComPtr<IWICBitmapSource>& source) {
    ComPtr<IWICBitmapFlipRotator> transform;
    HRESULT hr = factory->CreateBitmapFlipRotator(transform.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = transform->Initialize(source.Get(), option);
    if (SUCCEEDED(hr)) source = transform;
    return hr;
}

HRESULT Orient(IWICImagingFactory* factory, UINT orientation, ComPtr<IWICBitmapSource>& source) {
    HRESULT hr = S_OK;
    // Split the mirrored diagonal cases into two transforms so their order is
    // explicit: first flip horizontally, then rotate.
    if (orientation == 2 || orientation == 5 || orientation == 7)
        hr = ApplyTransform(factory, WICBitmapTransformFlipHorizontal, source);
    if (FAILED(hr)) return hr;
    switch (orientation) {
    case 3: return ApplyTransform(factory, WICBitmapTransformRotate180, source);
    case 4: return ApplyTransform(factory, WICBitmapTransformFlipVertical, source);
    case 5: return ApplyTransform(factory, WICBitmapTransformRotate270, source);
    case 6: return ApplyTransform(factory, WICBitmapTransformRotate90, source);
    case 7: return ApplyTransform(factory, WICBitmapTransformRotate90, source);
    case 8: return ApplyTransform(factory, WICBitmapTransformRotate270, source);
    default: return S_OK;
    }
}

}  // namespace

HRESULT ImageDecoder::Decode(const std::wstring& path, UINT maxSide, DecodedImage& out) const noexcept {
    out = {};
    if (path.empty()) return E_INVALIDARG;
    try {
        if (IsPdfPath(path)) {
            std::wstring file;
            UINT page = 0;
            SplitPdfPagePath(path, file, page);
            DecodedImage result;
            UINT fullWidth = 0, fullHeight = 0;
            const HRESULT hr = RenderPdfPage(file, page, maxSide, result.bitmap, result.width, result.height,
                                             fullWidth, fullHeight, result.pageCount);
            if (FAILED(hr)) return hr;
            result.originalWidth = fullWidth;
            result.originalHeight = fullHeight;
            out = std::move(result);
            return S_OK;
        }
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) return hr;
        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
        if (FAILED(hr)) return hr;
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(hr)) return hr;
        PhotoMetadata metadata;
        hr = Metadata{}.ReadFrame(frame.Get(), metadata);
        if (FAILED(hr)) return hr;
        if (!metadata.width || !metadata.height) return WINCODEC_ERR_BADIMAGE;
        const UINT orientation = metadata.orientation.value_or(1);
        const bool swap = orientation >= 5 && orientation <= 8;
        DecodedImage result;
        result.originalWidth = swap ? metadata.height : metadata.width;
        result.originalHeight = swap ? metadata.width : metadata.height;

        UINT targetWidth = metadata.width, targetHeight = metadata.height;
        const UINT longest = std::max(targetWidth, targetHeight);
        const bool downscale = maxSide && longest > maxSide;
        if (downscale) {
            targetWidth = Scaled(metadata.width, longest, maxSide);
            targetHeight = Scaled(metadata.height, longest, maxSide);
        }
        ComPtr<IWICBitmapSource> source = frame;
        if (downscale) {
            // Microsoft's built-in WIC scaler queries IWICBitmapSourceTransform
            // when it immediately follows a frame decoder, enabling native
            // downsampling when supported and using Fant for the final size.
            // Avoid embedded JPEG thumbnails, which may have another orientation.
            ComPtr<IWICBitmapScaler> scaler;
            hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
            if (FAILED(hr)) return hr;
            hr = scaler->Initialize(source.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);
            if (FAILED(hr)) return hr;
            source = scaler;
        }
        // Convert and buffer before orienting: IWICBitmapFlipRotator pulls
        // pixels one at a time, which is quadratic on an unbuffered decoder.
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return hr;
        ComPtr<IWICBitmap> cached;
        hr = factory->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, cached.GetAddressOf());
        if (FAILED(hr)) return hr;
        if (orientation >= 2 && orientation <= 8) {
            ComPtr<IWICBitmapSource> oriented = cached;
            hr = Orient(factory.Get(), orientation, oriented);
            if (FAILED(hr)) return hr;
            ComPtr<IWICBitmap> orientedCache;
            hr = factory->CreateBitmapFromSource(oriented.Get(), WICBitmapCacheOnLoad,
                                                 orientedCache.GetAddressOf());
            if (FAILED(hr)) return hr;
            cached = orientedCache;
        }
        hr = cached->GetSize(&result.width, &result.height);
        if (FAILED(hr)) return hr;
        result.bitmap = cached;
        out = std::move(result);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
