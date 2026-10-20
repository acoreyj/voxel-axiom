# Easter Island Statue Assets

`easter-isle-low-poly.glb` is the **source of truth**. Do not edit the derived
files as the original asset; re-export them from the GLB when it changes.

## Derived files

| File | Derivation |
| :--- | :--- |
| `easter_island.obj` | Blender export of `easter-isle-low-poly.glb`, uniformly scaled **16×** (3 m → 48 m), Y-up, feet at `y = 0`, centered on X/Z. Used by the demo's pristine-mesh Option 1. |
| `easter_island_albedo.png` | The GLB's `texture_0` base-color image (2048 × 2048). |
| `easter_island_albedo.ppm` | Binary P6 conversion of `easter_island_albedo.png`, consumed by `voxel_baker --texture`. |
| `easter_island_statue.vreg` | Watertight voxel bake of `easter_island.obj` at **0.5 m per voxel**, region center `(160, 160, 128)`, color depth `2.8 m`. Used by the demo's baked-voxel Option 2. |

At `voxel_scale = 0.5`, the 48 m statue spans 96 region samples. Its base sits
at region sample `160 − 24 / 0.5 = 112`. The model is roughly 24.9 m wide and
30.8 m deep, so the demo spaces its pair at `x = ±16`.

## Rebuild pipeline

1. Import `easter-isle-low-poly.glb` in Blender.
2. Scale the mesh by **16** on all axes and apply the transform.
3. Export `easter_island.obj` with Y-up and UVs.
4. Export the base-color texture as `easter_island_albedo.png`, then convert it
   to P6:

   ```text
   magick easter_island_albedo.png -depth 8 easter_island_albedo.ppm
   ```

5. Bake the voxel region:

   ```text
   voxel_baker easter_island.obj easter_island_statue.vreg --voxel-scale 0.5 \
       --watertight --texture easter_island_albedo.ppm --color-depth 2.8 \
       --center-x 160 --center-y 160 --center-z 128
   ```

Changing the model scale or placement requires updating the demo's
`EASTER_ISLAND_REGION_CENTER` / `EASTER_ISLAND_BASE_LOCAL_Y` /
`EASTER_ISLAND_*_WORLD_CENTER` constants in `terrain_demo.gd` and the matching
stamp-origin math.
