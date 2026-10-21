# 0.5m Voxel Scale: 3D Model Detail Guide & Image-to-3D Generation Prompts

## 1. What Level of Detail Can Models Have at `0.5m` Voxel Scale?

In a **Surface Nets TSDF voxel engine**, the smallest geometry that can be represented without aliasing or holes is governed by the **Nyquist sampling limit of the signed distance field** (`2` to `3` voxel samples across a feature) and the **2-voxel narrow-band truncation delta** (`delta = 2.0 * voxel_scale = 1.0 m` at `voxel_scale = 0.5 m`).

### Geometric & Color Resolution Breakdown (`voxel_scale = 0.5 m`)

| Feature Type | Minimum Physical Size (`1.0 m` Voxels) | Minimum Physical Size (`0.5 m` Voxels) | Recommended Sweet Spot (`0.5 m` Voxels) | Why It Matters in Surface Nets |
| :--- | :--- | :--- | :--- | :--- |
| **Overall Asset Bounding Box** | Up to `250 m × 250 m × 250 m` | Up to **`124 m × 124 m × 124 m`** | **`20 m – 60 m`** tall/wide | Fits inside a single `8 × 8 × 8` chunk `.vreg` region (`256³` samples = `128 m³` minus a `2 m` TSDF border). |
| **Freestanding Pillars, Limbs & Horns** | `2.5 m – 3.0 m` thick | **`1.25 m – 1.5 m`** thick (`2.5–3` voxels) | **`2.0 m+`** thick (`4+` voxels) | Features thinner than `2` voxels (`< 1.0 m`) can pinch or get pruned by the `3×3×3` anti-crumb kernel during CSG edits. |
| **Carved Grooves, Eye Sockets & Runes** | `1.5 m – 2.0 m` wide/deep | **`0.75 m – 1.0 m`** wide/deep (`1.5–2` voxels) | **`1.0 m – 1.5 m`** wide & deep | Surface Nets places one dual vertex per `0.5 m` cell; a groove needs at least `1.5–2` cells (`0.75–1.0 m`) to indent the zero-crossing clearly. |
| **Bevels, Brow Ridges & ChiseledLips** | `1.0 m` step | **`0.5 m`** step (`1` voxel) | **`0.75 m – 1.5 m`** overhang | Because `SurfaceNetsMesher` interpolates edge crossings using 16-bit TSDF gradients, angled planes and bevels remain sharp down to `0.5 m`. |
| **Thin Walls & Ruin Arches (`ShellThickness`)** | `1.6 m – 2.0 m` | **`0.8 m – 1.2 m`** (`--thickness 1.0`) | **`1.0 m – 1.5 m`** | Guarantees a solid negative-TSDF core on both sides of the zero crossing so walls never develop Swiss-cheese holes. |
| **Per-Voxel Baked Color (`RGB5551`)** | `1.0 m` color texel | **`0.5 m` color texel** | Color bands **`≥ 1.0 m`** wide | `VoxelMeshBaker` samples UV/vertex colors per `0.5 m` voxel corner and blends them across the dual cell; high-frequency noise smaller than `0.5 m` is shader-driven via triplanar rock/moss textures. |

### What Works Best vs. What to Avoid at `0.5 m` Scale

* **Works Great (`≥ 0.75 m` features)**:
  * Chunky, monolithic stone statues, guardian golems, totems, temple gates, dragon skulls, dwarven anvils, and stepped ruins.
  * Bold recessed carvings, sunken eye sockets, broad nose bridges, layered armor plates, thick braided stone beards, and mossy crown caps.
  * Solid, closed silhouettes (`WatertightSolid` mode) where arms, weapons, or pillars connect back to the torso or pedestal instead of dangling on thin stalks.
* **Avoid (`< 0.5 m` micro-geometry)**:
  * Paper-thin cloth capes, wire cages, individual chains, thin sword blades (`< 0.5 m` thick), twigs, or separated fingers.
  * Micro-filigree or tiny engravings (`< 0.4 m` wide)—let the triplanar normal map and `chisel_normal_strength` shader handle fine surface roughness instead.

---

## 2. Ready-to-Use Image Generation Prompts (Optimized for Image-to-3D & `0.5m` Voxelization)

When generating a reference image for an **Image-to-3D generator** (such as Trellis, Hunyuan3D-2, Tripo3D, Rodin, or Meshy) that you will convert to `.obj` and bake with `voxel_baker --voxel-scale 0.5 --watertight`, use a prompt that enforces **chunky, watertight, monolithic geometry** and **clean diffuse lighting**.

### Prompt Option A: Ancient Mossy Stone Guardian Golem / Totem (Best All-Around Destructible Landmark)

```text
3D game asset render of a monolithic ancient carved stone guardian statue, stylized chunky proportions designed for voxel destruction, inspired by Donkey Kong Bananza and Zelda Tears of the Kingdom Zonai ruins. Broad chiseled basalt body with thick merged arms folded across a heavy stone chest, deep-set glowing-carved eye sockets, bold geometric brow ridge, wide recessed rune grooves carved into the shoulders, and a thick vibrant green moss cap growing over the flat-topped head and upper shoulders. Watertight solid silhouette with no thin protruding spikes, no dangling wires, and no paper-thin gaps; all features have bold, thick bevels and deep relief cuts. Neutral front-three-quarter orthographic turntable view, isolated on a pure solid mid-gray background, soft even studio diffuse lighting with no harsh cast shadows, crisp PBR stone and moss albedo colors.
```

### Prompt Option B: Dwarven Magma-Core Fortress Shrine (Great for Multi-Material / Obsidian Reactions)

```text
3D game asset render of a chunky monolithic volcanic basalt shrine shaped like a roaring horned lion head carved into a heavy stepped stone pedestal, stylized low-frequency bold geometry for a destructible voxel world. Thick curved stone horns merged solidly into the brow, wide open maw with recessed lava-basin channels, thick blocky fangs, and vibrant green moss and teal lichen patches on the upper cranium and stepped base. Watertight manifold silhouette with thick structural walls, deep chiseled planar facets, and zero thin fragile appendages. Front-three-quarter isometric product render, isolated on a clean neutral gray background, flat studio global illumination, no dramatic rim lighting or heavy shadows, sharp texture detail.
```

### Prompt Option C: Negative Prompt (If Your Generator Supports Negative Prompts)

```text
thin wires, chains, cloth, thin blades, spindly legs, separated thin fingers, hair strands, foliage twigs, floating geometry, hollow paper-thin shells, harsh black shadows, lens flare, depth of field blur, busy background terrain, dramatic dark lighting, motion blur
```

---

## 3. Baking Your Generated Model at `0.5m` Voxel Scale

Once you convert the generated image into a textured `.obj` + `.ppm` texture (using `convert albedo.png -Adjoin albedo.ppm`), bake it at `0.5 m` resolution (`2×` linear detail, `8×` volumetric density) with:

```bash
./build/voxel_baker guardian.obj guardian_05m.vreg \
    --voxel-scale 0.5 \
    --delta 1.0 \
    --scale 1.0 \
    --watertight \
    --texture guardian_albedo.ppm \
    --color-depth 1.2 \
    --center-x 128 --center-y 140 --center-z 128
```

* **`--voxel-scale 0.5`**: Samples the mesh every `0.5 m` across the `256 × 256 × 256` region (`128 m × 128 m × 128 m` physical span).
* **`--delta 1.0`**: Keeps the TSDF narrow band at `2 × voxel_scale = 1.0 m` so `SurfaceNetsMesher` interpolates smooth sub-voxel vertices and normals without step artifacts.
* **`--color-depth 1.2`**: Paints the outer `1.2 m` (`2.4` voxel layers) with the model's `RGB5551` texture colors while leaving the deeper interior core unpainted `kMaterialHardRock` so 3-hit craters expose raw gray stone.
