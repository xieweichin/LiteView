#pragma once

#include <windows.h>
#include <wincodec.h>

#include <optional>
#include <string>

// Dimensions are the raw frame dimensions, before applying orientation.
// An absent EXIF field is represented by an empty string or empty optional.
struct PhotoMetadata {
    UINT width = 0;
    UINT height = 0;
    std::optional<UINT> orientation;
    std::wstring dateTaken;
    std::wstring cameraMake;
    std::wstring cameraModel;
    std::optional<double> exposureSeconds;
    std::optional<double> fNumber;
    std::optional<UINT> iso;
    std::optional<double> focalLengthMm;
};

// The calling thread must initialize COM and keep it initialized while using
// WIC objects. Each Read call creates its own WIC factory and decoder.
class Metadata {
public:
    HRESULT Read(const std::wstring& path, PhotoMetadata& out) const noexcept;
    HRESULT ReadFrame(IWICBitmapFrameDecode* frame, PhotoMetadata& out) const noexcept;
};
