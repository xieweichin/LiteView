#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

// PDF pages are drawn by Windows itself (Windows.Data.Pdf, Windows 10+), so
// LiteView ships no PDF engine. The rest of the program sees a page as a
// picture whose path is "<file>|<page>" ('|' cannot appear in a file name).

// "C:\a.pdf" + 2 -> "C:\a.pdf|2". Page 0 returns the file path unchanged.
std::wstring PdfPagePath(const std::wstring& file, UINT page);

// Splits a path made by PdfPagePath. Returns false (and file = path,
// page = 0) for a plain path.
bool SplitPdfPagePath(const std::wstring& path, std::wstring& file, UINT& page);

bool IsPdfPath(const std::wstring& path) noexcept;

// Renders one page to a cached 32bppPBGRA bitmap. maxSide = 0 renders at
// twice the page's size in DIPs (about 192 dpi), capped at 4096 px on the
// long side; otherwise the long side is min(maxSide, that size).
// COM must be initialized (MTA) on the calling thread.
HRESULT RenderPdfPage(const std::wstring& file, UINT page, UINT maxSide,
                      Microsoft::WRL::ComPtr<IWICBitmapSource>& bitmap, UINT& width, UINT& height,
                      UINT& fullWidth, UINT& fullHeight, UINT& pageCount) noexcept;

// Which step of the last RenderPdfPage call on this thread failed (0 = none);
// for diagnostics in tests.
int LastPdfRenderStep() noexcept;
