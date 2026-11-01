#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

enum class TextEncoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi };

// A plain-text file read for display: CRLF line ends (what an EDIT control
// expects), NUL characters replaced, at most the requested number of bytes.
struct TextDocument {
    std::wstring text;
    TextEncoding encoding = TextEncoding::Utf8;
    bool truncated = false;      // the file was longer than maxBytes
    std::uint64_t fileSize = 0;
};

// Text-like extensions LiteView previews (".txt", ".md", source code...).
bool IsTextExtension(const wchar_t* extension) noexcept;

// Reads up to maxBytes from the start of the file. Encoding: BOM if present,
// else UTF-16 by NUL pattern, else UTF-8 if valid, else the ANSI code page.
HRESULT LoadTextFile(const std::wstring& path, std::size_t maxBytes, TextDocument& out) noexcept;

// The decoding step of LoadTextFile, for bytes already in memory. `cut` says
// the bytes stop mid-file, so a split character at the end is dropped.
HRESULT DecodeText(const char* bytes, std::size_t size, bool cut, TextDocument& out) noexcept;

// Short name for the status line ("UTF-8", "UTF-16 LE", "ANSI").
const wchar_t* TextEncodingName(TextEncoding encoding) noexcept;
