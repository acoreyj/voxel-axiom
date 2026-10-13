#pragma once

#if defined(VOXEL_WITH_GODOT_CPP)
#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#else
#include <cstdint>
#include <string>
#include <vector>

// Standalone compatibility shim when compiling unit tests without external godot-cpp headers.
namespace godot {
enum ModuleInitializationLevel {
    MODULE_INITIALIZATION_LEVEL_CORE = 0,
    MODULE_INITIALIZATION_LEVEL_SERVERS = 1,
    MODULE_INITIALIZATION_LEVEL_SCENE = 2,
    MODULE_INITIALIZATION_LEVEL_EDITOR = 3
};

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    constexpr Vector3() = default;
    constexpr Vector3(float px, float py, float pz) : x(px), y(py), z(pz) {}
    constexpr bool operator==(const Vector3&) const noexcept = default;
};

using PackedVector3Array = std::vector<Vector3>;
using String = std::string;

class ClassRegistryTracker {
public:
    static std::vector<std::string>& registered_classes() {
        static std::vector<std::string> classes;
        return classes;
    }
};
} // namespace godot

using GDExtensionBool = uint8_t;
using GDExtensionInterfaceGetProcAddress = void*;
using GDExtensionClassLibraryPtr = void*;
struct GDExtensionInitialization {};
#define GDE_EXPORT
#endif

void initialize_voxel_module(godot::ModuleInitializationLevel p_level);
void uninitialize_voxel_module(godot::ModuleInitializationLevel p_level);

extern "C" {
GDExtensionBool GDE_EXPORT voxel_engine_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization* r_initialization);
}
