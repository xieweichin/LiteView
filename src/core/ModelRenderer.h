#pragma once

#include <cstdint>
#include <vector>

#include "core/ModelLoader.h"

// A small software rasterizer for previews: flat-shaded triangles with a
// depth buffer, lit from the upper left, drawn on a transparent background.
// No GPU is needed, so thumbnails work everywhere. Plain C++.
struct PreparedModel {
    Mesh mesh;  // Y-up, centred on the origin, scaled into the unit sphere
    std::size_t triangles = 0;
};

// Converts to Y-up and normalises the size; false for an empty mesh.
bool PrepareModel(Mesh&& mesh, PreparedModel& out);

struct ModelCamera {
    float yaw = -0.65f;   // radians around the vertical axis
    float pitch = 0.45f;  // radians, positive looks from above
    float zoom = 1.0f;
};

// pixels receives width * height premultiplied BGRA values (0xAARRGGBB),
// top row first. supersample 1..3 smooths the edges.
bool RasterizeModel(const PreparedModel& model, const ModelCamera& camera, unsigned width, unsigned height,
                    unsigned supersample, std::vector<std::uint32_t>& pixels);

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

// Renders a model file for thumbnails and the preview pane. Full size is
// 1200 x 900; maxSide > 0 fits within that. COM must be initialized.
HRESULT RenderModel(const std::wstring& path, UINT maxSide, Microsoft::WRL::ComPtr<IWICBitmapSource>& bitmap,
                    UINT& width, UINT& height, UINT& fullWidth, UINT& fullHeight) noexcept;
#endif
