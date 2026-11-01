#include "core/ModelLoader.h"

#include "core/Zip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <utility>

namespace {

constexpr std::uint32_t kNoColor = kModelNoColor;
constexpr std::size_t kMaxPart = 512u * 1024 * 1024;

std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float F32(const std::uint8_t* p) {
    const std::uint32_t bits = U32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

const char* SkipSpace(const char* p) {
    while (IsSpace(*p)) ++p;
    return p;
}

// Locale-independent number parser for text formats; p must point into a
// NUL-terminated buffer. Returns false (and leaves p) if no number is there.
bool ParseNumber(const char*& p, double& value) {
    const char* s = p;
    bool negative = false;
    if (*s == '-' || *s == '+') negative = *s++ == '-';
    double v = 0;
    bool digits = false;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        digits = true;
    }
    if (*s == '.') {
        ++s;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * scale;
            scale *= 0.1;
            digits = true;
        }
    }
    if (!digits) return false;
    if (*s == 'e' || *s == 'E') {
        const char* e = s + 1;
        bool negExp = false;
        if (*e == '-' || *e == '+') negExp = *e++ == '-';
        if (*e >= '0' && *e <= '9') {
            int exponent = 0;
            while (*e >= '0' && *e <= '9') {
                if (exponent < 1000) exponent = exponent * 10 + (*e - '0');
                ++e;
            }
            v *= std::pow(10.0, negExp ? -exponent : exponent);
            s = e;
        }
    }
    value = negative ? -v : v;
    p = s;
    return true;
}

bool ParseFloat(const char*& p, float& value) {
    double d;
    if (!ParseNumber(p, d)) return false;
    value = static_cast<float>(d);
    return std::isfinite(value);
}

bool ParseInt(const char*& p, long long& value) {
    const char* s = p;
    bool negative = false;
    if (*s == '-' || *s == '+') negative = *s++ == '-';
    if (*s < '0' || *s > '9') return false;
    long long v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v < (1LL << 40)) v = v * 10 + (*s - '0');
        ++s;
    }
    value = negative ? -v : v;
    p = s;
    return true;
}

std::string Terminated(const std::uint8_t* data, std::size_t size) {
    std::string text(reinterpret_cast<const char*>(data), size);
    return text;  // std::string keeps a NUL after the last character
}

bool Finish(Mesh& mesh) {
    if (mesh.indices.empty() || mesh.positions.empty()) return false;
    if (!mesh.colors.empty()) {
        if (mesh.colors.size() != mesh.TriangleCount()) {
            mesh.colors.clear();
        } else if (std::all_of(mesh.colors.begin(), mesh.colors.end(),
                               [](std::uint32_t c) { return c == kNoColor; })) {
            mesh.colors.clear();
        }
    }
    return true;
}

bool RoomFor(const Mesh& mesh, std::size_t triangles) {
    return mesh.TriangleCount() + triangles <= kMaxModelTriangles;
}

// ---- STL --------------------------------------------------------------------

bool ParseStlBinary(const std::uint8_t* data, std::size_t size, Mesh& mesh) {
    if (size < 84) return false;
    const std::uint32_t count = U32(data + 80);
    if (count == 0 || count > kMaxModelTriangles || (size - 84) / 50 < count) return false;
    mesh.positions.resize(static_cast<std::size_t>(count) * 9);
    mesh.indices.resize(static_cast<std::size_t>(count) * 3);
    const std::uint8_t* p = data + 84;
    for (std::uint32_t t = 0; t < count; ++t, p += 50) {
        for (int i = 0; i < 9; ++i) mesh.positions[t * 9u + i] = F32(p + 12 + i * 4);
        for (std::uint32_t i = 0; i < 3; ++i) mesh.indices[t * 3u + i] = t * 3u + i;
    }
    return true;
}

bool ParseStlText(const std::uint8_t* data, std::size_t size, Mesh& mesh) {
    const std::string text = Terminated(data, size);
    const char* p = text.c_str();
    while ((p = std::strstr(p, "vertex")) != nullptr) {
        p += 6;
        float xyz[3];
        for (float& v : xyz) {
            p = SkipSpace(p);
            if (!ParseFloat(p, v)) return false;
        }
        if (mesh.positions.size() / 3 >= kMaxModelTriangles * 3) return false;
        mesh.positions.insert(mesh.positions.end(), xyz, xyz + 3);
    }
    const std::size_t vertices = mesh.positions.size() / 3;
    mesh.positions.resize(vertices / 3 * 9);
    for (std::uint32_t i = 0; i < vertices / 3 * 3; ++i) mesh.indices.push_back(i);
    return !mesh.indices.empty();
}

// ---- Minimal JSON for glTF ----------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string string;
    std::vector<Json> items;                         // arrays
    std::vector<std::pair<std::string, Json>> members;  // objects

    const Json* Get(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& m : members) {
            if (m.first == key) return &m.second;
        }
        return nullptr;
    }
    const Json* At(std::size_t i) const { return type == Type::Array && i < items.size() ? &items[i] : nullptr; }
    double Num(const char* key, double fallback) const {
        const Json* v = Get(key);
        return v && v->type == Type::Number ? v->number : fallback;
    }
    long long Int(const char* key, long long fallback) const {
        const Json* v = Get(key);
        return v && v->type == Type::Number ? static_cast<long long>(v->number) : fallback;
    }
    std::size_t Size() const { return type == Type::Array ? items.size() : 0; }
};

class JsonParser {
public:
    explicit JsonParser(const char* text) : p_(text) {}

    bool Parse(Json& out) {
        if (!Value(out, 0)) return false;
        p_ = SkipSpace(p_);
        return *p_ == '\0';
    }

private:
    bool Value(Json& out, int depth) {
        if (depth > 64) return false;
        p_ = SkipSpace(p_);
        switch (*p_) {
        case '{': {
            ++p_;
            out.type = Json::Type::Object;
            p_ = SkipSpace(p_);
            if (*p_ == '}') return ++p_, true;
            for (;;) {
                p_ = SkipSpace(p_);
                std::string key;
                if (*p_ != '"' || !String(key)) return false;
                p_ = SkipSpace(p_);
                if (*p_++ != ':') return false;
                out.members.emplace_back(std::move(key), Json{});
                if (!Value(out.members.back().second, depth + 1)) return false;
                p_ = SkipSpace(p_);
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == '}') return ++p_, true;
                return false;
            }
        }
        case '[': {
            ++p_;
            out.type = Json::Type::Array;
            p_ = SkipSpace(p_);
            if (*p_ == ']') return ++p_, true;
            for (;;) {
                out.items.emplace_back();
                if (!Value(out.items.back(), depth + 1)) return false;
                p_ = SkipSpace(p_);
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == ']') return ++p_, true;
                return false;
            }
        }
        case '"':
            out.type = Json::Type::String;
            return String(out.string);
        case 't':
            if (std::strncmp(p_, "true", 4) != 0) return false;
            p_ += 4;
            out.type = Json::Type::Bool;
            out.number = 1;
            return true;
        case 'f':
            if (std::strncmp(p_, "false", 5) != 0) return false;
            p_ += 5;
            out.type = Json::Type::Bool;
            return true;
        case 'n':
            if (std::strncmp(p_, "null", 4) != 0) return false;
            p_ += 4;
            return true;
        default:
            out.type = Json::Type::Number;
            return ParseNumber(p_, out.number);
        }
    }

    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& cp) {
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = Hex(p_[i]);
            if (h < 0) return false;
            cp = cp * 16 + static_cast<unsigned>(h);
        }
        p_ += 4;
        return true;
    }

    bool String(std::string& out) {
        ++p_;  // opening quote
        for (;;) {
            const char c = *p_++;
            if (c == '\0') return false;
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            const char e = *p_++;
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    unsigned low;
                    if (!Hex4(low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                Utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
    }

    const char* p_;
};

// ---- glTF ---------------------------------------------------------------------

struct Matrix {
    double m[16];  // column-major, like glTF
    static Matrix Identity() {
        Matrix r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    Matrix operator*(const Matrix& b) const {
        Matrix r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }
};

bool DecodeBase64(const char* text, std::vector<std::uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    unsigned buffer = 0;
    int bits = 0;
    for (const char* p = text; *p && *p != '='; ++p) {
        const int v = value(*p);
        if (v < 0) {
            if (IsSpace(*p)) continue;
            return false;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string PercentDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::uint32_t LinearToSrgbColor(const Json* factor) {
    if (!factor || factor->Size() < 3) return kNoColor;
    std::uint32_t rgb = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const Json* c = factor->At(i);
        double v = c && c->type == Json::Type::Number ? c->number : 1.0;
        v = std::pow(std::min(1.0, std::max(0.0, v)), 1.0 / 2.2);
        rgb = (rgb << 8) | static_cast<std::uint32_t>(std::lround(v * 255.0));
    }
    return rgb;
}

class GltfReader {
public:
    GltfReader(const Json& root, const ResourceReader& resources, const std::uint8_t* bin, std::size_t binSize, Mesh& mesh)
        : root_(root), resources_(resources), bin_(bin), binSize_(binSize), mesh_(mesh) {}

    bool Read() {
        const Json* asset = root_.Get("asset");
        if (!asset) return false;
        const Json* version = asset->Get("version");
        if (version && version->type == Json::Type::String && !version->string.empty() && version->string[0] == '1')
            return false;  // glTF 1.0 is a different format
        const Json* nodes = root_.Get("nodes");
        const Json* scenes = root_.Get("scenes");
        const std::size_t sceneIndex = static_cast<std::size_t>(std::max(0LL, root_.Int("scene", 0)));
        const Json* scene = scenes ? scenes->At(sceneIndex) : nullptr;
        if (scene && scene->Get("nodes")) {
            for (const Json& n : scene->Get("nodes")->items) {
                if (n.type == Json::Type::Number) Node(static_cast<std::size_t>(n.number), Matrix::Identity(), 0);
            }
        } else if (nodes && nodes->Size()) {
            // No scene: draw every root node.
            std::vector<bool> child(nodes->Size(), false);
            for (const Json& n : nodes->items) {
                if (const Json* children = n.Get("children")) {
                    for (const Json& c : children->items) {
                        if (c.type == Json::Type::Number && c.number >= 0 && c.number < child.size())
                            child[static_cast<std::size_t>(c.number)] = true;
                    }
                }
            }
            for (std::size_t i = 0; i < child.size(); ++i) {
                if (!child[i]) Node(i, Matrix::Identity(), 0);
            }
        } else if (const Json* meshes = root_.Get("meshes")) {
            for (std::size_t i = 0; i < meshes->Size(); ++i) DrawMesh(i, Matrix::Identity());
        }
        return ok_ && Finish(mesh_);
    }

private:
    void Node(std::size_t index, const Matrix& parent, int depth) {
        const Json* nodes = root_.Get("nodes");
        const Json* node = nodes ? nodes->At(index) : nullptr;
        if (!node || depth > 64 || !ok_) return;
        Matrix local = Matrix::Identity();
        if (const Json* matrix = node->Get("matrix"); matrix && matrix->Size() == 16) {
            for (std::size_t i = 0; i < 16; ++i) local.m[i] = matrix->At(i)->number;
        } else {
            double t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            auto read = [node](const char* key, double* out, std::size_t n) {
                const Json* v = node->Get(key);
                if (!v || v->Size() != n) return;
                for (std::size_t i = 0; i < n; ++i) out[i] = v->At(i)->number;
            };
            read("translation", t, 3);
            read("rotation", r, 4);
            read("scale", s, 3);
            const double x = r[0], y = r[1], z = r[2], w = r[3];
            const double rot[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                                   2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                                   2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
            for (int c = 0; c < 3; ++c)
                for (int row = 0; row < 3; ++row) local.m[c * 4 + row] = rot[c * 3 + row] * s[c];
            local.m[12] = t[0];
            local.m[13] = t[1];
            local.m[14] = t[2];
        }
        const Matrix world = parent * local;
        if (const Json* meshIndex = node->Get("mesh"); meshIndex && meshIndex->type == Json::Type::Number)
            DrawMesh(static_cast<std::size_t>(meshIndex->number), world);
        if (const Json* children = node->Get("children")) {
            for (const Json& c : children->items) {
                if (c.type == Json::Type::Number) Node(static_cast<std::size_t>(c.number), world, depth + 1);
            }
        }
    }

    const std::vector<std::uint8_t>* Buffer(std::size_t index) {
        auto found = buffers_.find(index);
        if (found != buffers_.end()) return found->second.get();
        auto data = std::make_unique<std::vector<std::uint8_t>>();
        const Json* buffers = root_.Get("buffers");
        const Json* buffer = buffers ? buffers->At(index) : nullptr;
        bool loaded = false;
        if (buffer) {
            const Json* uri = buffer->Get("uri");
            if (!uri || uri->type != Json::Type::String) {
                if (bin_) {
                    data->assign(bin_, bin_ + binSize_);
                    loaded = true;
                }
            } else if (uri->string.compare(0, 5, "data:") == 0) {
                const auto comma = uri->string.find(',');
                loaded = comma != std::string::npos && uri->string.find(";base64") < comma &&
                         DecodeBase64(uri->string.c_str() + comma + 1, *data);
            } else if (resources_) {
                loaded = resources_(PercentDecode(uri->string), *data);
            }
        }
        if (!loaded) data.reset();
        const auto* result = data.get();
        buffers_[index] = std::move(data);
        return result;
    }

    // Reads accessor elements as doubles; components per element in "count".
    bool Accessor(std::size_t index, std::vector<double>& out, int& components) {
        out.clear();
        const Json* accessors = root_.Get("accessors");
        const Json* accessor = accessors ? accessors->At(index) : nullptr;
        if (!accessor) return false;
        const Json* typeValue = accessor->Get("type");
        if (!typeValue || typeValue->type != Json::Type::String) return false;
        const std::string& type = typeValue->string;
        components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
        if (!components) return false;
        const long long componentType = accessor->Int("componentType", 0);
        int size;
        switch (componentType) {
        case 5120: case 5121: size = 1; break;
        case 5122: case 5123: size = 2; break;
        case 5125: case 5126: size = 4; break;
        default: return false;
        }
        const long long count = accessor->Int("count", 0);
        if (count <= 0 || static_cast<unsigned long long>(count) > kMaxModelTriangles * 3ull) return false;
        const Json* viewIndex = accessor->Get("bufferView");
        const Json* views = root_.Get("bufferViews");
        const Json* view = viewIndex && views ? views->At(static_cast<std::size_t>(viewIndex->number)) : nullptr;
        if (!view) return false;  // sparse-only accessors are not supported
        const std::vector<std::uint8_t>* buffer = Buffer(static_cast<std::size_t>(view->Int("buffer", -1)));
        if (!buffer) return false;
        const long long viewOffset = view->Int("byteOffset", 0);
        const long long viewLength = view->Int("byteLength", 0);
        const long long element = static_cast<long long>(size) * components;
        long long stride = view->Int("byteStride", 0);
        if (stride < element) stride = element;
        const long long start = viewOffset + accessor->Int("byteOffset", 0);
        const long long needed = (count - 1) * stride + element;
        if (viewOffset < 0 || start < viewOffset || start - viewOffset + needed > viewLength ||
            viewOffset + viewLength > static_cast<long long>(buffer->size()))
            return false;
        const bool normalized = accessor->Get("normalized") && accessor->Get("normalized")->number != 0;
        out.resize(static_cast<std::size_t>(count * components));
        for (long long i = 0; i < count; ++i) {
            const std::uint8_t* p = buffer->data() + start + i * stride;
            for (int c = 0; c < components; ++c, p += size) {
                double v;
                switch (componentType) {
                case 5120: v = static_cast<std::int8_t>(*p); if (normalized) v = std::max(v / 127.0, -1.0); break;
                case 5121: v = *p; if (normalized) v /= 255.0; break;
                case 5122: {
                    const std::int16_t s = static_cast<std::int16_t>(p[0] | (p[1] << 8));
                    v = s;
                    if (normalized) v = std::max(v / 32767.0, -1.0);
                    break;
                }
                case 5123: v = p[0] | (p[1] << 8); if (normalized) v /= 65535.0; break;
                case 5125: v = U32(p); break;
                default: v = F32(p); break;
                }
                out[static_cast<std::size_t>(i * components + c)] = v;
            }
        }
        return true;
    }

    void DrawMesh(std::size_t index, const Matrix& world) {
        const Json* meshes = root_.Get("meshes");
        const Json* mesh = meshes ? meshes->At(index) : nullptr;
        const Json* primitives = mesh ? mesh->Get("primitives") : nullptr;
        if (!primitives) return;
        for (const Json& primitive : primitives->items) {
            const long long mode = primitive.Int("mode", 4);
            if (mode < 4 || mode > 6) continue;  // points and lines
            const Json* attributes = primitive.Get("attributes");
            const Json* position = attributes ? attributes->Get("POSITION") : nullptr;
            if (!position || position->type != Json::Type::Number) continue;
            std::vector<double> points;
            int components = 0;
            if (!Accessor(static_cast<std::size_t>(position->number), points, components) || components != 3) continue;
            const std::size_t vertexCount = points.size() / 3;
            std::vector<std::uint32_t> order;
            if (const Json* indices = primitive.Get("indices"); indices && indices->type == Json::Type::Number) {
                std::vector<double> values;
                int one = 0;
                if (!Accessor(static_cast<std::size_t>(indices->number), values, one) || one != 1) continue;
                order.reserve(values.size());
                bool valid = true;
                for (double v : values) {
                    if (v < 0 || v >= vertexCount) { valid = false; break; }
                    order.push_back(static_cast<std::uint32_t>(v));
                }
                if (!valid) continue;
            } else {
                order.resize(vertexCount);
                for (std::size_t i = 0; i < vertexCount; ++i) order[i] = static_cast<std::uint32_t>(i);
            }
            std::vector<std::uint32_t> triangles;
            if (mode == 4) {
                triangles.assign(order.begin(), order.begin() + order.size() / 3 * 3);
            } else {
                for (std::size_t i = 2; i < order.size(); ++i) {
                    if (mode == 5) {  // strip, keeping the winding
                        if (i % 2 == 0) triangles.insert(triangles.end(), {order[i - 2], order[i - 1], order[i]});
                        else triangles.insert(triangles.end(), {order[i - 1], order[i - 2], order[i]});
                    } else {  // fan
                        triangles.insert(triangles.end(), {order[0], order[i - 1], order[i]});
                    }
                }
            }
            if (triangles.empty()) continue;
            if (!RoomFor(mesh_, triangles.size() / 3)) {
                ok_ = false;
                return;
            }
            const std::uint32_t base = static_cast<std::uint32_t>(mesh_.positions.size() / 3);
            const double* m = world.m;
            for (std::size_t v = 0; v < vertexCount; ++v) {
                const double x = points[v * 3], y = points[v * 3 + 1], z = points[v * 3 + 2];
                mesh_.positions.push_back(static_cast<float>(m[0] * x + m[4] * y + m[8] * z + m[12]));
                mesh_.positions.push_back(static_cast<float>(m[1] * x + m[5] * y + m[9] * z + m[13]));
                mesh_.positions.push_back(static_cast<float>(m[2] * x + m[6] * y + m[10] * z + m[14]));
            }
            for (std::uint32_t i : triangles) mesh_.indices.push_back(base + i);
            std::uint32_t color = kNoColor;
            const Json* materials = root_.Get("materials");
            if (const Json* material = primitive.Get("material"); material && materials) {
                if (const Json* entry = materials->At(static_cast<std::size_t>(material->number))) {
                    const Json* pbr = entry->Get("pbrMetallicRoughness");
                    color = LinearToSrgbColor(pbr ? pbr->Get("baseColorFactor") : nullptr);
                }
            }
            mesh_.colors.resize(mesh_.TriangleCount(), kNoColor);
            std::fill(mesh_.colors.end() - static_cast<std::ptrdiff_t>(triangles.size() / 3), mesh_.colors.end(), color);
        }
    }

    const Json& root_;
    const ResourceReader& resources_;
    const std::uint8_t* bin_;
    std::size_t binSize_;
    Mesh& mesh_;
    std::map<std::size_t, std::unique_ptr<std::vector<std::uint8_t>>> buffers_;
    bool ok_ = true;
};

bool ReadGltf(const char* json, const ResourceReader& resources, const std::uint8_t* bin, std::size_t binSize,
              Mesh& mesh) {
    Json root;
    if (!JsonParser(json).Parse(root) || root.type != Json::Type::Object) return false;
    mesh.zUp = false;
    return GltfReader(root, resources, bin, binSize, mesh).Read();
}

// ---- 3MF ------------------------------------------------------------------------

// A forgiving scanner for the XML in 3MF parts: elements and attributes only.
struct XmlTag {
    std::string name;  // without namespace prefix
    std::vector<std::pair<std::string, std::string>> attributes;
    bool closing = false;
    bool selfClosing = false;

    const std::string* Attr(const char* key) const {
        for (const auto& a : attributes) {
            if (a.first == key) return &a.second;
        }
        return nullptr;
    }
};

class XmlScanner {
public:
    explicit XmlScanner(const char* text) : p_(text) {}

    bool Next(XmlTag& tag) {
        for (;;) {
            p_ = std::strchr(p_, '<');
            if (!p_) return false;
            if (std::strncmp(p_, "<!--", 4) == 0) {
                const char* end = std::strstr(p_ + 4, "-->");
                if (!end) return false;
                p_ = end + 3;
                continue;
            }
            if (p_[1] == '?' || p_[1] == '!') {
                const char* end = std::strchr(p_, '>');
                if (!end) return false;
                p_ = end + 1;
                continue;
            }
            break;
        }
        ++p_;
        tag.attributes.clear();
        tag.closing = *p_ == '/';
        if (tag.closing) ++p_;
        tag.name = Name();
        for (;;) {
            p_ = SkipSpace(p_);
            if (*p_ == '\0') return false;
            if (*p_ == '>') {
                ++p_;
                tag.selfClosing = false;
                return true;
            }
            if (p_[0] == '/' && p_[1] == '>') {
                p_ += 2;
                tag.selfClosing = true;
                return true;
            }
            std::string key = Name();
            if (key.empty()) {
                ++p_;
                continue;
            }
            p_ = SkipSpace(p_);
            std::string value;
            if (*p_ == '=') {
                p_ = SkipSpace(p_ + 1);
                const char quote = *p_;
                if (quote == '"' || quote == '\'') {
                    const char* end = std::strchr(p_ + 1, quote);
                    if (!end) return false;
                    value.assign(p_ + 1, end);
                    p_ = end + 1;
                }
            }
            tag.attributes.emplace_back(std::move(key), std::move(value));
        }
    }

private:
    std::string Name() {
        const char* start = p_;
        const char* local = p_;
        while (*p_ && !IsSpace(*p_) && *p_ != '>' && *p_ != '/' && *p_ != '=') {
            if (*p_ == ':') local = p_ + 1;
            ++p_;
        }
        (void)start;
        return std::string(local, p_);
    }

    const char* p_;
};

struct Transform3mf {
    double m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};  // m00 m01 m02 m10 ... m32

    static Transform3mf Parse(const std::string* text) {
        Transform3mf t;
        if (!text) return t;
        const char* p = text->c_str();
        double v[12];
        for (double& x : v) {
            p = SkipSpace(p);
            if (!ParseNumber(p, x)) return t;
        }
        std::memcpy(t.m, v, sizeof(v));
        return t;
    }
    // Row-vector convention: first this, then b.
    Transform3mf Then(const Transform3mf& b) const {
        Transform3mf r;
        for (int row = 0; row < 4; ++row)
            for (int c = 0; c < 3; ++c) {
                double s = row == 3 ? b.m[9 + c] : 0;
                for (int k = 0; k < 3; ++k) s += m[row * 3 + k] * b.m[k * 3 + c];
                r.m[row * 3 + c] = s;
            }
        return r;
    }
};

struct Object3mf {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> colors;
    struct Component {
        std::string path;
        std::string id;
        Transform3mf transform;
    };
    std::vector<Component> components;
};

struct Part3mf {
    std::map<std::string, Object3mf> objects;
    struct Item {
        std::string path;
        std::string id;
        Transform3mf transform;
    };
    std::vector<Item> build;
};

std::uint32_t HexColor(const std::string* text) {
    if (!text || text->size() < 7 || (*text)[0] != '#') return kNoColor;
    std::uint32_t rgb = 0;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = (*text)[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return kNoColor;
        rgb = rgb * 16 + static_cast<std::uint32_t>(v);
    }
    return rgb;
}

long long IntAttr(const XmlTag& tag, const char* key, long long fallback) {
    const std::string* text = tag.Attr(key);
    if (!text) return fallback;
    const char* p = text->c_str();
    long long v;
    return ParseInt(p, v) ? v : fallback;
}

bool ParseModelPart(const std::string& xml, Part3mf& part, std::size_t& triangles) {
    XmlScanner scanner(xml.c_str());
    XmlTag tag;
    std::map<std::string, std::vector<std::uint32_t>> palettes;  // basematerials / colorgroup by id
    std::vector<std::uint32_t>* palette = nullptr;
    Object3mf* object = nullptr;
    std::string objectPid;
    long long objectIndex = -1;
    while (scanner.Next(tag)) {
        const std::string& n = tag.name;
        if (tag.closing) {
            if (n == "object") object = nullptr;
            if (n == "basematerials" || n == "colorgroup") palette = nullptr;
            continue;
        }
        if (n == "basematerials" || n == "colorgroup") {
            const std::string* id = tag.Attr("id");
            palette = id ? &palettes[*id] : nullptr;
            if (tag.selfClosing) palette = nullptr;
        } else if (palette && (n == "base" || n == "color")) {
            palette->push_back(HexColor(tag.Attr(n == "base" ? "displaycolor" : "color")));
        } else if (n == "object") {
            const std::string* id = tag.Attr("id");
            const std::string* type = tag.Attr("type");
            object = id && !(type && (*type == "support" || *type == "solidsupport")) ? &part.objects[*id] : nullptr;
            const std::string* pid = tag.Attr("pid");
            objectPid = pid ? *pid : std::string();
            objectIndex = IntAttr(tag, "pindex", -1);
            if (tag.selfClosing) object = nullptr;
        } else if (object && n == "vertex") {
            float xyz[3] = {0, 0, 0};
            const char* keys[3] = {"x", "y", "z"};
            for (int i = 0; i < 3; ++i) {
                if (const std::string* v = tag.Attr(keys[i])) {
                    const char* p = v->c_str();
                    ParseFloat(p, xyz[i]);
                }
            }
            object->positions.insert(object->positions.end(), xyz, xyz + 3);
        } else if (object && n == "triangle") {
            const long long v1 = IntAttr(tag, "v1", -1), v2 = IntAttr(tag, "v2", -1), v3 = IntAttr(tag, "v3", -1);
            if (v1 < 0 || v2 < 0 || v3 < 0) continue;
            if (++triangles > kMaxModelTriangles) return false;
            object->indices.insert(object->indices.end(), {static_cast<std::uint32_t>(v1), static_cast<std::uint32_t>(v2),
                                                          static_cast<std::uint32_t>(v3)});
            const std::string* pid = tag.Attr("pid");
            const std::string& group = pid ? *pid : objectPid;
            const long long index = IntAttr(tag, "p1", pid ? 0 : objectIndex);
            std::uint32_t color = kNoColor;
            auto found = palettes.find(group);
            if (found != palettes.end() && index >= 0 && static_cast<std::size_t>(index) < found->second.size())
                color = found->second[static_cast<std::size_t>(index)];
            object->colors.push_back(color);
        } else if (object && n == "component") {
            const std::string* id = tag.Attr("objectid");
            if (!id) continue;
            const std::string* path = tag.Attr("path");
            object->components.push_back({path ? *path : std::string(), *id, Transform3mf::Parse(tag.Attr("transform"))});
        } else if (n == "item") {
            const std::string* id = tag.Attr("objectid");
            if (!id) continue;
            const std::string* path = tag.Attr("path");
            part.build.push_back({path ? *path : std::string(), *id, Transform3mf::Parse(tag.Attr("transform"))});
        }
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string PartName(const std::string& path) {
    std::string name = path;
    while (!name.empty() && name[0] == '/') name.erase(0, 1);
    return Lower(PercentDecode(name));
}

class ThreeMfReader {
public:
    ThreeMfReader(const std::vector<std::uint8_t>& zip, Mesh& mesh) : zip_(zip), mesh_(mesh) {}

    bool Read() {
        if (!ListZip(zip_, entries_)) return false;
        std::string root;
        std::vector<std::uint8_t> rels;
        if (Extract("_rels/.rels", rels)) {
            const std::string xml(rels.begin(), rels.end());
            XmlScanner scanner(xml.c_str());
            XmlTag tag;
            while (scanner.Next(tag)) {
                const std::string* type = tag.Attr("Type");
                const std::string* target = tag.Attr("Target");
                if (tag.name == "Relationship" && type && target && type->find("3dmodel") != std::string::npos) {
                    root = PartName(*target);
                    break;
                }
            }
        }
        if (root.empty() || !Find(root)) {
            root.clear();
            for (const ZipEntry& e : entries_) {
                const std::string name = Lower(e.name);
                if (name.size() > 6 && name.compare(name.size() - 6, 6, ".model") == 0) {
                    root = name;
                    break;
                }
            }
        }
        const Part3mf* part = Part(root);
        if (!part) return false;
        mesh_.zUp = true;
        for (const auto& item : part->build) Emit(item.path.empty() ? root : PartName(item.path), item.id, item.transform, 0);
        if (part->build.empty()) {
            for (const auto& object : part->objects) Emit(root, object.first, Transform3mf{}, 0);
        }
        return ok_ && Finish(mesh_);
    }

private:
    const ZipEntry* Find(const std::string& name) const {
        for (const ZipEntry& e : entries_) {
            if (PartName(e.name) == name) return &e;
        }
        return nullptr;
    }

    bool Extract(const std::string& name, std::vector<std::uint8_t>& out) const {
        const ZipEntry* entry = Find(name);
        return entry && ReadZipEntry(zip_, *entry, kMaxPart, out);
    }

    const Part3mf* Part(const std::string& name) {
        auto found = parts_.find(name);
        if (found != parts_.end()) return found->second.get();
        auto part = std::make_unique<Part3mf>();
        std::vector<std::uint8_t> data;
        if (!Extract(name, data) || !ParseModelPart(std::string(data.begin(), data.end()), *part, triangles_)) {
            if (triangles_ > kMaxModelTriangles) ok_ = false;
            part.reset();
        }
        const Part3mf* result = part.get();
        parts_[name] = std::move(part);
        return result;
    }

    void Emit(const std::string& path, const std::string& id, const Transform3mf& transform, int depth) {
        if (depth > 32 || !ok_) return;
        const Part3mf* part = Part(path);
        if (!part) return;
        auto found = part->objects.find(id);
        if (found == part->objects.end()) return;
        const Object3mf& object = found->second;
        const std::size_t vertexCount = object.positions.size() / 3;
        if (!object.indices.empty()) {
            if (!RoomFor(mesh_, object.indices.size() / 3)) {
                ok_ = false;
                return;
            }
            const std::uint32_t base = static_cast<std::uint32_t>(mesh_.positions.size() / 3);
            const double* m = transform.m;
            for (std::size_t v = 0; v < vertexCount; ++v) {
                const double x = object.positions[v * 3], y = object.positions[v * 3 + 1], z = object.positions[v * 3 + 2];
                mesh_.positions.push_back(static_cast<float>(x * m[0] + y * m[3] + z * m[6] + m[9]));
                mesh_.positions.push_back(static_cast<float>(x * m[1] + y * m[4] + z * m[7] + m[10]));
                mesh_.positions.push_back(static_cast<float>(x * m[2] + y * m[5] + z * m[8] + m[11]));
            }
            mesh_.colors.resize(mesh_.TriangleCount(), kNoColor);
            for (std::size_t t = 0; t + 2 < object.indices.size(); t += 3) {
                const std::uint32_t a = object.indices[t], b = object.indices[t + 1], c = object.indices[t + 2];
                if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
                mesh_.indices.insert(mesh_.indices.end(), {base + a, base + b, base + c});
                mesh_.colors.push_back(t / 3 < object.colors.size() ? object.colors[t / 3] : kNoColor);
            }
        }
        for (const auto& component : object.components) {
            Emit(component.path.empty() ? path : PartName(component.path), component.id,
                 component.transform.Then(transform), depth + 1);
        }
    }

    const std::vector<std::uint8_t>& zip_;
    Mesh& mesh_;
    std::vector<ZipEntry> entries_;
    std::map<std::string, std::unique_ptr<Part3mf>> parts_;
    std::size_t triangles_ = 0;
    bool ok_ = true;
};

}  // namespace

ModelFormat ModelFormatForExtension(const wchar_t* extension) noexcept {
    if (!extension) return ModelFormat::None;
    wchar_t lower[8] = {};
    for (int i = 0; extension[i]; ++i) {
        if (i == 7) return ModelFormat::None;
        const wchar_t c = extension[i];
        lower[i] = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }
    const std::wstring e(lower);
    if (e == L".stl") return ModelFormat::Stl;
    if (e == L".obj") return ModelFormat::Obj;
    if (e == L".gltf") return ModelFormat::Gltf;
    if (e == L".glb") return ModelFormat::Glb;
    if (e == L".3mf") return ModelFormat::ThreeMf;
    return ModelFormat::None;
}

bool ParseStl(const std::uint8_t* data, std::size_t size, Mesh& mesh) {
    mesh = {};
    mesh.zUp = true;
    try {
        // A binary file can start with "solid" too; its size gives it away.
        const bool binarySize = size >= 84 && (size - 84) == static_cast<std::size_t>(U32(data + 80)) * 50u;
        const bool text = !binarySize && size >= 5 && std::memcmp(data, "solid", 5) == 0;
        if (text ? ParseStlText(data, size, mesh) : ParseStlBinary(data, size, mesh)) return Finish(mesh);
    } catch (const std::bad_alloc&) {
    }
    mesh = {};
    return false;
}

bool ParseObj(const std::uint8_t* data, std::size_t size, Mesh& mesh) {
    mesh = {};
    mesh.zUp = false;
    try {
        const std::string text = Terminated(data, size);
        const char* p = text.c_str();
        std::vector<std::uint32_t> face;
        while (*p) {
            p = SkipSpace(p);
            if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
                p += 2;
                float xyz[3] = {0, 0, 0};
                for (float& v : xyz) {
                    p = SkipSpace(p);
                    ParseFloat(p, v);
                }
                mesh.positions.insert(mesh.positions.end(), xyz, xyz + 3);
            } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
                p += 2;
                face.clear();
                const long long count = static_cast<long long>(mesh.positions.size() / 3);
                for (;;) {
                    while (*p == ' ' || *p == '\t') ++p;
                    long long index;
                    if (!ParseInt(p, index)) break;
                    while (*p && !IsSpace(*p)) ++p;  // skip /texture/normal
                    if (index < 0) index += count;
                    else --index;
                    if (index < 0 || index >= count) {
                        face.clear();
                        break;
                    }
                    face.push_back(static_cast<std::uint32_t>(index));
                }
                for (std::size_t i = 2; i < face.size(); ++i) {
                    if (!RoomFor(mesh, 1)) return mesh = {}, false;
                    mesh.indices.insert(mesh.indices.end(), {face[0], face[i - 1], face[i]});
                }
            }
            while (*p && *p != '\n') ++p;
        }
        if (Finish(mesh)) return true;
    } catch (const std::bad_alloc&) {
    }
    mesh = {};
    return false;
}

bool ParseGltf(const std::uint8_t* data, std::size_t size, const ResourceReader& resources, Mesh& mesh) {
    mesh = {};
    try {
        std::string json = Terminated(data, size);
        if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF) json.erase(0, 3);  // UTF-8 BOM
        if (ReadGltf(json.c_str(), resources, nullptr, 0, mesh)) return true;
    } catch (const std::bad_alloc&) {
    }
    mesh = {};
    return false;
}

bool ParseGlb(const std::uint8_t* data, std::size_t size, const ResourceReader& resources, Mesh& mesh) {
    mesh = {};
    try {
        if (size < 20 || std::memcmp(data, "glTF", 4) != 0 || U32(data + 4) != 2) return false;
        const std::size_t length = std::min<std::size_t>(size, U32(data + 8));
        std::size_t pos = 12;
        std::string json;
        const std::uint8_t* bin = nullptr;
        std::size_t binSize = 0;
        while (pos + 8 <= length) {
            const std::uint32_t chunkLength = U32(data + pos);
            const std::uint32_t type = U32(data + pos + 4);
            pos += 8;
            if (chunkLength > length - pos) return false;
            if (type == 0x4E4F534Au && json.empty()) json.assign(reinterpret_cast<const char*>(data + pos), chunkLength);
            else if (type == 0x004E4942u && !bin) bin = data + pos, binSize = chunkLength;
            pos += (chunkLength + 3u) & ~3u;
        }
        if (!json.empty() && ReadGltf(json.c_str(), resources, bin, binSize, mesh)) return true;
    } catch (const std::bad_alloc&) {
    }
    mesh = {};
    return false;
}

bool Parse3mf(const std::vector<std::uint8_t>& zip, Mesh& mesh) {
    mesh = {};
    try {
        if (ThreeMfReader(zip, mesh).Read()) return true;
    } catch (const std::bad_alloc&) {
    }
    mesh = {};
    return false;
}

bool ParseModel(ModelFormat format, const std::vector<std::uint8_t>& data, const ResourceReader& resources,
                Mesh& mesh) {
    switch (format) {
    case ModelFormat::Stl: return ParseStl(data.data(), data.size(), mesh);
    case ModelFormat::Obj: return ParseObj(data.data(), data.size(), mesh);
    case ModelFormat::Gltf: return ParseGltf(data.data(), data.size(), resources, mesh);
    case ModelFormat::Glb: return ParseGlb(data.data(), data.size(), resources, mesh);
    case ModelFormat::ThreeMf: return Parse3mf(data, mesh);
    default: mesh = {}; return false;
    }
}

#ifdef _WIN32
#include <shlwapi.h>

namespace {

bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= kMaxPart;
    if (ok) {
        try {
            data.resize(static_cast<std::size_t>(size.QuadPart));
        } catch (const std::bad_alloc&) {
            ok = false;
        }
    }
    std::size_t done = 0;
    while (ok && done < data.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 24));
        ok = ReadFile(file, data.data() + done, want, &got, nullptr) && got > 0;
        done += got;
    }
    CloseHandle(file);
    return ok;
}

std::wstring Widen(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);
    return wide;
}

}  // namespace

bool IsModelPath(const std::wstring& path) noexcept {
    return ModelFormatForExtension(PathFindExtensionW(path.c_str())) != ModelFormat::None;
}

HRESULT LoadModel(const std::wstring& path, Mesh& mesh) noexcept {
    mesh = {};
    try {
        const ModelFormat format = ModelFormatForExtension(PathFindExtensionW(path.c_str()));
        if (format == ModelFormat::None) return E_INVALIDARG;
        std::vector<std::uint8_t> data;
        if (!ReadWholeFile(path, data)) {
            const DWORD error = GetLastError();
            return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
        const auto slash = path.find_last_of(L"\\/");
        const std::wstring folder = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
        const ResourceReader resources = [&folder](const std::string& uri, std::vector<std::uint8_t>& out) {
            std::wstring relative = Widen(uri);
            // Only files beside the model or below it.
            if (relative.empty() || relative.find(L"..") != std::wstring::npos || relative.find(L':') != std::wstring::npos ||
                relative[0] == L'/' || relative[0] == L'\\')
                return false;
            for (wchar_t& c : relative) {
                if (c == L'/') c = L'\\';
            }
            return ReadWholeFile(folder + relative, out);
        };
        return ParseModel(format, data, resources, mesh) ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    } catch (const std::bad_alloc&) {
        mesh = {};
        return E_OUTOFMEMORY;
    } catch (...) {
        mesh = {};
        return E_FAIL;
    }
}
#endif
