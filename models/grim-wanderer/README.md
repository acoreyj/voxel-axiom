# Grim Wanderer (Rigged, Mixamo) Assets

`grim_wanderer.blend` is the **source of truth** for the rig and animation
library. Do not edit `grim_wanderer.glb` / `grim_wanderer.fbx`; re-export them
from the `.blend`.

The character is "The Grim Wanderer" (Meshy AI asset `1030192718`), auto-rigged
on Mixamo, with 64 animation clips pulled from four Mixamo animation packs.

## Files

| File | Size | Derivation |
| :--- | ---: | :--- |
| `grim_wanderer.blend` | 96.7 MB | Blender source: skinned mesh, 65-bone Mixamo rig, 64 actions, base-colour texture packed (2048²). |
| `grim_wanderer.glb` | 22.5 MB | glTF 2.0 binary export: 1 skinned mesh, 1 skin (65 joints), 64 animations, texture embedded. Runtime format for Godot. |
| `grim_wanderer.fbx` | 33.8 MB | FBX 7.4 binary export: skinned mesh + 64 takes (`bake_anim_use_all_actions`), texture embedded. For DCC / non-glTF pipelines. |

All three are generated from the same scene (`Grim_Wanderer`) in the `.blend`.

## Character

| Property | Value |
| :--- | :--- |
| Source | Meshy AI "The Grim Wanderer", asset id `1030192718` |
| Mesh objects | 1 (`Grim_Wanderer`) |
| Vertices / faces | 79,752 verts / 79,701 faces (76,439 quads, 2,201 triangles) |
| Materials | 1 (`BakedMaterialmat`), Principled BSDF |
| Textures | 2048² base colour, packed. Original Meshy export also had 4096² normal / roughness / metallic maps, but the **Mixamo rigged download carries base colour only** |
| UVs | 1 map (`UVMap`) |
| Height | 1.902 m, facing **−Y** in Blender (Y-up / −Z forward in the exports) |
| Rig | Mixamo auto-rig, 65 bones, `mixamorig:` name prefix |
| Weighting | Mixamo auto-weights, 52 vertex groups, 4 influences/vertex, normalised |
| Extra data | no shape keys, no IK controls, no facial bones, no physics |

### Skeleton

```
Hips
└ Spine → Spine1 → Spine2
  ├ Neck → Head → HeadTop_End
  └ Left/Right Shoulder → Arm → ForeArm → Hand
      └ (per hand) Thumb1-4, Index1-4, Middle1-4, Ring1-4, Pinky1-4
Hips
└ Left/Right UpLeg → Leg → Foot → ToeBase → Toe_End
```

Bone names are the standard Mixamo set (`mixamorig:Hips`, `mixamorig:LeftForeArm`,
…), so the rig is directly compatible with the Mixamo Rig addon and with any
Mixamo FBX downloaded for this character.

### Origin / rest pose (important)

The Mixamo rig's rest origin sits at the **pelvis**, not the feet: at rest the
mesh spans `z = −0.951 … 0.952`. Every animation clip contains hip translation
that lifts the character so the **feet land on `y = 0` while a clip is playing**.

Consequence: a spawned instance with no animation playing stands half-buried.
Play an idle clip (or offset the node by `−0.95 m`) when spawning.

## Animation library

64 clips, 30 fps, all on the `Grim_Wanderer_Rig` armature. Action names are
snake_case (`<pack>_<clip>`) for engine use; the original Mixamo file name is in
the second column.

### `AA` — Action Adventure Pack (22 clips)

| Action | Source clip | Frames | Length |
| :--- | :--- | ---: | ---: |
| `aa_cover_to_stand` | cover to stand | 1–46 | 1.50 s |
| `aa_cover_to_stand_2` | cover to stand (2) | 1–43 | 1.40 s |
| `aa_crouched_sneaking_left` | crouched sneaking left | 1–39 | 1.27 s |
| `aa_crouched_sneaking_right` | crouched sneaking right | 1–34 | 1.10 s |
| `aa_falling_idle` | falling idle | 1–22 | 0.70 s |
| `aa_falling_to_roll` | falling to roll | 1–55 | 1.80 s |
| `aa_hard_landing` | hard landing | 1–61 | 2.00 s |
| `aa_idle` | idle | 1–119 | 3.93 s |
| `aa_idle_2` | idle (2) | 1–301 | 10.00 s |
| `aa_idle_3` | idle (3) | 1–99 | 3.27 s |
| `aa_idle_4` | idle (4) | 1–75 | 2.47 s |
| `aa_idle_5` | idle (5) | 1–69 | 2.27 s |
| `aa_jumping_up` | jumping up | 1–8 | 0.23 s |
| `aa_left_cover_sneak` | left cover sneak | 1–35 | 1.13 s |
| `aa_left_turn` | left turn | 1–32 | 1.03 s |
| `aa_right_cover_sneak` | right cover sneak | 1–35 | 1.13 s |
| `aa_right_turn` | right turn | 1–32 | 1.03 s |
| `aa_run_to_stop` | run to stop | 1–27 | 0.87 s |
| `aa_running` | running | 1–22 | 0.70 s |
| `aa_stand_to_cover` | stand to cover | 1–35 | 1.13 s |
| `aa_stand_to_cover_2` | stand to cover (2) | 1–39 | 1.27 s |
| `aa_walking` | walking | 1–32 | 1.03 s |

### `BS` — Basic Shooter Pack (16 clips)

| Action | Source clip | Frames | Length |
| :--- | :--- | ---: | ---: |
| `bs_firing_rifle` | firing rifle | 1–9 | 0.27 s |
| `bs_hit_reaction` | hit reaction | 1–14 | 0.43 s |
| `bs_reloading` | reloading | 1–100 | 3.30 s |
| `bs_rifle_aiming_idle` | rifle aiming idle | 1–94 | 3.10 s |
| `bs_rifle_jump` | rifle jump | 1–19 | 0.60 s |
| `bs_rifle_run` | rifle run | 1–23 | 0.73 s |
| `bs_run_backwards` | run backwards | 1–17 | 0.53 s |
| `bs_strafe` | strafe | 1–21 | 0.67 s |
| `bs_strafe_2` | strafe (2) | 1–17 | 0.53 s |
| `bs_strafe_left` | strafe left | 1–31 | 1.00 s |
| `bs_strafe_right` | strafe right | 1–44 | 1.43 s |
| `bs_toss_grenade` | toss grenade | 1–97 | 3.20 s |
| `bs_turn_left` | turn left | 1–29 | 0.93 s |
| `bs_turning_right_45_degrees` | turning right 45 degrees | 1–31 | 1.00 s |
| `bs_walking` | walking | 1–42 | 1.37 s |
| `bs_walking_backwards` | walking backwards | 1–43 | 1.40 s |

### `MGL` — Magic Locomotion Pack (16 clips)

| Action | Source clip | Frames | Length |
| :--- | :--- | ---: | ---: |
| `mgl_standing_idle` | standing idle | 1–52 | 1.70 s |
| `mgl_standing_jump` | Standing Jump | 1–72 | 2.37 s |
| `mgl_standing_jump_running` | Standing Jump Running | 1–33 | 1.07 s |
| `mgl_standing_jump_running_landing` | Standing Jump Running Landing | 1–42 | 1.37 s |
| `mgl_standing_land_to_standing_idle` | Standing Land To Standing Idle | 1–34 | 1.10 s |
| `mgl_standing_run_back` | Standing Run Back | 1–20 | 0.63 s |
| `mgl_standing_run_forward` | Standing Run Forward | 1–23 | 0.73 s |
| `mgl_standing_run_left` | Standing Run Left | 1–24 | 0.77 s |
| `mgl_standing_run_right` | Standing Run Right | 1–24 | 0.77 s |
| `mgl_standing_sprint_forward` | Standing Sprint Forward | 1–18 | 0.57 s |
| `mgl_standing_turn_left_90` | Standing Turn Left 90 | 1–56 | 1.83 s |
| `mgl_standing_turn_right_90` | Standing Turn Right 90 | 1–48 | 1.57 s |
| `mgl_standing_walk_back` | Standing Walk Back | 1–37 | 1.20 s |
| `mgl_standing_walk_forward` | Standing Walk Forward | 1–35 | 1.13 s |
| `mgl_standing_walk_left` | Standing Walk Left | 1–36 | 1.17 s |
| `mgl_standing_walk_right` | Standing Walk Right | 1–37 | 1.20 s |

### `ML` — Male Locomotion Pack (10 clips)

| Action | Source clip | Frames | Length |
| :--- | :--- | ---: | ---: |
| `ml_idle` | idle | 1–251 | 8.33 s |
| `ml_jump` | jump | 1–66 | 2.17 s |
| `ml_left_strafe` | left strafe | 1–21 | 0.67 s |
| `ml_left_strafe_walking` | left strafe walking | 1–32 | 1.03 s |
| `ml_left_turn_90` | left turn 90 | 1–29 | 0.93 s |
| `ml_right_strafe` | right strafe | 1–21 | 0.67 s |
| `ml_right_strafe_walking` | right strafe walking | 1–32 | 1.03 s |
| `ml_right_turn_90` | right turn 90 | 1–29 | 0.93 s |
| `ml_standard_run` | standard run | 1–23 | 0.73 s |
| `ml_walking` | walking | 1–32 | 1.03 s |

All actions have a fake user so they survive save/reload, and one action
(`ml_idle`) is assigned to the rig for preview.

## The A-pose / T-pose retarget (read before adding clips)

The asset went through two Mixamo downloads that **do not share a rest pose**:

* the **rigged character** was auto-rigged from an **A-pose** upload, so its
  rest upper arm points ~46° down;
* the **animation packs** are authored on Mixamo's **T-pose** rest, and their
  FBX files are Y-up internally while the character rig is Z-up.

Assigning a raw Mixamo action to this rig therefore adds the 46° a second time:
the arms swing inward and the elbows meet at the belt. This is exactly the
"broken arms" state.

Every action in this file was converted with a per-bone rest-orientation offset,
applied to rotation *and* translation curves:

```
L_target = R_target⁻¹ · R_source · L_source
```

where `R` is the bone's rest orientation relative to its parent (the root bone
uses the full world rest orientation, which absorbs the Y-up/Z-up difference).
After conversion the target reproduces Mixamo's original playback pose.

> **Adding new Mixamo clips:** they must be converted the same way. Import the
> clip's FBX, then apply the offset to every `pose.bones[...].rotation_quaternion`
> and `location` curve before assigning the action to `Grim_Wanderer_Rig`.
> Dropping a raw action on the rig will reproduce the broken-arm pose.

## Verification

| Check | Result |
| :--- | :--- |
| Converted pose vs untouched Mixamo playback (`ml_idle`) | worst bone 6.4 mm, mean 6.2 mm |
| Same check for `mgl_standing_jump`, `aa_running`, `bs_rifle_run` | worst 6.3 mm |
| All 64 clips swept at start/middle/end | 64/64 sane, heights 0.90–1.92 m, no exploded poses |
| Deformation | 45° shoulder rotation moves ~0.39 m of vertices; 0 unweighted verts |
| GLB | glTF 2.0, 1 mesh, 1 skin / 65 joints, 64 animations, 1 embedded PNG |
| FBX | re-imported: 65 bones, 79,752 verts, 64 takes |

The ~6 mm residual is the A-pose vs T-pose difference in bone chain offsets, not
a retarget error — it is invisible at any normal viewing distance.

## Rebuild pipeline

Original artwork and Mixamo downloads live in `%USERPROFILE%\Downloads`:

```
Meshy_AI_The_Grim_Wanderer_1030192718_texture_obj\   Meshy OBJ + MTL + 8192² PNG
  Action Adventure Pack.zip     rigged character FBX + 22 clips
  Basic Shooter Pack.zip        rigged character FBX + 16 clips
  Magic Locomotion Pack.zip     rigged character FBX + 16 clips
  Male Locomotion Pack.zip      rigged character FBX + 10 clips
Meshy_AI_Character_mixamo_upload_tpose.zip           T-pose OBJ upload package
```

1. **Upload** (only needed for a new character): pack `obj` + `mtl` + textures
   into a ZIP for Mixamo's auto-rigger. The `usemtl` name inside the `.obj` must
   match `newmtl` in the `.mtl`, otherwise Mixamo renders the character grey.
2. **Download** the animation packs; each ZIP repeats the rigged character FBX
   plus one FBX per clip.
3. **Import** the character FBX. Mixamo exports come in at **1/100 scale**
   (armature object scale `0.01`) — set the armature scale to `1.0`, then
   unparent the mesh, apply rotation+scale on both, and re-parent.
4. **Convert** every clip with the rest-offset formula above, rename actions to
   `<pack>_<clip>` and give them a fake user.
5. **Export GLB** (what this folder contains):

   ```python
   bpy.ops.export_scene.gltf(
       filepath="grim_wanderer.glb", export_format='GLB', use_selection=True,
       export_apply=False, export_yup=True,
       export_animations=True, export_animation_mode='ACTIONS',
       export_nla_strips=False, export_anim_single_armature=True,
       export_optimize_animation_size=False, export_anim_slide_to_zero=True,
       export_skins=True, export_image_format='AUTO')
   ```

6. **Export FBX** (interchange):

   ```python
   bpy.ops.export_scene.fbx(
       filepath="grim_wanderer.fbx", use_selection=True,
       object_types={'ARMATURE', 'MESH'}, use_mesh_modifiers=True,
       add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=True,
       bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True,
       apply_scale_options='FBX_SCALE_NONE', axis_forward='-Z', axis_up='Y',
       path_mode='COPY', embed_textures=True)
   ```

Selection for both exports: `Grim_Wanderer_Rig` + `Grim_Wanderer`.

## Godot usage notes

* Import `grim_wanderer.glb`; Godot exposes all 64 clips as animations on the
  `AnimationPlayer` with the same snake_case names (`anim.play("ml_idle")`).
* The mesh is skinned to the single 65-joint skeleton; no retarget step is
  needed for this asset.
* Clips are in-place (hip translation only inside the skeleton, no separate root
  motion node). Drive movement from the character controller.
* Scale: 1 unit = 1 m. Height 1.9 m.
* Spawn with an idle playing (see *Origin / rest pose*), otherwise the rest
  position sits at the pelvis.

## Known limitations

* Single material / single 2048² base-colour texture — normal, roughness and
  metallic maps from the Meshy source are not part of the rigged download.
* No finger, face or IK controls; the skeleton is the plain Mixamo deform rig.
* Clips are baked at 30 fps with a key per frame, which is why the `.fbx` is the
  largest file in the folder.
