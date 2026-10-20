#pragma once

#include "persistence_and_streaming.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace voxel {

// ============================================================================
// 1. Triangle, UV, Texture2D, and Barycentric Closest-Point Primitives
// ============================================================================

struct Vec2f {
    float u = 0.0f;
    float v = 0.0f;
};

struct TextureImage2D {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> rgb; // 3 bytes per pixel (R, G, B)

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgb.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3ULL;
    }

    [[nodiscard]] Vec3f sample_bilinear(float u, float v) const noexcept {
        if (!valid()) {
            return Vec3f{0.58f, 0.54f, 0.48f};
        }
        u = u - std::floor(u);
        v = 1.0f - (v - std::floor(v));

        const float fx = std::clamp(u * static_cast<float>(width - 1), 0.0f, static_cast<float>(width - 1));
        const float fy = std::clamp(v * static_cast<float>(height - 1), 0.0f, static_cast<float>(height - 1));

        const int32_t x0 = static_cast<int32_t>(std::floor(fx));
        const int32_t y0 = static_cast<int32_t>(std::floor(fy));
        const int32_t x1 = std::min(width - 1, x0 + 1);
        const int32_t y1 = std::min(height - 1, y0 + 1);
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        auto fetch = [&](int32_t x, int32_t y) noexcept -> Vec3f {
            const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)) * 3ULL;
            return Vec3f{
                static_cast<float>(rgb[idx + 0]) / 255.0f,
                static_cast<float>(rgb[idx + 1]) / 255.0f,
                static_cast<float>(rgb[idx + 2]) / 255.0f
            };
        };

        const Vec3f c00 = fetch(x0, y0);
        const Vec3f c10 = fetch(x1, y0);
        const Vec3f c01 = fetch(x0, y1);
        const Vec3f c11 = fetch(x1, y1);

        const Vec3f cx0{
            c00.x + tx * (c10.x - c00.x),
            c00.y + tx * (c10.y - c00.y),
            c00.z + tx * (c10.z - c00.z)
        };
        const Vec3f cx1{
            c01.x + tx * (c11.x - c01.x),
            c01.y + tx * (c11.y - c01.y),
            c01.z + tx * (c11.z - c01.z)
        };
        return Vec3f{
            cx0.x + ty * (cx1.x - cx0.x),
            cx0.y + ty * (cx1.y - cx0.y),
            cx0.z + ty * (cx1.z - cx0.z)
        };
    }

    [[nodiscard]] bool load_ppm(const std::string& filepath) {
        std::ifstream f(filepath, std::ios::binary);
        if (!f.is_open()) {
            return false;
        }
        std::string magic;
        f >> magic;
        if (magic != "P6" && magic != "P3") {
            return false;
        }
        auto skip_comments = [&]() {
            while (f >> std::ws && f.peek() == '#') {
                std::string dummy;
                std::getline(f, dummy);
            }
        };
        skip_comments();
        int max_val = 255;
        f >> width;
        skip_comments();
        f >> height;
        skip_comments();
        f >> max_val;
        if (width <= 0 || height <= 0 || max_val <= 0) {
            return false;
        }
        rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3ULL);
        if (magic == "P6") {
            f.get();
            f.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
            return f.good() || f.eof();
        } else {
            for (std::size_t i = 0; i < rgb.size(); ++i) {
                int v = 0;
                if (!(f >> v)) return false;
                rgb[i] = static_cast<uint8_t>(std::clamp(v * 255 / max_val, 0, 255));
            }
            return true;
        }
    }
};

struct Triangle3f {
    Vec3f v0{};
    Vec3f v1{};
    Vec3f v2{};
    Vec2f uv0{};
    Vec2f uv1{};
    Vec2f uv2{};
    Vec3f c0{0.58f, 0.54f, 0.48f};
    Vec3f c1{0.58f, 0.54f, 0.48f};
    Vec3f c2{0.58f, 0.54f, 0.48f};
    uint8_t material_id = MaterialHardness::kMaterialHardRock;
    bool has_uvs = false;
    bool has_custom_color = false;

    [[nodiscard]] Vec3f normal() const noexcept {
        const float e0x = v1.x - v0.x, e0y = v1.y - v0.y, e0z = v1.z - v0.z;
        const float e1x = v2.x - v0.x, e1y = v2.y - v0.y, e1z = v2.z - v0.z;
        const float nx = e0y * e1z - e0z * e1y;
        const float ny = e0z * e1x - e0x * e1z;
        const float nz = e0x * e1y - e0y * e1x;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-7f) {
            return Vec3f{nx / len, ny / len, nz / len};
        }
        return Vec3f{0.0f, 1.0f, 0.0f};
    }
};

[[nodiscard]] inline Vec3f point_triangle_closest_point_barycentric(
    Vec3f p,
    Vec3f a,
    Vec3f b,
    Vec3f c,
    float& out_u,
    float& out_v,
    float& out_w) noexcept {

    const float ab_x = b.x - a.x, ab_y = b.y - a.y, ab_z = b.z - a.z;
    const float ac_x = c.x - a.x, ac_y = c.y - a.y, ac_z = c.z - a.z;
    const float ap_x = p.x - a.x, ap_y = p.y - a.y, ap_z = p.z - a.z;

    const float d1 = ab_x * ap_x + ab_y * ap_y + ab_z * ap_z;
    const float d2 = ac_x * ap_x + ac_y * ap_y + ac_z * ap_z;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        out_u = 1.0f; out_v = 0.0f; out_w = 0.0f;
        return a;
    }

    const float bp_x = p.x - b.x, bp_y = p.y - b.y, bp_z = p.z - b.z;
    const float d3 = ab_x * bp_x + ab_y * bp_y + ab_z * bp_z;
    const float d4 = ac_x * bp_x + ac_y * bp_y + ac_z * bp_z;
    if (d3 >= 0.0f && d4 <= d3) {
        out_u = 0.0f; out_v = 1.0f; out_w = 0.0f;
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        out_u = 1.0f - v; out_v = v; out_w = 0.0f;
        return Vec3f{a.x + v * ab_x, a.y + v * ab_y, a.z + v * ab_z};
    }

    const float cp_x = p.x - c.x, cp_y = p.y - c.y, cp_z = p.z - c.z;
    const float d5 = ab_x * cp_x + ab_y * cp_y + ab_z * cp_z;
    const float d6 = ac_x * cp_x + ac_y * cp_y + ac_z * cp_z;
    if (d6 >= 0.0f && d5 <= d6) {
        out_u = 0.0f; out_v = 0.0f; out_w = 1.0f;
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        out_u = 1.0f - w; out_v = 0.0f; out_w = w;
        return Vec3f{a.x + w * ac_x, a.y + w * ac_y, a.z + w * ac_z};
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        out_u = 0.0f; out_v = 1.0f - w; out_w = w;
        const float bc_x = c.x - b.x, bc_y = c.y - b.y, bc_z = c.z - b.z;
        return Vec3f{b.x + w * bc_x, b.y + w * bc_y, b.z + w * bc_z};
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    out_u = 1.0f - v - w;
    out_v = v;
    out_w = w;
    return Vec3f{a.x + ab_x * v + ac_x * w,
                 a.y + ab_y * v + ac_y * w,
                 a.z + ab_z * v + ac_z * w};
}

[[nodiscard]] inline Vec3f point_triangle_closest_point(Vec3f p, Vec3f a, Vec3f b, Vec3f c) noexcept {
    float u = 0.0f, v = 0.0f, w = 0.0f;
    return point_triangle_closest_point_barycentric(p, a, b, c, u, v, w);
}

// ============================================================================
// 2. Wavefront .OBJ Mesh Loader (Positions, UVs, Vertex Colors & Material Palettes)
// ============================================================================

struct RawObjMesh {
    std::vector<Vec3f> vertices;
    std::vector<Vec2f> uvs;
    std::vector<Triangle3f> triangles;
    TextureImage2D texture;
    Vec3f aabb_min{1e30f, 1e30f, 1e30f};
    Vec3f aabb_max{-1e30f, -1e30f, -1e30f};

    void compute_aabb() noexcept {
        aabb_min = Vec3f{1e30f, 1e30f, 1e30f};
        aabb_max = Vec3f{-1e30f, -1e30f, -1e30f};
        for (const auto& tri : triangles) {
            for (const Vec3f& v : {tri.v0, tri.v1, tri.v2}) {
                aabb_min.x = std::min(aabb_min.x, v.x);
                aabb_min.y = std::min(aabb_min.y, v.y);
                aabb_min.z = std::min(aabb_min.z, v.z);
                aabb_max.x = std::max(aabb_max.x, v.x);
                aabb_max.y = std::max(aabb_max.y, v.y);
                aabb_max.z = std::max(aabb_max.z, v.z);
            }
        }
    }

    void transform(float scale, Vec3f translation) noexcept {
        for (auto& tri : triangles) {
            for (Vec3f* v : {&tri.v0, &tri.v1, &tri.v2}) {
                v->x = v->x * scale + translation.x;
                v->y = v->y * scale + translation.y;
                v->z = v->z * scale + translation.z;
            }
        }
        compute_aabb();
    }
};

class ObjMeshLoader {
public:
    static uint8_t parse_material_name(std::string_view name) noexcept {
        std::string lower;
        lower.reserve(name.size());
        for (char c : name) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        if (lower.find("lava") != std::string::npos || lower.find("magma") != std::string::npos) {
            return MaterialHardness::kMaterialLava;
        }
        if (lower.find("obsidian") != std::string::npos) {
            return MaterialHardness::kMaterialObsidian;
        }
        if (lower.find("ice") != std::string::npos || lower.find("frost") != std::string::npos) {
            return MaterialHardness::kMaterialIce;
        }
        if (lower.find("water") != std::string::npos) {
            return MaterialHardness::kMaterialWater;
        }
        if (lower.find("soil") != std::string::npos || lower.find("dirt") != std::string::npos ||
            lower.find("wood") != std::string::npos || lower.find("grass") != std::string::npos) {
            return MaterialHardness::kMaterialSoil;
        }
        return MaterialHardness::kMaterialHardRock;
    }

    static bool parse_material_color(std::string_view name, Vec3f& out_rgb) noexcept {
        std::string lower;
        lower.reserve(name.size());
        for (char c : name) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower.find("moss_crown") != std::string::npos || lower.find("moss_bright") != std::string::npos) {
            out_rgb = Vec3f{0.48f, 0.74f, 0.18f};
            return true;
        }
        if (lower.find("moss") != std::string::npos) {
            out_rgb = Vec3f{0.28f, 0.58f, 0.16f};
            return true;
        }
        if (lower.find("patina") != std::string::npos || lower.find("lichen") != std::string::npos) {
            out_rgb = Vec3f{0.44f, 0.56f, 0.46f};
            return true;
        }
        if (lower.find("eye_dark") != std::string::npos || lower.find("socket") != std::string::npos ||
            lower.find("nostril") != std::string::npos) {
            out_rgb = Vec3f{0.22f, 0.19f, 0.17f};
            return true;
        }
        if (lower.find("moai_stone") != std::string::npos || lower.find("tuff") != std::string::npos) {
            out_rgb = Vec3f{0.58f, 0.53f, 0.46f};
            return true;
        }
        if (lower.find("moai_highlight") != std::string::npos || lower.find("nose_bridge") != std::string::npos) {
            out_rgb = Vec3f{0.68f, 0.63f, 0.55f};
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool load_from_stream(
        std::istream& is,
        RawObjMesh& out_mesh,
        uint8_t default_mat = MaterialHardness::kMaterialHardRock) {

        std::vector<Vec3f> positions;
        std::vector<Vec3f> vertex_colors;
        std::vector<uint8_t> vertex_has_color;
        std::vector<Vec2f> texcoords;

        uint8_t current_mat = default_mat;
        Vec3f current_mat_color{0.58f, 0.53f, 0.46f};
        bool current_mat_has_color = false;

        std::string line;
        while (std::getline(is, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::stringstream ss(line);
            std::string token;
            if (!(ss >> token)) {
                continue;
            }

            if (token == "v") {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (ss >> x >> y >> z) {
                    positions.push_back(Vec3f{x, y, z});
                    float r = 0.0f, g = 0.0f, b = 0.0f;
                    if (ss >> r >> g >> b) {
                        if (r > 1.0f || g > 1.0f || b > 1.0f) {
                            r /= 255.0f; g /= 255.0f; b /= 255.0f;
                        }
                        vertex_colors.push_back(Vec3f{
                            std::clamp(r, 0.0f, 1.0f),
                            std::clamp(g, 0.0f, 1.0f),
                            std::clamp(b, 0.0f, 1.0f)
                        });
                        vertex_has_color.push_back(1U);
                    } else {
                        vertex_colors.push_back(Vec3f{0.58f, 0.53f, 0.46f});
                        vertex_has_color.push_back(0U);
                    }
                }
            } else if (token == "vt") {
                float u = 0.0f, v = 0.0f;
                if (ss >> u >> v) {
                    texcoords.push_back(Vec2f{u, v});
                }
            } else if (token == "usemtl") {
                std::string mat_name;
                if (ss >> mat_name) {
                    current_mat = parse_material_name(mat_name);
                    current_mat_has_color = parse_material_color(mat_name, current_mat_color);
                }
            } else if (token == "f") {
                struct FaceVert {
                    int32_t v_idx = -1;
                    int32_t vt_idx = -1;
                };
                std::vector<FaceVert> face_verts;
                std::string vertex_entry;
                while (ss >> vertex_entry) {
                    const std::size_t slash1 = vertex_entry.find('/');
                    const std::string v_str = (slash1 == std::string::npos)
                                                  ? vertex_entry
                                                  : vertex_entry.substr(0, slash1);
                    std::string vt_str;
                    if (slash1 != std::string::npos) {
                        const std::size_t slash2 = vertex_entry.find('/', slash1 + 1);
                        vt_str = (slash2 == std::string::npos)
                                     ? vertex_entry.substr(slash1 + 1)
                                     : vertex_entry.substr(slash1 + 1, slash2 - (slash1 + 1));
                    }

                    FaceVert fv{};
                    if (!v_str.empty()) {
                        try {
                            const int32_t idx = std::stoi(v_str);
                            const int32_t zb = (idx > 0) ? (idx - 1) : (static_cast<int32_t>(positions.size()) + idx);
                            if (zb >= 0 && zb < static_cast<int32_t>(positions.size())) {
                                fv.v_idx = zb;
                            }
                        } catch (...) {}
                    }
                    if (!vt_str.empty()) {
                        try {
                            const int32_t idx = std::stoi(vt_str);
                            const int32_t zb = (idx > 0) ? (idx - 1) : (static_cast<int32_t>(texcoords.size()) + idx);
                            if (zb >= 0 && zb < static_cast<int32_t>(texcoords.size())) {
                                fv.vt_idx = zb;
                            }
                        } catch (...) {}
                    }
                    if (fv.v_idx >= 0) {
                        face_verts.push_back(fv);
                    }
                }

                if (face_verts.size() >= 3) {
                    for (std::size_t i = 1; i + 1 < face_verts.size(); ++i) {
                        const FaceVert& f0 = face_verts[0];
                        const FaceVert& f1 = face_verts[i];
                        const FaceVert& f2 = face_verts[i + 1];

                        Triangle3f tri{};
                        tri.v0 = positions[f0.v_idx];
                        tri.v1 = positions[f1.v_idx];
                        tri.v2 = positions[f2.v_idx];
                        tri.material_id = current_mat;

                        if (f0.vt_idx >= 0 && f1.vt_idx >= 0 && f2.vt_idx >= 0) {
                            tri.uv0 = texcoords[f0.vt_idx];
                            tri.uv1 = texcoords[f1.vt_idx];
                            tri.uv2 = texcoords[f2.vt_idx];
                            tri.has_uvs = true;
                        }

                        if (vertex_has_color[f0.v_idx] || vertex_has_color[f1.v_idx] || vertex_has_color[f2.v_idx]) {
                            tri.c0 = vertex_colors[f0.v_idx];
                            tri.c1 = vertex_colors[f1.v_idx];
                            tri.c2 = vertex_colors[f2.v_idx];
                            tri.has_custom_color = true;
                        } else if (current_mat_has_color) {
                            tri.c0 = current_mat_color;
                            tri.c1 = current_mat_color;
                            tri.c2 = current_mat_color;
                            tri.has_custom_color = true;
                        }
                        out_mesh.triangles.push_back(tri);
                    }
                }
            }
        }

        out_mesh.vertices = std::move(positions);
        out_mesh.uvs = std::move(texcoords);
        out_mesh.compute_aabb();
        return !out_mesh.triangles.empty();
    }

    [[nodiscard]] static bool load_from_file(
        const std::string& filepath,
        RawObjMesh& out_mesh,
        uint8_t default_mat = MaterialHardness::kMaterialHardRock) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return false;
        }
        return load_from_stream(file, out_mesh, default_mat);
    }
};

// ============================================================================
// 3. Accelerated Mesh Spatial Grid (3D Distance + 2D XY Ray Parity for Watertight Meshes)
// ============================================================================

class MeshSpatialGrid {
public:
    static constexpr float kCellSize = 8.0f;
    static constexpr float kRayCellSize = 4.0f;

    void build(const std::vector<Triangle3f>& triangles, const TextureImage2D* texture = nullptr) {
        triangles_ = &triangles;
        texture_ = texture;
        cells_.clear();
        xy_ray_cells_.clear();
        if (triangles.empty()) return;

        for (uint32_t tri_idx = 0; tri_idx < static_cast<uint32_t>(triangles.size()); ++tri_idx) {
            const Triangle3f& tri = triangles[tri_idx];
            const float min_x = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
            const float max_x = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
            const float min_y = std::min({tri.v0.y, tri.v1.y, tri.v2.y});
            const float max_y = std::max({tri.v0.y, tri.v1.y, tri.v2.y});
            const float min_z = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
            const float max_z = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

            const int32_t gx0 = static_cast<int32_t>(std::floor(min_x / kCellSize));
            const int32_t gx1 = static_cast<int32_t>(std::floor(max_x / kCellSize));
            const int32_t gy0 = static_cast<int32_t>(std::floor(min_y / kCellSize));
            const int32_t gy1 = static_cast<int32_t>(std::floor(max_y / kCellSize));
            const int32_t gz0 = static_cast<int32_t>(std::floor(min_z / kCellSize));
            const int32_t gz1 = static_cast<int32_t>(std::floor(max_z / kCellSize));

            for (int32_t gz = gz0; gz <= gz1; ++gz) {
                for (int32_t gy = gy0; gy <= gy1; ++gy) {
                    for (int32_t gx = gx0; gx <= gx1; ++gx) {
                        const uint64_t key = pack_key(gx, gy, gz);
                        cells_[key].push_back(tri_idx);
                    }
                }
            }

            const int32_t rx0 = static_cast<int32_t>(std::floor(min_x / kRayCellSize));
            const int32_t rx1 = static_cast<int32_t>(std::floor(max_x / kRayCellSize));
            const int32_t ry0 = static_cast<int32_t>(std::floor(min_y / kRayCellSize));
            const int32_t ry1 = static_cast<int32_t>(std::floor(max_y / kRayCellSize));
            for (int32_t ry = ry0; ry <= ry1; ++ry) {
                for (int32_t rx = rx0; rx <= rx1; ++rx) {
                    const uint64_t rkey = pack_key(rx, ry, 0);
                    xy_ray_cells_[rkey].push_back(tri_idx);
                }
            }
        }
    }

    /// Determines whether point `p` is inside a closed watertight triangle mesh using
    /// jittered +Z ray-intersection parity accelerated by the 2D XY grid.
    [[nodiscard]] bool is_point_inside_watertight_mesh(Vec3f p) const noexcept {
        if (!triangles_ || triangles_->empty()) {
            return false;
        }
        // Apply sub-millimeter deterministic jitter in XY so the ray never hits exact vertices/edges
        const float rx = p.x + 0.000137f;
        const float ry = p.y + 0.000293f;
        const float rz = p.z;

        const int32_t cx = static_cast<int32_t>(std::floor(rx / kRayCellSize));
        const int32_t cy = static_cast<int32_t>(std::floor(ry / kRayCellSize));
        auto it = xy_ray_cells_.find(pack_key(cx, cy, 0));
        if (it == xy_ray_cells_.end()) {
            return false;
        }

        int intersections = 0;
        for (uint32_t tri_idx : it->second) {
            const Triangle3f& tri = (*triangles_)[tri_idx];
            // 2D barycentric test in XY for ray (rx, ry, z >= rz) along +Z
            const float x0 = tri.v0.x, y0 = tri.v0.y;
            const float x1 = tri.v1.x, y1 = tri.v1.y;
            const float x2 = tri.v2.x, y2 = tri.v2.y;

            const float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
            if (std::abs(denom) < 1e-9f) {
                continue;
            }
            const float inv_denom = 1.0f / denom;
            const float u = ((y1 - y2) * (rx - x2) + (x2 - x1) * (ry - y2)) * inv_denom;
            if (u < 0.0f || u > 1.0f) continue;
            const float v = ((y2 - y0) * (rx - x2) + (x0 - x2) * (ry - y2)) * inv_denom;
            if (v < 0.0f || (u + v) > 1.0f) continue;
            const float w = 1.0f - u - v;

            const float hit_z = u * tri.v0.z + v * tri.v1.z + w * tri.v2.z;
            if (hit_z >= rz) {
                ++intersections;
            }
        }
        return (intersections & 1) != 0;
    }

    struct QueryResult {
        float distance = 1e30f;
        Vec3f closest_point{};
        Vec3f normal{};
        uint8_t material_id = MaterialHardness::kMaterialHardRock;
        uint16_t baked_color = VoxelColor::kUnpainted;
        uint32_t triangle_index = 0;
    };

    [[nodiscard]] QueryResult query_closest(Vec3f p, float max_dist = 1e30f) const noexcept {
        QueryResult res{};
        res.distance = max_dist;
        if (!triangles_ || triangles_->empty()) {
            return res;
        }

        const int32_t center_gx = static_cast<int32_t>(std::floor(p.x / kCellSize));
        const int32_t center_gy = static_cast<int32_t>(std::floor(p.y / kCellSize));
        const int32_t center_gz = static_cast<int32_t>(std::floor(p.z / kCellSize));
        const int32_t r = std::max(1, static_cast<int32_t>(std::ceil(max_dist / kCellSize)));

        float best_d2 = max_dist * max_dist;
        bool found = false;
        float best_u = 1.0f, best_v = 0.0f, best_w = 0.0f;

        for (int32_t dz = -r; dz <= r; ++dz) {
            for (int32_t dy = -r; dy <= r; ++dy) {
                for (int32_t dx = -r; dx <= r; ++dx) {
                    const uint64_t key = pack_key(center_gx + dx, center_gy + dy, center_gz + dz);
                    auto it = cells_.find(key);
                    if (it == cells_.end()) continue;

                    for (uint32_t tri_idx : it->second) {
                        const Triangle3f& tri = (*triangles_)[tri_idx];
                        float u = 0.0f, v = 0.0f, w = 0.0f;
                        const Vec3f cp = point_triangle_closest_point_barycentric(
                            p, tri.v0, tri.v1, tri.v2, u, v, w);
                        const float diff_x = p.x - cp.x;
                        const float diff_y = p.y - cp.y;
                        const float diff_z = p.z - cp.z;
                        const float d2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;

                        if (d2 < best_d2) {
                            best_d2 = d2;
                            res.closest_point = cp;
                            res.normal = tri.normal();
                            res.material_id = tri.material_id;
                            res.triangle_index = tri_idx;
                            best_u = u;
                            best_v = v;
                            best_w = w;
                            found = true;
                        }
                    }
                }
            }
        }

        if (found) {
            res.distance = std::sqrt(best_d2);
            const Triangle3f& best_tri = (*triangles_)[res.triangle_index];
            if (texture_ && texture_->valid() && best_tri.has_uvs) {
                const float tu = best_u * best_tri.uv0.u + best_v * best_tri.uv1.u + best_w * best_tri.uv2.u;
                const float tv = best_u * best_tri.uv0.v + best_v * best_tri.uv1.v + best_w * best_tri.uv2.v;
                const Vec3f tex_rgb = texture_->sample_bilinear(tu, tv);
                res.baked_color = VoxelColor::encode(tex_rgb.x, tex_rgb.y, tex_rgb.z);
            } else if (best_tri.has_custom_color) {
                const float cr = best_u * best_tri.c0.x + best_v * best_tri.c1.x + best_w * best_tri.c2.x;
                const float cg = best_u * best_tri.c0.y + best_v * best_tri.c1.y + best_w * best_tri.c2.y;
                const float cb = best_u * best_tri.c0.z + best_v * best_tri.c1.z + best_w * best_tri.c2.z;
                res.baked_color = VoxelColor::encode(cr, cg, cb);
            }
        }
        return res;
    }

private:
    [[nodiscard]] static constexpr uint64_t pack_key(int32_t x, int32_t y, int32_t z) noexcept {
        const uint64_t ux = static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFFU);
        const uint64_t uy = static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFFU);
        const uint64_t uz = static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFFU);
        return ux | (uy << 21U) | (uz << 42U);
    }

    const std::vector<Triangle3f>* triangles_ = nullptr;
    const TextureImage2D* texture_ = nullptr;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> xy_ray_cells_;
};

// ============================================================================
// 4. Voxelization Pipeline Options & Baker
// ============================================================================

enum class VoxelizationMode : uint8_t {
    ShellThickness = 0, // Solid shell inside [-T/2, +T/2] around arbitrary open meshes/ruins
    WatertightSolid = 1 // Watertight interior is fully solid, exterior is air
};

struct AssetBakerConfig {
    float voxel_scale = 1.0f;                       // Meters per voxel sample (1.0 = 1m voxels)
    float truncation_delta = TsdfFormat::kDefaultTruncationDelta; // Default 2.0m narrow-band (2 voxel widths)
    VoxelizationMode mode = VoxelizationMode::ShellThickness;
    float shell_thickness = 1.6f;                   // Default 1.6m thickness for ruins/walls
    uint8_t default_material = MaterialHardness::kMaterialHardRock; // 3-hit stone
    bool auto_center_in_region = true;              // Center asset at target_region_center in 8x8x8 region
    Vec3f target_region_center{128.0f, 128.0f, 128.0f};
    bool bake_surface_colors = true;                // Bake triangle UV/vertex colors into outer-shell voxels
    float color_shell_depth = 1.8f;                 // Outer-skin depth (m) that receives baked color; deeper interior stays unpainted rock
    RegionKey region_coords{0, 0, 0};               // Output region file (rx, ry, rz)
};

struct BakedRegionData {
    RegionFileHeader header{};
    std::array<RegionTocEntry, RegionKey::kChunksPerRegion> toc{};
    std::vector<uint8_t> payload;
    std::size_t active_chunk_count = 0;
    std::size_t solid_chunk_count = 0;
    std::size_t empty_chunk_count = 0;
    std::size_t dense_chunk_count = 0;
};

class VoxelMeshBaker {
public:
    [[nodiscard]] static BakedRegionData bake_mesh_to_region(
        RawObjMesh mesh,
        const AssetBakerConfig& cfg = {}) {

        if (cfg.auto_center_in_region && !mesh.triangles.empty()) {
            mesh.compute_aabb();
            const float mid_x = 0.5f * (mesh.aabb_min.x + mesh.aabb_max.x);
            const float mid_y = 0.5f * (mesh.aabb_min.y + mesh.aabb_max.y);
            const float mid_z = 0.5f * (mesh.aabb_min.z + mesh.aabb_max.z);
            const Vec3f offset{
                cfg.target_region_center.x * cfg.voxel_scale - mid_x,
                cfg.target_region_center.y * cfg.voxel_scale - mid_y,
                cfg.target_region_center.z * cfg.voxel_scale - mid_z
            };
            mesh.transform(1.0f, offset);
        }

        MeshSpatialGrid spatial_grid;
        spatial_grid.build(mesh.triangles, mesh.texture.valid() ? &mesh.texture : nullptr);

        BakedRegionData result{};
        result.header.magic[0] = 'V';
        result.header.magic[1] = 'R';
        result.header.magic[2] = 'E';
        result.header.magic[3] = 'G';
        result.header.version = 4;
        result.header.rx = cfg.region_coords.rx;
        result.header.ry = cfg.region_coords.ry;
        result.header.rz = cfg.region_coords.rz;

        const float half_thick = 0.5f * cfg.shell_thickness;
        const float delta = cfg.truncation_delta;
        const float max_query_dist = (cfg.mode == VoxelizationMode::ShellThickness)
                                         ? (half_thick + delta + 1.0f)
                                         : ( std::max(delta, cfg.color_shell_depth) + 1.5f );
        const float chunk_cull_dist = (cfg.mode == VoxelizationMode::ShellThickness)
                                          ? (half_thick + delta + 1.0f)
                                          : (delta + 1.0f);

        std::vector<int16_t> tsdf_buf(TsdfFormat::kChunkSamples);
        std::vector<uint8_t> mat_buf(TsdfFormat::kChunkSamples);
        std::vector<uint16_t> col_buf(TsdfFormat::kChunkSamples);
        std::vector<uint8_t> low_plane(TsdfFormat::kChunkSamples);
        std::vector<uint8_t> high_plane(TsdfFormat::kChunkSamples);
        std::vector<uint8_t> col_low_plane(TsdfFormat::kChunkSamples);
        std::vector<uint8_t> col_high_plane(TsdfFormat::kChunkSamples);

        for (int32_t lz = 0; lz < 8; ++lz) {
            for (int32_t ly = 0; ly < 8; ++ly) {
                for (int32_t lx = 0; lx < 8; ++lx) {
                    const uint16_t toc_idx = static_cast<uint16_t>(lx + 8 * (ly + 8 * lz));
                    const int32_t chunk_origin_x = lx * 32;
                    const int32_t chunk_origin_y = ly * 32;
                    const int32_t chunk_origin_z = lz * 32;

                    const float cmin_x = static_cast<float>(chunk_origin_x) * cfg.voxel_scale;
                    const float cmin_y = static_cast<float>(chunk_origin_y) * cfg.voxel_scale;
                    const float cmin_z = static_cast<float>(chunk_origin_z) * cfg.voxel_scale;
                    const float cmax_x = static_cast<float>(chunk_origin_x + 32) * cfg.voxel_scale;
                    const float cmax_y = static_cast<float>(chunk_origin_y + 32) * cfg.voxel_scale;
                    const float cmax_z = static_cast<float>(chunk_origin_z + 32) * cfg.voxel_scale;

                    const float dist_to_mesh_box_x = std::max(0.0f, std::max(mesh.aabb_min.x - cmax_x, cmin_x - mesh.aabb_max.x));
                    const float dist_to_mesh_box_y = std::max(0.0f, std::max(mesh.aabb_min.y - cmax_y, cmin_y - mesh.aabb_max.y));
                    const float dist_to_mesh_box_z = std::max(0.0f, std::max(mesh.aabb_min.z - cmax_z, cmin_z - mesh.aabb_max.z));
                    const float box_dist = std::sqrt(dist_to_mesh_box_x * dist_to_mesh_box_x +
                                                     dist_to_mesh_box_y * dist_to_mesh_box_y +
                                                     dist_to_mesh_box_z * dist_to_mesh_box_z);

                    if (box_dist > chunk_cull_dist) {
                        result.toc[toc_idx].form = static_cast<uint8_t>(SerializedChunkForm::Empty);
                        result.toc[toc_idx].base_material_id = cfg.default_material;
                        result.toc[toc_idx].has_material_plane = 0;
                        result.toc[toc_idx].has_color_plane = 0;
                        result.toc[toc_idx].payload_offset = 0;
                        result.toc[toc_idx].payload_size = 0;
                        ++result.empty_chunk_count;
                        continue;
                    }

                    bool has_solid = false;
                    bool has_air = false;
                    uint8_t primary_mat = cfg.default_material;
                    bool non_uniform_mat = false;
                    bool has_painted_color = false;

                    for (int32_t vz = 0; vz < 32; ++vz) {
                        const float pz = static_cast<float>(chunk_origin_z + vz) * cfg.voxel_scale;
                        for (int32_t vy = 0; vy < 32; ++vy) {
                            const float py = static_cast<float>(chunk_origin_y + vy) * cfg.voxel_scale;
                            for (int32_t vx = 0; vx < 32; ++vx) {
                                const float px = static_cast<float>(chunk_origin_x + vx) * cfg.voxel_scale;
                                const uint32_t v_idx = static_cast<uint32_t>(vx + 32 * (vy + 32 * vz));

                                if (cfg.mode == VoxelizationMode::WatertightSolid &&
                                    (px < mesh.aabb_min.x - delta || px > mesh.aabb_max.x + delta ||
                                     py < mesh.aabb_min.y - delta || py > mesh.aabb_max.y + delta ||
                                     pz < mesh.aabb_min.z - delta || pz > mesh.aabb_max.z + delta)) {
                                    tsdf_buf[v_idx] = TsdfFormat::kClampedAir;
                                    mat_buf[v_idx] = cfg.default_material;
                                    col_buf[v_idx] = VoxelColor::kUnpainted;
                                    has_air = true;
                                    continue;
                                }

                                const Vec3f p_world{px, py, pz};
                                const auto q = spatial_grid.query_closest(p_world, max_query_dist);
                                float sdf = 0.0f;
                                uint8_t mat = q.material_id;

                                if (cfg.mode == VoxelizationMode::ShellThickness) {
                                    sdf = q.distance - half_thick;
                                } else {
                                    const bool inside = spatial_grid.is_point_inside_watertight_mesh(p_world);
                                    const float sign = inside ? -1.0f : 1.0f;
                                    sdf = sign * q.distance;
                                }

                                const int16_t encoded_q = TsdfFormat::encode(sdf, delta);
                                tsdf_buf[v_idx] = encoded_q;
                                mat_buf[v_idx] = mat;

                                uint16_t v_col = VoxelColor::kUnpainted;
                                if (cfg.bake_surface_colors &&
                                    encoded_q < 0 &&
                                    q.baked_color != VoxelColor::kUnpainted &&
                                    q.distance <= cfg.color_shell_depth) {
                                    v_col = q.baked_color;
                                    // Shell mode paints only the outer half of the shell: the
                                    // inner half is what a crater exposes, so it must stay raw.
                                    if (cfg.mode == VoxelizationMode::ShellThickness) {
                                        const float side_dot =
                                            (px - q.closest_point.x) * q.normal.x +
                                            (py - q.closest_point.y) * q.normal.y +
                                            (pz - q.closest_point.z) * q.normal.z;
                                        if (side_dot < 0.0f) {
                                            v_col = VoxelColor::kUnpainted;
                                        }
                                    }
                                    if (v_col != VoxelColor::kUnpainted) {
                                        has_painted_color = true;
                                    }
                                }
                                col_buf[v_idx] = v_col;

                                if (encoded_q < 0) {
                                    has_solid = true;
                                    primary_mat = mat;
                                } else {
                                    has_air = true;
                                }

                                if (mat != primary_mat) {
                                    non_uniform_mat = true;
                                }
                            }
                        }
                    }

                    if (!has_solid) {
                        result.toc[toc_idx].form = static_cast<uint8_t>(SerializedChunkForm::Empty);
                        result.toc[toc_idx].base_material_id = primary_mat;
                        result.toc[toc_idx].has_material_plane = 0;
                        result.toc[toc_idx].has_color_plane = 0;
                        result.toc[toc_idx].payload_offset = 0;
                        result.toc[toc_idx].payload_size = 0;
                        ++result.empty_chunk_count;
                        continue;
                    }

                    if (!has_air && !non_uniform_mat && !has_painted_color) {
                        result.toc[toc_idx].form = static_cast<uint8_t>(SerializedChunkForm::Solid);
                        result.toc[toc_idx].base_material_id = primary_mat;
                        result.toc[toc_idx].has_material_plane = 0;
                        result.toc[toc_idx].has_color_plane = 0;
                        result.toc[toc_idx].payload_offset = 0;
                        result.toc[toc_idx].payload_size = 0;
                        ++result.solid_chunk_count;
                        continue;
                    }

                    BytePlaneCodec::split_int16_planes(tsdf_buf.data(), low_plane.data(), high_plane.data());
                    const std::vector<uint8_t> comp_low =
                        BytePlaneCodec::compress_plane(low_plane.data(), low_plane.size());
                    const std::vector<uint8_t> comp_high =
                        BytePlaneCodec::compress_plane(high_plane.data(), high_plane.size());
                    std::vector<uint8_t> comp_mat;
                    if (non_uniform_mat) {
                        comp_mat = BytePlaneCodec::compress_plane(mat_buf.data(), mat_buf.size());
                    }

                    std::vector<uint8_t> comp_col_low;
                    std::vector<uint8_t> comp_col_high;
                    if (has_painted_color) {
                        BytePlaneCodec::split_int16_planes(
                            reinterpret_cast<const int16_t*>(col_buf.data()),
                            col_low_plane.data(),
                            col_high_plane.data());
                        comp_col_low = BytePlaneCodec::compress_plane(col_low_plane.data(), col_low_plane.size());
                        comp_col_high = BytePlaneCodec::compress_plane(col_high_plane.data(), col_high_plane.size());
                    }

                    const uint32_t low_sz = static_cast<uint32_t>(comp_low.size());
                    const uint32_t high_sz = static_cast<uint32_t>(comp_high.size());
                    const uint32_t mat_sz = static_cast<uint32_t>(comp_mat.size());
                    const uint32_t col_low_sz = static_cast<uint32_t>(comp_col_low.size());
                    const uint32_t col_high_sz = static_cast<uint32_t>(comp_col_high.size());

                    const std::size_t hdr_bytes = has_painted_color ? 20ULL : 12ULL;
                    const uint32_t payload_offset = static_cast<uint32_t>(result.payload.size());
                    const uint32_t chunk_payload_size =
                        static_cast<uint32_t>(hdr_bytes + low_sz + high_sz + mat_sz + col_low_sz + col_high_sz);

                    result.payload.resize(payload_offset + chunk_payload_size);
                    uint8_t* dst = result.payload.data() + payload_offset;

                    std::memcpy(dst + 0, &low_sz, sizeof(uint32_t));
                    std::memcpy(dst + 4, &high_sz, sizeof(uint32_t));
                    std::memcpy(dst + 8, &mat_sz, sizeof(uint32_t));
                    if (has_painted_color) {
                        std::memcpy(dst + 12, &col_low_sz, sizeof(uint32_t));
                        std::memcpy(dst + 16, &col_high_sz, sizeof(uint32_t));
                    }

                    std::size_t cur_off = hdr_bytes;
                    std::memcpy(dst + cur_off, comp_low.data(), low_sz);
                    cur_off += low_sz;
                    std::memcpy(dst + cur_off, comp_high.data(), high_sz);
                    cur_off += high_sz;
                    if (mat_sz > 0) {
                        std::memcpy(dst + cur_off, comp_mat.data(), mat_sz);
                        cur_off += mat_sz;
                    }
                    if (has_painted_color) {
                        if (col_low_sz > 0) {
                            std::memcpy(dst + cur_off, comp_col_low.data(), col_low_sz);
                            cur_off += col_low_sz;
                        }
                        if (col_high_sz > 0) {
                            std::memcpy(dst + cur_off, comp_col_high.data(), col_high_sz);
                        }
                    }

                    result.toc[toc_idx].form = static_cast<uint8_t>(SerializedChunkForm::Dense);
                    result.toc[toc_idx].base_material_id = primary_mat;
                    result.toc[toc_idx].has_material_plane = non_uniform_mat ? 1U : 0U;
                    result.toc[toc_idx].has_color_plane = has_painted_color ? 1U : 0U;
                    result.toc[toc_idx].payload_offset = payload_offset;
                    result.toc[toc_idx].payload_size = chunk_payload_size;
                    ++result.dense_chunk_count;
                }
            }
        }

        result.active_chunk_count = result.dense_chunk_count + result.solid_chunk_count;
        result.header.payload_checksum = compute_fnv1a64(result.payload.data(), result.payload.size());
        return result;
    }

    [[nodiscard]] static bool save_baked_region_to_file(
        const std::string& filepath,
        const BakedRegionData& baked_data) {

        FILE* fp = std::fopen(filepath.c_str(), "wb");
        if (!fp) {
            return false;
        }

        const bool ok =
            (std::fwrite(&baked_data.header, sizeof(RegionFileHeader), 1, fp) == 1) &&
            (std::fwrite(baked_data.toc.data(), sizeof(RegionTocEntry), RegionKey::kChunksPerRegion, fp) ==
             RegionKey::kChunksPerRegion) &&
            (baked_data.payload.empty() ||
             std::fwrite(baked_data.payload.data(), 1, baked_data.payload.size(), fp) == baked_data.payload.size());

        std::fflush(fp);
        std::fclose(fp);
        return ok;
    }

private:
    [[nodiscard]] static uint64_t compute_fnv1a64(const uint8_t* data, std::size_t len) noexcept {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (std::size_t i = 0; i < len; ++i) {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }
};

} // namespace voxel
