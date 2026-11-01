#include "doctest.h"

#include "core/ImageDecoder.h"
#include "core/ModelLoader.h"
#include "core/ModelRenderer.h"
#include "core/Zip.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring Fixture(const std::wstring& name) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    REQUIRE(length > 0);
    std::wstring executable(path.data(), length);
    const auto slash = executable.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    return executable.substr(0, slash) + L"\\fixtures\\" + name;
}

std::vector<std::uint8_t> Bytes(const std::string& text) { return {text.begin(), text.end()}; }

void Put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void PutFloat(std::string& out, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Put32(out, bits);
}

const float kCube[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
const int kFaces[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                           {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};

std::string BinaryStl() {
    std::string out = "solid but actually binary";
    out.resize(80, ' ');
    Put32(out, 12);
    for (const auto& face : kFaces) {
        for (int i = 0; i < 3; ++i) PutFloat(out, 0);
        for (int v : face)
            for (float c : kCube[v]) PutFloat(out, c);
        out += std::string(2, '\0');
    }
    return out;
}

// A ZIP with stored (uncompressed) entries.
std::string StoredZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, directory;
    for (const auto& [name, data] : files) {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        out += "PK\x03\x04";
        out += std::string("\x14\0\0\0\0\0\0\0\0\0", 10);
        Put32(out, 0);  // crc (not checked)
        Put32(out, static_cast<std::uint32_t>(data.size()));
        Put32(out, static_cast<std::uint32_t>(data.size()));
        out += static_cast<char>(name.size());
        out += std::string(3, '\0');
        out += name + data;
        directory += "PK\x01\x02";
        directory += std::string("\x14\0\x14\0\0\0\0\0\0\0\0\0", 12);
        Put32(directory, 0);
        Put32(directory, static_cast<std::uint32_t>(data.size()));
        Put32(directory, static_cast<std::uint32_t>(data.size()));
        directory += static_cast<char>(name.size());
        directory += std::string(13, '\0');
        Put32(directory, offset);
        directory += name;
    }
    const std::uint32_t start = static_cast<std::uint32_t>(out.size());
    out += directory;
    out += "PK\x05\x06";
    out += std::string(4, '\0');
    out += static_cast<char>(files.size());
    out += '\0';
    out += static_cast<char>(files.size());
    out += '\0';
    Put32(out, static_cast<std::uint32_t>(directory.size()));
    Put32(out, start);
    out += std::string(2, '\0');
    return out;
}

std::array<BYTE, 4> PixelAt(IWICBitmapSource* bitmap, UINT x, UINT y) {
    WICRect rect{static_cast<INT>(x), static_cast<INT>(y), 1, 1};
    std::array<BYTE, 4> pixel{};
    CHECK(bitmap->CopyPixels(&rect, 4, 4, pixel.data()) == S_OK);
    return pixel;
}

}  // namespace

TEST_CASE("model extensions") {
    CHECK(ModelFormatForExtension(L".STL") == ModelFormat::Stl);
    CHECK(ModelFormatForExtension(L".obj") == ModelFormat::Obj);
    CHECK(ModelFormatForExtension(L".gltf") == ModelFormat::Gltf);
    CHECK(ModelFormatForExtension(L".glb") == ModelFormat::Glb);
    CHECK(ModelFormatForExtension(L".3mf") == ModelFormat::ThreeMf);
    CHECK(ModelFormatForExtension(L".png") == ModelFormat::None);
    CHECK(ModelFormatForExtension(L".stlx") == ModelFormat::None);
    CHECK(IsModelPath(L"C:\\a\\part.3MF"));
    CHECK_FALSE(IsModelPath(L"C:\\a\\notes.txt"));
}

TEST_CASE("inflate decodes stored and fixed-Huffman blocks") {
    const std::uint8_t fixed[] = {0x4b, 0x4c, 0x4a, 0x4e, 0x84, 0x21, 0x05, 0x9f, 0xcc, 0x92,
                                  0xd4, 0xb0, 0xcc, 0xd4, 0x72, 0x4c, 0x86, 0x22, 0x00};
    std::vector<std::uint8_t> out;
    REQUIRE(Inflate(fixed, sizeof(fixed), 1024, out));
    CHECK(std::string(out.begin(), out.end()) == "abcabcabcabc LiteView LiteView LiteView!");
    CHECK_FALSE(Inflate(fixed, sizeof(fixed), 10, out));  // output cap
    CHECK_FALSE(Inflate(fixed, 5, 1024, out));            // truncated

    const std::uint8_t stored[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
    REQUIRE(Inflate(stored, sizeof(stored), 1024, out));
    CHECK(std::string(out.begin(), out.end()) == "abc");
}

TEST_CASE("binary and ASCII STL") {
    const std::string binary = BinaryStl();
    Mesh mesh;
    REQUIRE(ParseStl(reinterpret_cast<const std::uint8_t*>(binary.data()), binary.size(), mesh));
    CHECK(mesh.TriangleCount() == 12);
    CHECK(mesh.zUp);

    std::string text = "solid cube\n";
    for (const auto& face : kFaces) {
        text += "facet normal 0 0 0\nouter loop\n";
        for (int v : face)
            text += "vertex " + std::to_string(kCube[v][0]) + " " + std::to_string(kCube[v][1]) + " " +
                    std::to_string(kCube[v][2]) + "\n";
        text += "endloop\nendfacet\n";
    }
    text += "endsolid cube\n";
    REQUIRE(ParseStl(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), mesh));
    CHECK(mesh.TriangleCount() == 12);
    CHECK(mesh.positions[3] == doctest::Approx(1.0f));

    const std::string junk = "not a model";
    CHECK_FALSE(ParseStl(reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size(), mesh));
}

TEST_CASE("OBJ polygons, slashes and negative indices") {
    const std::string obj =
        "# square and triangle\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
        "f 1/1/1 2/1/1 3/1/1 4/1/1\n"
        "f -1 -2 -3\n";
    Mesh mesh;
    REQUIRE(ParseObj(reinterpret_cast<const std::uint8_t*>(obj.data()), obj.size(), mesh));
    CHECK(mesh.TriangleCount() == 3);
    CHECK(mesh.indices[0] == 0);
    CHECK(mesh.indices[6] == 3);  // -1 is the last vertex
    CHECK_FALSE(mesh.zUp);
}

TEST_CASE("glTF with an embedded buffer, node transform and material") {
    // Positions of one triangle (3 x VEC3 float) followed by three u16 indices.
    std::string buffer;
    const float points[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (float f : points) PutFloat(buffer, f);
    buffer += std::string("\0\0\1\0\2\0", 6);
    static const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (std::size_t i = 0; i < buffer.size(); i += 3) {
        std::uint32_t n = static_cast<std::uint8_t>(buffer[i]) << 16;
        if (i + 1 < buffer.size()) n |= static_cast<std::uint8_t>(buffer[i + 1]) << 8;
        if (i + 2 < buffer.size()) n |= static_cast<std::uint8_t>(buffer[i + 2]);
        encoded += kBase64[(n >> 18) & 63];
        encoded += kBase64[(n >> 12) & 63];
        encoded += i + 1 < buffer.size() ? kBase64[(n >> 6) & 63] : '=';
        encoded += i + 2 < buffer.size() ? kBase64[n & 63] : '=';
    }
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"children":[1],"translation":[10,0,0]},{"mesh":0,"scale":[2,2,2]}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
        "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],
        "buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,)" + encoded + R"("}],
        "bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                     {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    Mesh mesh;
    REQUIRE(ParseGltf(reinterpret_cast<const std::uint8_t*>(json.data()), json.size(), nullptr, mesh));
    REQUIRE(mesh.TriangleCount() == 1);
    CHECK(mesh.positions[3] == doctest::Approx(12.0f));  // 1 * 2 + 10
    CHECK(mesh.positions[7] == doctest::Approx(2.0f));
    REQUIRE(mesh.colors.size() == 1);
    CHECK(mesh.colors[0] == 0xFF0000u);

    // The same document as GLB, with the buffer in the binary chunk.
    std::string glbJson = json;
    const auto uri = glbJson.find(",\"uri\"");
    glbJson.erase(uri, glbJson.find('}', uri) - uri);
    while (glbJson.size() % 4) glbJson += ' ';
    std::string bin = buffer;
    while (bin.size() % 4) bin += '\0';
    std::string glb = "glTF";
    Put32(glb, 2);
    Put32(glb, static_cast<std::uint32_t>(12 + 8 + glbJson.size() + 8 + bin.size()));
    Put32(glb, static_cast<std::uint32_t>(glbJson.size()));
    glb += "JSON" + glbJson;
    Put32(glb, static_cast<std::uint32_t>(bin.size()));
    glb += std::string("BIN\0", 4) + bin;
    REQUIRE(ParseGlb(reinterpret_cast<const std::uint8_t*>(glb.data()), glb.size(), nullptr, mesh));
    CHECK(mesh.TriangleCount() == 1);
    CHECK(mesh.positions[3] == doctest::Approx(12.0f));

    // An external buffer goes through the resource reader.
    std::string external = json;
    const auto data = external.find("data:");
    external.replace(data, external.find('"', data) - data, "tri%20angle.bin");
    std::string asked;
    const ResourceReader reader = [&](const std::string& name, std::vector<std::uint8_t>& out) {
        asked = name;
        out.assign(buffer.begin(), buffer.end());
        return true;
    };
    REQUIRE(ParseGltf(reinterpret_cast<const std::uint8_t*>(external.data()), external.size(), reader, mesh));
    CHECK(asked == "tri angle.bin");
}

TEST_CASE("3MF objects, components and colours") {
    std::string vertices, triangles;
    for (const auto& v : kCube)
        vertices += "<vertex x=\"" + std::to_string(v[0]) + "\" y=\"" + std::to_string(v[1]) + "\" z=\"" +
                    std::to_string(v[2]) + "\"/>";
    for (const auto& f : kFaces)
        triangles += "<triangle v1=\"" + std::to_string(f[0]) + "\" v2=\"" + std::to_string(f[1]) + "\" v3=\"" +
                     std::to_string(f[2]) + "\"/>";
    const std::string model =
        "<?xml version=\"1.0\"?><!-- comment --><model xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" "
        "xmlns:m=\"http://schemas.microsoft.com/3dmanufacturing/material/2015/02\"><resources>"
        "<m:colorgroup id=\"5\"><m:color color=\"#00FF00\"/></m:colorgroup>"
        "<object id=\"1\" type=\"model\" pid=\"5\" pindex=\"0\"><mesh><vertices>" + vertices +
        "</vertices><triangles>" + triangles + "</triangles></mesh></object>"
        "<object id=\"2\" type=\"model\"><components><component objectid=\"1\" "
        "transform=\"1 0 0 0 1 0 0 0 1 5 0 0\"/></components></object></resources>"
        "<build><item objectid=\"2\" transform=\"2 0 0 0 2 0 0 0 2 0 0 0\"/></build></model>";
    const std::string zip = StoredZip({{"[Content_Types].xml", "<Types/>"}, {"3D/3dmodel.model", model}});
    Mesh mesh;
    REQUIRE(Parse3mf(Bytes(zip), mesh));
    CHECK(mesh.TriangleCount() == 12);
    CHECK(mesh.zUp);
    float lowest = 1e9f, highest = -1e9f;
    for (std::size_t i = 0; i < mesh.positions.size(); i += 3) {
        lowest = std::min(lowest, mesh.positions[i]);
        highest = std::max(highest, mesh.positions[i]);
    }
    // Component moves by 5, then the build item doubles everything.
    CHECK(lowest == doctest::Approx(10.0f));
    CHECK(highest == doctest::Approx(12.0f));
    REQUIRE(mesh.colors.size() == 12);
    CHECK(mesh.colors[0] == 0x00FF00u);
    CHECK_FALSE(Parse3mf(Bytes("PK not really"), mesh));
}

TEST_CASE("the deflated 3MF fixture loads from disk") {
    Mesh mesh;
    REQUIRE(LoadModel(Fixture(L"two-cubes.3mf"), mesh) == S_OK);
    CHECK(mesh.TriangleCount() == 24);
    REQUIRE(mesh.colors.size() == 24);
    CHECK(mesh.colors[0] == 0xFF0000u);
}

TEST_CASE("rasterizer draws the model in the middle on a transparent background") {
    const std::string binary = BinaryStl();
    Mesh mesh;
    REQUIRE(ParseStl(reinterpret_cast<const std::uint8_t*>(binary.data()), binary.size(), mesh));
    PreparedModel model;
    REQUIRE(PrepareModel(std::move(mesh), model));
    CHECK(model.triangles == 12);
    std::vector<std::uint32_t> pixels;
    REQUIRE(RasterizeModel(model, ModelCamera{}, 80, 60, 2, pixels));
    REQUIRE(pixels.size() == 80u * 60u);
    CHECK((pixels[30 * 80 + 40] >> 24) == 255);  // centre: solid
    CHECK(pixels[0] == 0);                         // corner: transparent
    CHECK_FALSE(RasterizeModel(model, ModelCamera{}, 0, 60, 1, pixels));
}

TEST_CASE("ImageDecoder renders 3D files for thumbnails") {
    ComApartment com;
    ImageDecoder decoder;
    DecodedImage image;
    REQUIRE(decoder.Decode(Fixture(L"two-cubes.3mf"), 200, image) == S_OK);
    CHECK(image.width == 200);
    CHECK(image.height == 150);
    CHECK(image.originalWidth == 1200);
    CHECK(image.originalHeight == 900);
    const auto corner = PixelAt(image.bitmap.Get(), 0, 0);
    CHECK(corner[3] == 0);
    // Some red pixels from the red cubes.
    bool red = false;
    for (UINT y = 0; y < 150 && !red; y += 3) {
        for (UINT x = 0; x < 200 && !red; x += 3) {
            const auto p = PixelAt(image.bitmap.Get(), x, y);
            red = p[3] == 255 && p[2] > 120 && p[1] < 60 && p[0] < 60;
        }
    }
    CHECK(red);
}
