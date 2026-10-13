#include "register_types.h"
#include "voxel_terrain_3d.h"
#include "voxel_ai_companion.h"

using namespace godot;

void initialize_voxel_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
#if defined(VOXEL_WITH_GODOT_CPP)
    ClassDB::register_class<VoxelTerrain3D>();
    ClassDB::register_class<VoxelAICompanion>();
#else
    VoxelTerrain3D::_bind_methods();
    VoxelAICompanion::_bind_methods();
    ClassRegistryTracker::registered_classes().push_back("VoxelTerrain3D");
    ClassRegistryTracker::registered_classes().push_back("VoxelAICompanion");
#endif
}

void uninitialize_voxel_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
#if !defined(VOXEL_WITH_GODOT_CPP)
    ClassRegistryTracker::registered_classes().clear();
#endif
}

extern "C" {
GDExtensionBool GDE_EXPORT voxel_engine_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization* r_initialization) {
#if defined(VOXEL_WITH_GODOT_CPP)
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_voxel_module);
    init_obj.register_uninitializer(uninitialize_voxel_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
#else
    (void)p_get_proc_address;
    (void)p_library;
    (void)r_initialization;
    initialize_voxel_module(MODULE_INITIALIZATION_LEVEL_SCENE);
    return 1;
#endif
}
}
