# Gameplay Systems & Hero Sandbox

This document describes the gameplay layer built on top of the voxel engine: the two play
modes, the SDF-driven hero controller, the GAS-lite ability system, the ricochet carbine,
temporary voxel constructs, the combat bot, the ability telegraphs, and the Grim Wanderer
character integration.

It is the top-level reference for the demo layer. The engine internals (chunks, streaming,
meshing, persistence, navigation) are documented in the design docs listed at the end.

---

## 1. Modes

| Mode | Enter | Exit | Camera | Controller |
| --- | --- | --- | --- | --- |
| **Fly** | default / `F` / `H` | — | Free camera | Camera + terrain tools |
| **Classic Play** | `F` from Fly | `F` | First person | Original pickaxe body |
| **Hero Play** | `H` from Fly | `H` | Third person default (`V` toggles first person) | `VoxelHeroController`-equivalent inline controller + Grim Wanderer model |
| **Benchmark** | `M` or `F8` | `M` / `F8` / `Esc` | Scripted | Automated streaming/CSG/nav passes |

`F` and `H` are independent toggles. Pressing `F` while in Hero Play drops Hero and enters
Classic Play; pressing `H` while in Classic Play enters Hero. Both `_enter_*` paths restore
the camera priorities and pickup the player from the current feet position.

### Controls

| Input | Action |
| --- | --- |
| `F` | Toggle Fly / Classic Play |
| `H` | Toggle Fly / Hero Play |
| `V` | Toggle third-person / first-person in Hero Play |
| `M` / `F8` | Start or abort the benchmark suite |
| `W A S D` | Move / run |
| `Shift` | Fly boost / Classic sprint / Hero stamina dash |
| `Space` | Fly up; jump in Classic; jump / mantle / dash-jump in Hero |
| `Ctrl` / `C` | Hero slope slide |
| `Q` | Hero Ground Strike (airborne dive or in-place ground pound) |
| `E` | Hero Kelvin Ice Path toggle |
| `G` | Hero Abrams Shoulder Charge (wall-pin / breach) |
| `Z` | Hero Bebop Hyper Beam |
| `X` | Hero Cryo Bounce Orb |
| `1` | Fly: add terrain; Hero: Kelvin Frozen Shelter |
| `2` | Fly: paint; Hero: McGinnis Spectral Wall |
| `3`–`0` | Fly terrain/element tools |
| `Shift + 4` | Rolling boulder mode |
| `LMB` | Fly: active tool; Classic: pickaxe swing; Hero: hold to fire |
| `RMB` | Hero: hold for ADS; Fly/Classic: companion path / placement rules |
| `R` | Hero: reload; other modes: reset statues (`Shift+R` full reset) |
| `M` / `MMB` | Hero: melee lunge |
| `K` | Toggle Sparring Combat Bot AI |
| `J` | Cycle 3D ability telegraph previews |
| `B` / `Shift+B` | Stamp timed / permanent asset |
| `T` | Toggle add/paint material |
| `P` | Place companion |
| `F1` | Show/hide controls overlay |
| `U` | Hide all UI |
| `Z` / `Ctrl+Z` | Undo (plain `Z` is Hyper Beam in Hero; `Ctrl+Z` still undoes) |
| `Y` | Redo |
| `Esc` | Release/recapture mouse |

---

## 2. Hero Play controller

The hero controller runs in `terrain_demo.gd` (`_process_play`, `_process_classic`,
`_update_hero_camera_and_visuals`, `_update_hero_animations`). It shares the
`VoxelHeroController` design but is wired directly into the demo HUD/abilities.

### 2.1 Body and camera

| Setting | Default | Notes |
| --- | --- | --- |
| `play_body_radius` | `0.45 m` | SDF collision radius |
| `play_body_height` | `1.85 m` | Head clearance |
| `play_eye_height` | `1.65 m` | Camera pivot |
| `play_walk_speed` | `6.8 m/s` | Ground target speed |
| `play_run_speed` | `9.5 m/s` | Hero run/sprint reference |
| `play_jump_speed` | `8.4 m/s` | Jump impulse |
| `play_gravity` | `24 m/s²` | Hero gravity |
| `play_step_height` | `0.60 m` | Step/ledge threshold |
| `play_fov` | `78°` | Base Hero FOV |
| `play_cam_arm_length` | `4.2 m` | Third-person spring arm |
| `play_cam_shoulder_offset` | `(0.65, 0.35, 0)` | Shoulder offset |
| ADS FOV / arm | `55°` / `2.55 m` | Lerped while RMB held |
| First person | `V` | Camera at eye height; model hidden |

Camera anti-clipping uses `raycast_terrain` plus `PhysicsServer3D` rays from the shoulder
pivot, then `sample_sdf(cam_pos)` and `sample_sdf_normal(cam_pos)` to push out of concave
voxel walls before meshes commit.

### 2.2 Movement and collision

- Three body probes (waist, chest, head) read `sample_sdf`.
- When a probe is within `play_body_radius`, the hero is pushed along the horizontal
  `sample_sdf_normal` and velocity is projected onto the wall tangent.
- Horizontal motion also uses `raycast_terrain` + a physics ray at two heights to block
  walls and pristine statue colliders.
- Ground contact uses a downward `raycast_terrain` probe with step height, walkable-slope
  limit, coyote time, and a fall-safety teleport.
- Ceiling bumps use `sample_sdf` above the head.

### 2.3 State machine

| State | Trigger | Behavior |
| --- | --- | --- |
| `GROUNDED` | On ground | Normal locomotion, dash/slide transitions |
| `AIRBORNE` | Left ground / jump / dash-jump | Gravity, air strafe, fall |
| `DASHING` | `Shift`, consumes a stamina pip | `18.5 m/s` for `0.26 s` |
| `SLIDING` | `Ctrl`/`C` on ground | Downhill acceleration, `17.5 m/s` cap, lower camera |
| `MANTLING` | `Space` into a rim / auto when blocked in air | 3-probe mantle: wall face, ledge top, headroom; `0.34 s` pull-up + vault |
| `GROUND_SLAM` | `Q` | Dive at `28 m/s`, impact resolves elemental combo |
| `SHOULDER_CHARGE` | `G` | `16 m/s`, `1.15 s`, wall-pin / breach / glance |

Stamina: 3 charges, `2.4 s` per pip; dash-jump window `0.05–0.24 s` into the dash.

### 2.4 Ledge mantle probes

1. Chest ray (`raycast_terrain`) finds a steep wall (`|normal.y| <= 0.62`).
2. Downward probe past the lip finds a walkable ledge (`0.65–2.35 m` high,
   `normal.y >= 0.65`).
3. Waist and head `sample_sdf` checks require `>= 0.75 * radius` clearance.
4. The controller lerps the feet through a two-stage pull-up/vault and applies a forward
   exit boost.

### 2.5 Elemental underfoot

`get_material_at(feet)` drives:

| Material | Effect |
| --- | --- |
| Ice (`6`) | ~80% less ground/slide friction, +20% slide cap |
| Lava (`3`) | `9.2 m/s` thermal hop and Burning status |
| Water (`4`) | Wading drag; cleanses Burning |
| Soil/Rock/etc. | Normal movement |

### 2.6 Kelvin Ice Path

`E` toggles an ice-path emitter. Every `1.1 m` of travel it calls
`trigger_elemental_capsule(last_anchor, current_anchor, 3.2, Frost)`, pitched by the camera
look angle, producing a continuous 31°-ramped ice bridge. The GAS buff grants `+18 Spirit`
and `+24%` movement speed while active.

### 2.7 Animation driver

The third-person model uses the Grim Wanderer Mixamo clips (see
[Hero Character & Animation](Hero%20Character%20&%20Animation.md)). Highlights:

| Situation | Clip |
| --- | --- |
| Relaxed idle | `aa_idle` |
| Armed/ADS/firing idle | `bs_rifle_aiming_idle` |
| Walk | `bs_walking` |
| Run | `bs_rifle_run` |
| Sprint | `mgl_standing_sprint_forward` |
| Backpedal | `bs_walking_backwards` / `bs_run_backwards` |
| Strafe | `bs_strafe_left` / `bs_strafe_right` |
| Fall / dive | `aa_falling_idle` |
| Slide | `aa_crouched_sneaking_left` |
| Mantle | `aa_cover_to_stand` at high playback speed |
| Land / slam | `aa_hard_landing` |
| Fire | `bs_firing_rifle` |
| Reload | `bs_reloading` |
| Melee / casts | `bs_toss_grenade` (stand-in) |
| Damage / pin / frozen | `bs_hit_reaction` |

One-shots are guarded by `_hero_anim_lock`; looping clips are selected by state, speed,
local velocity direction, ADS, reload, and status tags.

---

## 3. Terrain query APIs

These are the C++ `VoxelTerrain3D` methods used by the gameplay layer.

| Method | Returns | Use |
| --- | --- | --- |
| `sample_sdf(world_pos)` | `float` | Trilinear SDF in world meters (negative solid) |
| `sample_sdf_normal(world_pos)` | `Vector3` | Unit surface gradient |
| `get_material_at(world_pos)` | `int` | Nearest solid material ID |
| `get_damage_at(world_pos)` | `float` | Crack damage ratio `[0,1]` |
| `raycast_terrain(from, to)` | `Vector3` | First surface hit, `+INF` on miss |
| `raycast_terrain_material(from, to)` | `int` | Material at first hit, `-1` on miss |
| `raycast_terrain_normal(from, to)` | `Vector3` | SDF normal at hit |
| `query_sphere_materials(center, radius)` | `PackedInt32Array` | 8-bin material histogram |
| `get_dominant_material_in_sphere(center, radius)` | `int` | Most common non-air material |
| `sweep_sphere(from, to, radius)` | `Vector3` | Swept-sphere contact or `+INF` |
| `measure_wall_thickness(hit, dir, max_t)` | `float` | Wall thickness or `max_t + 1` |
| `carve_capsule(from, to, radius)` | `void` | Swept capsule carve |
| `add_capsule_with_material(from, to, radius, mat)` | `void` | Swept capsule fill (`mat 1..7`) |
| `trigger_elemental_capsule(from, to, radius, kind)` | `void` | Swept elemental reaction |
| `spawn_temporary_dome(center, radius, thickness, mat, ttl)` | `int` | Hollow TTL shell |
| `spawn_temporary_wall(from, to, height, thickness, mat, ttl)` | `int` | Oriented TTL slab |
| `get_structure_remaining_ttl(id)` | `float` | Seconds left, `0` expired/dismissed |
| `stamp_asset(vreg, chunk_coords, recreate_delay)` | `int64` | Stamp baked `.vreg` |
| `remove_structure(id)` | `bool` | Restore pre-stamp baseline or dismiss construct |

Engine notes:

- The SDF is stored as normalized `int16` per 32³ chunk; `sample_sdf` decodes and
  trilinearly interpolates, then converts to world meters.
- Raycasts are DDA-style lattice walks; unloaded chunks fall back to the procedural
  density field.
- Capsule CSG uses `SphereCsgTerm::closest_axis_point`, so fill/carve/elemental logic is
  the same code as spheres, just with an axis segment.
- Swept sphere contact returns `center - normal * sdf`, not a thin ray hit.
- Temporary constructs use per-voxel delta backups (`ConstructVoxelBackup`) rather than
  whole-chunk snapshots, so battle damage survives expiration.

---

## 4. GAS-lite ability system

`voxel_ability_system.gd` defines `VoxelAbilitySystem` (`extends Node`).

### 4.1 Tags and attributes

Tags are a bitmask: `GROUNDED`, `AIRBORNE`, `STUNNED`, `SILENCED`, `ROOTED`, `UNSTOPPABLE`,
`CHANNELING`, `BURNING`, `CHILLED`, `FROZEN`, `ICE_SURGE`.

`HeroAttributes` holds base values plus modifier stacks:

| Attribute | Base |
| --- | --- |
| Max health | `750` |
| Spirit power | `42` |
| Weapon damage | `24` |
| Move-speed multiplier | `1.0` |
| Cooldown reduction | `0.15` (15%) |
| Spirit resistance | `0.10` |
| Bullet resistance | `0.10` |

Spirit power scales both damage and voxel radii:
`radius = base_voxel_radius + spirit * spirit_radius_scale`.

### 4.2 Status effects

`StatusEffectSpec` supports timed buffs, damage-over-time, stacking slows, and hard CC:

| Effect | Behavior |
| --- | --- |
| `Burning` | DoT; applied by Lava, cleansed by Water |
| `Chilled` | Stacking slow; 3 stacks promote to Frozen |
| `Frozen` | `2.2 s` ice shell; movement/abilities locked |
| `Wall-Pin Stun` | `1.5 s`; movement/abilities locked |
| `Ice Surge` | `+18 Spirit`, `+24%` move speed |

Frozen stamps a destructible `2.4 m` Ice shell. If the shell is destroyed (explosion or
Lava, detected via `get_dominant_material_in_sphere != Ice`), the target breaks free early.

### 4.3 Abilities

| Key | Ability ID | Mode | CD | Behavior |
| --- | --- | --- | --- | --- |
| `E` | `kelvin_ice_path` | Toggle channel | `1.0 s` | Frost capsule bridge + Ice Surge |
| `G` | `abrams_shoulder_charge` | Movement charge | `5.5 s` | Capture target; `110 + 1.5*Spirit` wall-pin damage; +50% on cracked walls; stun |
| `Q` | `lash_ground_strike` | Airborne/ground slam | `4.5 s` | Dive or ground pound; elemental combo |
| `Z` | `bebop_hyper_beam` | Hitscan channel | `9.0 s` | `2.6 s` lava trench, 11 Hz voxel edits, Burning |
| `X` | `cryo_bounce_orb` | Arc projectile | `4.0 s`, 2 charges | SDF ricochet orb; Chilled/Frozen |
| `1` | `kelvin_frozen_shelter` | Temporary construct | `12.0 s` | Hollow `8.5 m` Ice Dome, `6.5 s` TTL |
| `2` | `mcginnis_spectral_wall` | Temporary construct | `9.0 s` | `14 x 6.5 x 2.2 m` Hard Rock wall, `7.5 s` TTL |

Constructs are dismissed early by pressing the same key again. The HUD cooldown bar shows
live TTL countdowns and active buffs.

### 4.4 Ground Strike elemental combos

`resolve_ground_strike_combo(hit, targets)` queries
`get_dominant_material_in_sphere(hit, radius * 0.7)`:

| Dominant material | Combo | Effect |
| --- | --- | --- |
| Lava (`3`) | Magma Eruption | `+35%` radius, Burning |
| Ice (`6`) | Cryo Shatter Nova | Frozen |
| Water (`4`) | Hydro Geyser | Water splash / knock-up |
| Soil/Rock | Seismic Crater | Standard crater and damage |

`Q` now works from the ground as an in-place pound (small hop into the dive) as well as
from the air.

---

## 5. Primary weapon

`WeaponSpec` defaults:

| Field | Value |
| --- | --- |
| Name | Pulse Carbine |
| Magazine | 24 |
| Fire interval | `0.14 s` (~7.1 rounds/s) |
| Reload | `1.55 s` |
| Bullet damage | 24 |
| Headshot multiplier | 1.85 |
| Ricochet multiplier | 1.25 |
| Max ricochets | 2 |
| Bullet chip radius | 0.95 m |

### 5.1 Firing loop

`fire_ricochet_weapon(muzzle, aim_origin, aim_dir, max_dist, is_sliding, targets)`:

1. Blocks if Stunned/Frozen (unless Unstoppable), cooling down, or reloading.
2. Consumes ammo unless sliding; auto-starts reload at 0.
3. Raycasts terrain; tests each target capsule before the wall hit.
4. On terrain hit: spawns a 0.95 m explosion, reads `sample_sdf_normal`, reflects the
   direction, and repeats up to 2 bounces.
5. On target hit: applies headshot/ricochet/shatter multipliers and deals ability damage.
6. Emits `weapon_fired`; records tracer segments for the 3D renderer.

### 5.2 Headshots and trickshots

- Upper 32% of the target capsule = `1.85x` headshot (red tracer).
- Any hit after one or more wall bounces = `+25%` (cyan tracer).
- Frozen or Wall-Pin Stunned target = another `+25%` shatter bonus.
- Slide (`Ctrl`/`C`, including on an Ice Path) = zero ammo and no reload lockout.

### 5.3 ADS and melee

- Hold RMB: FOV lerps `78° → 55°`, camera arm `4.2 → 2.55 m`, shoulder offset to `0.52`.
- `R`: reload (`Shift+R` resets statues).
- `M` / middle click: lunge at `4.5 m/s` plus pickaxe swing (0.55 s, 1.5 m chip).
- Tracers: ImmediateMesh lines, `0.16 s` TTL, yellow normal / red crit / cyan ricochet.

The Grim Wanderer GLB has no weapon mesh; the third-person character plays rifle clips
without a visible gun. The first-person Hero view currently has no arms/weapon model.

---

## 6. Temporary voxel constructs

Implemented in `include/asset_stamp_manager.hpp` and `src/voxel_terrain_3d.cpp`.

### 6.1 Spawning

`spawn_temporary_dome(center, radius, wall_thickness, material, duration)`:

- SDF: `max(abs(distance(center) - r_mid) - half_thickness, base_plane - y)`.
- Interior remains air; shell is solid material; `radius` is world meters.
- `r_mid = max(half_t, r_outer - half_t)`.

`spawn_temporary_wall(from, to, height, thickness, material, duration)`:

- Oriented slab around the segment `from -> to`.
- Horizontal distance to the segment minus half thickness; vertical clamp to height.

Both use `AssetStampManager::stamp_procedural_sdf`, which records
`ConstructVoxelBackup{linear_index, pre_tsdf, pre_material, pre_damage, pre_color}` only for
samples the construct actually modified.

### 6.2 Expiration and dismissal

- TTL is decremented in `process_reconstitution(delta)` each frame.
- On TTL <= 0 or `remove_structure(id)`, `revert_structure_instance_unlocked` restores only
  backup voxels.
- A voxel that was solid before the construct but was carved to air during its lifetime is
  **not** restored, preserving battle damage.
- Normal stamped assets (no construct backups) still restore whole chunk baselines.
- `notify_chunk_damaged` ignores temporary constructs so they do not trigger reconstitution.

### 6.3 Undo / redo

Spawning a construct records a normal undo entry (before/after chunk snapshots plus a stamp
manager snapshot). Undo restores the chunks and removes the structure bookkeeping; redo
restores both.

---

## 7. Combat bot AI

`voxel_combat_bot_ai.gd` defines `VoxelCombatBotAI` (`extends Node`), toggled with `K` on the
companion (auto-places it if needed).

### 7.1 States

| State | Condition | Behavior |
| --- | --- | --- |
| `IDLE_FOLLOW` | Combat disabled or no target | Follow/standby |
| `PURSUE_3D_NAV` | No LoS or distance > 24 m | Query `get_voxel_path` every 1.25 s, flank |
| `STRAFE_ENGAGE` | LoS clear and distance <= 24 m | Orbit/strafe, fire bolts, lob mortars |
| `HAZARD_ESCAPE` | Lava underfoot | Escape jump (`10.5 m/s`) + lateral dash |
| `BREACH_COVER` | LoS blocked within 11 m by Ice/HardRock | Fire breach blast (`trigger_explosion` 3.8) |

### 7.2 Combat behavior

- Predictive lead aiming at the hero's chest.
- Fires Plasma Bolts every `0.75 s` at `34 m/s`, 16 damage.
- Lobs Magma Mortars every `6.5 s`; impact splashes Lava (3.6 m).
- Strafing probes `get_material_at` / `sample_sdf` 1.8 m ahead and reverses before Lava or
  walls.
- Wall-Pin Stun and Frozen from `VoxelAbilitySystem` fully lock movement and attacks.
- Companion tint: cyan idle, crimson/magenta combat, cyan frozen, orange stunned/burning.

### 7.3 HUD

`format_bot_hud()` returns the FSM state and `LoS: CLEAR/BLOCKED`; the hero HUD shows it in
the companion line.

---

## 8. Ability telegraphs and impact decals

`voxel_ability_telegraphs.gd` defines `VoxelAbilityTelegraphs` (`extends Node3D`), toggled
through four modes with `J`:

1. `AUTO_CONTEXT`
2. `SPECTRAL_WALL`
3. `ICE_DOME`
4. `OFF`

### 8.1 Draping

Every perimeter sample is projected with `raycast_terrain` from above/below and offset along
`sample_sdf_normal * 0.15 m`, so rings/spokes/curtains follow crater floors, slopes, and
ledges.

### 8.2 Telegraphs

| Telegraph | Mode | Colors / queries |
| --- | --- | --- |
| Ground Strike dive line + 32-segment ring + 8 spokes | Auto while airborne / ground slam | Dominant material: Lava orange (+35%), Ice cyan (+20%), Water azure, soil gold |
| Shoulder Charge wall disc | Auto / charge | Swept sphere at chest; thickness + cosine: green breachable, orange-gold solid pin, blue glance |
| Spectral Wall curtain | `SPECTRAL_WALL` | 14 m base + 6.5 m top, draped |
| Ice Dome base ring + 2 arches | `ICE_DOME` | 8.5 m ring + hemisphere arches |
| Fly tool ring + normal pin | Fly mode | Active tool radius at crosshair |

### 8.3 Impact decals

`spawn_impact_ring_decal(center, normal, radius, color, duration)` creates two expanding,
terrain-draped rings over `0.65–0.85 s`. Spawned by:

- Ground Strike combos
- Wall pins
- Elemental combo hits
- Bot Magma Mortar impacts

---

## 9. Grim Wanderer character

See [Hero Character & Animation](Hero%20Character%20&%20Animation.md) and
`models/grim-wanderer/README.md`.

Runtime summary:

- `models/grim-wanderer/grim_wanderer.glb`, 1.9 m, 65-joint Mixamo skeleton, 64 clips.
- Loaded under `HeroVisualRig`; capsule/visor fallback if missing.
- Model faces `+Z`, so it is rotated 180° to match hero forward `-Z`.
- Rig origin is the pelvis; clips place feet at `y = 0`, so always play an idle.
- Clips are in-place; movement remains controller-driven.

---

## 10. Demo wiring

`terrain_demo.gd` owns:

- Mode switching (`FLY`, `CLASSIC`, `PLAY`, `BENCHMARK`).
- Classic first-person pickaxe controller.
- Hero controller, stamina, camera/ADS, animation driver.
- HUD (mode, health, Spirit, stamina, weapon, buffs, cooldowns, bot state, telegraph text).
- Setup order in `_ready()`:
  1. statue pairs, paths, phantom cameras
  2. hero visual rig (GLB + animation)
  3. ability system, bot, telegraphs, beam/tracer meshes
  4. stream focus, benchmark/trailer CLI hooks
- Input routing: `F` classic, `H` hero, `F1` controls, `K` bot, `J` telegraphs,
  `V` camera, `1/2` constructs (Hero) or tools (Fly), `Q/G/E/Z/X` abilities.

---

## 11. File map

| File | Role |
| --- | --- |
| `src/voxel_terrain_3d.{h,cpp}` | Godot node, SDF/query/construct bindings, process/streaming |
| `include/*.hpp` | Engine core, edit pool, stamp manager, procgen, navigation |
| `voxel_ability_system.gd` | GAS-lite tags, statuses, abilities, weapon, projectiles |
| `voxel_combat_bot_ai.gd` | 5-state combat FSM, bolts/mortars |
| `voxel_ability_telegraphs.gd` | Terrain-draped previews and impact decals |
| `voxel_hero_controller.gd` | Standalone reusable hero controller |
| `terrain_demo.gd` | Demo integration, modes, HUD, input |
| `models/grim-wanderer/` | Grim Wanderer GLB + import + asset notes |
| `tests/test_gdextension_bindings.cpp` | End-to-end GDExtension/ability/construct tests |

---

## 12. Build and verification

### Windows

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The `build` tree is configured with `VOXEL_BUILD_GDEXTENSION=ON`; its post-build step copies
`libvoxel_engine.windows.release.x86_64.dll` into `bin/` for the Godot project.

### Sanitizers (WSL/Linux)

```bash
bash run_all_tests.sh
```

Runs every standalone suite under ASan/UBSan and the concurrency/GDExtension suites under
TSan.

### Godot checks

```powershell
Godot_v4.7.2-stable_mono_win64_console.exe --headless --path . --check-only --script res://terrain_demo.gd
Godot_v4.7.2-stable_mono_win64_console.exe --headless --path . --quit-after 120
```

### Last verified state

- Windows full build: success.
- `ctest`: 11/11 passed.
- ASan/UBSan + TSan: all suites passed (209 GDExtension assertions).
- Godot: scripts parse; headless run exits 0 with no errors.
- Runtime smoke tests: modes, classic jump/swing, hero animations, abilities, constructs,
  bot FSM, telegraphs, weapon ricochets/headshots/ADS/reload, grounded Ground Strike.

---

## 13. Tuning reference

Key exported values in `terrain_demo.gd`:

| Group | Values |
| --- | --- |
| Classic | `play_eye_height` 1.65, `play_walk_speed` 6.8, `play_run_speed` 9.5, `play_jump_speed` 8.4, `play_gravity` 24 |
| Hero body | `play_body_radius` 0.45, `play_body_height` 1.85, `play_step_height` 0.60 |
| Stamina | `play_max_stamina` 3, regen 2.4 s, dash 18.5 m/s / 0.26 s |
| Dash-jump | 15.5 m/s horizontal, 8.8 m/s vertical |
| Slide | 17.5 m/s cap |
| Mantle | 0.85 m reach, 0.65–2.35 m height, 0.34 s |
| Charge | 16 m/s, 1.15 s, capture 1.65 m, breach 2.2 m, blast 4.2 m, stun 1.5 s |
| Abilities | Ice path radius 3.2, slam radius 5.5, dome radius 8.5, wall half-width 7.0 |
| Weapon | 24 mag, 0.14 s interval, 1.55 s reload, 0.95 m chip, 2 ricochets |
| Bot | 7.8/6.2 m/s, 7.5–24 m range, 0.75 s fire, 6.5 s mortar, 4.0 s breach |

---

## 14. Known limitations and next steps

- Grim Wanderer has no weapon mesh; third-person rifle clips animate an invisible carbine.
  First-person Hero has no arms/weapon viewmodel.
- Melee and construct casts reuse `bs_toss_grenade`; dedicated clips would improve them.
- No hero death/ragdoll; hero health is tracked but no death state is wired.
- Bot damage does not apply status effects by default (only the ability system's explicit
  effects do).
- Temporary construct expiration mutates structure bookkeeping in `process_reconstitution`;
  undo snapshots during the same frame should be treated carefully.
- The `project.godot` working tree may enable the local `godot_ai` plugin/autoload; that is
  a developer-tool setting, separate from gameplay.

---

## 15. Related documentation

- [Voxel Design Doc v4](Voxel%20Design%20Doc%20v4.md)
- [AI Companion & Navigation](AI%20Companion%20%26%20Navigation.md)
- [Asset Stamping and Reconstitution](asset%20stamping%20and%20reconstitution.md)
- [Voxel Performance Analysis](Voxel%20Performance%20Analysis.md)
- [Terrain depth](terrain%20depth.md)
- [Hero Character & Animation](Hero%20Character%20&%20Animation.md)
- [Grim Wanderer asset notes](../models/grim-wanderer/README.md)
