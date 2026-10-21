# Ape Statue Assets

`ape.glb` is the **source of truth**. Do not edit the derived files as the
original asset; re-export them from the GLB when it changes.

## Derived files

| File | Derivation |
| :--- | :--- |
| `ape.obj` | Blender export of `ape.glb`, uniformly scaled **5.3333×** (9 m → 48 m), decimated to **28,546 triangles** (0.09 of the source's 317,184), Y-up, feet at `y = 0`, centered on X/Z. Used by the demo's pristine-mesh Option 1. |
| `ape_albedo.png` | The GLB's `texture_0` base-color image (2048 × 2048). |
| `ape_albedo.ppm` | Binary P6 conversion of `ape_albedo.png`, consumed by `voxel_baker --texture`. |
| `ape_statue.vreg` | Watertight voxel bake of `ape.obj` at **0.5 m per voxel**, region center `(160, 160, 128)`, color depth `2.8 m`. Used by the demo's baked-voxel Option 2. |

At `voxel_scale = 0.5`, the 48 m statue spans 96 region samples. Its base sits
at region sample `160 − 24 / 0.5 = 112`. The model is roughly 52.8 m wide and
31.4 m deep, so the demo spaces its pair at `x = ±32`.

The GLB ships ~317k triangles. The OBJ is decimated before baking because the
narrow-band TSDF bake cost grows sharply with triangle density, and the 0.5 m
voxel grid cannot resolve detail finer than the decimated mesh.

## Rebuild pipeline

1. Import `ape.glb` in Blender.
2. Scale the mesh by **5.3333** on all axes and apply the transform.
3. Add a Decimate modifier at ratio **0.09**, apply it, then export `ape.obj`
   with Y-up and UVs.
4. Export the base-color texture as `ape_albedo.png`, then convert it to P6:

   ```text
   magick ape_albedo.png -depth 8 ape_albedo.ppm
   ```

5. Bake the voxel region:

   ```text
   voxel_baker ape.obj ape_statue.vreg --voxel-scale 0.5 --watertight \
       --texture ape_albedo.ppm --color-depth 2.8 \
       --center-x 160 --center-y 160 --center-z 128
   ```

Changing the model scale or placement requires updating the demo's
`APE_REGION_CENTER` / `APE_BASE_LOCAL_Y` / `APE_*_WORLD_CENTER` constants in
`terrain_demo.gd` and the matching stamp-origin math.
