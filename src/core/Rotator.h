#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Rotates picture files by 90 degrees and saves them.
//
// JPEG: only the EXIF orientation tag changes; the compressed image data is
// left byte-for-byte intact. If the file has no EXIF block, a minimal one is
// inserted. PNG, BMP and TIFF are decoded, rotated and re-encoded.
// The result is written to a temporary file next to the original and swapped
// in with ReplaceFileW; the original modified time is kept.
// Callers must initialize COM on the calling thread.
class Rotator {
public:
    // clockwise = Ctrl+], counter-clockwise = Ctrl+[.
    static HRESULT RotateFile(const std::wstring& path, bool clockwise) noexcept;

    // EXIF orientation after one quarter turn (1..8 in, 1..8 out).
    static int NextOrientation(int orientation, bool clockwise) noexcept;

    // Reads the EXIF orientation from JPEG bytes: 1..8, or 0 when the file
    // has EXIF but no orientation tag, or -1 when it has no EXIF block.
    static int ReadJpegOrientation(const std::vector<std::uint8_t>& jpeg) noexcept;

    // Sets the orientation in JPEG bytes without touching image data.
    // Returns S_FALSE if the EXIF block exists but lacks the tag (the bytes
    // are left unchanged; the caller needs a metadata rewrite).
    static HRESULT SetJpegOrientation(std::vector<std::uint8_t>& jpeg, int orientation) noexcept;
};
