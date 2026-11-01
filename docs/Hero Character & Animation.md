# Hero Character & Animation

This document covers the third-person player character asset, its Mixamo rig, the animation
library, the controller-to-clip mapping, and the caveats that matter when extending it.

The runtime asset is `models/grim-wanderer/grim_wanderer.glb`. Its source folder contains a
fuller asset README with the Blender/Mixamo pipeline details; see
[`models/grim-wanderer/README.md`](../models/grim-wanderer/README.md).

---

## 1. Asset summary

| Property | Value |
| --- | --- |
| Character | Meshy AI "The Grim Wanderer" (`1030192718`) |
| Rig | Mixamo auto-rig, 65 bones, `mixamorig_` prefix |
| Height | 1.902 m |
| Mesh | 1 skinned mesh, 79,752 verts / 79,701 faces |
| Material | 1 baked base-color material |
| Texture | 2048² base color, embedded in the GLB |
| Animations | 64 clips, 30 fps, in-place |
| Runtime file | `grim_wanderer.glb` (~23.6 MB) |
| Source of truth | `grim_wanderer.blend` (kept local; not in git) |
| Interchange | `grim_wanderer.fbx` (kept local; not in git) |

### Imported scene tree

```
Grim_Wanderer2 (Node3D)
  Grim_Wanderer_Rig (Node3D)
    Skeleton3D (65 bones)
      Grim_Wanderer (MeshInstance3D, skinned)
  AnimationPlayer (64 animations)
```

`terrain_demo.gd` loads the GLB with `load(...).instantiate()`, adds it under
`HeroVisualRig`, and finds the `AnimationPlayer` with `find_child`.

### Orientation

- The asset faces **+Z** (toes point +Z in skeleton space).
- The hero controller's forward is **−Z** at `yaw = 0`.
- Therefore the model node is rotated `180°` around Y at load:
  `_hero_model.rotation.y = PI`.
- The rig is 1:1 meters; no scale correction is needed.

### Origin / feet

- The rig origin is the **pelvis**, not the feet.
- At rest, the mesh spans roughly `y = -0.95 .. +0.95`.
- Every Mixamo clip contains hip translation that puts the feet on `y = 0`.
- Consequence: always play an idle clip when spawning; a static instance stands half-buried.
- `HeroVisualRig` is placed at `_player_feet`, so the animated character’s feet line up with
  the SDF ground contact.

---

## 2. Controller integration

### Loading and fallback

`_setup_hero_visual_rig()` builds the old capsule/visor first, then attempts to load the GLB.
If it succeeds, the capsule and visor are hidden and the model is used. If loading fails, a
warning is printed and the capsule remains as the fallback.

### Visibility

- The model is visible only in **Hero Play** and only in third person.
- `V` switches to first person; `_hero_visual_root.visible` becomes false.
- Classic Play and Fly always hide the model.
- The procedural pickaxe viewmodel is exclusive to Classic Play.

### Animation driver

`_update_hero_animations(delta)` runs once per Hero frame after `_process_play`. It:

1. Ticks `_hero_anim_lock` for one-shot actions.
2. Freezes on `bs_hit_reaction` at zero speed while the hero has `FROZEN` or `STUNNED`.
3. Detects hard landings (`_hero_prev_vel_y < -14`) and plays `aa_hard_landing`.
4. Selects a locomotion/idle clip from the hero state, grounded flag, horizontal speed,
   velocity direction relative to yaw, ADS, firing, reload, and Ice Path.
5. Crossfades only when the desired clip changes.

One-shot clips use `_play_hero_action(name, duration, speed)`, which sets a lock so the
locomotion selector does not immediately override them. The lock duration is tuned to the
gameplay timing, not necessarily the raw clip length.

### Clip playback notes

- Imported glTF clips are treated as one-shots by default; the driver sets
  `Animation.LOOP_LINEAR` for looping clips.
- Speeds are tuned to the actual gameplay timings:
  - fire: `bs_firing_rifle` at ~2.0x (0.135 s vs 0.14 s fire interval)
  - reload: `bs_reloading` at ~2.13x (3.3 s clip vs 1.55 s game reload)
  - melee/casts: `bs_toss_grenade` at 4.0–5.8x (stand-in; no dedicated clip)
  - mantle: `aa_cover_to_stand` at 4.4x (matches the 0.34 s controller vault)
- Clips are in-place; no root motion is consumed.
- No skeleton retargeting is needed for this asset; it is already a single Mixamo rig.

---

## 3. Clip → gameplay mapping

### Locomotion

| Gameplay condition | Clip | Loop | Notes |
| --- | --- | --- | --- |
| Idle, no weapon/ADS | `aa_idle` | yes | Default Hero idle |
| Idle while ADS/firing/reloading | `bs_rifle_aiming_idle` | yes | Armed stance |
| Walk forward | `bs_walking` | yes | Speed-scaled |
| Run forward | `bs_rifle_run` | yes | Speed-scaled |
| Sprint | `mgl_standing_sprint_forward` | yes | Speed > 11 m/s |
| Walk backward | `bs_walking_backwards` | yes | Backpedal |
| Run backward | `bs_run_backwards` | yes | Fast backpedal |
| Strafe left | `bs_strafe_left` | yes | Lateral velocity dominant |
| Strafe right | `bs_strafe_right` | yes | Lateral velocity dominant |
| Airborne | `aa_falling_idle` | yes | Also dive/slam fallback |
| Slide | `aa_crouched_sneaking_left` | yes | 1.4x speed |
| Ice Path skating | `mgl_standing_run_forward` | yes | Replaces ground run while active |
| Hard landing | `aa_hard_landing` | no | 2.5x speed, 0.5 s lock |
| Dash | `mgl_standing_sprint_forward` | no | 1.4x, 0.26 s lock |
| Jump | `bs_rifle_jump` | no | 1.2x, 0.55 s lock |
| Dash-jump | `mgl_standing_jump_running` | no | 2.0x, 0.55 s lock |
| Mantle | `aa_cover_to_stand` | no | 4.4x, 0.34 s lock |
| Ground slam dive | `aa_falling_idle` | no | 1.3x, 0.3 s lock |
| Slam impact | `aa_hard_landing` | no | 2.5x, 0.5 s lock |
| Shoulder charge | `mgl_standing_sprint_forward` | no | 1.5x burst + state loop |
| Wall-pin impact | `bs_hit_reaction` | no | 1.0x, 0.5 s lock |
| Damage reaction | `bs_hit_reaction` | no | Triggered by `damage_dealt` |
| Frozen / stunned | `bs_hit_reaction` | held | Speed 0 |

### Combat / weapon

| Event | Clip | Notes |
| --- | --- | --- |
| Carbine shot | `bs_firing_rifle` | ~2.0x; one-shot per accepted shot |
| Reload | `bs_reloading` | ~2.13x; 1.55 s lock |
| Melee swing / lunge | `bs_toss_grenade` | Stand-in at 5.8x |
| Ice Dome cast | `bs_toss_grenade` | Stand-in at 4.0x |
| Spectral Wall cast | `bs_toss_grenade` | Stand-in at 4.0x |
| Cryo Orb throw | `bs_toss_grenade` | Stand-in at 4.0x |
| Hyper Beam start | `bs_firing_rifle` | Short burst; channel loop is idle/rifle |
| Ground Strike | `aa_falling_idle` → `aa_hard_landing` | Dive then impact |

---

## 4. Clip inventory (64)

### Action Adventure (`AA`, 22)

`aa_cover_to_stand`, `aa_cover_to_stand_2`, `aa_crouched_sneaking_left`,
`aa_crouched_sneaking_right`, `aa_falling_idle`, `aa_falling_to_roll`, `aa_hard_landing`,
`aa_idle`, `aa_idle_2`, `aa_idle_3`, `aa_idle_4`, `aa_idle_5`, `aa_jumping_up`,
`aa_left_cover_sneak`, `aa_left_turn`, `aa_right_cover_sneak`, `aa_right_turn`,
`aa_run_to_stop`, `aa_running`, `aa_stand_to_cover`, `aa_stand_to_cover_2`, `aa_walking`.

### Basic Shooter (`BS`, 16)

`bs_firing_rifle`, `bs_hit_reaction`, `bs_reloading`, `bs_rifle_aiming_idle`,
`bs_rifle_jump`, `bs_rifle_run`, `bs_run_backwards`, `bs_strafe`, `bs_strafe_2`,
`bs_strafe_left`, `bs_strafe_right`, `bs_toss_grenade`, `bs_turn_left`,
`bs_turning_right_45_degrees`, `bs_walking`, `bs_walking_backwards`.

### Magic Locomotion (`MGL`, 16)

`mgl_standing_idle`, `mgl_standing_jump`, `mgl_standing_jump_running`,
`mgl_standing_jump_running_landing`, `mgl_standing_land_to_standing_idle`,
`mgl_standing_run_back`, `mgl_standing_run_forward`, `mgl_standing_run_left`,
`mgl_standing_run_right`, `mgl_standing_sprint_forward`, `mgl_standing_turn_left_90`,
`mgl_standing_turn_right_90`, `mgl_standing_walk_back`, `mgl_standing_walk_forward`,
`mgl_standing_walk_left`, `mgl_standing_walk_right`.

### Male Locomotion (`ML`, 10)

`ml_idle`, `ml_jump`, `ml_left_strafe`, `ml_left_strafe_walking`, `ml_left_turn_90`,
`ml_right_strafe`, `ml_right_strafe_walking`, `ml_right_turn_90`, `ml_standard_run`,
`ml_walking`.

Additional clips are available for future directional/turn-in-place blends, cover
transitions, and alternate idles.

---

## 5. Extending or replacing clips

If you add Mixamo clips, keep the asset README’s retarget rule in mind: the character was
auto-rigged from an A-pose, while the animation packs use a T-pose rest. Raw Mixamo actions
must be converted with the per-bone rest-orientation offset (rotation and translation) before
assignment. Dropping a raw action onto the rig produces the “broken arms” pose.

After adding clips:

1. Re-export the GLB from the `.blend` (do not hand-edit the GLB/FBX).
2. Re-import in Godot and confirm the `AnimationPlayer` lists the new names.
3. Add or adjust a mapping in `_update_hero_animations` or a `_play_hero_action` trigger.
4. If a clip is a loop, let the driver set `LOOP_LINEAR`; if one-shot, use an action lock.
5. Keep clips in-place; the controller owns translation and rotation.

---

## 6. Known limitations

- No weapon mesh is included in the character asset. The rifle clips animate empty hands.
- No first-person arms/weapon viewmodel exists for Hero; `V` currently hides the character
  and leaves no first-person body.
- No dedicated melee, cast, death, or hit-reaction-direction clips are used; the driver
  substitutes throw/hit clips where needed.
- The source `.blend` (~97 MB) and `.fbx` (~34 MB) are local-only. Only the runtime GLB and
  its import metadata are committed.
- The character is a single-material baked asset; no normal/roughness maps from the original
  Meshy source are present in the rigged download.
