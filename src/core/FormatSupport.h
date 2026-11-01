#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Image formats that Windows decodes only after a free (or paid) codec
// extension is installed from the Microsoft Store. LiteView itself ships no
// decoders: every format it lists comes from the WIC decoders on the system.
enum class CodecPack : std::uint32_t { Heif, WebP, Avif, Raw, JpegXl, Count };

constexpr std::uint32_t CodecPackBit(CodecPack pack) noexcept {
    return 1u << static_cast<std::uint32_t>(pack);
}

// Extensions of every installed WIC decoder, with the leading dot (".jpg").
// COM must be initialized on the calling thread.
HRESULT DecoderExtensions(std::vector<std::wstring>& extensions) noexcept;

// True when extension (".JPG", case-insensitive) is in the list.
bool HasExtension(const std::vector<std::wstring>& extensions, const wchar_t* extension) noexcept;

// The codec pack that adds a decoder for this extension, if it is one of the
// formats LiteView knows needs an extension.
bool CodecPackForExtension(const wchar_t* extension, CodecPack& pack) noexcept;

// Packs none of whose extensions has a decoder in the list, as CodecPackBit flags.
std::uint32_t MissingCodecPacks(const std::vector<std::wstring>& extensions) noexcept;

// The first pack in a CodecPackBit mask, or CodecPack::Count when it is empty.
CodecPack FirstCodecPack(std::uint32_t mask) noexcept;

// Product name shown to the user ("HEIF", "WebP", ...).
const wchar_t* CodecPackName(CodecPack pack) noexcept;

// ms-windows-store: link to the extension that adds the decoder.
const wchar_t* CodecPackStoreUri(CodecPack pack) noexcept;
