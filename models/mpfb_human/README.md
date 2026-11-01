# MPFB Human Assets

Human base meshes generated with **MPFB** (MakeHuman Plugin for Blender).
Everything MPFB-related lives in this folder; the project root should stay clean.

## Files

| File | Purpose |
| :--- | :--- |
| `mpfb_human.blend` | Male character, MPFB-generated mesh + rig + clothing. **Primary source.** |
| `mpfb_human_female.blend` | Female character, MPFB-generated mesh + rig + clothing. |
| `mpfb_human.fbx` | Engine-oriented export used for reference/testing. |
| `mpfb_human_*.png`, `*.png` | Texture copies pulled out of the MPFB library (see below). |

## Notes

- The `.blend` files reference most of their textures from the MPFB extension
  install (`.../extensions/.user/blender_org/mpfb/data/...`), not from this
  folder. The local PNGs are kept only as fallbacks/working copies; renaming
  them does not affect the `.blend` files.
- A few duplicate image datablocks still point at
  `models/mpfb_human/mpfb_human.fbm/*` and `models/mpfb_human/BootsAnkle.png`.
  Those paths never existed on disk and the datablocks are unused duplicates of
  the primary material images.
- Godot imports the two `.blend` files as scenes. Nothing in the project
  currently references them by `res://models/mpfb_human/...`, so moving or
  deleting them does not break any scene or script.
