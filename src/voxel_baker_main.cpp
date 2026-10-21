#include "voxel_mesh_baker.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <input.obj> <output.vreg> [options]\n"
              << "Options:\n"
              << "  --scale <float>        Uniform scale multiplier for model vertices (default: 1.0)\n"
              << "  --voxel-scale <float>  Meters per voxel sample (default: 1.0); narrow band scales with it\n"
              << "  --thickness <float>    Shell wall thickness in meters for ruins/walls (default: 1.6)\n"
              << "  --material <name>      Default material name: hard_rock (default), soil, obsidian, ice, lava\n"
              << "  --watertight           Use watertight interior parity mode instead of surface shell mode\n"
              << "  --texture <file.ppm>   Optional PPM texture image to sample via OBJ UVs for per-voxel RGB5551\n"
              << "  --color-depth <float>  Outer surface depth (m) that receives baked color (default: 1.8)\n"
              << "  --center-x <float>     Target X center in region voxels (default: 128.0)\n"
              << "  --center-y <float>     Target Y center in region voxels (default: 128.0)\n"
              << "  --center-z <float>     Target Z center in region voxels (default: 128.0)\n"
              << "  --no-color             Disable per-voxel RGB5551 surface color baking\n"
              << "  --no-center            Do not auto-center model in region\n"
              << "  --rx <int>             Region X coordinate (default: 0)\n"
              << "  --ry <int>             Region Y coordinate (default: 0)\n"
              << "  --rz <int>             Region Z coordinate (default: 0)\n"
              << "  --delta <float>        Narrow-band truncation delta (default: 2.0)\n"
              << "  --help, -h             Show this help message\n";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::string input_obj_path = argv[1];
    std::string output_vreg_path = argv[2];
    std::string texture_ppm_path;

    voxel::AssetBakerConfig cfg{};
    float user_scale = 1.0f;
    bool user_delta = false;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--scale" && i + 1 < argc) {
            user_scale = std::stof(argv[++i]);
        } else if (arg == "--voxel-scale" && i + 1 < argc) {
            const float scale = std::stof(argv[++i]);
            if (!(scale > 0.0f)) {
                std::cerr << "[VoxelBaker] Error: --voxel-scale must be positive\n";
                return EXIT_FAILURE;
            }
            cfg.voxel_scale = scale;
            // The canonical narrow band is 2 voxel samples wide, so in world meters
            // the truncation distance scales with the voxel size.
            if (!user_delta) {
                cfg.truncation_delta = voxel::TsdfFormat::kDefaultTruncationDelta * scale;
            }
        } else if (arg == "--thickness" && i + 1 < argc) {
            cfg.shell_thickness = std::stof(argv[++i]);
        } else if (arg == "--material" && i + 1 < argc) {
            cfg.default_material = voxel::ObjMeshLoader::parse_material_name(argv[++i]);
        } else if (arg == "--watertight") {
            cfg.mode = voxel::VoxelizationMode::WatertightSolid;
        } else if (arg == "--texture" && i + 1 < argc) {
            texture_ppm_path = argv[++i];
        } else if (arg == "--color-depth" && i + 1 < argc) {
            cfg.color_shell_depth = std::stof(argv[++i]);
        } else if (arg == "--center-x" && i + 1 < argc) {
            cfg.target_region_center.x = std::stof(argv[++i]);
        } else if (arg == "--center-y" && i + 1 < argc) {
            cfg.target_region_center.y = std::stof(argv[++i]);
        } else if (arg == "--center-z" && i + 1 < argc) {
            cfg.target_region_center.z = std::stof(argv[++i]);
        } else if (arg == "--no-color") {
            cfg.bake_surface_colors = false;
        } else if (arg == "--no-center") {
            cfg.auto_center_in_region = false;
        } else if (arg == "--rx" && i + 1 < argc) {
            cfg.region_coords.rx = std::stoi(argv[++i]);
        } else if (arg == "--ry" && i + 1 < argc) {
            cfg.region_coords.ry = std::stoi(argv[++i]);
        } else if (arg == "--rz" && i + 1 < argc) {
            cfg.region_coords.rz = std::stoi(argv[++i]);
        } else if (arg == "--delta" && i + 1 < argc) {
            cfg.truncation_delta = std::stof(argv[++i]);
            user_delta = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    std::cout << "[VoxelBaker] Loading 3D model from '" << input_obj_path << "'...\n";
    voxel::RawObjMesh raw_mesh{};
    if (!voxel::ObjMeshLoader::load_from_file(input_obj_path, raw_mesh, cfg.default_material)) {
        std::cerr << "[VoxelBaker] Error: Failed to load .obj from '" << input_obj_path << "'\n";
        return EXIT_FAILURE;
    }

    if (!texture_ppm_path.empty()) {
        if (!raw_mesh.texture.load_ppm(texture_ppm_path)) {
            std::cerr << "[VoxelBaker] Error: Failed to load .ppm texture from '" << texture_ppm_path << "'\n";
            return EXIT_FAILURE;
        }
        std::cout << "[VoxelBaker] Loaded PPM texture '" << texture_ppm_path << "' ("
                  << raw_mesh.texture.width << "x" << raw_mesh.texture.height << ")\n";
    }

    if (user_scale != 1.0f) {
        raw_mesh.transform(user_scale, voxel::Vec3f{0.0f, 0.0f, 0.0f});
    }

    std::cout << "[VoxelBaker] Parsed " << raw_mesh.vertices.size() << " vertices, "
              << raw_mesh.triangles.size() << " triangles.\n"
              << "             AABB: [" << raw_mesh.aabb_min.x << ", " << raw_mesh.aabb_min.y << ", " << raw_mesh.aabb_min.z
              << "] to [" << raw_mesh.aabb_max.x << ", " << raw_mesh.aabb_max.y << ", " << raw_mesh.aabb_max.z << "]\n";

    std::cout << "[VoxelBaker] Voxelizing into 8x8x8 region (256x256x256 voxels, "
              << (256.0f * cfg.voxel_scale) << "m per side at voxel_scale="
              << cfg.voxel_scale << "m)...\n"
              << "             Mode: " << (cfg.mode == voxel::VoxelizationMode::ShellThickness ? "Shell Thickness" : "Watertight Solid")
              << " (thickness=" << cfg.shell_thickness << "m, delta=" << cfg.truncation_delta
              << "m, color_depth=" << cfg.color_shell_depth << "m)\n";

    const auto start_time = std::chrono::steady_clock::now();
    const voxel::BakedRegionData baked = voxel::VoxelMeshBaker::bake_mesh_to_region(raw_mesh, cfg);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    std::cout << "[VoxelBaker] Voxelization complete in " << elapsed << " ms!\n"
              << "             Active chunks: " << baked.active_chunk_count
              << " (Dense: " << baked.dense_chunk_count << ", Solid: " << baked.solid_chunk_count << ")\n"
              << "             Empty chunks:  " << baked.empty_chunk_count << " / 512\n"
              << "             Compressed payload: " << baked.payload.size() << " bytes\n";

    if (!voxel::VoxelMeshBaker::save_baked_region_to_file(output_vreg_path, baked)) {
        std::cerr << "[VoxelBaker] Error: Failed to write .vreg output file to '" << output_vreg_path << "'\n";
        return EXIT_FAILURE;
    }

    const std::uintmax_t file_size = std::filesystem::file_size(output_vreg_path);
    std::cout << "[VoxelBaker] Successfully wrote region file '" << output_vreg_path
              << "' (" << file_size << " bytes on disk).\n";
    return EXIT_SUCCESS;
}
