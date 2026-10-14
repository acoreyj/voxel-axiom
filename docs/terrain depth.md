Terrain depth is controlled by the distance between **bedrock (`y = 0`)** and the **average surface height (`surface_base_height`)**, paired with the vertical chunk ceiling (`world_height_chunks`):

| Desired Depth Profile | `world_height_chunks` (`voxel_terrain_3d.h`) | `surface_base_height` (`procgen_and_surface_nets.hpp`) | `surface_height_threshold` (`voxel_triplanar.gdshader`) | Resulting Underground Depth & Triangle Impact |
| :--- | right | right | right | :--- |
| **Shallow (Fastest)** | `2` (`64 m` ceiling) | `36.0f` | `26.0` | **~36 m** of underground rock (1 cave layer); cuts scene triangles by **~55%** (`~540k` tris at view distance 6). |
| **Default (Current)** | `4` (`128 m` ceiling) | `68.0f` | `56.0` | **~68 m** of underground rock (2 cave layers, `Cy = 0..1`); `1.20M` tris at view distance 6. |
| **Deep Caverns** | `6` (`192 m` ceiling) | `132.0f` | `120.0` | **~132 m** of underground rock (4 cave layers, `Cy = 0..3`). |
| **Abyssal (HLD Max)** | `8` (`256 m` ceiling) | `196.0f` | `184.0` | **~196 m** of underground rock (6 cave layers, `Cy = 0..5`). |

### Where to Change It Right Now

1. **In `procgen_and_surface_nets.hpp` (Line 65)**: Change `float surface_base_height = 68.0f;` to your desired depth in meters/voxels above $y = 0$ bedrock (keep `surface_base_height + surface_amplitude` at least `20 m` below `world_height_chunks * 32` so hilltops never clip the sky ceiling).
2. **In `voxel_terrain_3d.h` (Line 143)**: Set `int world_height_chunks = 4;` to match your total height in 32-meter chunks (`ceil((surface_base_height + 24) / 32)`).
3. **In `voxel_triplanar.gdshader` (Line 8) & `voxel_terrain_demo.tscn**`: Set `surface_height_threshold` to roughly `surface_base_height - 12.0` (so cave walls transition to dark rock below the surface) and place `Camera3D`'s initial `Y` position at `surface_base_height + 16.0`.

### Inspector Sliders (Now Available)

`world_height_chunks` and `surface_base_height` are exposed as range-slider properties on `VoxelTerrain3D`, so terrain depth can be tuned in the Godot Inspector without editing C++ headers:

- **`world_height_chunks`** (`1..12`) — vertical chunk ceiling. Baked into `ChunkStore` when the node enters the tree, so set it before the node starts (i.e. in the scene/Inspector before running).
- **`surface_base_height`** (`16.0..256.0` m) — forwarded to `CaveNoiseGenerator::surface_base_height` at startup and on change; new chunks pick up the new value.

The `surface_height_threshold` shader uniform and the demo camera `Y` still need to be matched manually per the table above.