#include "core/ModelRenderer.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace {

constexpr std::uint32_t kDefaultColor = 0xA9B8CC;  // a soft steel blue
constexpr float kDistance = 3.2f;                  // camera distance in model radii

}  // namespace

bool PrepareModel(Mesh&& mesh, PreparedModel& out) {
    out = {};
    const std::size_t vertices = mesh.positions.size() / 3;
    if (!vertices || mesh.indices.size() < 3) return false;
    if (mesh.zUp) {
        // (x, y, z) Z-up becomes (x, z, -y) Y-up.
        for (std::size_t i = 0; i < vertices; ++i) {
            float* p = &mesh.positions[i * 3];
            const float y = p[1];
            p[1] = p[2];
            p[2] = -y;
        }
        mesh.zUp = false;
    }
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    // Bounds of the vertices that triangles use.
    for (std::uint32_t index : mesh.indices) {
        if (index >= vertices) return false;
        const float* p = &mesh.positions[static_cast<std::size_t>(index) * 3];
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(p[k])) continue;
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    if (!(lo[0] <= hi[0]) || !(lo[1] <= hi[1]) || !(lo[2] <= hi[2])) return false;
    const float centre[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    float radius = std::sqrt(dx * dx + dy * dy + dz * dz) / 2;
    if (!(radius > 0) || !std::isfinite(radius)) radius = 1;
    for (std::size_t i = 0; i < vertices; ++i) {
        for (int k = 0; k < 3; ++k) {
            float& v = mesh.positions[i * 3 + k];
            v = std::isfinite(v) ? (v - centre[k]) / radius : 0.0f;
        }
    }
    out.triangles = mesh.TriangleCount();
    out.mesh = std::move(mesh);
    return true;
}

bool RasterizeModel(const PreparedModel& model, const ModelCamera& camera, unsigned width, unsigned height,
                    unsigned supersample, std::vector<std::uint32_t>& pixels) {
    pixels.clear();
    if (!width || !height || width > 16384 || height > 16384) return false;
    try {
        const unsigned ss = std::min(3u, std::max(1u, supersample));
        const int w = static_cast<int>(width * ss), h = static_cast<int>(height * ss);
        const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        std::vector<std::uint32_t> color(count, 0);  // 0 = empty, else 0xFFRRGGBB
        std::vector<float> depth(count, 0.0f);         // 1/w, larger is nearer

        const Mesh& mesh = model.mesh;
        const std::size_t vertices = mesh.positions.size() / 3;
        const float cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
        const float cp = std::cos(camera.pitch), sp = std::sin(camera.pitch);
        const float zoom = std::min(20.0f, std::max(0.05f, camera.zoom));
        const float fit = 0.47f * static_cast<float>(std::min(w, h)) * zoom;
        const float focal = fit * std::sqrt(kDistance * kDistance - 1.0f);
        const float halfW = w / 2.0f, halfH = h / 2.0f;

        // View-space position and screen position per vertex.
        std::vector<float> view(vertices * 3), screen(vertices * 3);
        for (std::size_t i = 0; i < vertices; ++i) {
            const float* p = &mesh.positions[i * 3];
            const float x1 = cy * p[0] + sy * p[2];
            const float z1 = -sy * p[0] + cy * p[2];
            const float y2 = cp * p[1] - sp * z1;
            const float z2 = sp * p[1] + cp * z1;
            view[i * 3] = x1;
            view[i * 3 + 1] = y2;
            view[i * 3 + 2] = z2;
            const float inv = 1.0f / std::max(0.05f, kDistance - z2);
            screen[i * 3] = halfW + focal * x1 * inv;
            screen[i * 3 + 1] = halfH - focal * y2 * inv;
            screen[i * 3 + 2] = inv;
        }

        // Light from the upper left, slightly in front.
        const float lx = -0.42f, ly = 0.62f, lz = 0.66f;
        const float hl = std::sqrt(lx * lx + ly * ly + (lz + 1) * (lz + 1));
        const float hx = lx / hl, hy = ly / hl, hz = (lz + 1) / hl;

        const std::size_t triangles = mesh.indices.size() / 3;
        for (std::size_t t = 0; t < triangles; ++t) {
            const std::uint32_t ia = mesh.indices[t * 3], ib = mesh.indices[t * 3 + 1], ic = mesh.indices[t * 3 + 2];
            if (ia >= vertices || ib >= vertices || ic >= vertices) continue;
            const float* a = &screen[ia * 3];
            const float* b = &screen[ib * 3];
            const float* c = &screen[ic * 3];
            const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if (area == 0 || !std::isfinite(area)) continue;

            // Shade the face; both sides are lit.
            const float* va = &view[ia * 3];
            const float* vb = &view[ib * 3];
            const float* vc = &view[ic * 3];
            const float ux = vb[0] - va[0], uy = vb[1] - va[1], uz = vb[2] - va[2];
            const float wx = vc[0] - va[0], wy = vc[1] - va[1], wz = vc[2] - va[2];
            float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 0) {
                nx /= length;
                ny /= length;
                nz /= length;
            }
            if (nz < 0) {
                nx = -nx;
                ny = -ny;
                nz = -nz;
            }
            const float diffuse = std::max(0.0f, nx * lx + ny * ly + nz * lz);
            const float spec = std::pow(std::max(0.0f, nx * hx + ny * hy + nz * hz), 28.0f) * 0.22f;
            const float light = 0.30f + 0.62f * diffuse + 0.10f * nz;
            std::uint32_t base = t < mesh.colors.size() ? mesh.colors[t] : kModelNoColor;
            if (base == kModelNoColor) base = kDefaultColor;
            auto channel = [&](int shift) {
                const float v = ((base >> shift) & 0xFF) * light + 255.0f * spec;
                return static_cast<std::uint32_t>(std::min(255.0f, std::max(0.0f, v)));
            };
            const std::uint32_t shaded = 0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0);

            // Bounding box, clipped to the target.
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], c[0]}))));
            const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({a[0], b[0], c[0]}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a[1], b[1], c[1]}))));
            const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({a[1], b[1], c[1]}))));
            if (x0 > x1 || y0 > y1) continue;
            const float inv = 1.0f / area;
            // Edge functions e = A*x + B*y + C, normalised so inside >= 0.
            const float A0 = (b[1] - c[1]) * inv, B0 = (c[0] - b[0]) * inv, C0 = (b[0] * c[1] - c[0] * b[1]) * inv;
            const float A1 = (c[1] - a[1]) * inv, B1 = (a[0] - c[0]) * inv, C1 = (c[0] * a[1] - a[0] * c[1]) * inv;
            const float A2 = (a[1] - b[1]) * inv, B2 = (b[0] - a[0]) * inv, C2 = (a[0] * b[1] - b[0] * a[1]) * inv;
            for (int y = y0; y <= y1; ++y) {
                const float py = y + 0.5f;
                const float px = x0 + 0.5f;
                float e0 = A0 * px + B0 * py + C0, e1 = A1 * px + B1 * py + C1, e2 = A2 * px + B2 * py + C2;
                std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
                for (int x = x0; x <= x1; ++x, e0 += A0, e1 += A1, e2 += A2) {
                    if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                    const float z = e0 * a[2] + e1 * b[2] + e2 * c[2];
                    const std::size_t i = row + static_cast<std::size_t>(x);
                    if (z > depth[i]) {
                        depth[i] = z;
                        color[i] = shaded;
                    }
                }
            }
        }

        // Average the samples into premultiplied pixels.
        pixels.assign(static_cast<std::size_t>(width) * height, 0);
        const unsigned samples = ss * ss;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned r = 0, g = 0, bl = 0, covered = 0;
                for (unsigned j = 0; j < ss; ++j) {
                    const std::uint32_t* src = &color[(static_cast<std::size_t>(y) * ss + j) * w + x * ss];
                    for (unsigned i = 0; i < ss; ++i) {
                        const std::uint32_t v = src[i];
                        if (!v) continue;
                        ++covered;
                        r += (v >> 16) & 0xFF;
                        g += (v >> 8) & 0xFF;
                        bl += v & 0xFF;
                    }
                }
                if (!covered) continue;
                const unsigned alpha = (covered * 255 + samples / 2) / samples;
                pixels[static_cast<std::size_t>(y) * width + x] =
                    (alpha << 24) | (((r + samples / 2) / samples) << 16) | (((g + samples / 2) / samples) << 8) |
                    ((bl + samples / 2) / samples);
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        pixels.clear();
        return false;
    }
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

HRESULT RenderModel(const std::wstring& path, UINT maxSide, ComPtr<IWICBitmapSource>& bitmap, UINT& width,
                    UINT& height, UINT& fullWidth, UINT& fullHeight) noexcept {
    bitmap.Reset();
    width = height = fullWidth = fullHeight = 0;
    try {
        Mesh mesh;
        HRESULT hr = LoadModel(path, mesh);
        if (FAILED(hr)) return hr;
        PreparedModel model;
        if (!PrepareModel(std::move(mesh), model)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        fullWidth = 1200;
        fullHeight = 900;
        width = fullWidth;
        height = fullHeight;
        if (maxSide && maxSide < fullWidth) {
            width = std::max(1u, maxSide);
            height = std::max(1u, maxSide * 3 / 4);
        }
        // Smooth small pictures more; large ones are sharp enough.
        const unsigned ss = width * height <= 400u * 300u ? 3 : width * height <= 1200u * 900u ? 2 : 1;
        std::vector<std::uint32_t> pixels;
        if (!RasterizeModel(model, ModelCamera{}, width, height, ss, pixels)) return E_OUTOFMEMORY;
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) return hr;
        ComPtr<IWICBitmap> result;
        hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
                                             static_cast<UINT>(pixels.size() * 4),
                                             reinterpret_cast<BYTE*>(pixels.data()), result.GetAddressOf());
        if (FAILED(hr)) return hr;
        bitmap = result;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
#endif
