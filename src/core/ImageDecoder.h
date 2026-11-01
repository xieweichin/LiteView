#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

struct DecodedImage {
    Microsoft::WRL::ComPtr<IWICBitmapSource> bitmap;  // cached, oriented, 32bppPBGRA
    UINT width = 0;
    UINT height = 0;
    UINT originalWidth = 0;   // dimensions after applying EXIF orientation
    UINT originalHeight = 0;
};

// Callers must initialize COM on their calling thread, and obey COM apartment
// rules when passing the returned WIC bitmap to another thread. Each call
// creates its own factory and returns a fully cached bitmap.
class ImageDecoder {
public:
    // maxSide = 0 keeps original size; otherwise downscale without enlarging.
    HRESULT Decode(const std::wstring& path, UINT maxSide, DecodedImage& out) const noexcept;
};
