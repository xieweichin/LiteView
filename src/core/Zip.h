#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Just enough ZIP for 3MF packages: stored and deflated entries, no
// encryption, no ZIP64. Plain C++ so it can be tested anywhere.

// Decodes a raw DEFLATE stream (RFC 1951). Fails on corrupt data or when the
// output would grow past maxOutput bytes.
bool Inflate(const std::uint8_t* data, std::size_t size, std::size_t maxOutput, std::vector<std::uint8_t>& out);

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;          // 0 stored, 8 deflated
    std::uint32_t compressedSize = 0;
    std::uint32_t size = 0;
    std::uint32_t localOffset = 0;
};

// Reads the central directory of a ZIP archive held in memory.
bool ListZip(const std::vector<std::uint8_t>& zip, std::vector<ZipEntry>& entries);

// Extracts one entry; maxOutput caps the uncompressed size.
bool ReadZipEntry(const std::vector<std::uint8_t>& zip, const ZipEntry& entry, std::size_t maxOutput,
                  std::vector<std::uint8_t>& out);
