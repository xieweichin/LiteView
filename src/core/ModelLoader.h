#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 3D models as one triangle soup: STL (binary and ASCII), OBJ, glTF 2.0
// (.gltf with embedded or side-by-side buffers, and .glb) and 3MF. Only
// geometry and a flat colour per triangle are read; textures, Draco and other
// extensions are ignored. The parsers are plain C++; LoadModel reads files.
struct Mesh {
    std::vector<float> positions;       // x, y, z per vertex
    std::vector<std::uint32_t> indices;  // three per triangle
    std::vector<std::uint32_t> colors;   // 0xRRGGBB per triangle (or kModelNoColor), or empty
    bool zUp = false;                    // STL and 3MF are Z-up, glTF and OBJ Y-up

    std::size_t TriangleCount() const { return indices.size() / 3; }
};

enum class ModelFormat { None, Stl, Obj, Gltf, Glb, ThreeMf };

constexpr std::size_t kMaxModelTriangles = 10u * 1000 * 1000;
constexpr std::uint32_t kModelNoColor = 0xFF000000u;  // in Mesh::colors: use the default colour

ModelFormat ModelFormatForExtension(const wchar_t* extension) noexcept;  // ".stl" etc., any case

// Reads a file next to a .gltf (a relative buffer URI); false if missing.
using ResourceReader = std::function<bool(const std::string& uri, std::vector<std::uint8_t>& data)>;

// Each returns false for data that is not a usable model of that format.
bool ParseStl(const std::uint8_t* data, std::size_t size, Mesh& mesh);
bool ParseObj(const std::uint8_t* data, std::size_t size, Mesh& mesh);
bool ParseGltf(const std::uint8_t* data, std::size_t size, const ResourceReader& resources, Mesh& mesh);
bool ParseGlb(const std::uint8_t* data, std::size_t size, const ResourceReader& resources, Mesh& mesh);
bool Parse3mf(const std::vector<std::uint8_t>& zip, Mesh& mesh);
bool ParseModel(ModelFormat format, const std::vector<std::uint8_t>& data, const ResourceReader& resources,
                Mesh& mesh);

#ifdef _WIN32
#include <windows.h>
bool IsModelPath(const std::wstring& path) noexcept;
// S_OK, or E_FAIL / WINCODEC_ERR_BADIMAGE-style errors for unreadable files.
HRESULT LoadModel(const std::wstring& path, Mesh& mesh) noexcept;
#endif
