#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

// Vector pictures drawn by Windows: SVG with Direct2D (Windows 10 1703+),
// EMF / WMF with GDI+. Nothing is bundled.
bool IsVectorPath(const std::wstring& path) noexcept;

// Draws the picture into a cached 32bppPBGRA bitmap (transparent where the
// picture is). "Full size" is twice the picture's own size, at least 1024 and
// at most 4096 px on the long side; maxSide > 0 fits within that without
// enlarging. COM must be initialized on the calling thread.
HRESULT RenderVector(const std::wstring& path, UINT maxSide, Microsoft::WRL::ComPtr<IWICBitmapSource>& bitmap,
                     UINT& width, UINT& height, UINT& fullWidth, UINT& fullHeight) noexcept;
