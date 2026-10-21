# Atlas Statue Assets

`atlas.glb` is the **source of truth**. Do not edit the derived files as the
original asset; re-export them from the GLB when it changes.

## Derived files

| File | Derivation |
| :--- | :--- |
| `atlas.obj` | Blender export of `atlas.glb`, uniformly scaled **5.3333×** (9 m → 48 m), Y-up, feet at `y = 0`, centered on X/Z. Used by the demo's pristine-mesh Option 1. |
| `atlas_albedo.png` | The GLB's `texture_0` base-color image (2048 × 2048). |
| `atlas_albedo.ppm` | Binary P6 conversion of `atlas_albedo.png`, consumed by `voxel_baker --texture`. |
| `atlas_statue.vreg` | Watertight voxel bake of `atlas.obj` at **0.5 m per voxel**, region center `(160, 160, 128)`, color depth `2.8 m`. Used by the demo's baked-voxel Option 2. |

At `voxel_scale = 0.5`, the 48 m statue spans 96 region samples. Its base sits
at region sample `160 − 24 / 0.5 = 112`.

## Rebuild pipeline

1. Import `atlas.glb` in Blender.
2. Scale the mesh by **5.3333** on all axes and apply the transform.
3. Export `atlas.obj` with Y-up and UVs.
4. Export the base-color texture as `atlas_albedo.png`, then convert it to P6:

   ```text
   magick atlas_albedo.png -depth 8 atlas_albedo.ppm
   ```

5. Re-bake the voxel region:

   ```text
   voxel_baker atlas.obj atlas_statue.vreg --voxel-scale 0.5 --watertight \
       --texture atlas_albedo.ppm --color-depth 2.8 \
       --center-x 160 --center-y 160 --center-z 128
   ```

Changing the model scale or placement requires updating the demo's
`ATLAS_REGION_CENTER` / `ATLAS_BASE_LOCAL_Y` constants in `terrain_demo.gd`
and the matching stamp-origin math.
