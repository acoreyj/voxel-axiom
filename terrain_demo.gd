extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D and applies terrain tools on
## left-click. Tools progress from construction to destruction to elements:
##   Keys 1-4: Add terrain / Paint terrain / Paint delete / Place boulder
##   Shift+4:  Rolling boulder mode (dynamic RigidBody3D that bakes into 5-hit
##             voxels once settled; Ctrl+Click spawns a crushing roller)
##   Keys 5-7: Small hit (small_hit_radius) / Explode / Large explode
##   Keys 8-0: Water Quench / Frost-Ice Bridge / Lava-Magma
## Paint and paint-delete are hold-and-drag brushes. T toggles the add/paint
## material between soil and 3-hit rock. The elemental tools fire Bananza-style
## reaction spheres:
##   Key 8: Water Quench (turns Lava -> 3-hit Obsidian on contact & cools cracks)
##   Key 9: Frost / Ice Bridge (builds a flat-topped 31-deg ramped Ice Bridge & crater plug; flips Air/Water -> solid Ice & quenches Lava -> Obsidian)
##   Key 0: Lava / Magma (injects Lava, melts Ice -> Air, reacts with Water -> Obsidian)
## B stamps an offline-baked `.vreg` ruin asset at the crosshair with the
## reconstitution timer below; Shift+B stamps the same asset permanently
## destroyed (no timer). Blast the stamped structure and it is either restored
## after the quiet period or stays destroyed forever.
## H hides / shows the grouped controls overlay. U (or the Hide UI button) hides
## every HUD element except the targeting reticle, plus the world Label3D guidance,
## leaving the pickaxe viewmodel visible.
## Side-by-side 16x-scale Easter Island statues are seeded on startup (see
## `_setup_side_by_side_easter_island_statues`):
##   Option 1 `EasterIslandOption1_MeshSwap` shows a pristine 16x easter_island.obj
##   mesh + StaticBody3D until the first blast nearby, then swaps to a stamped
##   easter_island_statue.vreg voxel structure.
##   Option 2 `EasterIslandOption2_BakedVoxel` is stamped directly into VoxelTerrain3D
##   with baked RGB5551 colors, 3-hit crack damage, raw interior craters and a 15 s
##   reconstitution timer.
## A second Atlas statue pair at z=-64 uses the same two styles, baked from atlas.glb:
##   Option 1 `AtlasOption1_MeshSwap` swaps atlas.obj for models/atlas/atlas_statue.vreg.
##   Option 2 `AtlasOption2_BakedVoxel` is stamped from the same watertight .vreg.
## A third Ape statue pair at z=-128 uses the same two styles, baked from ape.glb:
##   Option 1 `ApeOption1_MeshSwap` swaps ape.obj for models/ape/ape_statue.vreg.
##   Option 2 `ApeOption2_BakedVoxel` is stamped from the same watertight .vreg.
## Key R resets all six statues (restores Option 1's pre-stamp chunks and pristine mesh);
## Key T toggles the add/paint material between soil and 3-hit rock.
## Right-click queries a 3D cave navigation path to the crosshair hit point and draws
## it as a green 3D ribbon that the cyan companion marker walks along. P (or Shift + RMB)
## places / teleports the AI Companion start marker at the crosshair hit point.
## The companion is gravity-grounded, so blasting the ground out from under it drops it
## into the fresh crater instead of leaving it floating on a stale path, and the drawn
## ribbon is surface-draped so string-pulled segments hug concave terrain instead of
## cutting through the air.

@export var move_speed: float = 24.0
@export var fast_multiplier: float = 3.0
@export var mouse_sensitivity: float = 0.0025
@export var explosion_radius: float = 8.5
@export var large_explosion_radius: float = 18.0
## Small-chip blast (Key 5): a much smaller carve for sculpting fine craters
## into statues without destroying half the model per click. Still respects the
## 3-hit hard-rock damage rules.
@export var small_hit_radius: float = 1.5
@export var add_radius: float = 6.0
@export var paint_radius: float = 3.5
@export var paint_delete_radius: float = 3.5
@export var boulder_radius: float = 4.0
## Rolling boulders (Shift+4) fall faster than the project default so they feel
## consistent with the demo's tuned 24 m/s^2 player gravity.
@export var boulder_gravity_scale: float = 2.4
## A rolling boulder must stay at rest this long before it bakes into 5-hit voxels.
@export var boulder_settle_seconds: float = 1.2
@export var boulder_max_count: int = 24
## Minimum speed before a crushing boulder (Ctrl+Click) carves the terrain it rolls over.
@export var boulder_crush_speed: float = 8.0
@export var boulder_crush_interval: float = 0.18
@export var boulder_crush_radius_scale: float = 0.55
## Simultaneous crushing boulders allowed before new spawns drop the crush flag.
@export var boulder_max_crushing: int = 2
@export var elemental_radius: float = 8.5
@export var brush_spacing_factor: float = 0.6
@export var brush_interval: float = 0.05
@export var max_ray_distance: float = 250.0
## Speed the cyan companion marker walks along the computed waypoints.
@export var companion_walk_speed: float = 10.0
## Downward acceleration (m/s^2) applied to the companion when the ground beneath it
## is carved away, so it falls into the fresh crater instead of floating on the old path.
@export var companion_gravity: float = 28.0
## A repeat explosive click whose re-raycast impact point has merely receded
## along the view ray (soft ground in front of the rock was blown away by the
## previous hit) by up to this fraction of the blast radius still counts as the
## same spot: the previous impact point is reused so blast spheres stack on the
## rock instead of creeping deeper every click. Moving the aim sideways beyond
## `boom_snap_lateral` releases the snap and starts a new spot.
@export var boom_snap_depth_factor: float = 0.5
@export var boom_snap_lateral: float = 0.75
## Offline-baked ruin asset stamped by Key B / Shift+B (see the `voxel_baker` CLI:
## `voxel_baker ancient_ruin.obj ancient_ruin.vreg`). Place the .vreg next to
## this scene or point this at any other exported region file.
@export var stamp_asset_path: String = "res://ancient_ruin.vreg"
## Reconstitution quiet period for Key B stamps, in seconds. Shift+B stamps the
## same region with 0.0 so it stays permanently destroyed once blasted.
@export var stamp_recreate_delay_seconds: float = 15.0
## Easter Island statue assets for the side-by-side demo. `easter_island_statue.vreg`
## is produced by the voxel_baker CLI from the 16x-scale (48 m) OBJ + albedo at
## 0.5 m voxels, and is stamped directly into the terrain for both demo options.
@export var easter_island_mesh_path: String = "res://models/easter-island/easter_island.obj"
@export var easter_island_albedo_path: String = "res://models/easter-island/easter_island_albedo.png"
@export var easter_island_vreg_path: String = "models/easter-island/easter_island_statue.vreg"
## Atlas statue assets for the second side-by-side demo pair. `atlas_statue.vreg`
## is baked from models/atlas/atlas.glb's 5.333x (48 m tall) OBJ + albedo at
## 0.5 m voxels with `--watertight --color-depth 2.8`.
@export var atlas_mesh_path: String = "res://models/atlas/atlas.obj"
@export var atlas_albedo_path: String = "res://models/atlas/atlas_albedo.png"
@export var atlas_vreg_path: String = "models/atlas/atlas_statue.vreg"
## Ape statue assets for the third side-by-side demo pair. `ape_statue.vreg` is
## baked from models/ape/ape.glb's 5.333x (48 m tall) OBJ + albedo at 0.5 m
## voxels with `--watertight --color-depth 2.8`. The OBJ is decimated to ~28.5k
## triangles so the narrow-band bake stays tractable at this voxel size.
@export var ape_mesh_path: String = "res://models/ape/ape.obj"
@export var ape_albedo_path: String = "res://models/ape/ape_albedo.png"
@export var ape_vreg_path: String = "models/ape/ape_statue.vreg"

## Deadlock-Style Third-Person / First-Person Hero Play Mode (F toggles Fly <-> Play,
## V toggles 3rd-Person Over-the-Shoulder <-> 1st-Person). Uses VoxelTerrain3D's O(1)
## C++ SDF & surface queries (`sample_sdf`, `sample_sdf_normal`, `get_material_at`,
## `get_damage_at`, `trigger_elemental_capsule`) for sub-voxel capsule collide-and-slide,
## 3-probe ledge mantling, downhill slope sliding, stamina dash-jumps, Kelvin Ice Paths (E),
## and Lash Ground Strike crater slams (Q).
@export var play_eye_height: float = 1.65
@export var play_walk_speed: float = 6.8
@export var play_run_speed: float = 9.5
@export var play_jump_speed: float = 8.4
@export var play_gravity: float = 24.0
@export var play_step_height: float = 0.60
@export var play_body_radius: float = 0.45
@export var play_body_height: float = 1.85
@export var play_fov: float = 78.0
@export var play_third_person: bool = true
@export var play_cam_arm_length: float = 4.2
@export var play_cam_shoulder_offset: Vector3 = Vector3(0.65, 0.35, 0.0)
@export var play_max_stamina: int = 3
@export var play_stamina_regen_seconds: float = 2.4
@export var play_dash_speed: float = 18.5
@export var play_dash_duration: float = 0.26
@export var play_dash_jump_horiz_boost: float = 15.5
@export var play_dash_jump_vert_speed: float = 8.8
@export var play_slide_max_speed: float = 17.5
@export var play_mantle_reach: float = 0.85
@export var play_mantle_min_height: float = 0.65
@export var play_mantle_max_height: float = 2.35
@export var play_mantle_duration: float = 0.34
@export var play_ice_path_radius: float = 3.2
@export var play_ground_slam_radius: float = 5.5
@export var play_charge_speed: float = 16.0
@export var play_charge_duration: float = 1.15
@export var play_charge_capture_radius: float = 1.65
@export var play_charge_pin_min_cos: float = 0.60
@export var play_charge_breach_max_thickness: float = 2.2
@export var play_charge_wall_pin_blast_radius: float = 4.2
@export var play_charge_pin_stun_seconds: float = 1.5
## Radius of the pickaxe chip carved on the swing's impact frame.
@export var pickaxe_hit_radius: float = 1.5
## Point in the swing (0-1 of its length) where the tool bites the terrain.
@export var pickaxe_impact_fraction: float = 0.42
## Swing length in seconds; the chip is applied at `pickaxe_impact_fraction`.
const PICKAXE_SWING_TIME := 0.55
const BASE_VIEWMODEL_POS := Vector3(0.64, -0.44, -0.18)
const BASE_SHOULDER_ROT := Vector3(0.14, 0.16, -0.10)
const BASE_ELBOW_ROT := Vector3(0.30, 0.06, 0.08)
const BASE_WRIST_ROT := Vector3(-0.20, -0.10, 0.06)

## Tool palette in keyboard order: construction (1-4), destruction (5-7),
## elemental reactions (8-0). `TOOL_KEYS` is the keycap shown in the HUD.
enum Tool { ADD_TERRAIN, PAINT, PAINT_DELETE, BOULDER, SMALL_HIT, EXPLODE, LARGE_EXPLODE, WATER_QUENCH, FROST_ICE, LAVA_MAGMA }

## Fly is the free camera; Play is the Deadlock-style 3rd/1st-person Hero Controller;
## Benchmark sweeps multiple VoxelTerrain3D settings and logs hardware + FPS/latency metrics.
enum Mode { FLY, PLAY, BENCHMARK }

enum BenchmarkPhase { SETTLE, STREAM_FLIGHT, DESTRUCTION_CSG, NAVIGATION_3D }

const BENCHMARK_PHASE_NAMES := ["Settle & Warmup", "Stream Flight", "Destruction & CSG", "3D Nav Pathing"]
const BENCHMARK_SETTLE_MAX_SEC := 3.5
const BENCHMARK_FLIGHT_SEC := 4.5
const BENCHMARK_DESTRUCTION_SEC := 4.0
const BENCHMARK_NAV_SEC := 2.5
const BENCHMARK_NAV_MAX_SEC := 5.5
const BENCHMARK_REPORT_JSON_USER := "user://voxel_benchmark_results.json"
const BENCHMARK_REPORT_MD_USER := "user://voxel_benchmark_report.md"
const BENCHMARK_REPORT_JSON_RES := "res://voxel_benchmark_results.json"
const BENCHMARK_REPORT_MD_RES := "res://voxel_benchmark_report.md"

const BENCHMARK_PRESETS := [
	{
		"name": "Low / Fast Stream",
		"view_distance_chunks": 6,
		"prefetch_distance_chunks": 1,
		"upload_budget_ms": 1.5,
		"collision_distance_chunks": 2,
		"resident_memory_budget_mb": 512,
	},
	{
		"name": "Balanced Default",
		"view_distance_chunks": 8,
		"prefetch_distance_chunks": 2,
		"upload_budget_ms": 2.5,
		"collision_distance_chunks": 4,
		"resident_memory_budget_mb": 1024,
	},
	{
		"name": "High Throughput Upload",
		"view_distance_chunks": 10,
		"prefetch_distance_chunks": 3,
		"upload_budget_ms": 5.0,
		"collision_distance_chunks": 4,
		"resident_memory_budget_mb": 1536,
	},
	{
		"name": "Extended Horizon",
		"view_distance_chunks": 14,
		"prefetch_distance_chunks": 4,
		"upload_budget_ms": 6.0,
		"collision_distance_chunks": 6,
		"resident_memory_budget_mb": 2048,
	},
	{
		"name": "Full Collision Stress",
		"view_distance_chunks": 12,
		"prefetch_distance_chunks": 2,
		"upload_budget_ms": 8.0,
		"collision_distance_chunks": 10,
		"resident_memory_budget_mb": 2048,
	},
]

enum HeroState { GROUNDED, AIRBORNE, DASHING, SLIDING, MANTLING, GROUND_SLAM, SHOULDER_CHARGE }
const HERO_STATE_NAMES := ["Grounded", "Airborne", "Dashing", "Sliding", "Mantling", "Ground Slam", "Shoulder Charge"]

const TOOL_NAMES := ["Add terrain", "Paint terrain", "Paint delete", "Place boulder", "Small hit", "Explode", "Large explode", "Water quench", "Frost / Ice", "Lava / Magma"]
const TOOL_KEYS := ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"]
const CONTROLS_TEXT := """CONTROLS  (H hide keys | U hide all UI)
Mode    F fly / hero play   V toggle 3rd/1st person (in Play)   M (or F8) benchmark
Hero    WASD move   Shift dash (Space mid-dash = Dash-Jump!)   Ctrl/C slope slide
Climb   Space jump/mantle   G Charge   E IcePath   Q Slam   Z HyperBeam   X CryoOrb
Build   1 Ice Dome   2 Spectral Wall   Shift+4 roller   LMB pickaxe
Weapon  Play: LMB Ricochet Gun (Slide=Inf Ammo!)   RMB Aim Zoom   R Reload   M Melee
Move    W/A/S/D   Space or E up   Ctrl or Q down   Shift sprint
Look    Mouse   Esc releases cursor   click recaptures
Build   1 Add terrain   2 Paint   3 Paint delete   4 Boulder
Roller  Shift+4 rolling boulder   Ctrl+Click crushing roller
Carve   5 Small hit     6 Explode   7 Large explode
Element 8 Water   9 Frost/Ice   0 Lava/Magma
Stamps  B timed ruin   Shift+B permanent ruin
History Undo %d (Z) | Redo %d (Y) | Reset All (Shift+R)
World   R reset statues   T material   P place companion
Mouse   Play LMB pickaxe   Fly LMB tool   RMB cave path   Shift+RMB place companion"""
const MATERIAL_NAMES := ["Soil", "Rock"]
## MaterialHardness::kMaterial* IDs -> display names for the "Looking at" HUD readout.
const MATERIAL_ID_NAMES := ["Air", "Soil", "Hard Rock", "Lava", "Water", "Obsidian", "Ice", "Boulder"]
## Refresh interval for the crosshair material readout (raycast is main-thread only).
const LOOK_AT_REFRESH_INTERVAL := 0.15
const MATERIAL_SOIL := 1
const MATERIAL_ROCK := 2
## ElementalKind wire values shared with the C++ core (1 = Water, 2 = Frost, 3 = Lava).
const ELEMENTAL_WATER := 1
const ELEMENTAL_FROST := 2
const ELEMENTAL_LAVA := 3

## Side-by-side Easter Island statue placement. `easter_island_statue.vreg` is
## baked at voxel_scale = 0.5 m with target region center (160, 160, 128) at
## 48 m tall x 24.9 m wide x 30.8 m deep, so stamping at `origin_chunk` puts the
## statue center at `(origin_chunk * 32 + EASTER_ISLAND_REGION_CENTER) * voxel_scale`
## and its base at region sample 112 (= 160 - 24 m half height / 0.5 m per voxel).
const EASTER_ISLAND_REGION_CENTER := Vector3(160.0, 160.0, 128.0)
const EASTER_ISLAND_OPT1_WORLD_CENTER := Vector3(-16.0, 96.0, 0.0)
const EASTER_ISLAND_OPT2_WORLD_CENTER := Vector3(16.0, 96.0, 0.0)
const EASTER_ISLAND_HALF_EXTENTS := Vector3(13.0, 24.0, 15.5)
## Model base in region-sample coords for the (160,160,128)-centred 0.5 m bake;
## used to seat the statue on the terrain at the 32-sample chunk quantisation.
const EASTER_ISLAND_BASE_LOCAL_Y := 112.0
## The exported OBJ origin is at the feet (y 0..48 m), so the pristine
## MeshInstance3D root is seated 24 m above the statue center.
const EASTER_ISLAND_ROOT_OFFSET_Y := 24.0

## Second side-by-side pair: the Atlas statue at z=-64. The source atlas.glb is
## 9 m tall, so it is baked at 5.333x to match the other 48 m statues and shares
## the same region center/base placement. Its exported OBJ origin is at the feet
## (y 0..48), so the pristine mesh root is seated 24 m above the statue center.
const ATLAS_REGION_CENTER := Vector3(160.0, 160.0, 128.0)
const ATLAS_BASE_LOCAL_Y := 112.0
const ATLAS_HALF_EXTENTS := Vector3(13.0, 25.0, 13.0)
const ATLAS_ROOT_OFFSET_Y := 24.0
const ATLAS_OPT1_WORLD_CENTER := Vector3(-16.0, 96.0, -64.0)
const ATLAS_OPT2_WORLD_CENTER := Vector3(16.0, 96.0, -64.0)

## Third side-by-side pair: the Ape statue at z=-128. ape.glb is also 9 m tall,
## so it shares the 5.333x bake and region center/base placement. The model is
## much wider (~53 m) than the Easter Island/Atlas statues, so the pair is spaced
## symmetrically at x=+/-32. Its OBJ origin is at the feet (y 0..48), so the
## pristine mesh root is seated 24 m above the statue center.
const APE_REGION_CENTER := Vector3(160.0, 160.0, 128.0)
const APE_BASE_LOCAL_Y := 112.0
const APE_HALF_EXTENTS := Vector3(26.5, 24.0, 15.8)
const APE_ROOT_OFFSET_Y := 24.0
const APE_OPT1_WORLD_CENTER := Vector3(-32.0, 96.0, -128.0)
const APE_OPT2_WORLD_CENTER := Vector3(32.0, 96.0, -128.0)

@onready var terrain: VoxelTerrain3D = $VoxelTerrain3D
@onready var ai_companion: VoxelAICompanion = $VoxelAICompanion
@onready var camera: Camera3D = $Camera3D
@onready var stats_label: Label = $HUD/StatsLabel
@onready var controls_label: Label = $HUD/ControlsLabel
@onready var hide_ui_button: Button = $HUD/HideUIButton
@onready var reticle: Control = $HUD/Reticle
@onready var opt1_root: Node3D = $EasterIslandOption1_MeshSwap
@onready var opt1_mesh_inst: MeshInstance3D = $EasterIslandOption1_MeshSwap/PristineMesh
@onready var opt1_static_body: StaticBody3D = $EasterIslandOption1_MeshSwap/StaticBody3D
@onready var opt1_col_shape: CollisionShape3D = $EasterIslandOption1_MeshSwap/StaticBody3D/CollisionShape3D
@onready var opt1_label: Label3D = $EasterIslandOption1_MeshSwap/Label3D
@onready var atlas_opt1_root: Node3D = $AtlasOption1_MeshSwap
@onready var atlas_opt1_mesh_inst: MeshInstance3D = $AtlasOption1_MeshSwap/PristineMesh
@onready var atlas_opt1_static_body: StaticBody3D = $AtlasOption1_MeshSwap/StaticBody3D
@onready var atlas_opt1_col_shape: CollisionShape3D = $AtlasOption1_MeshSwap/StaticBody3D/CollisionShape3D
@onready var atlas_opt1_label: Label3D = $AtlasOption1_MeshSwap/Label3D
@onready var ape_opt1_root: Node3D = $ApeOption1_MeshSwap
@onready var ape_opt1_mesh_inst: MeshInstance3D = $ApeOption1_MeshSwap/PristineMesh
@onready var ape_opt1_static_body: StaticBody3D = $ApeOption1_MeshSwap/StaticBody3D
@onready var ape_opt1_col_shape: CollisionShape3D = $ApeOption1_MeshSwap/StaticBody3D/CollisionShape3D
@onready var ape_opt1_label: Label3D = $ApeOption1_MeshSwap/Label3D

var _yaw: float = 0.0
var _pitch: float = -0.25
var _velocity: Vector3 = Vector3.ZERO

var _mode: int = Mode.FLY
var _hero_state: int = HeroState.GROUNDED
var _player_feet: Vector3 = Vector3.ZERO
var _player_vel: Vector3 = Vector3.ZERO
var _player_vel_y: float = 0.0
var _player_grounded: bool = false
var _ground_normal: Vector3 = Vector3.UP
var _foot_material: int = MATERIAL_SOIL
var _foot_damage: float = 0.0
var _stamina_charges: int = 3
var _stamina_regen_progress: float = 0.0
var _dash_timer: float = 0.0
var _dash_dir: Vector3 = Vector3.FORWARD
var _coyote_timer: float = 0.0
var _mantle_timer: float = 0.0
var _mantle_start_pos: Vector3 = Vector3.ZERO
var _mantle_target_pos: Vector3 = Vector3.ZERO
var _charge_timer: float = 0.0
var _charge_dir: Vector3 = Vector3.FORWARD
var _charge_pinned_companion: bool = false
var _companion_stun_timer: float = 0.0
var _companion_base_mat: StandardMaterial3D = null
var _gas: VoxelAbilitySystem = null
var _beam_mesh_inst: MeshInstance3D = null
var _beam_immediate_mesh: ImmediateMesh = null
var _beam_impact_sphere: MeshInstance3D = null
var _tracer_mesh_inst: MeshInstance3D = null
var _tracer_immediate_mesh: ImmediateMesh = null
var _play_fire_held: bool = false
var _play_ads_held: bool = false
var _ads_blend: float = 0.0
var _ice_path_active: bool = false
var _last_ice_path_anchor: Vector3 = Vector3(INF, INF, INF)
var _hero_visual_root: Node3D = null
var _hero_torso_mesh: MeshInstance3D = null
var _hero_visor_mesh: MeshInstance3D = null
var _last_safe_feet: Vector3 = Vector3.ZERO
var _spawn_pending: bool = false
var _spawn_wait: float = 0.0
var _swing_active: bool = false
var _swing_hit_done: bool = false
var _swing_elapsed: float = 0.0
var _swing_impact_time: float = 0.0
var _swing_hit_solid_recoil: float = 0.0
var _idle_bob_time: float = 0.0
var _mouse_sway_offset: Vector2 = Vector2.ZERO
var _viewmodel_root: Node3D = null
var _shoulder_pivot: Node3D = null
var _elbow_pivot: Node3D = null
var _wrist_pivot: Node3D = null
var _pickaxe_root: Node3D = null
var _pickaxe_tip_marker: Marker3D = null
var _pickaxe_accent_mat: StandardMaterial3D = null
var _impact_sparks: CPUParticles3D = null
var _pcam_host: PhantomCameraHost = null
var _fly_pcam: PhantomCamera3D = null
var _play_pcam: PhantomCamera3D = null
var _base_fov: float = 75.0
var _last_focus_chunk: Vector3i = Vector3i(999999, 999999, 999999)
var _committed_chunk_count: int = 0
var _fps_smoothed: float = 0.0
var _commits_in_window: int = 0
var _commits_per_second: float = 0.0
var _stats_accum: float = 0.0
var _active_tool: int = Tool.EXPLODE
var _controls_visible: bool = true
var _ui_hidden: bool = false
var _add_material: int = MATERIAL_SOIL
var _tool_pressed: bool = false
var _brush_cooldown: float = 0.0
var _last_brush_point: Vector3 = Vector3(INF, INF, INF)
var _last_boom_point: Vector3 = Vector3(INF, INF, INF)
var _last_event_print_ms: int = 0
var _last_fail_warn_ms: int = 0
var _look_at_name: String = "Sky"
var _look_at_accum: float = 0.0

## Dynamic rolling boulders spawned by Shift+4. They are session-only props: they
## are not part of the C++ undo history until they settle and bake back into voxels.
var _rolling_boulders: Array[RigidBody3D] = []
var _boulder_material: ShaderMaterial = null
var _boulder_rolling: bool = false
## Bake / crush fills complete asynchronously, so remember which request each
## terrain_add_completed / explosion_completed signal belongs to for undo accounting.
var _pending_bake_fills: Array[Dictionary] = []
var _pending_crush_blasts: Array[Dictionary] = []

var _companion_initialized: bool = false
var _companion_vertical_vel: float = 0.0
var _pending_path_goal: Vector3 = Vector3.ZERO
var _has_pending_path_query: bool = false
var _rebake_in_progress: bool = false
var _active_waypoints: PackedVector3Array = PackedVector3Array()
var _current_waypoint_idx: int = 0

var _companion_mesh_inst: MeshInstance3D = null
var _goal_mesh_inst: MeshInstance3D = null
var _path_mesh_inst: MeshInstance3D = null
var _path_immediate_mesh: ImmediateMesh = null

var _opt1_swapped_to_voxels: bool = false
var _opt1_structure_id: int = 0
var _opt2_structure_id: int = 0
## Option 1 pristine meshes waiting for their stamped voxels to finish remeshing.
## Hiding the mesh immediately leaves a visible hole until the chunk meshes commit.
var _pending_pristine_hides: Array[Dictionary] = []
## Surface-resolved placement (computed on startup / reset, not fixed in the scene).
var _opt1_world_center: Vector3 = EASTER_ISLAND_OPT1_WORLD_CENTER
var _opt2_world_center: Vector3 = EASTER_ISLAND_OPT2_WORLD_CENTER
var _opt1_origin_chunk: Vector3 = Vector3.ZERO
var _opt2_origin_chunk: Vector3 = Vector3.ZERO

var _atlas_opt1_swapped_to_voxels: bool = false
var _atlas_opt1_structure_id: int = 0
var _atlas_opt2_structure_id: int = 0
var _atlas_opt1_world_center: Vector3 = ATLAS_OPT1_WORLD_CENTER
var _atlas_opt2_world_center: Vector3 = ATLAS_OPT2_WORLD_CENTER
var _atlas_opt1_origin_chunk: Vector3 = Vector3.ZERO
var _atlas_opt2_origin_chunk: Vector3 = Vector3.ZERO

var _ape_opt1_swapped_to_voxels: bool = false
var _ape_opt1_structure_id: int = 0
var _ape_opt2_structure_id: int = 0
var _ape_opt1_world_center: Vector3 = APE_OPT1_WORLD_CENTER
var _ape_opt2_world_center: Vector3 = APE_OPT2_WORLD_CENTER
var _ape_opt1_origin_chunk: Vector3 = Vector3.ZERO
var _ape_opt2_origin_chunk: Vector3 = Vector3.ZERO

## Benchmark state & telemetry accumulators
var _bench_quit_on_finish: bool = false
var _bench_saved_settings: Dictionary = {}
var _bench_saved_cam_transform: Transform3D = Transform3D.IDENTITY
var _bench_saved_yaw: float = 0.0
var _bench_saved_pitch: float = 0.0
var _bench_saved_vsync: int = DisplayServer.VSYNC_ENABLED
var _bench_hardware_info: Dictionary = {}
var _bench_preset_idx: int = 0
var _bench_phase: int = BenchmarkPhase.SETTLE
var _bench_phase_elapsed: float = 0.0
var _bench_preset_elapsed: float = 0.0
var _bench_settle_time_ms: float = 0.0
var _bench_frame_times_ms: PackedFloat64Array = PackedFloat64Array()
var _bench_phase_frame_times: Dictionary = {}
var _bench_commits_count: int = 0
var _bench_csg_step_idx: int = 0
var _bench_csg_next_fire_sec: float = 0.0
var _bench_pending_blast_start_usec: PackedInt64Array = PackedInt64Array()
var _bench_blast_latencies_ms: PackedFloat64Array = PackedFloat64Array()
var _bench_affected_chunks_total: int = 0
var _bench_nav_query_started: bool = false
var _bench_nav_start_usec: int = 0
var _bench_nav_sync_ms: float = 0.0
var _bench_nav_query_ms: float = 0.0
var _bench_nav_waypoints: int = 0
var _bench_peak_vram_mb: float = 0.0
var _bench_peak_ram_mb: float = 0.0
var _bench_peak_draw_calls: int = 0
var _bench_peak_primitives: int = 0
var _bench_results: Array[Dictionary] = []
var _bench_last_summary_banner: String = ""
var _bench_banner_timer: float = 0.0
var _bench_suite_started_utc: String = ""
var _bench_suite_started_local: String = ""
var _bench_suite_started_unix: float = 0.0
var _bench_preset_started_utc: String = ""
var _bench_preset_started_local: String = ""
var _bench_preset_started_unix: float = 0.0
var _bench_last_reset_method: String = "undo_all"

## Undo / Redo action stack (groups continuous brush strokes & Option 1 PristineMesh swaps
## with C++ chunk-snapshot undo/redo entries, and provides a GDScript fallback if the
## loaded GDExtension binary was built before VoxelUndoRedoManager was added).
const MAX_UNDO_ACTIONS := 64
var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []
var _active_undo_action: Dictionary = {}

func _get_cli_arg(prefix: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return arg.substr(prefix.length())
	return ""

## The capture rig must choose its save directory before VoxelTerrain3D enters the
## tree, because the native save manager is constructed in _enter_tree().
func _enter_tree() -> void:
	var save_dir := _get_cli_arg("--trailer-save-dir=")
	if save_dir.is_empty():
		return
	var terrain_node := get_node_or_null("VoxelTerrain3D")
	if terrain_node != null and terrain_node.has_method("set_save_directory_path"):
		terrain_node.call("set_save_directory_path", save_dir)

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	# Dynamic props: a blast wakes/launches nearby rolling boulders, while a fill
	# (including a boulder settling back into voxels) only needs to wake them.
	terrain.explosion_completed.connect(_on_terrain_blast_physics)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.terrain_add_requested.connect(_on_terrain_add_requested)
	terrain.terrain_add_completed.connect(_on_terrain_add_completed)
	terrain.terrain_add_completed.connect(_on_terrain_fill_physics)
	terrain.terrain_add_failed.connect(_on_terrain_add_failed)
	terrain.chunk_committed.connect(_on_chunk_committed)
	terrain.structure_damaged.connect(_on_structure_damaged)
	terrain.structure_recreated.connect(_on_structure_recreated)
	if terrain.has_signal("structure_removed"):
		terrain.connect("structure_removed", Callable(self, "_on_structure_removed"))
	if terrain.has_signal("undo_completed"):
		terrain.connect("undo_completed", Callable(self, "_on_undo_completed"))
	if terrain.has_signal("redo_completed"):
		terrain.connect("redo_completed", Callable(self, "_on_redo_completed"))

	_setup_pickaxe_viewmodel()
	_setup_path_debug_visuals()
	_refresh_controls_label()
	controls_label.visible = _controls_visible
	hide_ui_button.pressed.connect(_toggle_ui_hidden)
	_setup_side_by_side_easter_island_statues()
	_setup_side_by_side_atlas_statues()
	_setup_side_by_side_ape_statues()
	# Treat the freshly stamped Option 2 statues as the pristine world baseline
	# so Undo / undo_all() never erases the initial showcase statues.
	_clear_undo_history_to_current_baseline()
	_setup_phantom_camera()
	_setup_hero_visual_rig()
	_setup_ability_system_and_beam_visuals()
	_update_stream_focus(true)
	_check_cli_benchmark_autostart()
	_check_cli_trailer_autostart()

func _setup_path_debug_visuals() -> void:
	# 1. Cyan capsule marker on VoxelAICompanion so you can see it walk along the path
	_companion_mesh_inst = MeshInstance3D.new()
	var cap := CapsuleMesh.new()
	cap.radius = 0.6
	cap.height = 1.8
	var cap_mat := StandardMaterial3D.new()
	cap_mat.albedo_color = Color(0.1, 0.95, 1.0)
	cap_mat.emission_enabled = true
	cap_mat.emission = Color(0.1, 0.85, 1.0) * 0.6
	cap.material = cap_mat
	_companion_base_mat = cap_mat
	_companion_mesh_inst.mesh = cap
	_companion_mesh_inst.position = Vector3(0.0, 0.9, 0.0)
	ai_companion.add_child(_companion_mesh_inst)

	# 2. Gold sphere marker at the path Goal
	_goal_mesh_inst = MeshInstance3D.new()
	var sph := SphereMesh.new()
	sph.radius = 0.55
	sph.height = 1.1
	var sph_mat := StandardMaterial3D.new()
	sph_mat.albedo_color = Color(1.0, 0.85, 0.1)
	sph_mat.emission_enabled = true
	sph_mat.emission = Color(1.0, 0.75, 0.1) * 0.8
	sph.material = sph_mat
	_goal_mesh_inst.mesh = sph
	_goal_mesh_inst.visible = false
	add_child(_goal_mesh_inst)

	# 3. 3D Line Strip (ImmediateMesh) for rendering the computed waypoint path
	_path_mesh_inst = MeshInstance3D.new()
	_path_immediate_mesh = ImmediateMesh.new()
	var line_mat := StandardMaterial3D.new()
	line_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	line_mat.albedo_color = Color(0.2, 1.0, 0.4)
	_path_mesh_inst.mesh = _path_immediate_mesh
	_path_mesh_inst.material_override = line_mat
	add_child(_path_mesh_inst)

## Returns the stamp origin chunk that seats a voxel model's base on the terrain
## surface at the requested world X/Z, keeping the region centred on `region_center`.
func _statue_origin_for_surface(region_center: Vector3, base_local_y: float, world_x: float, world_z: float) -> Vector3:
	var surface_y := terrain.get_surface_height(world_x, world_z)
	# region_center/base_local_y are region-sample coordinates; convert the desired
	# world placement into samples before solving for the origin chunk.
	var inv_scale: float = 1.0 / terrain.voxel_scale
	var origin_x := roundi((world_x * inv_scale - region_center.x) / 32.0)
	var origin_z := roundi((world_z * inv_scale - region_center.z) / 32.0)
	var origin_y := roundi((surface_y * inv_scale - base_local_y) / 32.0)
	return Vector3(origin_x, origin_y, origin_z)

func _easter_island_origin_for_surface(world_x: float, world_z: float) -> Vector3:
	return _statue_origin_for_surface(EASTER_ISLAND_REGION_CENTER, EASTER_ISLAND_BASE_LOCAL_Y, world_x, world_z)

func _atlas_origin_for_surface(world_x: float, world_z: float) -> Vector3:
	return _statue_origin_for_surface(ATLAS_REGION_CENTER, ATLAS_BASE_LOCAL_Y, world_x, world_z)

func _ape_origin_for_surface(world_x: float, world_z: float) -> Vector3:
	return _statue_origin_for_surface(APE_REGION_CENTER, APE_BASE_LOCAL_Y, world_x, world_z)

func _setup_side_by_side_easter_island_statues() -> void:
	# 1. Option 1 (Left at x=-16, z=0): Load the 16x (48 m) easter_island.obj +
	#    albedo onto PristineMesh. It stays a normal mesh + StaticBody3D until the
	#    first nearby blast, then swaps to a stamped voxel structure. Its height is
	#    matched to Option 2's chunk-quantized base below so both stand level.
	_load_pristine_easter_island_mesh_and_collider()
	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0

	# 2. Option 2 (Right at x=+16, z=0): Stamp easter_island_statue.vreg directly
	#    into VoxelTerrain3D with per-voxel RGB5551 baked colors and a 15-second
	#    auto-reconstitution timer, seated on the surface. A region's vertical
	#    origin is quantized to a whole 32 m chunk, so its base can sit up to half
	#    a chunk off the terrain; compute it first and seat Option 1 to match, so
	#    the pristine .obj and the voxel statue stand at exactly the same level.
	_opt2_origin_chunk = _easter_island_origin_for_surface(EASTER_ISLAND_OPT2_WORLD_CENTER.x, EASTER_ISLAND_OPT2_WORLD_CENTER.z)
	_opt2_world_center = (_opt2_origin_chunk * 32.0 + EASTER_ISLAND_REGION_CENTER) * terrain.voxel_scale
	_opt1_origin_chunk = _easter_island_origin_for_surface(EASTER_ISLAND_OPT1_WORLD_CENTER.x, EASTER_ISLAND_OPT1_WORLD_CENTER.z)
	_opt1_origin_chunk.y = _opt2_origin_chunk.y
	_opt1_world_center = (_opt1_origin_chunk * 32.0 + EASTER_ISLAND_REGION_CENTER) * terrain.voxel_scale
	opt1_root.global_position = _opt1_world_center - Vector3(0.0, EASTER_ISLAND_ROOT_OFFSET_Y, 0.0)

	_opt2_structure_id = _stamp_structure(easter_island_vreg_path, _opt2_origin_chunk, 15.0)
	if _opt2_structure_id <= 0:
		push_warning("[EasterIslandDemo] Failed to stamp '%s' - bake it first: voxel_baker models/easter-island/easter_island.obj %s --voxel-scale 0.5 --watertight --texture models/easter-island/easter_island_albedo.ppm --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128" % [
			easter_island_vreg_path, easter_island_vreg_path
		])
	print("[EasterIslandDemo] Option 1 (Pristine Mesh -> Voxel Swap) ready at ", _opt1_world_center,
		" | Option 2 (Baked Per-Voxel Albedo, id=#", _opt2_structure_id, ") stamped at ", _opt2_world_center)

func _setup_side_by_side_atlas_statues() -> void:
	# Same two styles as the Easter Island pair, one row further down the view
	# direction (z=-64). atlas.glb is baked at 5.333x so the statue is 48 m tall
	# and shares the region center/base, letting both pairs seat at the same level.
	_load_pristine_atlas_mesh_and_collider()
	_atlas_opt1_swapped_to_voxels = false
	_atlas_opt1_structure_id = 0
	_atlas_opt1_origin_chunk = _atlas_origin_for_surface(ATLAS_OPT1_WORLD_CENTER.x, ATLAS_OPT1_WORLD_CENTER.z)

	_atlas_opt2_origin_chunk = _atlas_origin_for_surface(ATLAS_OPT2_WORLD_CENTER.x, ATLAS_OPT2_WORLD_CENTER.z)
	_atlas_opt2_world_center = (_atlas_opt2_origin_chunk * 32.0 + ATLAS_REGION_CENTER) * terrain.voxel_scale
	_atlas_opt1_world_center = Vector3(ATLAS_OPT1_WORLD_CENTER.x, _atlas_opt2_world_center.y, ATLAS_OPT1_WORLD_CENTER.z)
	atlas_opt1_root.global_position = _atlas_opt1_world_center - Vector3(0.0, ATLAS_ROOT_OFFSET_Y, 0.0)

	_atlas_opt2_structure_id = _stamp_structure(atlas_vreg_path, _atlas_opt2_origin_chunk, 15.0)
	if _atlas_opt2_structure_id <= 0:
		push_warning("[AtlasDemo] Failed to stamp '%s' - bake it first: voxel_baker models/atlas/atlas.obj %s --voxel-scale 0.5 --watertight --texture models/atlas/atlas_albedo.ppm --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128" % [
			atlas_vreg_path, atlas_vreg_path
		])
	print("[AtlasDemo] Option 1 (Pristine Mesh -> Voxel Swap) ready at ", _atlas_opt1_world_center,
		" | Option 2 (Baked Per-Voxel Albedo, id=#", _atlas_opt2_structure_id, ") stamped at ", _atlas_opt2_world_center)

func _setup_side_by_side_ape_statues() -> void:
	# Same two styles as the Easter Island and Atlas pairs, one row further down
	# the view direction (z=-128). ape.glb is also baked at 5.333x (48 m tall) and
	# shares the region center/base, letting all three pairs seat at the same level.
	_load_pristine_ape_mesh_and_collider()
	_ape_opt1_swapped_to_voxels = false
	_ape_opt1_structure_id = 0

	_ape_opt2_origin_chunk = _ape_origin_for_surface(APE_OPT2_WORLD_CENTER.x, APE_OPT2_WORLD_CENTER.z)
	_ape_opt2_world_center = (_ape_opt2_origin_chunk * 32.0 + APE_REGION_CENTER) * terrain.voxel_scale
	# Resolve Option 1 from its own stamp origin (not the raw constant) so the
	# pristine mesh sits exactly where the voxel structure will appear. Reuse
	# Option 2's chunk-aligned Y so both halves of the pair stand level.
	_ape_opt1_origin_chunk = _ape_origin_for_surface(APE_OPT1_WORLD_CENTER.x, APE_OPT1_WORLD_CENTER.z)
	_ape_opt1_origin_chunk.y = _ape_opt2_origin_chunk.y
	_ape_opt1_world_center = (_ape_opt1_origin_chunk * 32.0 + APE_REGION_CENTER) * terrain.voxel_scale
	ape_opt1_root.global_position = _ape_opt1_world_center - Vector3(0.0, APE_ROOT_OFFSET_Y, 0.0)

	_ape_opt2_structure_id = _stamp_structure(ape_vreg_path, _ape_opt2_origin_chunk, 15.0)
	if _ape_opt2_structure_id <= 0:
		push_warning("[ApeDemo] Failed to stamp '%s' - bake it first: voxel_baker models/ape/ape.obj %s --voxel-scale 0.5 --watertight --texture models/ape/ape_albedo.ppm --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128" % [
			ape_vreg_path, ape_vreg_path
		])
	print("[ApeDemo] Option 1 (Pristine Mesh -> Voxel Swap) ready at ", _ape_opt1_world_center,
		" | Option 2 (Baked Per-Voxel Albedo, id=#", _ape_opt2_structure_id, ") stamped at ", _ape_opt2_world_center)

func _load_pristine_easter_island_mesh_and_collider() -> void:
	var mesh_candidates: Array[String] = [
		easter_island_mesh_path,
		"res://models/easter-island/easter_island.obj",
		"models/easter-island/easter_island.obj",
	]
	var albedo_candidates: Array[String] = [
		easter_island_albedo_path,
		"res://models/easter-island/easter_island_albedo.png",
		"models/easter-island/easter_island_albedo.png",
	]
	_load_pristine_statue_mesh(mesh_candidates, albedo_candidates, opt1_mesh_inst, opt1_col_shape, "EasterIslandDemo")

func _load_pristine_atlas_mesh_and_collider() -> void:
	var mesh_candidates: Array[String] = [
		atlas_mesh_path,
		"res://models/atlas/atlas.obj",
		"models/atlas/atlas.obj",
	]
	var albedo_candidates: Array[String] = [
		atlas_albedo_path,
		"res://models/atlas/atlas_albedo.png",
		"models/atlas/atlas_albedo.png",
	]
	_load_pristine_statue_mesh(mesh_candidates, albedo_candidates, atlas_opt1_mesh_inst, atlas_opt1_col_shape, "AtlasDemo")

func _load_pristine_ape_mesh_and_collider() -> void:
	var mesh_candidates: Array[String] = [
		ape_mesh_path,
		"res://models/ape/ape.obj",
		"models/ape/ape.obj",
	]
	var albedo_candidates: Array[String] = [
		ape_albedo_path,
		"res://models/ape/ape_albedo.png",
		"models/ape/ape_albedo.png",
	]
	_load_pristine_statue_mesh(mesh_candidates, albedo_candidates, ape_opt1_mesh_inst, ape_opt1_col_shape, "ApeDemo")

func _load_pristine_statue_mesh(mesh_candidates: Array[String], albedo_candidates: Array[String], target_mesh: MeshInstance3D, target_shape: CollisionShape3D, tag: String) -> void:
	var f: FileAccess = null
	for path in mesh_candidates:
		f = FileAccess.open(path, FileAccess.READ)
		if f != null:
			break
	if f == null:
		push_warning("[%s] Could not open .obj for Option 1 PristineMesh" % tag)
		return

	var raw_positions := PackedVector3Array()
	var raw_uvs := PackedVector2Array()
	var out_verts := PackedVector3Array()
	var out_norms := PackedVector3Array()
	var out_uvs := PackedVector2Array()

	while not f.eof_reached():
		var line := f.get_line().strip_edges()
		if line.is_empty() or line.begins_with("#"):
			continue
		var parts := line.split(" ", false)
		if parts.is_empty():
			continue
		if parts[0] == "v" and parts.size() >= 4:
			raw_positions.push_back(Vector3(parts[1].to_float(), parts[2].to_float(), parts[3].to_float()))
		elif parts[0] == "vt" and parts.size() >= 3:
			raw_uvs.push_back(Vector2(parts[1].to_float(), 1.0 - parts[2].to_float()))
		elif parts[0] == "f" and parts.size() >= 4:
			var face_v: Array[int] = []
			var face_vt: Array[int] = []
			for i in range(1, parts.size()):
				var sub := parts[i].split("/")
				face_v.push_back(sub[0].to_int() - 1)
				if sub.size() > 1 and not sub[1].is_empty():
					face_vt.push_back(sub[1].to_int() - 1)
				else:
					face_vt.push_back(0)
			for i in range(1, face_v.size() - 1):
				var i0 := face_v[0]
				var i1 := face_v[i]
				var i2 := face_v[i + 1]
				var p0 := raw_positions[i0]
				var p1 := raw_positions[i1]
				var p2 := raw_positions[i2]
				var n := (p1 - p0).cross(p2 - p0).normalized()
				out_verts.push_back(p0)
				out_verts.push_back(p2)
				out_verts.push_back(p1)
				out_norms.push_back(n)
				out_norms.push_back(n)
				out_norms.push_back(n)
				if not raw_uvs.is_empty():
					out_uvs.push_back(raw_uvs[face_vt[0]])
					out_uvs.push_back(raw_uvs[face_vt[i + 1]])
					out_uvs.push_back(raw_uvs[face_vt[i]])

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = out_verts
	arrays[Mesh.ARRAY_NORMAL] = out_norms
	if out_uvs.size() == out_verts.size():
		arrays[Mesh.ARRAY_TEX_UV] = out_uvs

	var arr_mesh := ArrayMesh.new()
	arr_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var mat := StandardMaterial3D.new()
	mat.roughness = 0.78
	var albedo_loaded := false
	for path in albedo_candidates:
		if not ResourceLoader.exists(path):
			continue
		var tex := load(path) as Texture2D
		if tex != null:
			mat.albedo_texture = tex
			albedo_loaded = true
			break
	# Fallback for a raw PNG that has not been imported as a Texture2D resource yet.
	if not albedo_loaded:
		for path in albedo_candidates:
			var img := Image.new()
			if img.load(path) == OK:
				mat.albedo_texture = ImageTexture.create_from_image(img)
				albedo_loaded = true
				break
	if not albedo_loaded:
		mat.albedo_color = Color(0.57, 0.52, 0.45)

	target_mesh.mesh = arr_mesh
	target_mesh.material_override = mat
	target_mesh.visible = true

	var shape := ConcavePolygonShape3D.new()
	shape.set_faces(out_verts)
	target_shape.shape = shape
	target_shape.disabled = false

## Option 1 starts as a pristine mesh. The first explosion or elemental sphere
## whose blast volume reaches the statue (or that directly hits its StaticBody3D)
## hides the mesh/collider and stamps the exact same Easter Island statue into the
## voxel terrain, immediately followed by the normal crater carve in the same frame.
func _check_and_swap_opt1_easter_island_on_blast(blast_pos: Vector3, blast_radius: float, hit_collider: Object) -> void:
	if _opt1_swapped_to_voxels:
		return
	var direct_hit := (hit_collider == opt1_static_body)
	var dx: float = max(0.0, abs(blast_pos.x - _opt1_world_center.x) - EASTER_ISLAND_HALF_EXTENTS.x)
	var dy: float = max(0.0, abs(blast_pos.y - _opt1_world_center.y) - EASTER_ISLAND_HALF_EXTENTS.y)
	var dz: float = max(0.0, abs(blast_pos.z - _opt1_world_center.z) - EASTER_ISLAND_HALF_EXTENTS.z)
	var dist_to_box := sqrt(dx * dx + dy * dy + dz * dz)

	if direct_hit or dist_to_box <= blast_radius + 2.0:
		_opt1_structure_id = _stamp_structure(easter_island_vreg_path, _opt1_origin_chunk, 0.0)
		_opt1_swapped_to_voxels = true
		_note_undo_cpp_op({
			"kind": "opt1_swap",
			"statue": "easter_island",
			"structure_id": _opt1_structure_id,
			"vreg_path": easter_island_vreg_path,
			"origin_chunk": _opt1_origin_chunk,
		})
		_queue_pristine_hide(opt1_mesh_inst, opt1_col_shape, opt1_label,
			"Easter Island Option 1 (16x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _opt1_structure_id)
		print("[EasterIslandSwap] Option 1 statue hit at ", blast_pos, " -> Stamped Voxel Structure #", _opt1_structure_id, ", hiding PristineMesh when committed!")

## Same pristine-mesh-to-voxel swap for the Atlas Option 1 statue.
func _check_and_swap_atlas_opt1_on_blast(blast_pos: Vector3, blast_radius: float, hit_collider: Object) -> void:
	if _atlas_opt1_swapped_to_voxels:
		return
	var direct_hit := (hit_collider == atlas_opt1_static_body)
	var dx: float = max(0.0, abs(blast_pos.x - _atlas_opt1_world_center.x) - ATLAS_HALF_EXTENTS.x)
	var dy: float = max(0.0, abs(blast_pos.y - _atlas_opt1_world_center.y) - ATLAS_HALF_EXTENTS.y)
	var dz: float = max(0.0, abs(blast_pos.z - _atlas_opt1_world_center.z) - ATLAS_HALF_EXTENTS.z)
	var dist_to_box := sqrt(dx * dx + dy * dy + dz * dz)

	if direct_hit or dist_to_box <= blast_radius + 2.0:
		_atlas_opt1_structure_id = _stamp_structure(atlas_vreg_path, _atlas_opt1_origin_chunk, 0.0)
		_atlas_opt1_swapped_to_voxels = true
		_note_undo_cpp_op({
			"kind": "opt1_swap",
			"statue": "atlas",
			"structure_id": _atlas_opt1_structure_id,
			"vreg_path": atlas_vreg_path,
			"origin_chunk": _atlas_opt1_origin_chunk,
		})
		_queue_pristine_hide(atlas_opt1_mesh_inst, atlas_opt1_col_shape, atlas_opt1_label,
			"Atlas Option 1 (5.33x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _atlas_opt1_structure_id)
		print("[AtlasSwap] Option 1 Atlas hit at ", blast_pos, " -> Stamped Voxel Structure #", _atlas_opt1_structure_id, ", hiding PristineMesh when committed!")

## Same pristine-mesh-to-voxel swap for the Ape Option 1 statue.
func _check_and_swap_ape_opt1_on_blast(blast_pos: Vector3, blast_radius: float, hit_collider: Object) -> void:
	if _ape_opt1_swapped_to_voxels:
		return
	var direct_hit := (hit_collider == ape_opt1_static_body)
	var dx: float = max(0.0, abs(blast_pos.x - _ape_opt1_world_center.x) - APE_HALF_EXTENTS.x)
	var dy: float = max(0.0, abs(blast_pos.y - _ape_opt1_world_center.y) - APE_HALF_EXTENTS.y)
	var dz: float = max(0.0, abs(blast_pos.z - _ape_opt1_world_center.z) - APE_HALF_EXTENTS.z)
	var dist_to_box := sqrt(dx * dx + dy * dy + dz * dz)

	if direct_hit or dist_to_box <= blast_radius + 2.0:
		_ape_opt1_structure_id = _stamp_structure(ape_vreg_path, _ape_opt1_origin_chunk, 0.0)
		_ape_opt1_swapped_to_voxels = true
		_note_undo_cpp_op({
			"kind": "opt1_swap",
			"statue": "ape",
			"structure_id": _ape_opt1_structure_id,
			"vreg_path": ape_vreg_path,
			"origin_chunk": _ape_opt1_origin_chunk,
		})
		_queue_pristine_hide(ape_opt1_mesh_inst, ape_opt1_col_shape, ape_opt1_label,
			"Ape Option 1 (5.33x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _ape_opt1_structure_id)
		print("[ApeSwap] Option 1 Ape hit at ", blast_pos, " -> Stamped Voxel Structure #", _ape_opt1_structure_id, ", hiding PristineMesh when committed!")

## Key R reset loop. Option 1 is removed via terrain.remove_structure(), which
## restores the pre-stamp chunks bit-exactly and brings back the pristine mesh +
## collider. Option 2 is re-stamped if it was removed successfully.
func _reset_statues() -> void:
	_reset_easter_island_statues()
	_reset_atlas_statues()
	_reset_ape_statues()

func _reset_easter_island_statues() -> void:
	if _opt1_swapped_to_voxels and _opt1_structure_id > 0:
		var ok := _remove_structure(_opt1_structure_id)
		print("[EasterIslandReset] Removed Option 1 voxel structure #", _opt1_structure_id, " (restored pre-stamp chunks: ", ok, ")")
	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0
	_cancel_pending_pristine_hide(opt1_mesh_inst)
	opt1_mesh_inst.visible = true
	opt1_col_shape.disabled = false
	opt1_label.text = "Easter Island Option 1 (16x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"

	var can_restamp_opt2 := true
	if _opt2_structure_id > 0:
		can_restamp_opt2 = _remove_structure(_opt2_structure_id)
	_opt2_structure_id = 0
	if can_restamp_opt2:
		_opt2_structure_id = _stamp_structure(easter_island_vreg_path, _opt2_origin_chunk, 15.0)
	print("[EasterIslandReset] Both Easter Island statues restored to pristine state! (Option 2 id=#", _opt2_structure_id, ")")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _reset_atlas_statues() -> void:
	if _atlas_opt1_swapped_to_voxels and _atlas_opt1_structure_id > 0:
		var ok := _remove_structure(_atlas_opt1_structure_id)
		print("[AtlasReset] Removed Option 1 voxel structure #", _atlas_opt1_structure_id, " (restored pre-stamp chunks: ", ok, ")")
	_atlas_opt1_swapped_to_voxels = false
	_atlas_opt1_structure_id = 0
	_cancel_pending_pristine_hide(atlas_opt1_mesh_inst)
	atlas_opt1_mesh_inst.visible = true
	atlas_opt1_col_shape.disabled = false
	atlas_opt1_label.text = "Atlas Option 1 (5.33x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"

	var can_restamp_opt2 := true
	if _atlas_opt2_structure_id > 0:
		can_restamp_opt2 = _remove_structure(_atlas_opt2_structure_id)
	_atlas_opt2_structure_id = 0
	if can_restamp_opt2:
		_atlas_opt2_structure_id = _stamp_structure(atlas_vreg_path, _atlas_opt2_origin_chunk, 15.0)
	print("[AtlasReset] Both Atlas statues restored to pristine state! (Option 2 id=#", _atlas_opt2_structure_id, ")")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _reset_ape_statues() -> void:
	if _ape_opt1_swapped_to_voxels and _ape_opt1_structure_id > 0:
		var ok := _remove_structure(_ape_opt1_structure_id)
		print("[ApeReset] Removed Option 1 voxel structure #", _ape_opt1_structure_id, " (restored pre-stamp chunks: ", ok, ")")
	_ape_opt1_swapped_to_voxels = false
	_ape_opt1_structure_id = 0
	_cancel_pending_pristine_hide(ape_opt1_mesh_inst)
	ape_opt1_mesh_inst.visible = true
	ape_opt1_col_shape.disabled = false
	ape_opt1_label.text = "Ape Option 1 (5.33x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"

	var can_restamp_opt2 := true
	if _ape_opt2_structure_id > 0:
		can_restamp_opt2 = _remove_structure(_ape_opt2_structure_id)
	_ape_opt2_structure_id = 0
	if can_restamp_opt2:
		_ape_opt2_structure_id = _stamp_structure(ape_vreg_path, _ape_opt2_origin_chunk, 15.0)
	print("[ApeReset] Both Ape statues restored to pristine state! (Option 2 id=#", _ape_opt2_structure_id, ")")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

## Removes one stamped voxel structure. The reset binding lives in the
## GDExtension core; if the loaded binary predates it, warn instead of crashing.
func _remove_structure(structure_id: int) -> bool:
	if structure_id <= 0:
		return false
	if not terrain.has_method("remove_structure"):
		push_warning("[StatueReset] VoxelTerrain3D has no remove_structure(); rebuild the GDExtension with the asset-stamp reset binding.")
		return false
	var ok: bool = bool(terrain.call("remove_structure", structure_id))
	return ok

## Stamps a baked .vreg structure through the dynamic binding. Using `call()`
## keeps the demo parsing when the editor still has an older GDExtension loaded;
## the launched game process loads the freshly built one from disk.
func _stamp_structure(vreg_path: String, origin: Vector3, recreate_delay: float) -> int:
	if not terrain.has_method("stamp_asset"):
		push_warning("[VoxelTerrain3D] stamp_asset() missing; rebuild the GDExtension.")
		return 0
	return int(terrain.call("stamp_asset", vreg_path, origin, recreate_delay))

## Queues an Option 1 pristine mesh for hiding once its stamped voxel chunks have
## finished remeshing. Hiding immediately leaves an empty hole for the roughly one
## second the stamp's chunks spend in the worker/dirty queues.
func _queue_pristine_hide(mesh: MeshInstance3D, shape: CollisionShape3D, label: Label3D, text: String) -> void:
	_pending_pristine_hides.append({
		"mesh": mesh,
		"shape": shape,
		"label": label,
		"text": text,
		"frames": 2,
		"idle_frames": 0,
		"elapsed": 0.0,
	})

## Drops a queued hide when Key R restores the pristine mesh before the swap
## finished, so the deferred hide cannot blank the restored mesh afterwards.
func _cancel_pending_pristine_hide(mesh: MeshInstance3D) -> void:
	for i in range(_pending_pristine_hides.size() - 1, -1, -1):
		if _pending_pristine_hides[i].get("mesh") == mesh:
			_pending_pristine_hides.remove_at(i)

func _update_pending_pristine_hides(delta: float) -> void:
	for i in range(_pending_pristine_hides.size() - 1, -1, -1):
		var pending: Dictionary = _pending_pristine_hides[i]
		pending["frames"] = int(pending["frames"]) - 1
		pending["elapsed"] = float(pending["elapsed"]) + delta
		if bool(terrain.core_idle()):
			pending["idle_frames"] = int(pending["idle_frames"]) + 1
		else:
			pending["idle_frames"] = 0
		if int(pending["frames"]) > 0:
			continue
		# core_idle() is false while the stamp's chunks are queued, dirty or
		# remeshing. Once it settles, wait ~24 frames so the main-thread commit
		# budget (at most 2 non-empty meshes per frame) can drain the completion
		# queue; fully_committed() also covers the navigation sync path.
		var meshes_drained := int(pending["idle_frames"]) >= 24
		if not bool(terrain.fully_committed()) and not meshes_drained \
				and float(pending["elapsed"]) < 8.0:
			continue
		var mesh := pending["mesh"] as MeshInstance3D
		var shape := pending["shape"] as CollisionShape3D
		var label := pending["label"] as Label3D
		if is_instance_valid(mesh):
			mesh.visible = false
		if is_instance_valid(shape):
			shape.disabled = true
		if is_instance_valid(label):
			label.text = String(pending["text"])
		_pending_pristine_hides.remove_at(i)

func _setup_phantom_camera() -> void:
	_base_fov = camera.fov
	_pcam_host = PhantomCameraHost.new()
	_pcam_host.name = "PhantomCameraHost"
	camera.add_child(_pcam_host)

	var instant := PhantomCameraTween.new()
	instant.duration = 0.0
	var blend := PhantomCameraTween.new()
	blend.duration = 0.25
	blend.transition = PhantomCameraTween.TransitionType.SINE
	blend.ease = PhantomCameraTween.EaseType.EASE_IN_OUT

	_fly_pcam = PhantomCamera3D.new()
	_fly_pcam.name = "FlyPhantomCamera3D"
	_fly_pcam.top_level = true
	_fly_pcam.follow_mode = PhantomCamera3D.FollowMode.NONE
	_fly_pcam.priority = 0
	_fly_pcam.tween_resource = instant
	_fly_pcam.global_transform = camera.global_transform
	add_child(_fly_pcam)

	_play_pcam = PhantomCamera3D.new()
	_play_pcam.name = "PlayPhantomCamera3D"
	_play_pcam.top_level = true
	_play_pcam.follow_mode = PhantomCamera3D.FollowMode.NONE
	_play_pcam.priority = 0
	_play_pcam.tween_resource = blend
	_play_pcam.global_transform = camera.global_transform
	add_child(_play_pcam)

	_fly_pcam.priority = 1

func _toggle_mode() -> void:
	if _mode == Mode.BENCHMARK:
		_stop_benchmark(true)
		return
	if _mode == Mode.FLY:
		_enter_play()
	else:
		_exit_play()

func _setup_pickaxe_viewmodel() -> void:
	_viewmodel_root = camera.get_node_or_null("ViewmodelRoot") as Node3D
	if _viewmodel_root == null:
		_viewmodel_root = Node3D.new()
		_viewmodel_root.name = "ViewmodelRoot"
		camera.add_child(_viewmodel_root)
	_viewmodel_root.position = BASE_VIEWMODEL_POS
	_viewmodel_root.visible = (_mode == Mode.PLAY)

	_shoulder_pivot = _viewmodel_root.get_node_or_null("ShoulderPivot") as Node3D
	if _shoulder_pivot == null:
		_shoulder_pivot = Node3D.new()
		_shoulder_pivot.name = "ShoulderPivot"
		_viewmodel_root.add_child(_shoulder_pivot)
	_shoulder_pivot.rotation = BASE_SHOULDER_ROT

	_elbow_pivot = _shoulder_pivot.get_node_or_null("ElbowPivot") as Node3D
	if _elbow_pivot == null:
		_elbow_pivot = Node3D.new()
		_elbow_pivot.name = "ElbowPivot"
		_shoulder_pivot.add_child(_elbow_pivot)
	_elbow_pivot.position = Vector3(-0.04, 0.06, -0.26)
	_elbow_pivot.rotation = BASE_ELBOW_ROT

	_wrist_pivot = _elbow_pivot.get_node_or_null("WristPivot") as Node3D
	if _wrist_pivot == null:
		_wrist_pivot = Node3D.new()
		_wrist_pivot.name = "WristPivot"
		_elbow_pivot.add_child(_wrist_pivot)
	_wrist_pivot.position = Vector3(-0.05, 0.08, -0.28)
	_wrist_pivot.rotation = BASE_WRIST_ROT

	_pickaxe_root = _wrist_pivot.get_node_or_null("PickaxeRoot") as Node3D
	if _pickaxe_root == null:
		_pickaxe_root = Node3D.new()
		_pickaxe_root.name = "PickaxeRoot"
		_wrist_pivot.add_child(_pickaxe_root)

	var sleeve_mat := StandardMaterial3D.new()
	sleeve_mat.albedo_color = Color(0.20, 0.25, 0.32)
	sleeve_mat.roughness = 0.82

	var skin_mat := StandardMaterial3D.new()
	skin_mat.albedo_color = Color(0.84, 0.65, 0.52)
	skin_mat.roughness = 0.65

	var bracer_mat := StandardMaterial3D.new()
	bracer_mat.albedo_color = Color(0.34, 0.23, 0.15)
	bracer_mat.roughness = 0.72
	bracer_mat.metallic = 0.15

	var glove_mat := StandardMaterial3D.new()
	glove_mat.albedo_color = Color(0.24, 0.18, 0.13)
	glove_mat.roughness = 0.78

	var wood_handle_mat := StandardMaterial3D.new()
	wood_handle_mat.albedo_color = Color(0.46, 0.29, 0.15)
	wood_handle_mat.roughness = 0.75

	var grip_wrap_mat := StandardMaterial3D.new()
	grip_wrap_mat.albedo_color = Color(0.18, 0.14, 0.11)
	grip_wrap_mat.roughness = 0.88

	var dark_iron_mat := StandardMaterial3D.new()
	dark_iron_mat.albedo_color = Color(0.28, 0.30, 0.34)
	dark_iron_mat.metallic = 0.82
	dark_iron_mat.roughness = 0.36

	_pickaxe_accent_mat = StandardMaterial3D.new()
	_pickaxe_accent_mat.albedo_color = Color(0.76, 0.78, 0.84)
	_pickaxe_accent_mat.metallic = 0.90
	_pickaxe_accent_mat.roughness = 0.24
	_pickaxe_accent_mat.emission_enabled = true
	_pickaxe_accent_mat.emission = Color(1.0, 0.55, 0.12) * 0.45

	# 1. Upper Arm (Shoulder -> Elbow, overlapped across the elbow joint)
	var upper_arm := MeshInstance3D.new()
	upper_arm.name = "UpperArmMesh"
	var upper_cap := CapsuleMesh.new()
	upper_cap.radius = 0.068
	upper_cap.height = 0.42
	upper_arm.mesh = upper_cap
	upper_arm.material_override = sleeve_mat
	upper_arm.position = Vector3(-0.02, 0.02, -0.11)
	upper_arm.rotation = Vector3(PI * 0.43, 0.15, 0.0)
	upper_arm.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_shoulder_pivot.add_child(upper_arm)

	# 2. Forearm & Leather Bracer (Elbow -> Wrist, overlapped with upper arm & hand)
	var forearm := MeshInstance3D.new()
	forearm.name = "ForearmMesh"
	var forearm_cap := CapsuleMesh.new()
	forearm_cap.radius = 0.058
	forearm_cap.height = 0.38
	forearm.mesh = forearm_cap
	forearm.material_override = skin_mat
	forearm.position = Vector3(-0.025, 0.04, -0.14)
	forearm.rotation = Vector3(PI * 0.41, 0.17, 0.0)
	forearm.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_elbow_pivot.add_child(forearm)

	var bracer := MeshInstance3D.new()
	bracer.name = "ForearmBracerMesh"
	var bracer_cyl := CylinderMesh.new()
	bracer_cyl.top_radius = 0.056
	bracer_cyl.bottom_radius = 0.064
	bracer_cyl.height = 0.16
	bracer.mesh = bracer_cyl
	bracer.material_override = bracer_mat
	bracer.position = Vector3(-0.035, 0.055, -0.19)
	bracer.rotation = Vector3(PI * 0.42, 0.16, 0.0)
	bracer.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_elbow_pivot.add_child(bracer)

	# 3. Gloved Hand & Fingers at WristPivot
	var hand := MeshInstance3D.new()
	hand.name = "HandFistMesh"
	var hand_box := BoxMesh.new()
	hand_box.size = Vector3(0.105, 0.115, 0.115)
	hand.mesh = hand_box
	hand.material_override = glove_mat
	hand.position = Vector3(0.0, 0.02, -0.04)
	hand.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_wrist_pivot.add_child(hand)

	var fingers := MeshInstance3D.new()
	fingers.name = "FingerKnucklesMesh"
	var fingers_box := BoxMesh.new()
	fingers_box.size = Vector3(0.085, 0.095, 0.055)
	fingers.mesh = fingers_box
	fingers.material_override = skin_mat
	fingers.position = Vector3(-0.025, 0.02, -0.09)
	fingers.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_wrist_pivot.add_child(fingers)

	# 4. Pickaxe Assembly inside _pickaxe_root
	_pickaxe_root.position = Vector3(0.0, 0.02, -0.05)
	_pickaxe_root.rotation = Vector3(-0.28, 0.10, 0.08)

	var shaft := MeshInstance3D.new()
	shaft.name = "HandleShaft"
	var shaft_cyl := CylinderMesh.new()
	shaft_cyl.top_radius = 0.026
	shaft_cyl.bottom_radius = 0.022
	shaft_cyl.height = 0.78
	shaft.mesh = shaft_cyl
	shaft.material_override = wood_handle_mat
	shaft.position = Vector3(0.0, 0.18, 0.0)
	shaft.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(shaft)

	var grip := MeshInstance3D.new()
	grip.name = "HandleGripWrap"
	var grip_cyl := CylinderMesh.new()
	grip_cyl.top_radius = 0.028
	grip_cyl.bottom_radius = 0.027
	grip_cyl.height = 0.24
	grip.mesh = grip_cyl
	grip.material_override = grip_wrap_mat
	grip.position = Vector3(0.0, 0.02, 0.0)
	grip.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(grip)

	var pommel := MeshInstance3D.new()
	pommel.name = "PommelCap"
	var pommel_cyl := CylinderMesh.new()
	pommel_cyl.top_radius = 0.030
	pommel_cyl.bottom_radius = 0.036
	pommel_cyl.height = 0.05
	pommel.mesh = pommel_cyl
	pommel.material_override = dark_iron_mat
	pommel.position = Vector3(0.0, -0.21, 0.0)
	pommel.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(pommel)

	var collar := MeshInstance3D.new()
	collar.name = "HeadCollar"
	var collar_box := BoxMesh.new()
	collar_box.size = Vector3(0.085, 0.11, 0.13)
	collar.mesh = collar_box
	collar.material_override = dark_iron_mat
	collar.position = Vector3(0.0, 0.50, 0.0)
	collar.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(collar)

	var top_stud := MeshInstance3D.new()
	top_stud.name = "TopSpikeStud"
	var stud_cyl := CylinderMesh.new()
	stud_cyl.top_radius = 0.006
	stud_cyl.bottom_radius = 0.028
	stud_cyl.height = 0.07
	top_stud.mesh = stud_cyl
	top_stud.material_override = _pickaxe_accent_mat
	top_stud.position = Vector3(0.0, 0.59, 0.0)
	top_stud.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(top_stud)

	var front_arm := MeshInstance3D.new()
	front_arm.name = "FrontPickArm"
	var front_arm_box := BoxMesh.new()
	front_arm_box.size = Vector3(0.052, 0.068, 0.24)
	front_arm.mesh = front_arm_box
	front_arm.material_override = dark_iron_mat
	front_arm.position = Vector3(0.0, 0.49, -0.14)
	front_arm.rotation = Vector3(0.18, 0.0, 0.0)
	front_arm.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(front_arm)

	var front_tip := MeshInstance3D.new()
	front_tip.name = "FrontPickSpikeTip"
	var front_tip_prism := PrismMesh.new()
	front_tip_prism.size = Vector3(0.048, 0.20, 0.064)
	front_tip.mesh = front_tip_prism
	front_tip.material_override = _pickaxe_accent_mat
	front_tip.position = Vector3(0.0, 0.0, -0.20)
	front_tip.rotation = Vector3(-PI * 0.5, 0.0, 0.0)
	front_tip.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	front_arm.add_child(front_tip)

	var back_arm := MeshInstance3D.new()
	back_arm.name = "BackPickArm"
	var back_arm_box := BoxMesh.new()
	back_arm_box.size = Vector3(0.054, 0.068, 0.18)
	back_arm.mesh = back_arm_box
	back_arm.material_override = dark_iron_mat
	back_arm.position = Vector3(0.0, 0.49, 0.11)
	back_arm.rotation = Vector3(-0.15, 0.0, 0.0)
	back_arm.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_pickaxe_root.add_child(back_arm)

	var back_blade := MeshInstance3D.new()
	back_blade.name = "BackChiselWedge"
	var back_prism := PrismMesh.new()
	back_prism.size = Vector3(0.064, 0.14, 0.064)
	back_blade.mesh = back_prism
	back_blade.material_override = _pickaxe_accent_mat
	back_blade.position = Vector3(0.0, 0.0, 0.15)
	back_blade.rotation = Vector3(PI * 0.5, 0.0, 0.0)
	back_blade.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	back_arm.add_child(back_blade)

	_pickaxe_tip_marker = Marker3D.new()
	_pickaxe_tip_marker.name = "PickaxeTipMarker"
	_pickaxe_tip_marker.position = Vector3(0.0, 0.41, -0.42)
	_pickaxe_root.add_child(_pickaxe_tip_marker)

	_impact_sparks = CPUParticles3D.new()
	_impact_sparks.name = "PickaxeImpactSparks"
	_impact_sparks.emitting = false
	_impact_sparks.one_shot = true
	_impact_sparks.amount = 28
	_impact_sparks.lifetime = 0.38
	_impact_sparks.explosiveness = 0.95
	var spark_mesh := BoxMesh.new()
	spark_mesh.size = Vector3(0.14, 0.14, 0.14)
	var spark_mat := StandardMaterial3D.new()
	spark_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	spark_mat.albedo_color = Color(1.0, 0.82, 0.32)
	spark_mesh.material = spark_mat
	_impact_sparks.mesh = spark_mesh
	_impact_sparks.direction = Vector3(0.0, 1.0, 0.0)
	_impact_sparks.spread = 75.0
	_impact_sparks.initial_velocity_min = 6.0
	_impact_sparks.initial_velocity_max = 16.0
	_impact_sparks.gravity = Vector3(0.0, -24.0, 0.0)
	add_child(_impact_sparks)

func _spawn_pickaxe_impact_sparks(world_pos: Vector3, hit_normal: Vector3) -> void:
	if _impact_sparks == null:
		return
	_impact_sparks.global_position = world_pos
	_impact_sparks.direction = hit_normal.normalized() if hit_normal.length_squared() > 0.01 else Vector3.UP
	_impact_sparks.restart()
	_impact_sparks.emitting = true


func _setup_hero_visual_rig() -> void:
	_hero_visual_root = Node3D.new()
	_hero_visual_root.name = "HeroVisualRig"
	_hero_visual_root.visible = false
	add_child(_hero_visual_root)

	# Stylized Deadlock-inspired hero capsule body
	_hero_torso_mesh = MeshInstance3D.new()
	var cap := CapsuleMesh.new()
	cap.radius = play_body_radius * 0.88
	cap.height = play_body_height
	var body_mat := StandardMaterial3D.new()
	body_mat.albedo_color = Color(0.18, 0.24, 0.34)
	body_mat.metallic = 0.35
	body_mat.roughness = 0.45
	cap.material = body_mat
	_hero_torso_mesh.mesh = cap
	_hero_torso_mesh.position = Vector3(0.0, play_body_height * 0.5, 0.0)
	_hero_visual_root.add_child(_hero_torso_mesh)

	# Glowing visor showing hero facing & pitch
	_hero_visor_mesh = MeshInstance3D.new()
	var visor_box := BoxMesh.new()
	visor_box.size = Vector3(0.48, 0.16, 0.28)
	var visor_mat := StandardMaterial3D.new()
	visor_mat.albedo_color = Color(0.15, 0.95, 1.0)
	visor_mat.emission_enabled = true
	visor_mat.emission = Color(0.15, 0.90, 1.0) * 1.4
	visor_box.material = visor_mat
	_hero_visor_mesh.mesh = visor_box
	_hero_visor_mesh.position = Vector3(0.0, play_eye_height - 0.12, -play_body_radius * 0.72)
	_hero_visual_root.add_child(_hero_visor_mesh)

func _setup_ability_system_and_beam_visuals() -> void:
	_gas = VoxelAbilitySystem.new()
	_gas.name = "VoxelAbilitySystem"
	_gas.terrain = terrain
	_gas.owner_node = self
	add_child(_gas)
	_gas.register_target(ai_companion, 750.0)

	_gas.ability_cast_blocked.connect(func(ability_id: String, reason: String) -> void:
		print("[GAS] %s blocked: %s" % [ability_id, reason])
	)
	_gas.status_effect_applied.connect(func(target: Node3D, effect_id: String, stacks: int, dur: float) -> void:
		var t_name := "Hero" if target == self else "Companion"
		print("[GAS] Status Applied -> %s: %s (stacks=%d, %.1fs)" % [t_name, effect_id.to_upper(), stacks, dur])
	)
	_gas.elemental_combo_triggered.connect(func(combo_name: String, world_pos: Vector3, radius: float) -> void:
		print("[GAS] *** %s *** at %s (scaled radius=%.1f m)!" % [combo_name, str(world_pos), radius])
	)

	# 3D Bebop Hyper Beam ribbon & molten impact core
	_beam_mesh_inst = MeshInstance3D.new()
	_beam_immediate_mesh = ImmediateMesh.new()
	var beam_mat := StandardMaterial3D.new()
	beam_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	beam_mat.albedo_color = Color(1.0, 0.42, 0.08)
	_beam_mesh_inst.mesh = _beam_immediate_mesh
	_beam_mesh_inst.material_override = beam_mat
	add_child(_beam_mesh_inst)

	_beam_impact_sphere = MeshInstance3D.new()
	var imp_sph := SphereMesh.new()
	imp_sph.radius = 0.85
	imp_sph.height = 1.7
	var imp_mat := StandardMaterial3D.new()
	imp_mat.albedo_color = Color(1.0, 0.55, 0.1)
	imp_mat.emission_enabled = true
	imp_mat.emission = Color(1.0, 0.35, 0.05) * 2.5
	imp_sph.material = imp_mat
	_beam_impact_sphere.mesh = imp_sph
	_beam_impact_sphere.visible = false
	add_child(_beam_impact_sphere)

	# 3D Ricochet Bullet Tracer Mesh (draws primary shots + SDF wall bounces)
	_tracer_mesh_inst = MeshInstance3D.new()
	_tracer_immediate_mesh = ImmediateMesh.new()
	var tracer_mat := StandardMaterial3D.new()
	tracer_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	tracer_mat.vertex_color_use_as_albedo = true
	_tracer_mesh_inst.mesh = _tracer_immediate_mesh
	_tracer_mesh_inst.material_override = tracer_mat
	add_child(_tracer_mesh_inst)

func _enter_play() -> void:
	_mode = Mode.PLAY
	_tool_pressed = false
	_play_fire_held = false
	_play_ads_held = false
	_ads_blend = 0.0
	_swing_active = false
	_swing_hit_done = false
	_swing_elapsed = 0.0
	_swing_hit_solid_recoil = 0.0
	if _viewmodel_root != null:
		_viewmodel_root.visible = true
		_viewmodel_root.position = BASE_VIEWMODEL_POS
		_shoulder_pivot.rotation = BASE_SHOULDER_ROT
		_elbow_pivot.rotation = BASE_ELBOW_ROT
		_wrist_pivot.rotation = BASE_WRIST_ROT
	_ice_path_active = false
	_last_ice_path_anchor = Vector3(INF, INF, INF)
	_stamina_charges = play_max_stamina
	_stamina_regen_progress = 0.0
	var eye := camera.global_position
	var ground_y := _ground_probe(eye + Vector3(0.0, play_step_height, 0.0), 400.0)
	_spawn_pending = not is_finite(ground_y)
	_spawn_wait = 0.0
	_player_feet = Vector3(eye.x, eye.y - play_eye_height, eye.z)
	if is_finite(ground_y):
		_player_feet.y = ground_y
	else:
		var aim := _pickaxe_aim_point()
		if is_finite(aim.x):
			var aim_ground := _ground_probe(aim + Vector3(0.0, play_step_height, 0.0), 400.0)
			_player_feet = Vector3(aim.x, aim_ground if is_finite(aim_ground) else aim.y, aim.z)
			_spawn_pending = false
	_last_safe_feet = _player_feet
	_player_vel = Vector3.ZERO
	_player_vel_y = 0.0
	_player_grounded = true
	_hero_state = HeroState.GROUNDED
	camera.fov = play_fov
	_update_hero_camera_and_visuals(0.0)
	_play_pcam.priority = 1
	_fly_pcam.priority = 0
	print("[HeroController] Entered Play Mode (3rd-Person=%s | V toggles view | Shift=Dash | Space=Jump/Mantle | Ctrl=Slide | E=Ice Path | Q=Ground Slam)" % str(play_third_person))

func _exit_play() -> void:
	_mode = Mode.FLY
	_play_fire_held = false
	_play_ads_held = false
	_ads_blend = 0.0
	_swing_active = false
	_swing_hit_done = false
	if _viewmodel_root != null:
		_viewmodel_root.visible = false
	_ice_path_active = false
	_player_vel = Vector3.ZERO
	_player_vel_y = 0.0
	_player_grounded = false
	_velocity = Vector3.ZERO
	if _hero_visual_root != null:
		_hero_visual_root.visible = false
	camera.fov = _base_fov
	_fly_pcam.global_transform = camera.global_transform
	_fly_pcam.priority = 1
	_play_pcam.priority = 0
	print("[HeroController] Exited to Fly Mode (F to enter Hero Play Mode)")

func _start_pickaxe_swing() -> void:
	if _swing_active:
		return
	_swing_active = true
	_swing_hit_done = false
	_swing_elapsed = 0.0
	_swing_hit_solid_recoil = 0.0
	_swing_impact_time = PICKAXE_SWING_TIME * pickaxe_impact_fraction

func _update_pickaxe_swing(delta: float) -> void:
	if _viewmodel_root == null:
		return
	# Allow holding Left Mouse Button in Play mode to continuously swing the pickaxe
	if _mode == Mode.PLAY and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		if not _swing_active:
			_start_pickaxe_swing()

	_mouse_sway_offset = _mouse_sway_offset.lerp(Vector2.ZERO, minf(1.0, delta * 12.0))
	var move_factor := clampf(_velocity.length() / maxf(0.1, play_run_speed), 0.0, 1.5)
	_idle_bob_time += delta * (2.2 + move_factor * 6.5)
	var bob_x := sin(_idle_bob_time * 0.5) * (0.006 + 0.014 * move_factor)
	var bob_y := absf(cos(_idle_bob_time)) * (0.008 + 0.018 * move_factor)
	var air_lag_y := clampf(-_player_vel_y * 0.0025, -0.04, 0.04)

	var target_pos := BASE_VIEWMODEL_POS + Vector3(
		bob_x + _mouse_sway_offset.x,
		-bob_y + _mouse_sway_offset.y + air_lag_y,
		0.0
	)
	var shoulder_rot := BASE_SHOULDER_ROT
	var elbow_rot := BASE_ELBOW_ROT
	var wrist_rot := BASE_WRIST_ROT

	if _swing_active:
		_swing_elapsed += delta
		var u := clampf(_swing_elapsed / maxf(0.05, PICKAXE_SWING_TIME), 0.0, 1.0)
		var impact_u := clampf(pickaxe_impact_fraction, 0.20, 0.75)
		var windup_end := impact_u * 0.62
		var follow_end := minf(0.90, impact_u + 0.24)

		# 1. Windup Pose: arm raises up and cocks back over the right shoulder
		var windup_shoulder := BASE_SHOULDER_ROT + Vector3(0.68, -0.26, -0.28)
		var windup_elbow := BASE_ELBOW_ROT + Vector3(0.46, 0.0, 0.16)
		var windup_wrist := BASE_WRIST_ROT + Vector3(0.42, 0.10, -0.12)
		var windup_offset := Vector3(0.06, 0.11, 0.08)

		# 2. Downstroke Strike Pose: arm drives forward and across toward the crosshair
		var strike_shoulder := BASE_SHOULDER_ROT + Vector3(-0.88, 0.48, 0.32)
		var strike_elbow := BASE_ELBOW_ROT + Vector3(-0.24, 0.12, -0.10)
		var strike_wrist := BASE_WRIST_ROT + Vector3(-0.92, -0.18, 0.18)
		var strike_offset := Vector3(-0.14, -0.08, -0.16)

		# 3. Follow-Through / Solid-Hit Recoil Pose
		var follow_shoulder := BASE_SHOULDER_ROT + Vector3(
			-0.52 + 0.42 * _swing_hit_solid_recoil,
			0.30 - 0.15 * _swing_hit_solid_recoil,
			0.18
		)
		var follow_elbow := BASE_ELBOW_ROT + Vector3(
			-0.12 + 0.30 * _swing_hit_solid_recoil,
			0.05,
			0.0
		)
		var follow_wrist := BASE_WRIST_ROT + Vector3(
			-0.55 + 0.48 * _swing_hit_solid_recoil,
			-0.08,
			0.10
		)
		var follow_offset := Vector3(-0.08, -0.05 + 0.06 * _swing_hit_solid_recoil, -0.06)

		if not _swing_hit_done and _swing_elapsed >= _swing_impact_time:
			_swing_hit_done = true
			var hit_surface := _apply_pickaxe_hit()
			_swing_hit_solid_recoil = 1.0 if hit_surface else 0.0

		if u < windup_end:
			var t := u / maxf(0.01, windup_end)
			var e := 1.0 - pow(1.0 - t, 2.5)
			shoulder_rot = BASE_SHOULDER_ROT.lerp(windup_shoulder, e)
			elbow_rot = BASE_ELBOW_ROT.lerp(windup_elbow, e)
			wrist_rot = BASE_WRIST_ROT.lerp(windup_wrist, e)
			target_pos += windup_offset * e
		elif u < impact_u:
			var t := (u - windup_end) / maxf(0.01, impact_u - windup_end)
			var e := t * t * (3.0 - 2.0 * t)
			shoulder_rot = windup_shoulder.lerp(strike_shoulder, e)
			elbow_rot = windup_elbow.lerp(strike_elbow, e)
			wrist_rot = windup_wrist.lerp(strike_wrist, e)
			target_pos += windup_offset.lerp(strike_offset, e)
		elif u < follow_end:
			var t := (u - impact_u) / maxf(0.01, follow_end - impact_u)
			var e := sin(t * PI * 0.5)
			shoulder_rot = strike_shoulder.lerp(follow_shoulder, e)
			elbow_rot = strike_elbow.lerp(follow_elbow, e)
			wrist_rot = strike_wrist.lerp(follow_wrist, e)
			target_pos += strike_offset.lerp(follow_offset, e)
		else:
			var t := (u - follow_end) / maxf(0.01, 1.0 - follow_end)
			var e := t * t * (3.0 - 2.0 * t)
			shoulder_rot = follow_shoulder.lerp(BASE_SHOULDER_ROT, e)
			elbow_rot = follow_elbow.lerp(BASE_ELBOW_ROT, e)
			wrist_rot = follow_wrist.lerp(BASE_WRIST_ROT, e)
			target_pos += follow_offset * (1.0 - e)

		if _swing_elapsed >= PICKAXE_SWING_TIME:
			_swing_active = false
			_swing_hit_done = false

	_viewmodel_root.position = target_pos
	_shoulder_pivot.rotation = shoulder_rot
	_elbow_pivot.rotation = elbow_rot
	_wrist_pivot.rotation = wrist_rot

func _apply_pickaxe_hit() -> bool:
	var point := _pickaxe_aim_point()
	if not is_finite(point.x):
		return false
	var hit := _raycast_crosshair()
	var hit_collider: Object = hit.get("collider") if not hit.is_empty() else null
	var hit_normal: Vector3 = hit.get("normal", Vector3.UP) if not hit.is_empty() else (camera.global_position - point).normalized()
	_spawn_pickaxe_impact_sparks(point, hit_normal)
	if _companion_initialized and _goal_mesh_inst.visible:
		_rebake_in_progress = true
	_begin_undo_action("Pickaxe hit")
	_check_and_swap_opt1_easter_island_on_blast(point, pickaxe_hit_radius, hit_collider)
	_check_and_swap_atlas_opt1_on_blast(point, pickaxe_hit_radius, hit_collider)
	_check_and_swap_ape_opt1_on_blast(point, pickaxe_hit_radius, hit_collider)
	terrain.trigger_explosion(point, pickaxe_hit_radius)
	_note_undo_cpp_op({
		"kind": "explosion",
		"point": point,
		"radius": pickaxe_hit_radius,
	})
	_end_undo_action()
	return true

func _pickaxe_aim_point() -> Vector3:
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * max_ray_distance
	var voxel_hit := terrain.raycast_terrain(from, to)
	if is_finite(voxel_hit.x):
		return voxel_hit
	var phys_hit := _raycast_crosshair()
	if not phys_hit.is_empty():
		return phys_hit.position
	return Vector3(INF, INF, INF)

## Highest walkable surface under `from` from the voxel terrain or a pristine
## statue collider. Returns INF when nothing solid is in range.
func _ground_height_below(from: Vector3, distance: float) -> float:
	var best := INF
	var to := from - Vector3(0.0, distance, 0.0)
	var voxel_hit := terrain.raycast_terrain(from, to)
	if is_finite(voxel_hit.x):
		best = voxel_hit.y
	var query := PhysicsRayQueryParameters3D.create(from, to)
	var phys_hit := get_world_3d().direct_space_state.intersect_ray(query)
	if not phys_hit.is_empty():
		var phys_y: float = phys_hit.position.y
		if phys_y > best:
			best = phys_y
	return best

## Samples the center and four points around the body so a foot cannot slip
## through a seam between chunks or miss the surface at the edge of a voxel.
func _ground_probe(from: Vector3, distance: float) -> float:
	var best := -INF
	for offset in [Vector3.ZERO, Vector3(0.3, 0.0, 0.0), Vector3(-0.3, 0.0, 0.0), Vector3(0.0, 0.0, 0.3), Vector3(0.0, 0.0, -0.3)]:
		var ground_y := _ground_height_below(from + offset, distance)
		if is_finite(ground_y) and ground_y > best:
			best = ground_y
	return best

func _process_play(delta: float) -> void:
	if _spawn_pending:
		_spawn_wait += delta
		var spawn_ground := _ground_probe(camera.global_position + Vector3(0.0, play_step_height, 0.0), 400.0)
		if is_finite(spawn_ground):
			_player_feet = Vector3(_player_feet.x, spawn_ground, _player_feet.z)
			_last_safe_feet = _player_feet
			_player_vel = Vector3.ZERO
			_player_vel_y = 0.0
			_player_grounded = true
			_hero_state = HeroState.GROUNDED
			_spawn_pending = false
		else:
			_player_vel = Vector3.ZERO
			_player_vel_y = 0.0
			_player_grounded = false
			_update_hero_camera_and_visuals(delta)
			_update_pickaxe_swing(delta)
			if _spawn_wait > 4.0:
				_spawn_pending = false
			return

	# 1. Regenerate Deadlock stamina charges (3 pips)
	if _stamina_charges < play_max_stamina:
		_stamina_regen_progress += delta / maxf(0.1, play_stamina_regen_seconds)
		if _stamina_regen_progress >= 1.0:
			_stamina_regen_progress -= 1.0
			_stamina_charges = mini(play_max_stamina, _stamina_charges + 1)
	else:
		_stamina_regen_progress = 0.0

	# 2. Sample O(1) SDF surface normal, material ID, and fracture damage beneath hero feet
	if terrain.has_method("sample_sdf_normal"):
		_ground_normal = terrain.sample_sdf_normal(_player_feet + Vector3(0.0, 0.2, 0.0))
		_foot_material = maxi(0, terrain.get_material_at(_player_feet - Vector3(0.0, 0.15, 0.0)))
		_foot_damage = terrain.get_damage_at(_player_feet - Vector3(0.0, 0.15, 0.0))

	# 3. Handle active Ledge Mantle trajectory
	if _hero_state == HeroState.MANTLING:
		_step_hero_mantle(delta)
		_update_kelvin_ice_path()
		_update_hero_camera_and_visuals(delta)
		_update_pickaxe_swing(delta)
		return

	# 4. Handle active Lash Ground Slam dive
	if _hero_state == HeroState.GROUND_SLAM:
		_step_hero_ground_slam(delta)
		_update_hero_camera_and_visuals(delta)
		_update_pickaxe_swing(delta)
		return

	# 4b. Handle active Abrams Shoulder Charge + Swept Wall-Pin / Thin-Wall Breach
	if _hero_state == HeroState.SHOULDER_CHARGE:
		_step_hero_shoulder_charge(delta)
		_update_kelvin_ice_path()
		_update_hero_camera_and_visuals(delta)
		_update_pickaxe_swing(delta)
		return

	var forward := Vector3(-sin(_yaw), 0.0, -cos(_yaw))
	var right := Vector3(cos(_yaw), 0.0, -sin(_yaw))
	var input_dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W):
		input_dir += forward
	if Input.is_key_pressed(KEY_S):
		input_dir -= forward
	if Input.is_key_pressed(KEY_A):
		input_dir -= right
	if Input.is_key_pressed(KEY_D):
		input_dir += right
	var wish_dir := input_dir.normalized() if input_dir.length_squared() > 1e-4 else Vector3.ZERO
	var want_slide := Input.is_key_pressed(KEY_CTRL) or Input.is_key_pressed(KEY_C)

	# 5. Step active Dash window or horizontal locomotion + slope slide + surface friction
	if _hero_state == HeroState.DASHING:
		_dash_timer += delta
		if _dash_timer >= play_dash_duration:
			_hero_state = HeroState.GROUNDED if _player_grounded else HeroState.AIRBORNE
		else:
			_player_vel.x = _dash_dir.x * play_dash_speed
			_player_vel.z = _dash_dir.z * play_dash_speed
			_player_vel_y = maxf(_player_vel_y - play_gravity * 0.25 * delta, -3.5)
	else:
		_apply_hero_horizontal_accel(delta, wish_dir, want_slide)

	# 6. Swept horizontal motion + O(1) SDF capsule wall depenetration + Auto-Mantle check
	_move_player_horizontal(Vector3(_player_vel.x, 0.0, _player_vel.z) * delta, wish_dir)
	if _hero_state == HeroState.MANTLING:
		_update_hero_camera_and_visuals(delta)
		_update_pickaxe_swing(delta)
		return

	# 7. Vertical gravity integration, SDF ceiling bump & walkable slope ground snap
	var prev_y := _player_feet.y
	if _hero_state != HeroState.DASHING:
		_player_vel_y = maxf(_player_vel_y - play_gravity * delta, -70.0)
	_player_feet.y += _player_vel_y * delta

	if terrain.has_method("sample_sdf") and _player_vel_y > 0.0:
		var head_pt := _player_feet + Vector3(0.0, play_body_height, 0.0)
		var head_sdf := terrain.sample_sdf(head_pt)
		if head_sdf < play_body_radius:
			var head_n := terrain.sample_sdf_normal(head_pt)
			if head_n.y < -0.25:
				_player_feet.y -= (play_body_radius - head_sdf)
				_player_vel_y = 0.0

	var probe_top := maxf(prev_y, _player_feet.y) + play_step_height
	var ground_y := _ground_probe(
		Vector3(_player_feet.x, probe_top, _player_feet.z),
		probe_top - _player_feet.y + 1.0
	)
	if not is_finite(ground_y) and not _player_grounded:
		var rescue_y := _ground_probe(Vector3(_player_feet.x, _player_feet.y + 2.0, _player_feet.z), 4.0)
		if is_finite(rescue_y) and rescue_y >= _player_feet.y - 0.2:
			ground_y = rescue_y
	if is_finite(ground_y):
		var within_step := ground_y - prev_y <= play_step_height + 0.08
		var crossing := _player_feet.y <= ground_y
		var snap_to_ground := _player_grounded and _player_vel_y <= 0.0 \
			and _player_feet.y - ground_y <= 0.38
		var walkable_slope := true
		if terrain.has_method("sample_sdf_normal"):
			var surf_n := terrain.sample_sdf_normal(Vector3(_player_feet.x, ground_y, _player_feet.z))
			walkable_slope = (surf_n.y >= 0.65)
			if walkable_slope:
				_ground_normal = surf_n
		if within_step and walkable_slope and (crossing or snap_to_ground):
			_player_feet.y = ground_y
			_player_vel_y = 0.0
			_player_grounded = true
			_coyote_timer = 0.14
			if _hero_state == HeroState.AIRBORNE:
				_hero_state = HeroState.GROUNDED
		else:
			_player_grounded = false
	else:
		_player_grounded = false

	if not _player_grounded:
		_coyote_timer = maxf(0.0, _coyote_timer - delta)
		if _hero_state == HeroState.GROUNDED or _hero_state == HeroState.SLIDING:
			_hero_state = HeroState.AIRBORNE

	if _player_grounded:
		_last_safe_feet = _player_feet
	elif _player_feet.y < _last_safe_feet.y - 80.0 or _player_feet.y < -64.0:
		_player_feet = _last_safe_feet
		_player_vel = Vector3.ZERO
		_player_vel_y = 0.0
		_player_grounded = true
		_hero_state = HeroState.GROUNDED

	_velocity = Vector3(_player_vel.x, _player_vel_y, _player_vel.z)
	_sync_and_tick_gas_in_play(delta)
	_update_kelvin_ice_path()
	_update_hero_camera_and_visuals(delta)
	_update_pickaxe_swing(delta)

func _sync_and_tick_gas_in_play(delta: float) -> void:
	if _gas == null:
		return
	if _player_grounded:
		_gas.hero_tags = (_gas.hero_tags & ~VoxelAbilitySystem.Tag.AIRBORNE) | VoxelAbilitySystem.Tag.GROUNDED
	else:
		_gas.hero_tags = (_gas.hero_tags & ~VoxelAbilitySystem.Tag.GROUNDED) | VoxelAbilitySystem.Tag.AIRBORNE

	if _ice_path_active:
		_gas.apply_status_effect(self, VoxelAbilitySystem.StatusEffectSpec.make_ice_path_surge(0.25))
	if _foot_material == 3: # Lava -> applies Burning DoT to hero
		_gas.apply_status_effect(self, VoxelAbilitySystem.StatusEffectSpec.make_burning(24.0, 2.5))
	elif _foot_material == 4 and _gas.hero_effects.has("burning"): # Water -> cleanses Burning!
		_gas.hero_effects.erase("burning")
		_gas._recompute_entity_tags_and_stats(self)

	var aim_fwd := -camera.global_transform.basis.z.normalized()
	var beam_start := _player_feet + Vector3(0.0, play_eye_height - 0.15, 0.0) + aim_fwd * 0.65
	var targets: Array[Node3D] = []
	if _companion_initialized:
		targets.append(ai_companion)
	_gas.tick_ability_system(delta, beam_start, aim_fwd, max_ray_distance, targets)
	if _play_fire_held:
		_fire_primary_ricochet_carbine()
	_render_hyper_beam_visuals()
	_render_bullet_tracers()

func _fire_primary_ricochet_carbine() -> void:
	if _gas == null:
		return
	var is_sliding := (_hero_state == HeroState.SLIDING)
	var aim_fwd := -camera.global_transform.basis.z.normalized()
	var right_vec := camera.global_transform.basis.x.normalized()
	var muzzle := _player_feet + Vector3(0.0, play_eye_height - 0.18, 0.0) + right_vec * 0.35 + aim_fwd * 0.55
	var targets: Array[Node3D] = []
	if _companion_initialized:
		targets.append(ai_companion)
	var res := _gas.fire_ricochet_weapon(muzzle, camera.global_position, aim_fwd, max_ray_distance, is_sliding, targets)
	if not bool(res.get("fired", false)):
		return

	# Check if any bullet or ricochet hit an Option 1 pristine statue to trigger its voxel swap!
	var phys_hit := _raycast_crosshair()
	var hit_collider: Object = phys_hit.get("collider") if not phys_hit.is_empty() else null
	var chipped: PackedVector3Array = res.get("chipped_points", PackedVector3Array())
	if not chipped.is_empty():
		var first_pt := chipped[0]
		_check_and_swap_opt1_easter_island_on_blast(first_pt, 1.5, hit_collider)
		_check_and_swap_atlas_opt1_on_blast(first_pt, 1.5, hit_collider)
		_check_and_swap_ape_opt1_on_blast(first_pt, 1.5, hit_collider)

	var dmg := float(res.get("damage", 0.0))
	var bounces := int(res.get("ricochets", 0))
	var is_crit := bool(res.get("crit", false))
	if dmg > 0.0:
		var tags := "%s%s" % [
			"HEADSHOT CRIT! " if is_crit else "",
			"RICOCHET x%d! " % bounces if bounces > 0 else ""
		]
		print("[Weapon] %sDealt %.0f bullet dmg to Companion (Ammo: %d/%d%s)" % [
			tags, dmg, int(res.get("ammo", 0)), _gas.primary_weapon.mag_size,
			" | SLIDE INFINITE AMMO" if is_sliding else ""
		])

func _render_bullet_tracers() -> void:
	if _tracer_immediate_mesh == null:
		return
	_tracer_immediate_mesh.clear_surfaces()
	if _gas == null or _gas.active_tracers.is_empty():
		return
	_tracer_immediate_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for seg in _gas.active_tracers:
		var alpha := clampf(seg.ttl / 0.16, 0.0, 1.0)
		var col := Color(1.0, 0.88, 0.2, alpha)
		if seg.is_crit:
			col = Color(1.0, 0.25, 0.15, alpha)
		elif seg.bounce_index > 0:
			col = Color(0.2, 0.95, 1.0, alpha)
		_tracer_immediate_mesh.surface_set_color(col)
		_tracer_immediate_mesh.surface_add_vertex(seg.from_pos)
		_tracer_immediate_mesh.surface_set_color(col)
		_tracer_immediate_mesh.surface_add_vertex(seg.to_pos)
	_tracer_immediate_mesh.surface_end()

func _render_hyper_beam_visuals() -> void:
	if _beam_immediate_mesh == null:
		return
	_beam_immediate_mesh.clear_surfaces()
	if _gas == null or _gas.active_beam_ability == null or _mode != Mode.PLAY:
		if _beam_impact_sphere != null:
			_beam_impact_sphere.visible = false
		return
	var a := _gas.beam_current_origin
	var b := _gas.beam_current_end
	var up := Vector3.UP * 0.14
	var side := (b - a).cross(Vector3.UP).normalized() * 0.14
	_beam_immediate_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	_beam_immediate_mesh.surface_add_vertex(a)
	_beam_immediate_mesh.surface_add_vertex(b)
	_beam_immediate_mesh.surface_add_vertex(a + up)
	_beam_immediate_mesh.surface_add_vertex(b + up)
	_beam_immediate_mesh.surface_add_vertex(a - up)
	_beam_immediate_mesh.surface_add_vertex(b - up)
	_beam_immediate_mesh.surface_add_vertex(a + side)
	_beam_immediate_mesh.surface_add_vertex(b + side)
	_beam_immediate_mesh.surface_add_vertex(a - side)
	_beam_immediate_mesh.surface_add_vertex(b - side)
	_beam_immediate_mesh.surface_end()
	if _beam_impact_sphere != null:
		_beam_impact_sphere.visible = true
		_beam_impact_sphere.global_position = b

func _apply_hero_horizontal_accel(delta: float, wish_dir: Vector3, want_slide: bool) -> void:
	var horiz_vel := Vector3(_player_vel.x, 0.0, _player_vel.z)
	var horiz_speed := horiz_vel.length()
	var on_ice := (_foot_material == 6)   # kMaterialIce
	var in_water := (_foot_material == 4) # kMaterialWater
	var on_lava := (_foot_material == 3)  # kMaterialLava

	if _player_grounded:
		var slope_steepness := clampf(1.0 - _ground_normal.y, 0.0, 1.0)
		var can_slide := want_slide and (horiz_speed > 4.8 or slope_steepness > 0.05 or on_ice)
		if can_slide:
			if _hero_state != HeroState.SLIDING and horiz_speed < 11.5:
				var slide_dir := wish_dir if wish_dir.length_squared() > 1e-4 else horiz_vel.normalized()
				if slide_dir.length_squared() > 1e-4:
					horiz_vel = slide_dir * 11.5
			_hero_state = HeroState.SLIDING
		elif _hero_state == HeroState.SLIDING and (not want_slide or horiz_speed < 2.0):
			_hero_state = HeroState.GROUNDED

		if _hero_state == HeroState.SLIDING:
			var downhill := Vector3(_ground_normal.x, 0.0, _ground_normal.z)
			if downhill.length_squared() > 1e-5:
				horiz_vel += downhill.normalized() * (26.0 * (slope_steepness * 2.5 + 0.15) * delta)
			var slide_fric := 2.6 * (0.20 if on_ice else 1.0)
			horiz_vel = horiz_vel.move_toward(Vector3.ZERO, slide_fric * delta)
			var cap := play_slide_max_speed * (1.2 if on_ice else 1.0)
			if horiz_vel.length() > cap:
				horiz_vel = horiz_vel.normalized() * cap
		else:
			var top_speed := play_walk_speed * (0.76 if in_water else (1.18 if on_ice else 1.0))
			var target_vel := wish_dir * top_speed
			var fric := 14.0 * (0.18 if on_ice else 1.0)
			var accel := 42.0 if wish_dir.length_squared() > 1e-4 else fric
			horiz_vel = horiz_vel.move_toward(target_vel, accel * delta)

		if on_lava:
			_player_vel_y = 9.2
			_player_grounded = false
			_hero_state = HeroState.AIRBORNE
	else:
		# Preserve Dash-Jump momentum in air while allowing air-strafe control
		if wish_dir.length_squared() > 1e-4:
			var proj := horiz_vel.dot(wish_dir)
			var max_air := play_walk_speed * 1.15
			if proj < max_air:
				horiz_vel += wish_dir * minf(16.0 * delta, max_air - proj)
		horiz_vel = horiz_vel.move_toward(Vector3.ZERO, 1.1 * delta)

	_player_vel.x = horiz_vel.x
	_player_vel.z = horiz_vel.z

func _move_player_horizontal(motion: Vector3, wish_dir: Vector3 = Vector3.ZERO) -> void:
	if motion.length_squared() >= 0.000001:
		if not _player_path_blocked(motion):
			_player_feet += motion
		else:
			# If blocked by a wall while airborne (or jumping), try 3-probe SDF Ledge Mantle!
			if not _player_grounded and wish_dir.length_squared() > 1e-4:
				if _try_start_ledge_mantle(wish_dir):
					return
			var slide_x := Vector3(motion.x, 0.0, 0.0)
			var slide_z := Vector3(0.0, 0.0, motion.z)
			if absf(slide_x.x) > 0.0001 and not _player_path_blocked(slide_x):
				_player_feet += slide_x
			if absf(slide_z.z) > 0.0001 and not _player_path_blocked(slide_z):
				_player_feet += slide_z

	# Continuous O(1) SDF sphere depenetration so hero capsule glides smoothly along curved cave walls
	if terrain.has_method("sample_sdf"):
		for h in [play_step_height + 0.15, 1.15, 1.55]:
			var pt := _player_feet + Vector3(0.0, h, 0.0)
			var d := terrain.sample_sdf(pt)
			if d < play_body_radius:
				var n := terrain.sample_sdf_normal(pt)
				var push := Vector3(n.x, 0.0, n.z)
				if push.length_squared() > 1e-4:
					push = push.normalized()
					_player_feet += push * (play_body_radius - d)
					var vn := _player_vel.x * push.x + _player_vel.z * push.z
					if vn < 0.0:
						_player_vel.x -= push.x * vn
						_player_vel.z -= push.z * vn

## Voxel terrain walls are checked via O(1) SDF/DDA raycasts; pristine statue colliders
## come from PhysicsServer3D. Rays sit just above step_height so 0.5 m steps don't block.
func _player_path_blocked(motion: Vector3) -> bool:
	var dir := motion.normalized()
	var distance := motion.length() + play_body_radius
	for height in [play_step_height + 0.05, 1.2]:
		var from: Vector3 = _player_feet + Vector3(0.0, height, 0.0)
		var to: Vector3 = from + dir * distance
		var voxel_hit := terrain.raycast_terrain(from, to)
		if is_finite(voxel_hit.x) and from.distance_to(voxel_hit) < distance:
			return true
		var query := PhysicsRayQueryParameters3D.create(from, to)
		if not get_world_3d().direct_space_state.intersect_ray(query).is_empty():
			return true
	return false

## 3-Probe SDF + Raycast Ledge Mantling:
##   1. Forward chest raycast locates a steep wall face.
##   2. Downward probe past the wall lip locates a walkable ledge top within [min_h, max_h].
##   3. O(1) sample_sdf checks at waist & head above the ledge verify capsule clearance.
func _try_start_ledge_mantle(move_dir: Vector3) -> bool:
	var fwd := Vector3(move_dir.x, 0.0, move_dir.z).normalized()
	if fwd.length_squared() < 1e-4:
		return false

	var chest_from := _player_feet + Vector3(0.0, 1.05, 0.0)
	var chest_to := chest_from + fwd * (play_body_radius + play_mantle_reach)
	var wall_hit := terrain.raycast_terrain(chest_from, chest_to)
	if not is_finite(wall_hit.x):
		return false

	if terrain.has_method("sample_sdf_normal"):
		var wall_n := terrain.sample_sdf_normal(wall_hit)
		if absf(wall_n.y) > 0.62:
			return false

	var lip_xz := wall_hit + fwd * (play_body_radius + 0.25)
	var top_from := Vector3(lip_xz.x, _player_feet.y + play_mantle_max_height + 0.35, lip_xz.z)
	var top_to := Vector3(lip_xz.x, _player_feet.y + play_mantle_min_height, lip_xz.z)
	var ledge_hit := terrain.raycast_terrain(top_from, top_to)
	if not is_finite(ledge_hit.y):
		return false

	var climb_h := ledge_hit.y - _player_feet.y
	if climb_h < play_mantle_min_height or climb_h > play_mantle_max_height:
		return false

	if terrain.has_method("sample_sdf"):
		var ledge_n := terrain.sample_sdf_normal(ledge_hit)
		if ledge_n.y < 0.65:
			return false
		var waist_sdf := terrain.sample_sdf(ledge_hit + Vector3(0.0, 0.90, 0.0))
		var head_sdf := terrain.sample_sdf(ledge_hit + Vector3(0.0, 1.65, 0.0))
		if waist_sdf < play_body_radius * 0.75 or head_sdf < play_body_radius * 0.75:
			return false

	_hero_state = HeroState.MANTLING
	_mantle_timer = 0.0
	_mantle_start_pos = _player_feet
	_mantle_target_pos = ledge_hit
	_player_vel = Vector3.ZERO
	_player_vel_y = 0.0
	_player_grounded = false
	print("[HeroController] Ledge Mantle started -> climbing +%.2f m onto crater/cliff rim!" % climb_h)
	return true

func _step_hero_mantle(delta: float) -> void:
	_mantle_timer += delta
	var t := clampf(_mantle_timer / maxf(0.08, play_mantle_duration), 0.0, 1.0)
	if t < 0.55:
		var u := smoothstep(0.0, 1.0, t / 0.55)
		_player_feet.x = lerpf(_mantle_start_pos.x, _mantle_target_pos.x, u * 0.25)
		_player_feet.z = lerpf(_mantle_start_pos.z, _mantle_target_pos.z, u * 0.25)
		_player_feet.y = lerpf(_mantle_start_pos.y, _mantle_target_pos.y + 0.12, u)
	else:
		var u := smoothstep(0.0, 1.0, (t - 0.55) / 0.45)
		_player_feet.x = lerpf(_mantle_start_pos.x, _mantle_target_pos.x, 0.25 + 0.75 * u)
		_player_feet.z = lerpf(_mantle_start_pos.z, _mantle_target_pos.z, 0.25 + 0.75 * u)
		_player_feet.y = lerpf(_mantle_target_pos.y + 0.12, _mantle_target_pos.y, u)

	if t >= 1.0:
		_player_feet = _mantle_target_pos
		var exit_dir := _mantle_target_pos - _mantle_start_pos
		exit_dir.y = 0.0
		if exit_dir.length_squared() > 1e-4:
			var boost := exit_dir.normalized() * 4.8
			_player_vel.x = boost.x
			_player_vel.z = boost.z
		_player_vel_y = 0.0
		_player_grounded = true
		_hero_state = HeroState.GROUNDED

func _try_dash() -> void:
	if _mode != Mode.PLAY or _hero_state == HeroState.DASHING or _hero_state == HeroState.MANTLING:
		return
	if _stamina_charges <= 0:
		return
	_stamina_charges -= 1
	var forward := Vector3(-sin(_yaw), 0.0, -cos(_yaw))
	var right := Vector3(cos(_yaw), 0.0, -sin(_yaw))
	var move_in := Vector3.ZERO
	if Input.is_key_pressed(KEY_W): move_in += forward
	if Input.is_key_pressed(KEY_S): move_in -= forward
	if Input.is_key_pressed(KEY_A): move_in -= right
	if Input.is_key_pressed(KEY_D): move_in += right
	_dash_dir = move_in.normalized() if move_in.length_squared() > 1e-4 else forward
	_dash_timer = 0.0
	_hero_state = HeroState.DASHING
	_player_vel.x = _dash_dir.x * play_dash_speed
	_player_vel.z = _dash_dir.z * play_dash_speed
	_player_vel_y = maxf(_player_vel_y, 0.0)

func _try_jump() -> void:
	if _mode != Mode.PLAY:
		return
	var forward := Vector3(-sin(_yaw), 0.0, -cos(_yaw))
	var right := Vector3(cos(_yaw), 0.0, -sin(_yaw))
	var move_in := Vector3.ZERO
	if Input.is_key_pressed(KEY_W): move_in += forward
	if Input.is_key_pressed(KEY_S): move_in -= forward
	if Input.is_key_pressed(KEY_A): move_in -= right
	if Input.is_key_pressed(KEY_D): move_in += right
	var climb_dir := move_in.normalized() if move_in.length_squared() > 1e-4 else forward

	# 1. Deadlock Dash-Jump combo: pressing Space inside the active Dash window launches a long arc!
	if _hero_state == HeroState.DASHING and _dash_timer >= 0.04 and _dash_timer <= 0.24:
		_hero_state = HeroState.AIRBORNE
		_player_grounded = false
		_coyote_timer = 0.0
		_player_vel.x = _dash_dir.x * play_dash_jump_horiz_boost
		_player_vel.z = _dash_dir.z * play_dash_jump_horiz_boost
		_player_vel_y = play_dash_jump_vert_speed
		print("[HeroController] Dash-Jump triggered! Horiz speed=%.1f m/s" % play_dash_jump_horiz_boost)
		return

	# 2. Try Ledge Mantle if close to a cliff/crater wall
	if _try_start_ledge_mantle(climb_dir):
		return

	# 3. Standard or Coyote-Time Jump (with slide-jump speed preservation)
	if _player_grounded or _coyote_timer > 0.0:
		if _hero_state == HeroState.SLIDING:
			_player_vel.x *= 1.12
			_player_vel.z *= 1.12
		_player_vel_y = play_jump_speed
		_player_grounded = false
		_coyote_timer = 0.0
		_hero_state = HeroState.AIRBORNE

func _try_shoulder_charge() -> void:
	if _mode != Mode.PLAY or _hero_state == HeroState.MANTLING or _hero_state == HeroState.SHOULDER_CHARGE:
		return
	if _gas != null:
		var check := _gas.can_activate_ability("abrams_shoulder_charge", _gas.hero_tags)
		if not bool(check.get("ok", false)):
			print("[GAS] Shoulder Charge blocked: %s" % String(check.get("reason", "")))
			return
		_gas.commit_ability_cooldown("abrams_shoulder_charge")
	_hero_state = HeroState.SHOULDER_CHARGE
	_charge_timer = 0.0
	_charge_pinned_companion = false
	_charge_dir = Vector3(-sin(_yaw), 0.0, -cos(_yaw)).normalized()
	_player_vel = _charge_dir * play_charge_speed
	print("[HeroController] Abrams Shoulder Charge started! (Steer with mouse | Captures companion & pins/breaches walls)")

func _step_hero_shoulder_charge(delta: float) -> void:
	_charge_timer += delta
	if _charge_timer >= play_charge_duration:
		_charge_pinned_companion = false
		_hero_state = HeroState.GROUNDED if _player_grounded else HeroState.AIRBORNE
		return

	# Smoothly steer charge direction toward current camera yaw (clamped turn rate)
	var desired_dir := Vector3(-sin(_yaw), 0.0, -cos(_yaw)).normalized()
	_charge_dir = _charge_dir.lerp(desired_dir, clampf(2.2 * delta, 0.0, 1.0)).normalized()

	var step_dist := play_charge_speed * delta
	var horiz_step := _charge_dir * step_dist
	var next_feet := _player_feet + horiz_step

	# Phase A: Sweep Capture Corridor for AI Companion
	if not _charge_pinned_companion and _companion_initialized:
		var to_comp := ai_companion.global_position - _player_feet
		if absf(to_comp.y) <= 2.4:
			var horiz_comp := Vector3(to_comp.x, 0.0, to_comp.z)
			var fwd_proj := horiz_comp.dot(_charge_dir)
			if fwd_proj >= -0.35 and fwd_proj <= step_dist + play_charge_capture_radius:
				var lat_dist := (horiz_comp - _charge_dir * fwd_proj).length()
				if lat_dist <= play_charge_capture_radius:
					_charge_pinned_companion = true
					print("[WallPin] Captured AI Companion on Shoulder Charge bumper!")

	# Phase B: Swept Sphere Wall-Pin Detection (voxel SDF sweep_sphere + pristine statue PhysicsServer3D ray)
	var lead_extra := 0.85 if _charge_pinned_companion else 0.25
	var sweep_r := play_body_radius * 0.92
	var hit_pos := Vector3(INF, INF, INF)
	var hit_collider: Object = null

	for h in [0.85, 1.35]:
		var from_pt := _player_feet + Vector3(0.0, h, 0.0)
		var to_pt := next_feet + Vector3(0.0, h, 0.0) + _charge_dir * (play_body_radius + lead_extra)
		var vox_contact: Vector3 = terrain.sweep_sphere(from_pt, to_pt, sweep_r) if terrain.has_method("sweep_sphere") else terrain.raycast_terrain(from_pt, to_pt)
		if is_finite(vox_contact.x):
			hit_pos = vox_contact
			break
		var query := PhysicsRayQueryParameters3D.create(from_pt, to_pt)
		var phys_hit := get_world_3d().direct_space_state.intersect_ray(query)
		if not phys_hit.is_empty():
			hit_pos = phys_hit.position
			hit_collider = phys_hit.get("collider")
			break

	if is_finite(hit_pos.x):
		# If we slammed into an Option 1 pristine statue, swap it to destructible voxels first!
		if hit_collider != null:
			_check_and_swap_opt1_easter_island_on_blast(hit_pos, play_charge_wall_pin_blast_radius, hit_collider)
			_check_and_swap_atlas_opt1_on_blast(hit_pos, play_charge_wall_pin_blast_radius, hit_collider)
			_check_and_swap_ape_opt1_on_blast(hit_pos, play_charge_wall_pin_blast_radius, hit_collider)

		var wall_normal := terrain.sample_sdf_normal(hit_pos) if terrain.has_method("sample_sdf_normal") else -_charge_dir
		if absf(wall_normal.y) < 0.62:
			var wall_n_xz := Vector3(wall_normal.x, 0.0, wall_normal.z).normalized()
			if wall_n_xz.length_squared() < 1e-4:
				wall_n_xz = -_charge_dir
			var impact_cos := -_charge_dir.dot(wall_n_xz)

			# Outcome 1: Glancing Wall Hit (< 53 deg into wall) -> slide smoothly along wall tangent!
			if impact_cos < play_charge_pin_min_cos:
				var slide_dir := (_charge_dir - wall_n_xz * _charge_dir.dot(wall_n_xz)).normalized()
				if slide_dir.length_squared() > 1e-4:
					_charge_dir = slide_dir
					next_feet = _player_feet + _charge_dir * (step_dist * maxf(0.45, 1.0 - impact_cos))
			else:
				# Direct Wall Impact! Inspect material, damage ratio, and wall thickness
				var wall_mat := terrain.get_material_at(hit_pos) if terrain.has_method("get_material_at") else MATERIAL_SOIL
				var wall_dmg := terrain.get_damage_at(hit_pos) if terrain.has_method("get_damage_at") else 0.0
				var wall_thick := terrain.measure_wall_thickness(
					hit_pos, _charge_dir, play_charge_breach_max_thickness + 1.0
				) if terrain.has_method("measure_wall_thickness") else 99.0

				var mat_name: String = MATERIAL_ID_NAMES[wall_mat] if wall_mat >= 0 and wall_mat < MATERIAL_ID_NAMES.size() else "Rock"
				var can_breach_mat := (
					wall_mat == MATERIAL_SOIL or
					wall_mat == 6 or # Ice
					(wall_mat == MATERIAL_ROCK and wall_dmg >= 0.60)
				)

				# Outcome 2: Thin-Wall Breach (smash straight through thin Soil/Ice/cracked walls!)
				if not _charge_pinned_companion and can_breach_mat and wall_thick > 0.05 and wall_thick <= play_charge_breach_max_thickness:
					var tunnel_end := hit_pos + _charge_dir * (wall_thick + 0.9)
					terrain.carve_capsule(hit_pos - _charge_dir * 0.4, tunnel_end, 2.3)
					print("[WallPin] BREACHED thin %s wall (thickness=%.2f m) at %s — keeping charge momentum!" % [
						mat_name, wall_thick, str(hit_pos)
					])
					_player_feet = next_feet
					return

				# Outcome 3: Solid Wall-Pin & Stun!
				if _companion_initialized and _goal_mesh_inst.visible:
					_rebake_in_progress = true
				terrain.trigger_explosion(hit_pos, play_charge_wall_pin_blast_radius)
				if _charge_pinned_companion:
					ai_companion.global_position = Vector3(
						hit_pos.x + wall_n_xz.x * 0.65,
						_player_feet.y + 0.15,
						hit_pos.z + wall_n_xz.z * 0.65
					)
					_companion_stun_timer = play_charge_pin_stun_seconds
					if _gas != null:
						var base_dmg := _gas.get_ability("abrams_shoulder_charge").compute_scaled_damage(_gas.hero_attributes.get_spirit_power())
						# Bonus 50% Shatter Damage if pinned against already-cracked rock!
						var shatter_mult := 1.5 if wall_dmg >= 0.30 else 1.0
						var dealt := _gas.deal_ability_damage(ai_companion, base_dmg * shatter_mult, false, "abrams_shoulder_charge")
						_gas.apply_status_effect(ai_companion, VoxelAbilitySystem.StatusEffectSpec.make_wall_pin_stun(play_charge_pin_stun_seconds))
						print("[WallPin+GAS] PINNED Companion against %s wall -> dealt %.0f dmg (shatter=%.1fx) & applied WALL-PIN STUN (%.1fs)!" % [
							mat_name, dealt, shatter_mult, play_charge_pin_stun_seconds
						])
				else:
					print("[WallPin] Heavy Shoulder Charge slammed %s wall at %s (cos=%.2f, thickness=%.2f m)!" % [
						mat_name, str(hit_pos), impact_cos, wall_thick
					])

				_charge_pinned_companion = false
				_player_vel = wall_n_xz * 3.8
				_player_vel_y = 3.2
				_player_grounded = false
				_hero_state = HeroState.AIRBORNE
				return

	# Advance hero and drag pinned companion on front bumper
	_player_feet = next_feet
	var ground_y := _ground_probe(_player_feet + Vector3(0.0, play_step_height + 0.3, 0.0), 2.2)
	if is_finite(ground_y):
		_player_feet.y = ground_y
		_player_vel_y = 0.0
		_player_grounded = true
	else:
		_player_vel_y = maxf(_player_vel_y - play_gravity * delta, -40.0)
		_player_feet.y += _player_vel_y * delta
		_player_grounded = false

	if _charge_pinned_companion:
		ai_companion.global_position = _player_feet + _charge_dir * 1.15 + Vector3(0.0, 0.15, 0.0)
		_companion_vertical_vel = 0.0

func _try_ground_slam() -> void:
	if _mode != Mode.PLAY or _player_grounded or _hero_state == HeroState.MANTLING:
		return
	if _gas != null:
		var check := _gas.can_activate_ability("lash_ground_strike", _gas.hero_tags | VoxelAbilitySystem.Tag.AIRBORNE)
		if not bool(check.get("ok", false)):
			print("[GAS] Ground Strike blocked: %s" % String(check.get("reason", "")))
			return
		_gas.commit_ability_cooldown("lash_ground_strike")
	_hero_state = HeroState.GROUND_SLAM
	var aim_fwd := Vector3(-sin(_yaw), 0.0, -cos(_yaw)) * 8.0
	_player_vel.x = aim_fwd.x
	_player_vel.z = aim_fwd.z
	_player_vel_y = -28.0
	print("[HeroController] Lash Ground Strike dive initiated!")

func _step_hero_ground_slam(delta: float) -> void:
	var step_vec := Vector3(_player_vel.x, _player_vel_y, _player_vel.z) * delta
	var next_feet := _player_feet + step_vec
	var hit := terrain.raycast_terrain(_player_feet + Vector3(0.0, 0.8, 0.0), next_feet - Vector3(0.0, 0.4, 0.0))
	if is_finite(hit.y) and next_feet.y <= hit.y + 0.25:
		_player_feet = hit
		_player_vel = Vector3.ZERO
		_player_vel_y = 0.0
		_player_grounded = true
		_hero_state = HeroState.GROUNDED
		if _gas != null:
			var targets: Array[Node3D] = []
			if _companion_initialized:
				targets.append(ai_companion)
			var res := _gas.resolve_ground_strike_combo(hit, targets)
			var dom_mat := int(res.get("material", MATERIAL_SOIL))
			var mat_name: String = MATERIAL_ID_NAMES[dom_mat] if dom_mat >= 0 and dom_mat < MATERIAL_ID_NAMES.size() else "Terrain"
			print("[GAS] Ground Strike impacted %s -> %s (radius=%.1f m, dmg=%.0f)!" % [
				mat_name, String(res.get("combo", "Crater")), float(res.get("radius", play_ground_slam_radius)), float(res.get("damage", 125.0))
			])
		else:
			terrain.trigger_explosion(hit, play_ground_slam_radius)
		return
	_player_feet = next_feet

func _update_kelvin_ice_path() -> void:
	if not _ice_path_active or not terrain.has_method("trigger_elemental_capsule"):
		_last_ice_path_anchor = Vector3(INF, INF, INF)
		return
	var pitch_offset := clampf(sin(_pitch) * 0.85, -0.55, 0.65)
	var cur_anchor := _player_feet + Vector3(0.0, pitch_offset - 0.35, 0.0)
	if not is_finite(_last_ice_path_anchor.x):
		_last_ice_path_anchor = cur_anchor
		terrain.trigger_elemental_sphere(cur_anchor, play_ice_path_radius, ELEMENTAL_FROST)
		return
	if cur_anchor.distance_to(_last_ice_path_anchor) >= 1.1:
		terrain.trigger_elemental_capsule(_last_ice_path_anchor, cur_anchor, play_ice_path_radius, ELEMENTAL_FROST)
		_last_ice_path_anchor = cur_anchor

func _update_hero_camera_and_visuals(delta: float) -> void:
	var target_ads := 1.0 if (_mode == Mode.PLAY and _play_ads_held) else 0.0
	_ads_blend = move_toward(_ads_blend, target_ads, delta * 9.0) if delta > 0.0 else target_ads
	if _mode == Mode.PLAY:
		camera.fov = lerpf(play_fov, 55.0, _ads_blend)

	var cam_rot := Basis.from_euler(Vector3(_pitch, _yaw, 0.0))
	var crouch_offset := -0.45 if _hero_state == HeroState.SLIDING else 0.0
	var pivot := _player_feet + Vector3(0.0, play_eye_height + crouch_offset, 0.0)

	if _hero_visual_root != null:
		_hero_visual_root.visible = (_mode == Mode.PLAY and play_third_person)
		_hero_visual_root.global_position = _player_feet
		var lean_pitch := 0.0
		if _hero_state == HeroState.DASHING:
			lean_pitch = -0.28
		elif _hero_state == HeroState.SLIDING:
			lean_pitch = 0.42
		elif _hero_state == HeroState.MANTLING:
			lean_pitch = -0.35
		elif _hero_state == HeroState.SHOULDER_CHARGE:
			lean_pitch = -0.38
		_hero_visual_root.rotation = Vector3(lean_pitch, _yaw, 0.0)

	if not play_third_person:
		_play_pcam.global_position = pivot
		_play_pcam.rotation = Vector3(_pitch, _yaw, 0.0)
		return

	var eff_arm := lerpf(play_cam_arm_length, 2.55, _ads_blend)
	var eff_shoulder_x := lerpf(play_cam_shoulder_offset.x, 0.52, _ads_blend)
	var desired_cam := pivot + cam_rot * Vector3(eff_shoulder_x, play_cam_shoulder_offset.y, eff_arm)
	var final_cam := desired_cam
	var voxel_hit := terrain.raycast_terrain(pivot, desired_cam)
	if is_finite(voxel_hit.x):
		var dist := maxf(0.5, pivot.distance_to(voxel_hit) - 0.35)
		final_cam = pivot + (desired_cam - pivot).normalized() * dist
	var query := PhysicsRayQueryParameters3D.create(pivot, final_cam)
	var phys_hit := get_world_3d().direct_space_state.intersect_ray(query)
	if not phys_hit.is_empty():
		var dist := maxf(0.5, pivot.distance_to(phys_hit.position) - 0.35)
		final_cam = pivot + (desired_cam - pivot).normalized() * dist

	if terrain.has_method("sample_sdf"):
		var cam_sdf := terrain.sample_sdf(final_cam)
		if cam_sdf < 0.32:
			var cam_n := terrain.sample_sdf_normal(final_cam)
			final_cam += cam_n * (0.32 - cam_sdf)

	_play_pcam.global_position = final_cam
	_play_pcam.rotation = Vector3(_pitch, _yaw, 0.0)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch = clamp(_pitch - event.relative.y * mouse_sensitivity, -1.45, 1.45)
		_mouse_sway_offset.x = clampf(_mouse_sway_offset.x - event.relative.x * 0.00035, -0.04, 0.04)
		_mouse_sway_offset.y = clampf(_mouse_sway_offset.y + event.relative.y * 0.00035, -0.04, 0.04)
		if _mode == Mode.FLY:
			camera.rotation = Vector3(_pitch, _yaw, 0.0)

	elif event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_M or event.keycode == KEY_F8:
			_toggle_benchmark_mode()
			return
		if _mode == Mode.BENCHMARK and event.keycode not in [KEY_ESCAPE, KEY_H, KEY_U, KEY_F]:
			return
		if _mode == Mode.PLAY and _gas != null:
			if event.keycode == KEY_1:
				var dome_res := _gas.cast_or_dismiss_frozen_shelter(_player_feet, _gas.hero_tags)
				if dome_res.get("action") == "spawned":
					print("[GAS] Spawned Kelvin Frozen Shelter (Hollow Ice Dome #%d, r=%.1fm, TTL=%.1fs - press 1 again to shatter early)!" % [
						int(dome_res.get("structure_id", 0)), float(dome_res.get("radius", 8.5)), float(dome_res.get("duration", 6.5))
					])
				elif dome_res.get("action") == "dismissed":
					print("[GAS] Dismissed Kelvin Frozen Shelter (#%d) early!" % int(dome_res.get("structure_id", 0)))
				return
			if event.keycode == KEY_2:
				var fwd := Vector3(-sin(_yaw), 0.0, -cos(_yaw)).normalized()
				var aim_pt := _pickaxe_aim_point()
				if not is_finite(aim_pt.x) or aim_pt.distance_to(_player_feet) > 28.0:
					aim_pt = _player_feet + fwd * 10.0
					var g_y := _ground_probe(aim_pt + Vector3(0.0, 6.0, 0.0), 24.0)
					if is_finite(g_y):
						aim_pt.y = g_y
				var wall_res := _gas.cast_or_dismiss_spectral_wall(aim_pt, fwd, _gas.hero_tags)
				if wall_res.get("action") == "spawned":
					print("[GAS] Raised McGinnis Spectral Wall (#%d, width=%.1fm, 3-Hit Rock, TTL=%.1fs - press 2 to drop or G to Wall-Pin into it)!" % [
						int(wall_res.get("structure_id", 0)), float(wall_res.get("width", 14.0)), float(wall_res.get("duration", 7.5))
					])
				elif wall_res.get("action") == "dismissed":
					print("[GAS] Dropped McGinnis Spectral Wall (#%d) early!" % int(wall_res.get("structure_id", 0)))
				return
		if _mode == Mode.PLAY and event.keycode in [KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_B]:
			return
		match event.keycode:
			KEY_ESCAPE:
				if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
					if _tool_pressed:
						_tool_pressed = false
						_end_undo_action()
					Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
				else:
					Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			KEY_F:
				_toggle_mode()
			KEY_V:
				if _mode == Mode.PLAY:
					play_third_person = not play_third_person
					_update_hero_camera_and_visuals(0.0)
					print("[HeroController] Camera view: %s" % ("3rd-Person Over-the-Shoulder" if play_third_person else "1st-Person"))
			KEY_SHIFT:
				if _mode == Mode.PLAY and not event.echo:
					_try_dash()
			KEY_E:
				if _mode == Mode.PLAY and not event.echo:
					_ice_path_active = not _ice_path_active
					if _gas != null:
						var a_ice := _gas.get_ability("kelvin_ice_path")
						if a_ice != null:
							a_ice.is_toggled_on = _ice_path_active
					if not _ice_path_active:
						_last_ice_path_anchor = Vector3(INF, INF, INF)
					print("[GAS] Kelvin Ice Path skyway: %s" % ("ACTIVE (+18 Spirit, +24% Speed)" if _ice_path_active else "OFF"))
			KEY_Z:
				if _mode == Mode.PLAY and not event.echo and _gas != null \
						and not (event.ctrl_pressed or event.meta_pressed):
					if _gas.start_hyper_beam(_gas.hero_tags):
						print("[GAS] Bebop Hyper Beam CHANNELING (2.6s molten laser trench, Unstoppable)!")
				elif event.shift_pressed and (event.ctrl_pressed or event.meta_pressed):
					_perform_redo()
				else:
					_perform_undo()
			KEY_X:
				if _mode == Mode.PLAY and not event.echo and _gas != null:
					var aim_fwd := -camera.global_transform.basis.z.normalized()
					var orb_origin := _player_feet + Vector3(0.0, play_eye_height - 0.1, 0.0)
					if _gas.spawn_bouncing_cryo_orb(orb_origin, aim_fwd, _gas.hero_tags, self):
						print("[GAS] Fired Cryo-Bounce Orb (ricochets off SDF normals, Chills/Freezes companion in Voxel Ice)!")
			KEY_G:
				if _mode == Mode.PLAY and not event.echo:
					_try_shoulder_charge()
			KEY_Q:
				if _mode == Mode.PLAY and not event.echo:
					_try_ground_slam()
			KEY_SPACE:
				_try_jump()
			KEY_1:
				_set_tool(Tool.ADD_TERRAIN)
			KEY_2:
				_set_tool(Tool.PAINT)
			KEY_3:
				_set_tool(Tool.PAINT_DELETE)
			KEY_4:
				_set_tool(Tool.BOULDER, event.shift_pressed)
			KEY_5:
				_set_tool(Tool.SMALL_HIT)
			KEY_6:
				_set_tool(Tool.EXPLODE)
			KEY_7:
				_set_tool(Tool.LARGE_EXPLODE)
			KEY_8:
				_set_tool(Tool.WATER_QUENCH)
			KEY_9:
				_set_tool(Tool.FROST_ICE)
			KEY_0:
				_set_tool(Tool.LAVA_MAGMA)
			KEY_B:
				_stamp_asset_at_crosshair(0.0 if Input.is_key_pressed(KEY_SHIFT) else stamp_recreate_delay_seconds)
			KEY_H:
				_controls_visible = not _controls_visible
				controls_label.visible = _controls_visible and not _ui_hidden
			KEY_U:
				_toggle_ui_hidden()
			KEY_Y:
				_perform_redo()
			KEY_M:
				if _mode == Mode.PLAY and not event.echo:
					_player_vel += Vector3(-sin(_yaw), 0.0, -cos(_yaw)) * 4.5
					_start_pickaxe_swing()
			KEY_R:
				if _mode == Mode.PLAY and not event.shift_pressed and _gas != null:
					if _gas.start_weapon_reload():
						print("[Weapon] Reloading Pulse Carbine (%.2fs)... (Press Shift+R to reset statues)" % _gas.primary_weapon.reload_duration)
				elif event.shift_pressed or event.ctrl_pressed or event.meta_pressed:
					_perform_undo_all_and_reset()
				else:
					_reset_statues()
			KEY_T:
				_toggle_add_material()
			KEY_P:
				_place_companion_at_crosshair()

	elif event is InputEventMouseButton:
		# While the cursor is released, a click recaptures instead of firing a
		# tool, so Esc / alt-tab can never carve terrain by accident.
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			if _tool_pressed:
				_tool_pressed = false
				_end_undo_action()
			_play_fire_held = false
			_play_ads_held = false
			if event.pressed:
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			return
		if _mode == Mode.BENCHMARK:
			return
		if event.button_index == MOUSE_BUTTON_LEFT:
			if _mode == Mode.PLAY:
				_tool_pressed = false
				_play_fire_held = event.pressed
				if event.pressed:
					_fire_primary_ricochet_carbine()
				return
			_tool_pressed = event.pressed
			if event.pressed:
				if _is_drag_tool():
					_begin_undo_action(TOOL_NAMES[_active_tool] + " stroke")
					_last_brush_point = Vector3(INF, INF, INF)
					_brush_cooldown = 0.0
					_process_brush(0.0)
				else:
					_begin_undo_action(TOOL_NAMES[_active_tool])
					_apply_click_tool()
					_end_undo_action()
			else:
				if not _active_undo_action.is_empty():
					_end_undo_action()
		elif event.button_index == MOUSE_BUTTON_MIDDLE and event.pressed and _mode == Mode.PLAY:
			_player_vel += Vector3(-sin(_yaw), 0.0, -cos(_yaw)) * 4.5
			_start_pickaxe_swing()
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			if _mode == Mode.PLAY and not Input.is_key_pressed(KEY_SHIFT):
				_play_ads_held = event.pressed
				return
			if event.pressed:
				if Input.is_key_pressed(KEY_SHIFT):
					_place_companion_at_crosshair()
				else:
					_query_companion_cave_path_to_crosshair()

func _process(delta: float) -> void:
	if _bench_banner_timer > 0.0:
		_bench_banner_timer = maxf(0.0, _bench_banner_timer - delta)

	if _mode == Mode.BENCHMARK:
		_process_benchmark(delta)
	elif _mode == Mode.PLAY:
		_process_play(delta)
	else:
		_process_fly(delta)

	if _tool_pressed and _mode == Mode.FLY and _is_drag_tool():
		_process_brush(delta)

	_update_rolling_boulders(delta)
	_update_stream_focus(false)
	_apply_companion_gravity_and_ground_snap(delta)
	_poll_pending_path_query()
	_step_companion_along_path(delta)
	_update_pending_pristine_hides(delta)
	_update_hud(delta)

func _process_fly(delta: float) -> void:
	var input_dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W):
		input_dir -= camera.global_transform.basis.z
	if Input.is_key_pressed(KEY_S):
		input_dir += camera.global_transform.basis.z
	if Input.is_key_pressed(KEY_A):
		input_dir -= camera.global_transform.basis.x
	if Input.is_key_pressed(KEY_D):
		input_dir += camera.global_transform.basis.x
	if Input.is_key_pressed(KEY_E) or Input.is_key_pressed(KEY_SPACE):
		input_dir += Vector3.UP
	if Input.is_key_pressed(KEY_Q) or Input.is_key_pressed(KEY_CTRL):
		input_dir -= Vector3.UP

	var speed := move_speed * (fast_multiplier if Input.is_key_pressed(KEY_SHIFT) else 1.0)
	_velocity = input_dir.normalized() * speed if input_dir.length_squared() > 0.0 else Vector3.ZERO
	camera.global_position += _velocity * delta
	_fly_pcam.global_transform = camera.global_transform

func _update_stream_focus(force: bool) -> void:
	var cur_pos := _player_feet if _mode == Mode.PLAY else camera.global_position
	# Chunks stay 32^3 samples, so their world size scales with the node's voxel size.
	var chunk_size := 32.0 * terrain.voxel_scale
	var cur_chunk := Vector3i(
		int(floor(cur_pos.x / chunk_size)),
		int(floor(cur_pos.y / chunk_size)),
		int(floor(cur_pos.z / chunk_size))
	)
	# Only call into C++ set_stream_focus when the camera crosses into a new chunk!
	if force or cur_chunk != _last_focus_chunk:
		_last_focus_chunk = cur_chunk
		terrain.set_stream_focus(cur_pos, _velocity)

func _update_hud(delta: float) -> void:
	var fps := float(Engine.get_frames_per_second())
	_fps_smoothed = fps if _fps_smoothed <= 0.0 else lerpf(_fps_smoothed, fps, 0.1)

	# Commit rate over a rolling half-second window (chunks remeshed/committed per second).
	_stats_accum += delta
	if _stats_accum >= 0.5:
		_commits_per_second = float(_commits_in_window) / _stats_accum
		_commits_in_window = 0
		_stats_accum = 0.0

	var streaming_state := "idle"
	if not terrain.core_idle():
		streaming_state = "core busy"
	elif not terrain.fully_committed():
		streaming_state = "committing"

	var material_state := ""
	if _active_tool == Tool.ADD_TERRAIN or _active_tool == Tool.PAINT:
		material_state = " | Material: %s" % MATERIAL_NAMES[_add_material - 1]

	if _mode == Mode.BENCHMARK:
		var preset: Dictionary = BENCHMARK_PRESETS[_bench_preset_idx]
		var phase_name: String = BENCHMARK_PHASE_NAMES[_bench_phase]
		stats_label.text = "=== BENCHMARK MODE ===\nPreset %d/%d: %s\nPhase: %s (%.1fs)\nSettings: view=%d prefetch=%d budget=%.1fms mem=%dMB\nFPS: %.0f (%.2f ms) | Committed: %d (%.0f/s) | %s\nGPU: %s\nCPU: %s (%d threads)" % [
			_bench_preset_idx + 1,
			BENCHMARK_PRESETS.size(),
			String(preset["name"]),
			phase_name,
			_bench_phase_elapsed,
			terrain.view_distance_chunks,
			terrain.prefetch_distance_chunks,
			terrain.upload_budget_ms,
			terrain.resident_memory_budget_mb,
			_fps_smoothed,
			1000.0 / max(_fps_smoothed, 1.0),
			_committed_chunk_count,
			_commits_per_second,
			streaming_state,
			String(_bench_hardware_info.get("gpu_name", "Unknown GPU")),
			String(_bench_hardware_info.get("cpu_name", "Unknown CPU")),
			int(_bench_hardware_info.get("cpu_threads", 0)),
		]
		return

	var mode_name := ("Hero 3P" if play_third_person else "Hero 1P") if _mode == Mode.PLAY else "Fly"
	var pips := "■".repeat(_stamina_charges) + "□".repeat(maxi(0, play_max_stamina - _stamina_charges))
	var foot_mat_name: String = MATERIAL_ID_NAMES[_foot_material] if _foot_material >= 0 and _foot_material < MATERIAL_ID_NAMES.size() else "Air"
	var tool_text := "[%s] %s%s" % [TOOL_KEYS[_active_tool], TOOL_NAMES[_active_tool], material_state]
	if _mode == Mode.PLAY:
		var spirit_val := _gas.hero_attributes.get_spirit_power() if _gas != null else 42.0
		var hero_hp := _gas.hero_attributes.health if _gas != null else 750.0
		var hero_buffs := _gas.format_active_effects(self) if _gas != null else "None"
		var cd_bar := _gas.format_cooldown_bar() if _gas != null else ""
		var comp_line := ""
		if _companion_initialized and _gas != null:
			var c_attr: VoxelAbilitySystem.HeroAttributes = _gas.target_attributes.get(ai_companion.get_instance_id(), null)
			var c_hp := c_attr.health if c_attr != null else 750.0
			var c_eff := _gas.format_active_effects(ai_companion)
			comp_line = "
Companion Target: %.0f/750 HP | Status: %s" % [c_hp, c_eff]
		var w_hud := _gas.format_weapon_hud(_hero_state == HeroState.SLIDING, _play_ads_held) if _gas != null else "Pulse Carbine"
		tool_text = "%s | %s | HP: %.0f | Spirit: %.0f | Stamina: [%s] | Underfoot: %s | Buffs: %s
Abilities: %s%s" % [
			HERO_STATE_NAMES[_hero_state], w_hud, hero_hp, spirit_val, pips, foot_mat_name, hero_buffs, cd_bar, comp_line
		]

	_look_at_accum += delta
	if _look_at_accum >= LOOK_AT_REFRESH_INTERVAL:
		_look_at_accum = 0.0
		_refresh_look_at_material()

	_refresh_controls_label()
	var banner_suffix := ("\n" + _bench_last_summary_banner) if _bench_banner_timer > 0.0 and not _bench_last_summary_banner.is_empty() else ""
	stats_label.text = "FPS: %.0f  (%.2f ms)\nView dist: %d / %d chunks  (prefetch %d)\nCommitted: %d chunks  (%.0f/s)\nStreaming: %s\nMode: %s\nLooking at: %s\nTool: %s%s" % [
		_fps_smoothed,
		1000.0 / max(_fps_smoothed, 1.0),
		terrain.get_effective_view_distance_chunks(),
		terrain.view_distance_chunks,
		terrain.prefetch_distance_chunks,
		_committed_chunk_count,
		_commits_per_second,
		streaming_state,
		mode_name,
		_look_at_name,
		tool_text,
		banner_suffix,
	]

## Hides every HUD element and world Label3D except the reticle, leaving only the
## targeting dot and the 3D pickaxe viewmodel on screen. Press U or the HUD button
## again to restore. The reticle is never hidden.
func _toggle_ui_hidden() -> void:
	_ui_hidden = not _ui_hidden
	stats_label.visible = not _ui_hidden
	controls_label.visible = not _ui_hidden and _controls_visible
	hide_ui_button.visible = not _ui_hidden
	reticle.visible = true
	for label in find_children("*", "Label3D", true, false):
		label.visible = not _ui_hidden

## Rebuilds the controls overlay with live undo/redo depths. All key bindings
## live here; StatsLabel only reports state, so this is the single controls view.
func _refresh_controls_label() -> void:
	if controls_label == null:
		return
	var text := CONTROLS_TEXT % [_get_undo_depth_display(), _get_redo_depth_display()]
	if controls_label.text != text:
		controls_label.text = text

## Raycasts the crosshair ray and caches the display name of the material under the
## reticle: "Sky" when the ray misses, otherwise the solid sample's material name.
func _refresh_look_at_material() -> void:
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * max_ray_distance
	var material_id := terrain.raycast_terrain_material(from, to)
	if material_id < 0:
		_look_at_name = "Sky"
	elif material_id < MATERIAL_ID_NAMES.size():
		_look_at_name = MATERIAL_ID_NAMES[material_id]
	else:
		_look_at_name = "Material %d" % material_id

func _set_tool(tool: int, rolling_boulder: bool = false) -> void:
	# Assign before the early-out so Shift+4 still toggles the mode while the
	# Boulder tool is already selected; selecting any other tool clears it.
	_boulder_rolling = rolling_boulder and tool == Tool.BOULDER
	if tool == _active_tool:
		if tool == Tool.BOULDER:
			if _boulder_rolling:
				print("[VoxelTerrain3D] Boulder mode: rolling (Shift+4) | Ctrl+Click = crushing roller")
			else:
				print("[VoxelTerrain3D] Boulder mode: static 5-hit stamp (Shift+4 = rolling)")
		return
	_active_tool = tool
	_last_boom_point = Vector3(INF, INF, INF)
	var suffix := ""
	if _boulder_rolling:
		suffix = " (rolling)"
	print("[VoxelTerrain3D] Active tool: [%s] %s%s" % [TOOL_KEYS[tool], TOOL_NAMES[tool], suffix])

func _toggle_add_material() -> void:
	_add_material = MATERIAL_ROCK if _add_material == MATERIAL_SOIL else MATERIAL_SOIL
	print("[VoxelTerrain3D] Add/paint material: %s" % MATERIAL_NAMES[_add_material - 1])

func _is_drag_tool() -> bool:
	return _active_tool == Tool.PAINT or _active_tool == Tool.PAINT_DELETE

func _is_boom_tool() -> bool:
	return _active_tool == Tool.EXPLODE or _active_tool == Tool.LARGE_EXPLODE \
		or _active_tool == Tool.SMALL_HIT or _is_elemental_tool()

func _is_elemental_tool() -> bool:
	return _active_tool == Tool.WATER_QUENCH or _active_tool == Tool.FROST_ICE \
		or _active_tool == Tool.LAVA_MAGMA

## Applies a discrete (click) tool. Repeat explosive clicks on the same spot
## reuse the previous impact point: each explosion re-raycasts from the camera,
## so once an earlier hit blows the soft ground out in front of the rock the
## next ray hits a voxel or two deeper and the blast spheres creep down the hole
## instead of stacking their damage on the same rock. Compatibility test: the
## surface receded forward along the view ray by at most boom_snap_depth_factor
## of the blast radius and the aim did not slide sideways.
func _apply_click_tool() -> void:
	var point := _aim_point()
	# Aiming straight at a pristine statue should blast its surface, not the
	# terrain behind it, so the swap and the crater land on the same spot.
	if _is_boom_tool() and (not _opt1_swapped_to_voxels or not _atlas_opt1_swapped_to_voxels or not _ape_opt1_swapped_to_voxels):
		var phys_hit := _raycast_crosshair()
		if not phys_hit.is_empty():
			var hit_collider: Object = phys_hit.get("collider")
			if not _opt1_swapped_to_voxels and hit_collider == opt1_static_body:
				point = phys_hit.position
			elif not _atlas_opt1_swapped_to_voxels and hit_collider == atlas_opt1_static_body:
				point = phys_hit.position
			elif not _ape_opt1_swapped_to_voxels and hit_collider == ape_opt1_static_body:
				point = phys_hit.position
	if _is_boom_tool():
		if is_finite(_last_boom_point.x):
			var forward := -camera.global_transform.basis.z
			var drift := point - _last_boom_point
			var recession := drift.dot(forward)
			var lateral := (drift - forward * recession).length()
			if recession >= -0.1 \
					and recession <= _active_radius() * boom_snap_depth_factor \
					and lateral <= boom_snap_lateral:
				point = _last_boom_point
		_last_boom_point = point
	_apply_tool_at(point)

func _active_radius() -> float:
	match _active_tool:
		Tool.ADD_TERRAIN:
			return add_radius
		Tool.LARGE_EXPLODE:
			return large_explosion_radius
		Tool.PAINT:
			return paint_radius
		Tool.PAINT_DELETE:
			return paint_delete_radius
		Tool.BOULDER:
			return boulder_radius
		Tool.SMALL_HIT:
			return small_hit_radius
		Tool.WATER_QUENCH, Tool.FROST_ICE, Tool.LAVA_MAGMA:
			return elemental_radius
		_:
			return explosion_radius

func _aim_point() -> Vector3:
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * max_ray_distance
	var hit := terrain.raycast_terrain(from, to)
	if is_finite(hit.x):
		return hit
	return from - camera.global_transform.basis.z * 24.0

func _process_brush(delta: float) -> void:
	_brush_cooldown = maxf(0.0, _brush_cooldown - delta)
	if _brush_cooldown > 0.0:
		return
	var point := _aim_point()
	var radius := _active_radius()
	if point.distance_to(_last_brush_point) < radius * brush_spacing_factor:
		return
	_last_brush_point = point
	_brush_cooldown = brush_interval
	_apply_tool_at(point)

func _apply_tool_at(point: Vector3) -> void:
	# Pause walking along stale pre-explosion segments while the terrain & NavMesh update.
	# Carve tools (explosions / elemental / paint-delete) all report via explosion_completed,
	# which flags a fresh path query and clears _rebake_in_progress once the remesh settles.
	if _companion_initialized and _goal_mesh_inst.visible \
			and (_is_boom_tool() or _active_tool == Tool.PAINT_DELETE):
		_rebake_in_progress = true
	# First blast/elemental hit near either Option 1 statue swaps its pristine
	# mesh for the stamped voxel version; the crater carve lands on the same frame.
	if _is_boom_tool() and (not _opt1_swapped_to_voxels or not _atlas_opt1_swapped_to_voxels or not _ape_opt1_swapped_to_voxels):
		var hit := _raycast_crosshair()
		var hit_collider: Object = hit.get("collider") if not hit.is_empty() else null
		_check_and_swap_opt1_easter_island_on_blast(point, _active_radius(), hit_collider)
		_check_and_swap_atlas_opt1_on_blast(point, _active_radius(), hit_collider)
		_check_and_swap_ape_opt1_on_blast(point, _active_radius(), hit_collider)
	match _active_tool:
		Tool.ADD_TERRAIN:
			terrain.add_terrain_with_material(point, add_radius, _add_material)
			_note_undo_cpp_op({"kind": "add_terrain", "point": point, "radius": add_radius, "material": _add_material})
		Tool.LARGE_EXPLODE:
			terrain.trigger_explosion(point, large_explosion_radius)
			_note_undo_cpp_op({"kind": "explosion", "point": point, "radius": large_explosion_radius})
		Tool.PAINT:
			terrain.add_terrain_with_material(point, paint_radius, _add_material)
			_note_undo_cpp_op({"kind": "add_terrain", "point": point, "radius": paint_radius, "material": _add_material})
		Tool.PAINT_DELETE:
			terrain.trigger_explosion(point, paint_delete_radius)
			_note_undo_cpp_op({"kind": "explosion", "point": point, "radius": paint_delete_radius})
		Tool.BOULDER:
			if _boulder_rolling:
				# Rolling bodies are transient props, so they add no click-time undo
				# op; the bake/crush completions register their own native edit later.
				spawn_rolling_boulder(point, boulder_radius, true, Input.is_key_pressed(KEY_CTRL))
			else:
				terrain.place_boulder(point, boulder_radius)
				_note_undo_cpp_op({"kind": "boulder", "point": point, "radius": boulder_radius})
		Tool.SMALL_HIT:
			terrain.trigger_explosion(point, small_hit_radius)
			_note_undo_cpp_op({"kind": "explosion", "point": point, "radius": small_hit_radius})
		Tool.WATER_QUENCH:
			terrain.trigger_elemental_sphere(point, elemental_radius, ELEMENTAL_WATER)
			_note_undo_cpp_op({"kind": "elemental", "point": point, "radius": elemental_radius, "element": ELEMENTAL_WATER})
		Tool.FROST_ICE:
			terrain.trigger_elemental_sphere(point, elemental_radius, ELEMENTAL_FROST)
			_note_undo_cpp_op({"kind": "elemental", "point": point, "radius": elemental_radius, "element": ELEMENTAL_FROST})
		Tool.LAVA_MAGMA:
			terrain.trigger_elemental_sphere(point, elemental_radius, ELEMENTAL_LAVA)
			_note_undo_cpp_op({"kind": "elemental", "point": point, "radius": elemental_radius, "element": ELEMENTAL_LAVA})
		_:
			terrain.trigger_explosion(point, explosion_radius)
			_note_undo_cpp_op({"kind": "explosion", "point": point, "radius": explosion_radius})

# ==============================================================================
# Rolling Physics Boulders (Shift+4 / Ctrl+Click)
# ==============================================================================

## Spawns a dynamic granite boulder above `hit_point`. Boulders roll on the
## per-chunk PhysicsServer3D concave shapes and, once settled, bake back into
## permanent 5-hit voxel terrain. `crush` boulders carve the ground they roll over.
func spawn_rolling_boulder(hit_point: Vector3, radius: float = 2.0, bake_when_settled: bool = true, crush: bool = false) -> RigidBody3D:
	if _rolling_boulders.size() >= boulder_max_count:
		var oldest := _rolling_boulders.pop_front() as RigidBody3D
		if is_instance_valid(oldest):
			oldest.queue_free()
	var crushing := 0
	for existing in _rolling_boulders:
		if is_instance_valid(existing) and bool(existing.get_meta("crush", false)):
			crushing += 1
	if crush and crushing >= boulder_max_crushing:
		crush = false

	var center := _find_boulder_spawn_center(hit_point, radius)
	var body := RigidBody3D.new()
	body.name = "RollingBoulder"
	body.mass = maxf(40.0, (4.0 / 3.0) * PI * pow(radius, 3.0) * 25.0)
	body.gravity_scale = boulder_gravity_scale
	body.continuous_cd = true
	body.angular_damp = 0.35
	body.linear_damp = 0.05

	# Smoother than a default PhysicsMaterial so the sphere glides over internal
	# Surface Nets triangle edges instead of catching on them.
	var phys_mat := PhysicsMaterial.new()
	phys_mat.friction = 0.45
	phys_mat.bounce = 0.08
	body.physics_material_override = phys_mat

	var col := CollisionShape3D.new()
	col.name = "CollisionShape3D"
	var sphere_shape := SphereShape3D.new()
	sphere_shape.radius = radius
	col.shape = sphere_shape
	body.add_child(col)

	var mesh_inst := MeshInstance3D.new()
	mesh_inst.name = "BoulderMesh"
	mesh_inst.mesh = _build_boulder_sphere_mesh(radius)
	var boulder_mat := _get_boulder_material()
	if boulder_mat != null:
		mesh_inst.material_override = boulder_mat
	body.add_child(mesh_inst)

	body.set_meta("radius", radius)
	body.set_meta("crush", crush)
	body.set_meta("last_crush_ms", 0)
	body.set_meta("last_crush_pos", center)
	if bake_when_settled:
		body.set_meta("settle_timer", 0.0)

	add_child(body)
	body.global_position = center
	_rolling_boulders.append(body)
	return body

## Picks the spawn height from the highest ground under the sphere footprint so a
## boulder aimed at a slope does not start intersecting uphill geometry. The
## concave terrain shape is one-sided, so spawning inside it would fall through.
func _find_boulder_spawn_center(hit_point: Vector3, radius: float) -> Vector3:
	var offsets: Array[Vector3] = [
		Vector3.ZERO,
		Vector3(radius * 0.7, 0.0, 0.0),
		Vector3(-radius * 0.7, 0.0, 0.0),
		Vector3(0.0, 0.0, radius * 0.7),
		Vector3(0.0, 0.0, -radius * 0.7),
	]
	var up_clearance := radius * 2.0 + 2.0
	var down_reach := radius * 4.0 + 4.0
	var best_y := -INF
	var found := false
	for offset in offsets:
		var from: Vector3 = hit_point + offset + Vector3(0.0, up_clearance, 0.0)
		var hit := terrain.raycast_terrain(from, from - Vector3(0.0, down_reach, 0.0))
		if is_finite(hit.x):
			best_y = maxf(best_y, hit.y)
			found = true
	var surface_y := best_y if found else hit_point.y
	return Vector3(hit_point.x, surface_y + radius + 0.2, hit_point.z)

## Shares one duplicated terrain material across every boulder. The duplicate only
## flips the shader's pattern space so the granite does not swim while it tumbles.
func _get_boulder_material() -> ShaderMaterial:
	if _boulder_material != null:
		return _boulder_material
	var terrain_mat := terrain.terrain_material
	if terrain_mat is ShaderMaterial:
		var dup := (terrain_mat as ShaderMaterial).duplicate() as ShaderMaterial
		dup.set_shader_parameter("pattern_in_object_space", true)
		_boulder_material = dup
	return _boulder_material

## Builds the boulder sphere with the exact vertex-color domain the terrain shader
## expects: R = damage ratio (0 = pristine), G = persistent-damage rock flag,
## B = hits-to-break / 255 (5/255 = placed boulder), A = baked-paint weight (0).
func _build_boulder_sphere_mesh(radius: float) -> ArrayMesh:
	var prim := SphereMesh.new()
	prim.radius = radius
	prim.height = radius * 2.0
	prim.radial_segments = 32
	prim.rings = 16
	var arrays := prim.get_mesh_arrays()
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var colors := PackedColorArray()
	colors.resize(verts.size())
	colors.fill(Color(0.0, 1.0, 5.0 / 255.0, 0.0))
	arrays[Mesh.ARRAY_COLOR] = colors
	# The terrain shader reads UV2.x for its baked albedo; the paint weight is 0,
	# but supplying the attribute keeps the surface format explicit.
	var uv2 := PackedVector2Array()
	uv2.resize(verts.size())
	arrays[Mesh.ARRAY_TEX_UV2] = uv2
	var arr_mesh := ArrayMesh.new()
	arr_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return arr_mesh

## Explosions carve the ground asynchronously; once the edit transaction commits,
## sleeping RigidBody3Ds underneath it must be woken or they hang over the crater.
func _on_terrain_blast_physics(center: Vector3, radius: float, _affected_chunks: int) -> void:
	var wake_radius := radius + 6.0
	var blast_radius := radius + 4.0
	for i in range(_rolling_boulders.size() - 1, -1, -1):
		var body := _rolling_boulders[i]
		if not is_instance_valid(body):
			_rolling_boulders.remove_at(i)
			continue
		var offset := body.global_position - center
		var dist := offset.length()
		if dist <= wake_radius:
			body.sleeping = false
		if dist <= blast_radius and dist > 0.001:
			# A crushing roller is exactly one radius above its own contact blast. If
			# the crush radius is tuned above the default, skip the self-impulse so it
			# keeps trenching instead of hopping on every carve.
			var grace_ms := int(body.get_meta("self_blast_grace_ms", 0))
			if Time.get_ticks_msec() < grace_ms:
				continue
			var falloff := clampf(1.0 - (dist / blast_radius), 0.0, 1.0)
			var dir := (offset / dist + Vector3.UP * 0.35).normalized()
			body.apply_central_impulse(dir * (body.mass * 14.0 * falloff))

## Fills (Add/Paint and boulder bakes) never blast, but they can bury a sleeping
## boulder, so wake anything inside the edited volume's neighbourhood.
func _on_terrain_fill_physics(center: Vector3, radius: float, _affected_chunks: int) -> void:
	var wake_radius := radius + 6.0
	for body in _rolling_boulders:
		if is_instance_valid(body) and body.global_position.distance_to(center) <= wake_radius:
			body.sleeping = false

func _update_rolling_boulders(delta: float) -> void:
	if _rolling_boulders.is_empty():
		return
	# collision_distance_chunks is -1 here ("collide every resident chunk"), so use
	# the effective streaming radius rather than a negative despawn distance.
	var view_chunks := terrain.get_effective_view_distance_chunks()
	if view_chunks <= 0:
		view_chunks = terrain.view_distance_chunks
	var max_live_dist := float(view_chunks + 1) * 32.0 * terrain.voxel_scale
	var world_floor := terrain.surface_base_height \
		- float(terrain.world_height_chunks) * 32.0 * terrain.voxel_scale - 16.0

	for i in range(_rolling_boulders.size() - 1, -1, -1):
		var body := _rolling_boulders[i]
		if not is_instance_valid(body):
			_rolling_boulders.remove_at(i)
			continue
		var pos := body.global_position
		if pos.distance_to(camera.global_position) > max_live_dist or pos.y < world_floor:
			_rolling_boulders.remove_at(i)
			body.queue_free()
			continue

		if bool(body.get_meta("crush", false)):
			_update_crushing_boulder(body, pos)

		if body.has_meta("settle_timer"):
			if body.sleeping or (body.linear_velocity.length() < 0.25 and body.angular_velocity.length() < 0.4):
				var settled := float(body.get_meta("settle_timer")) + delta
				body.set_meta("settle_timer", settled)
				if settled >= boulder_settle_seconds:
					_try_bake_rolling_boulder(i)
			else:
				body.set_meta("settle_timer", 0.0)

## Titan-style crushing rollers carve a throttled crater trail along their path.
## Blasts are centered on the ground contact point, not the sphere center, so they
## actually bite into the terrain the boulder is rolling over.
func _update_crushing_boulder(body: RigidBody3D, pos: Vector3) -> void:
	if body.linear_velocity.length() < boulder_crush_speed:
		return
	var now := Time.get_ticks_msec()
	var last_ms := int(body.get_meta("last_crush_ms", 0))
	var last_pos: Vector3 = body.get_meta("last_crush_pos", pos)
	if now - last_ms < int(boulder_crush_interval * 1000.0):
		return
	if pos.distance_to(last_pos) < 0.75:
		return
	var radius := float(body.get_meta("radius", 1.0))
	var ground := terrain.raycast_terrain(pos, pos - Vector3(0.0, radius * 2.5, 0.0))
	# Only carve while actually in contact; airborne blasts would waste a worker edit.
	if not is_finite(ground.x) or pos.distance_to(ground) > radius * 1.35:
		return
	var blast_radius := radius * boulder_crush_radius_scale
	terrain.trigger_explosion(ground, blast_radius)
	_pending_crush_blasts.append({"point": ground, "radius": blast_radius})
	body.set_meta("last_crush_ms", now)
	body.set_meta("last_crush_pos", pos)
	body.set_meta("self_blast_grace_ms", now + 500)

## Bakes a settled boulder into permanent 5-hit voxels. A downward voxel probe
## ensures it actually reached the terrain instead of resting on another boulder.
func _try_bake_rolling_boulder(index: int) -> void:
	var body := _rolling_boulders[index]
	var radius := float(body.get_meta("radius", 1.0))
	var center := body.global_position
	var hit := terrain.raycast_terrain(center, center - Vector3(0.0, radius * 3.0, 0.0))
	var grounded := is_finite(hit.x) and center.distance_to(hit) <= radius * 2.0
	if not grounded:
		# Came to rest on a prop or mid-air: retry later instead of baking in the air.
		body.set_meta("settle_timer", 0.0)
		return
	var bake_radius := radius * 0.95
	terrain.place_boulder(center, bake_radius)
	_pending_bake_fills.append({"point": center, "radius": bake_radius})
	_rolling_boulders.remove_at(index)
	body.queue_free()

## Bakes and crush blasts land in the native undo manager without a click-time
## GDScript action. Register a synthetic one-op action so `cpp_ops` stays in
## lockstep with terrain.undo() and multi-op brush strokes are not over-undone.
func _register_async_edit(label: String, point: Vector3, radius: float) -> void:
	if _mode == Mode.BENCHMARK:
		return
	var op := {"kind": "boulder", "point": point, "radius": radius}
	if not _active_undo_action.is_empty():
		_note_undo_cpp_op(op)
		return
	_begin_undo_action(label)
	_note_undo_cpp_op(op)
	_end_undo_action()

## Matches a completion signal to a queued bake/crush request. Signals report
## grid-rounded world coordinates (up to half a voxel off), hence the tolerances.
func _consume_pending_edit(pending: Array[Dictionary], point: Vector3, radius: float) -> bool:
	for i in range(pending.size() - 1, -1, -1):
		var entry: Dictionary = pending[i]
		var entry_point: Vector3 = entry.get("point", Vector3.ZERO)
		if entry_point.distance_to(point) <= 1.0 \
				and absf(float(entry.get("radius", 0.0)) - radius) <= 0.5:
			pending.remove_at(i)
			return true
	return false

func _clear_rolling_boulders() -> void:
	for body in _rolling_boulders:
		if is_instance_valid(body):
			body.queue_free()
	_rolling_boulders.clear()
	_pending_bake_fills.clear()
	_pending_crush_blasts.clear()

func _raycast_crosshair() -> Dictionary:
	var space_state := get_world_3d().direct_space_state
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * max_ray_distance
	var query := PhysicsRayQueryParameters3D.create(from, to)
	return space_state.intersect_ray(query)

## Downward physics raycast used for companion gravity, goal snapping and path draping.
func _raycast_ground_at(pos: Vector3, clearance_up: float = 3.5, down_dist: float = 64.0) -> Dictionary:
	var space_state := get_world_3d().direct_space_state
	var from := pos + Vector3(0.0, clearance_up, 0.0)
	var to := pos - Vector3(0.0, down_dist, 0.0)
	var query := PhysicsRayQueryParameters3D.create(from, to)
	return space_state.intersect_ray(query)

## Every frame, checks the terrain beneath the cyan pill. If the ground was blown out
## from under it, the companion falls with gravity onto the crater floor, pauses its
## horizontal walk while falling or while a remesh/rebake is in progress, and re-queries
## the path from its new landed position.
func _apply_companion_gravity_and_ground_snap(delta: float) -> void:
	if not _companion_initialized:
		return
	var cur := ai_companion.global_position
	var hit := _raycast_ground_at(cur, 4.5, 80.0)
	if not hit.is_empty():
		var ground_y: float = hit.position.y + 0.15
		if cur.y > ground_y + 0.35:
			# Ground was carved away beneath the pill -> fall with gravity into the crater!
			_companion_vertical_vel = min(_companion_vertical_vel + companion_gravity * delta, 40.0)
			cur.y = max(ground_y, cur.y - _companion_vertical_vel * delta)
			ai_companion.global_position = cur
			# Once it lands on the new crater floor, trigger a fresh path query from its landed position
			if cur.y <= ground_y + 0.05 and _goal_mesh_inst.visible:
				_companion_vertical_vel = 0.0
				_has_pending_path_query = true
		else:
			# Smoothly step up/down onto newly added Ice Bridges or slopes beneath the pill
			_companion_vertical_vel = 0.0
			cur.y = ground_y
			ai_companion.global_position = cur

## Stamps the configured offline-baked `.vreg` region at the crosshair with the given
## reconstitution delay (0 or less keeps the ruin permanently destroyed once blasted).
## Baked regions are auto-centered on their local sample (128, 128, 128) = local chunk
## (4, 4, 4), so the origin is offset by that much to straddle the hit point.
func _stamp_asset_at_crosshair(recreate_delay: float) -> void:
	var hit := _raycast_crosshair()
	if hit.is_empty():
		print("[VoxelTerrain3D] Aim at terrain surface and press 0 (timed ruin) or - (permanent ruin) to stamp an asset.")
		return
	var hit_pos: Vector3 = hit.position
	var chunk_size: float = 32.0 * terrain.voxel_scale
	var origin := Vector3i(
		int(floor(hit_pos.x / chunk_size)) - 4,
		int(floor(hit_pos.y / chunk_size)) - 4,
		int(floor(hit_pos.z / chunk_size)) - 4
	)
	_begin_undo_action("Stamp asset")
	var structure_id := _stamp_structure(stamp_asset_path, Vector3(origin), recreate_delay)
	if structure_id > 0:
		_note_undo_cpp_op({
			"kind": "stamp_asset",
			"structure_id": structure_id,
			"vreg_path": stamp_asset_path,
			"origin_chunk": Vector3(origin),
			"recreate_delay": recreate_delay,
		})
		_end_undo_action()
		print("[VoxelTerrain3D] Stamped asset '%s' structure_id=%d at chunk %s delay=%.1fs" % [
			stamp_asset_path, structure_id, str(origin), recreate_delay
		])
	else:
		_active_undo_action.clear()
		push_warning("[VoxelTerrain3D] Failed to stamp '%s' — bake one first: voxel_baker <model.obj> %s" % [
			stamp_asset_path, stamp_asset_path
		])

func _place_companion_at_crosshair() -> void:
	var hit := _raycast_crosshair()
	if hit.is_empty():
		print("[Nav3D] Aim at terrain surface and press P (or Shift+RMB) to place AI Companion.")
		return
	ai_companion.global_position = hit.position + Vector3(0.0, 0.15, 0.0)
	_companion_vertical_vel = 0.0
	_companion_initialized = true
	_active_waypoints.clear()
	_path_immediate_mesh.clear_surfaces()
	print("[Nav3D] Placed AI Companion start at ", ai_companion.global_position)

func _query_companion_cave_path_to_crosshair() -> void:
	var hit := _raycast_crosshair()
	var goal_pos: Vector3 = (hit.position + Vector3(0.0, 0.15, 0.0)) if not hit.is_empty() else camera.global_position

	# First RMB click places the companion if it hasn't been placed on the surface yet
	if not _companion_initialized:
		ai_companion.global_position = goal_pos
		_companion_vertical_vel = 0.0
		_companion_initialized = true
		print("[Nav3D] Companion placed at ", goal_pos, " — Right-click another spot to path there!")
		return

	_pending_path_goal = goal_pos
	_goal_mesh_inst.global_position = goal_pos
	_goal_mesh_inst.visible = true
	_has_pending_path_query = true
	_poll_pending_path_query()

func _poll_pending_path_query() -> void:
	if not _has_pending_path_query:
		return
	var start_pos := ai_companion.global_position
	var start_synced := ai_companion.is_path_region_synced(start_pos)
	var goal_synced := ai_companion.is_path_region_synced(_pending_path_goal)
	if not start_synced or not goal_synced:
		return

	_has_pending_path_query = false
	_rebake_in_progress = false
	var raw_path: PackedVector3Array = ai_companion.get_voxel_path(start_pos, _pending_path_goal)
	var draped_path: PackedVector3Array = _drape_path_onto_terrain(raw_path)
	print("[Nav3D] Companion 3D path from ", start_pos, " to ", _pending_path_goal,
		" -> nav waypoints: ", raw_path.size(), " (surface-draped points: ", draped_path.size(), ")")
	_active_waypoints = draped_path
	_current_waypoint_idx = 0
	_draw_path_ribbon(draped_path)

## Subdivides Detour's string-pulled segments every ~1.25m and snaps their Y coordinate
## onto the physical terrain/Ice Bridge surface so the green ribbon and companion follow
## the true 3D contour of craters, hills, and Ice Bridges without cutting through mid-air.
func _drape_path_onto_terrain(raw_path: PackedVector3Array) -> PackedVector3Array:
	var out := PackedVector3Array()
	if raw_path.is_empty():
		return out
	for i in range(raw_path.size() - 1):
		var a := raw_path[i]
		var b := raw_path[i + 1]
		var dist := a.distance_to(b)
		var steps := maxi(1, int(ceil(dist / 1.25)))
		for s in range(steps):
			var t := float(s) / float(steps)
			var sample_pt := a.lerp(b, t)
			var hit := _raycast_ground_at(sample_pt, 6.0, 24.0)
			if not hit.is_empty():
				sample_pt.y = hit.position.y + 0.15
			out.push_back(sample_pt)
	var last_pt := raw_path[raw_path.size() - 1]
	var last_hit := _raycast_ground_at(last_pt, 6.0, 24.0)
	if not last_hit.is_empty():
		last_pt.y = last_hit.position.y + 0.15
	out.push_back(last_pt)
	return out

func _draw_path_ribbon(path: PackedVector3Array) -> void:
	_path_immediate_mesh.clear_surfaces()
	if path.size() < 2:
		return
	_path_immediate_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for pt in path:
		_path_immediate_mesh.surface_add_vertex(pt + Vector3(0.0, 0.35, 0.0))
	_path_immediate_mesh.surface_end()

func _step_companion_along_path(delta: float) -> void:
	if _companion_stun_timer > 0.0:
		_companion_stun_timer = maxf(0.0, _companion_stun_timer - delta)
	var gas_stunned := _gas != null and _gas.is_target_stunned(ai_companion)
	if _companion_base_mat != null and _gas != null:
		var c_tags := int(_gas.target_tags.get(ai_companion.get_instance_id(), 0))
		if (c_tags & VoxelAbilitySystem.Tag.FROZEN) != 0:
			_companion_base_mat.albedo_color = Color(0.55, 0.96, 1.0)
			_companion_base_mat.emission = Color(0.35, 0.90, 1.0) * 1.6
		elif _companion_stun_timer > 0.0 or (c_tags & VoxelAbilitySystem.Tag.STUNNED) != 0:
			_companion_base_mat.albedo_color = Color(1.0, 0.45, 0.1)
			_companion_base_mat.emission = Color(1.0, 0.4, 0.05) * 1.2
		elif (c_tags & VoxelAbilitySystem.Tag.BURNING) != 0:
			_companion_base_mat.albedo_color = Color(1.0, 0.28, 0.06)
			_companion_base_mat.emission = Color(1.0, 0.22, 0.04) * 1.3
		else:
			_companion_base_mat.albedo_color = Color(0.1, 0.95, 1.0)
			_companion_base_mat.emission = Color(0.1, 0.85, 1.0) * 0.6
	if _companion_stun_timer > 0.0 or gas_stunned:
		return
	if _charge_pinned_companion:
		return
	if _rebake_in_progress or _has_pending_path_query:
		return
	if _current_waypoint_idx >= _active_waypoints.size():
		return
	# Wait until the pill has landed on the ground before walking horizontally
	if _companion_vertical_vel > 0.5:
		return

	var cur := ai_companion.global_position
	var target := _active_waypoints[_current_waypoint_idx]
	var horiz_to_target := Vector3(target.x - cur.x, 0.0, target.z - cur.z)
	var horiz_dist := horiz_to_target.length()
	var step := companion_walk_speed * delta

	if horiz_dist <= step or horiz_dist < 0.15:
		ai_companion.global_position = Vector3(target.x, cur.y, target.z)
		_current_waypoint_idx += 1
	else:
		var dir := horiz_to_target / horiz_dist
		ai_companion.global_position = Vector3(cur.x + dir.x * step, cur.y, cur.z + dir.z * step)

func _print_throttled(message: String) -> void:
	var now := Time.get_ticks_msec()
	if now - _last_event_print_ms >= 500:
		_last_event_print_ms = now
		print(message)

func _warn_throttled(message: String) -> void:
	var now := Time.get_ticks_msec()
	if now - _last_fail_warn_ms >= 2000:
		_last_fail_warn_ms = now
		push_warning(message)

func _on_explosion_requested(pos: Vector3, radius: float) -> void:
	_print_throttled("[VoxelTerrain3D] Explosion requested at %s radius=%.1f" % [pos, radius])

func _on_explosion_completed(pos: Vector3, radius: float, affected_chunks: int) -> void:
	if _mode == Mode.BENCHMARK:
		_bench_affected_chunks_total += affected_chunks
		if not _bench_pending_blast_start_usec.is_empty():
			var t0 := _bench_pending_blast_start_usec[0]
			_bench_pending_blast_start_usec.remove_at(0)
			var lat_ms := float(Time.get_ticks_usec() - t0) / 1000.0
			_bench_blast_latencies_ms.push_back(lat_ms)
	_print_throttled("[VoxelTerrain3D] Explosion completed at %s radius=%.1f chunks=%d" % [pos, radius, affected_chunks])
	# Snap the goal sphere down if the ground under the goal was carved away
	if _goal_mesh_inst.visible:
		var g_hit := _raycast_ground_at(_pending_path_goal, 6.0, 32.0)
		if not g_hit.is_empty():
			_pending_path_goal.y = g_hit.position.y + 0.15
			_goal_mesh_inst.global_position = _pending_path_goal
	# Automatically re-query the path if a goal is active so you can watch paths reroute
	# around new craters or across freshly frozen Ice bridges!
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true
	# A crushing roller's carve was completed by the worker pool; give it a matching
	# undo action so native and GDScript undo depths stay in sync. A zero-chunk
	# completion records no native entry, so it must not create a synthetic action.
	if _consume_pending_edit(_pending_crush_blasts, pos, radius) and affected_chunks > 0:
		_register_async_edit("Boulder crush", pos, radius)

func _on_explosion_failed(reason: String) -> void:
	_rebake_in_progress = false
	_warn_throttled("[VoxelTerrain3D] Explosion failed: " + reason)

func _on_terrain_add_requested(pos: Vector3, radius: float) -> void:
	_print_throttled("[VoxelTerrain3D] Add terrain requested at %s radius=%.1f" % [pos, radius])

func _on_terrain_add_completed(pos: Vector3, radius: float, affected_chunks: int) -> void:
	if _mode == Mode.BENCHMARK:
		_bench_affected_chunks_total += affected_chunks
		if not _bench_pending_blast_start_usec.is_empty():
			var t0 := _bench_pending_blast_start_usec[0]
			_bench_pending_blast_start_usec.remove_at(0)
			var lat_ms := float(Time.get_ticks_usec() - t0) / 1000.0
			_bench_blast_latencies_ms.push_back(lat_ms)
	_print_throttled("[VoxelTerrain3D] Add terrain completed at %s radius=%.1f chunks=%d" % [pos, radius, affected_chunks])
	# A settled roller just stamped itself back into the grid; record the edit so
	# undo consumes exactly one native entry per GDScript action.
	if _consume_pending_edit(_pending_bake_fills, pos, radius) and affected_chunks > 0:
		_register_async_edit("Boulder settled", pos, radius)

func _on_terrain_add_failed(reason: String) -> void:
	_warn_throttled("[VoxelTerrain3D] Add terrain failed: " + reason)

func _on_chunk_committed(_chunk_key: int) -> void:
	_committed_chunk_count += 1
	_commits_in_window += 1
	if _mode == Mode.BENCHMARK:
		_bench_commits_count += 1

## A stamped structure was carved by an explosion/elemental reaction. Its
## reconstitution timer is now counting down (permanent stamps ignore this).
func _on_structure_damaged(structure_id: int, asset_name: String) -> void:
	_print_throttled("[VoxelTerrain3D] Structure damaged: id=%d name=%s (reconstitution timer armed)" % [
		structure_id, asset_name
	])

## A stamped structure's quiet period elapsed and its exact pre-damage voxels
## were restored; its chunks were re-meshed and navmesh rebaked.
func _on_structure_recreated(structure_id: int, asset_name: String) -> void:
	print("[VoxelTerrain3D] Structure reconstituted: id=%d name=%s" % [structure_id, asset_name])
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

## A stamped structure was removed by the reset loop; its pre-stamp chunk
## baseline was restored, so re-plan any active companion path.
func _on_structure_removed(structure_id: int, asset_name: String) -> void:
	print("[VoxelTerrain3D] Structure removed: id=%d name=%s -> pre-stamp chunk baseline restored" % [
		structure_id, asset_name
	])
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true


func _on_undo_completed(restored_chunks: int, remaining_undo: int, available_redo: int) -> void:
	print("[UndoRedo] Undo completed -> restored %d chunk(s) (undo=%d, redo=%d)" % [
		restored_chunks, remaining_undo, available_redo
	])
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _on_redo_completed(restored_chunks: int, available_undo: int, remaining_redo: int) -> void:
	print("[UndoRedo] Redo completed -> re-applied %d chunk(s) (undo=%d, redo=%d)" % [
		restored_chunks, available_undo, remaining_redo
	])
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

# ==============================================================================
# Full Undo / Redo System (Keys Z / Y / Shift+R & Benchmark Pass Reset)
# ==============================================================================

func _has_native_undo_redo() -> bool:
	return terrain != null and terrain.has_method("undo") and terrain.has_method("redo")

func _get_undo_depth_display() -> int:
	if not _undo_stack.is_empty():
		return _undo_stack.size()
	if _has_native_undo_redo() and terrain.has_method("get_undo_count"):
		return int(terrain.call("get_undo_count"))
	return 0

func _get_redo_depth_display() -> int:
	if not _redo_stack.is_empty():
		return _redo_stack.size()
	if _has_native_undo_redo() and terrain.has_method("get_redo_count"):
		return int(terrain.call("get_redo_count"))
	return 0

func _clear_undo_history_to_current_baseline() -> void:
	_active_undo_action.clear()
	_undo_stack.clear()
	_redo_stack.clear()
	if terrain != null and terrain.has_method("clear_undo_history"):
		terrain.call("clear_undo_history")

func _begin_undo_action(label: String) -> void:
	if _mode == Mode.BENCHMARK:
		return
	_active_undo_action = {
		"label": label,
		"cpp_ops": 0,
		"ops": [],
	}

func _note_undo_cpp_op(op_record: Dictionary) -> void:
	if _mode == Mode.BENCHMARK or _active_undo_action.is_empty():
		return
	_active_undo_action["cpp_ops"] = int(_active_undo_action.get("cpp_ops", 0)) + 1
	var ops_arr: Array = _active_undo_action["ops"]
	ops_arr.append(op_record)

func _end_undo_action() -> void:
	if _active_undo_action.is_empty():
		return
	var cpp_ops := int(_active_undo_action.get("cpp_ops", 0))
	if cpp_ops > 0:
		_undo_stack.append(_active_undo_action.duplicate(true))
		if _undo_stack.size() > MAX_UNDO_ACTIONS:
			_undo_stack.pop_front()
		_redo_stack.clear()
	_active_undo_action.clear()

func _restore_opt1_visual_state_after_undo(statue_id: String) -> void:
	match statue_id:
		"easter_island":
			_opt1_swapped_to_voxels = false
			_opt1_structure_id = 0
			_cancel_pending_pristine_hide(opt1_mesh_inst)
			opt1_mesh_inst.visible = true
			opt1_col_shape.disabled = false
			opt1_label.text = "Easter Island Option 1 (16x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"
		"atlas":
			_atlas_opt1_swapped_to_voxels = false
			_atlas_opt1_structure_id = 0
			_cancel_pending_pristine_hide(atlas_opt1_mesh_inst)
			atlas_opt1_mesh_inst.visible = true
			atlas_opt1_col_shape.disabled = false
			atlas_opt1_label.text = "Atlas Option 1 (5.33x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"
		"ape":
			_ape_opt1_swapped_to_voxels = false
			_ape_opt1_structure_id = 0
			_cancel_pending_pristine_hide(ape_opt1_mesh_inst)
			ape_opt1_mesh_inst.visible = true
			ape_opt1_col_shape.disabled = false
			ape_opt1_label.text = "Ape Option 1 (5.33x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"

func _reapply_opt1_visual_state_after_redo(statue_id: String, struct_id: int) -> void:
	match statue_id:
		"easter_island":
			_opt1_swapped_to_voxels = true
			_opt1_structure_id = struct_id
			_queue_pristine_hide(opt1_mesh_inst, opt1_col_shape, opt1_label,
				"Easter Island Option 1 (16x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _opt1_structure_id)
		"atlas":
			_atlas_opt1_swapped_to_voxels = true
			_atlas_opt1_structure_id = struct_id
			_queue_pristine_hide(atlas_opt1_mesh_inst, atlas_opt1_col_shape, atlas_opt1_label,
				"Atlas Option 1 (5.33x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _atlas_opt1_structure_id)
		"ape":
			_ape_opt1_swapped_to_voxels = true
			_ape_opt1_structure_id = struct_id
			_queue_pristine_hide(ape_opt1_mesh_inst, ape_opt1_col_shape, ape_opt1_label,
				"Ape Option 1 (5.33x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _ape_opt1_structure_id)

func _perform_undo() -> bool:
	if _tool_pressed:
		_tool_pressed = false
		_end_undo_action()
	_last_boom_point = Vector3(INF, INF, INF)

	if not _undo_stack.is_empty():
		var action: Dictionary = _undo_stack.pop_back()
		var cpp_ops := int(action.get("cpp_ops", 1))
		var ops: Array = action.get("ops", [])
		var any_undone := false

		if _has_native_undo_redo():
			for _i in range(cpp_ops):
				if bool(terrain.call("undo")):
					any_undone = true
			for i in range(ops.size() - 1, -1, -1):
				var op: Dictionary = ops[i]
				if String(op.get("kind", "")) == "opt1_swap":
					_restore_opt1_visual_state_after_undo(String(op.get("statue", "")))
		else:
			# Fallback when running against an older GDExtension binary without C++ undo()
			for i in range(ops.size() - 1, -1, -1):
				var op: Dictionary = ops[i]
				var kind := String(op.get("kind", ""))
				if kind == "opt1_swap":
					var sid := int(op.get("structure_id", 0))
					if sid > 0:
						_remove_structure(sid)
					_restore_opt1_visual_state_after_undo(String(op.get("statue", "")))
					any_undone = true
				elif kind == "stamp_asset":
					var sid := int(op.get("structure_id", 0))
					if sid > 0 and _remove_structure(sid):
						any_undone = true
				elif kind == "add_terrain" or kind == "boulder":
					terrain.trigger_explosion(op.get("point", Vector3.ZERO), float(op.get("radius", 4.0)))
					any_undone = true
				elif kind == "explosion":
					terrain.add_terrain_with_material(op.get("point", Vector3.ZERO), float(op.get("radius", 4.0)), MATERIAL_SOIL)
					any_undone = true

		_redo_stack.append(action)
		_bench_last_summary_banner = "[Undo] %s (Undo: %d | Redo: %d)" % [
			String(action.get("label", "Edit")),
			_get_undo_depth_display(),
			_get_redo_depth_display(),
		]
		_bench_banner_timer = 3.5
		return any_undone

	if _has_native_undo_redo() and bool(terrain.call("can_undo")):
		var ok := bool(terrain.call("undo"))
		if ok:
			_bench_last_summary_banner = "[Undo] Restored previous terrain state (Undo: %d | Redo: %d)" % [
				_get_undo_depth_display(),
				_get_redo_depth_display(),
			]
			_bench_banner_timer = 3.5
		return ok

	print("[UndoRedo] Nothing to undo.")
	return false

func _perform_redo() -> bool:
	if _tool_pressed:
		_tool_pressed = false
		_end_undo_action()
	_last_boom_point = Vector3(INF, INF, INF)

	if not _redo_stack.is_empty():
		var action: Dictionary = _redo_stack.pop_back()
		var cpp_ops := int(action.get("cpp_ops", 1))
		var ops: Array = action.get("ops", [])
		var any_redone := false

		if _has_native_undo_redo():
			for _i in range(cpp_ops):
				if bool(terrain.call("redo")):
					any_redone = true
			for op_var in ops:
				var op: Dictionary = op_var
				if String(op.get("kind", "")) == "opt1_swap":
					_reapply_opt1_visual_state_after_redo(
						String(op.get("statue", "")),
						int(op.get("structure_id", 0))
					)
		else:
			# Fallback when running against an older GDExtension binary without C++ redo()
			for op_var in ops:
				var op: Dictionary = op_var
				var kind := String(op.get("kind", ""))
				if kind == "opt1_swap":
					var new_sid := _stamp_structure(
						String(op.get("vreg_path", "")),
						op.get("origin_chunk", Vector3.ZERO),
						0.0
					)
					op["structure_id"] = new_sid
					_reapply_opt1_visual_state_after_redo(String(op.get("statue", "")), new_sid)
					any_redone = true
				elif kind == "stamp_asset":
					var new_sid := _stamp_structure(
						String(op.get("vreg_path", "")),
						op.get("origin_chunk", Vector3.ZERO),
						float(op.get("recreate_delay", 0.0))
					)
					op["structure_id"] = new_sid
					any_redone = true
				elif kind == "add_terrain":
					terrain.add_terrain_with_material(
						op.get("point", Vector3.ZERO),
						float(op.get("radius", 4.0)),
						int(op.get("material", MATERIAL_SOIL))
					)
					any_redone = true
				elif kind == "boulder":
					terrain.place_boulder(op.get("point", Vector3.ZERO), float(op.get("radius", 4.0)))
					any_redone = true
				elif kind == "explosion":
					terrain.trigger_explosion(op.get("point", Vector3.ZERO), float(op.get("radius", 4.0)))
					any_redone = true
				elif kind == "elemental":
					terrain.trigger_elemental_sphere(
						op.get("point", Vector3.ZERO),
						float(op.get("radius", 4.0)),
						int(op.get("element", ELEMENTAL_WATER))
					)
					any_redone = true

		_undo_stack.append(action)
		_bench_last_summary_banner = "[Redo] %s (Undo: %d | Redo: %d)" % [
			String(action.get("label", "Edit")),
			_get_undo_depth_display(),
			_get_redo_depth_display(),
		]
		_bench_banner_timer = 3.5
		return any_redone

	if _has_native_undo_redo() and bool(terrain.call("can_redo")):
		var ok := bool(terrain.call("redo"))
		if ok:
			_bench_last_summary_banner = "[Redo] Re-applied terrain state (Undo: %d | Redo: %d)" % [
				_get_undo_depth_display(),
				_get_redo_depth_display(),
			]
			_bench_banner_timer = 3.5
		return ok

	print("[UndoRedo] Nothing to redo.")
	return false

func _perform_undo_all_and_reset() -> void:
	if _tool_pressed:
		_tool_pressed = false
		_end_undo_action()
	_last_boom_point = Vector3(INF, INF, INF)
	_clear_rolling_boulders()

	var steps_undone := 0
	if terrain != null and terrain.has_method("undo_all"):
		steps_undone = int(terrain.call("undo_all"))
	elif _has_native_undo_redo():
		while bool(terrain.call("can_undo")):
			if bool(terrain.call("undo")):
				steps_undone += 1
			else:
				break
	else:
		while not _undo_stack.is_empty():
			if _perform_undo():
				steps_undone += 1

	_restore_opt1_visual_state_after_undo("easter_island")
	_restore_opt1_visual_state_after_undo("atlas")
	_restore_opt1_visual_state_after_undo("ape")
	_reset_statues()
	_clear_undo_history_to_current_baseline()

	_bench_last_summary_banner = "[World Reset] Reverted %d edit(s) & restored all statues to pristine state" % steps_undone
	_bench_banner_timer = 4.0
	print("[UndoRedo] Reverted all %d edit(s) and restored pristine world baseline." % steps_undone)

## Guarantees every benchmark preset pass starts from an identical, un-carved
## terrain state (restoring the 3-hit Hard Rock boulder, Molten Lava dome, soft
## soil craters, Frost Ice bridges, and all stamped statues).
## Primary path: uses `terrain.undo_all()` + `terrain.clear_undo_history()` for
## instant, bit-exact chunk baseline restoration without re-allocating the engine.
## Fallback path (if running an older GDExtension binary without `undo_all`):
## removes any disk save files and cycles `terrain` out of and back into the
## SceneTree with `save_on_exit = false`.
func _reset_terrain_for_benchmark_pass() -> void:
	_active_waypoints.clear()
	_current_waypoint_idx = 0
	_has_pending_path_query = false
	_rebake_in_progress = false
	_clear_rolling_boulders()
	if _path_immediate_mesh != null:
		_path_immediate_mesh.clear_surfaces()
	if _goal_mesh_inst != null:
		_goal_mesh_inst.visible = false

	if terrain != null and terrain.has_method("undo_all"):
		var reverted: int = int(terrain.call("undo_all"))
		_restore_opt1_visual_state_after_undo("easter_island")
		_restore_opt1_visual_state_after_undo("atlas")
		_restore_opt1_visual_state_after_undo("ape")
		_reset_statues()
		_clear_undo_history_to_current_baseline()
		_last_focus_chunk = Vector3i(999999, 999999, 999999)
		_bench_last_reset_method = "undo_all (%d edits reverted)" % reverted
		print("[VoxelBenchmark] Reset terrain before pass via undo_all() (%d edit transactions reverted)." % reverted)
		return

	# Fallback for an older GDExtension binary built before undo_all() was added:
	var prev_save_on_exit: bool = terrain.get_save_on_exit() if terrain.has_method("get_save_on_exit") else true
	if terrain.has_method("set_save_on_exit"):
		terrain.set_save_on_exit(false)

	var save_dir_path: String = terrain.get_save_directory_path() if terrain.has_method("get_save_directory_path") else "/tmp/voxel_world_saves"
	var parent_node := terrain.get_parent()
	var terrain_idx := terrain.get_index()
	parent_node.remove_child(terrain)

	var dir := DirAccess.open(save_dir_path)
	if dir != null:
		dir.list_dir_begin()
		var fname := dir.get_next()
		while not fname.is_empty():
			if not dir.current_is_dir() and (fname.ends_with(".vreg") or fname.ends_with(".tmp")):
				dir.remove(fname)
			fname = dir.get_next()
		dir.list_dir_end()

	parent_node.add_child(terrain)
	parent_node.move_child(terrain, terrain_idx)
	if terrain.has_method("set_save_on_exit"):
		terrain.set_save_on_exit(prev_save_on_exit)

	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0
	_opt2_structure_id = 0
	_atlas_opt1_swapped_to_voxels = false
	_atlas_opt1_structure_id = 0
	_atlas_opt2_structure_id = 0
	_ape_opt1_swapped_to_voxels = false
	_ape_opt1_structure_id = 0
	_ape_opt2_structure_id = 0
	_setup_side_by_side_easter_island_statues()
	_setup_side_by_side_atlas_statues()
	_setup_side_by_side_ape_statues()
	_clear_undo_history_to_current_baseline()
	_last_focus_chunk = Vector3i(999999, 999999, 999999)
	_bench_last_reset_method = "engine_reinit_fallback"
	print("[VoxelBenchmark] Reset terrain before pass via engine_reinit_fallback.")


# ==============================================================================
# Trailer Capture API (used only by --trailer-shot)
# ==============================================================================

func _check_cli_trailer_autostart() -> void:
	var shot_id := _get_cli_arg("--trailer-shot=")
	if shot_id.is_empty():
		return
	var director_script := load("res://trailer/trailer_director.gd")
	if director_script == null:
		push_error("[Trailer] Could not load res://trailer/trailer_director.gd")
		return
	var director = director_script.new()
	director.name = "TrailerDirector"
	director.demo = self
	director.shot_id = shot_id
	add_child(director)

func trailer_hide_ui(hide: bool) -> void:
	var hud := get_node_or_null("HUD")
	if hud != null:
		hud.visible = not hide
	for label in find_children("*", "Label3D", true, false):
		label.visible = not hide
	Input.mouse_mode = Input.MOUSE_MODE_HIDDEN if hide else Input.MOUSE_MODE_CAPTURED

func _trailer_aim_camera(target: Vector3) -> void:
	var dir := target - camera.global_position
	if dir.length_squared() < 0.000001:
		return
	dir = dir.normalized()
	_yaw = atan2(-dir.x, -dir.z)
	_pitch = asin(clampf(dir.y, -1.0, 1.0))
	camera.rotation = Vector3(_pitch, _yaw, 0.0)

func trailer_set_fly_camera(pos: Vector3, look_target: Vector3, fov: float = -1.0) -> void:
	if _mode != Mode.FLY:
		_exit_play()
	camera.global_position = pos
	_trailer_aim_camera(look_target)
	if fov > 0.0:
		camera.fov = fov
	if _fly_pcam != null:
		_fly_pcam.global_transform = camera.global_transform
		_fly_pcam.priority = 1
	if _play_pcam != null:
		_play_pcam.priority = 0

func trailer_set_fov(fov: float) -> void:
	if fov > 0.0:
		camera.fov = fov

func trailer_enter_play_at(feet: Vector3, look_target: Vector3) -> void:
	if _mode == Mode.PLAY:
		_exit_play()
	camera.global_position = feet + Vector3(0.0, play_eye_height, 0.0)
	_trailer_aim_camera(look_target)
	_enter_play()
	_player_feet = feet
	_player_vel_y = 0.0
	_player_grounded = true
	_last_safe_feet = feet
	_spawn_pending = false
	camera.global_position = feet + Vector3(0.0, play_eye_height, 0.0)
	_play_pcam.global_position = camera.global_position
	_play_pcam.rotation = camera.rotation

func trailer_enter_play_on_surface(x: float, z: float, look_target: Vector3) -> void:
	var ground := terrain.get_surface_height(x, z)
	if not is_finite(ground):
		ground = 68.0
	trailer_enter_play_at(Vector3(x, ground, z), look_target)

func trailer_aim_at(target: Vector3) -> void:
	_trailer_aim_camera(target)
	if _mode == Mode.PLAY:
		_play_pcam.rotation = camera.rotation

func trailer_swing_pickaxe() -> bool:
	if _mode != Mode.PLAY:
		return false
	_start_pickaxe_swing()
	return true

func trailer_set_active_tool(tool: int) -> void:
	_active_tool = tool

func trailer_apply_tool(tool: int, point: Vector3) -> void:
	_active_tool = tool
	_last_boom_point = Vector3(INF, INF, INF)
	_apply_tool_at(point)

func trailer_add_material(point: Vector3, radius: float, material: int) -> void:
	terrain.add_terrain_with_material(point, radius, material)

func trailer_trigger_explosion(point: Vector3, radius: float) -> void:
	terrain.trigger_explosion(point, radius)

func trailer_trigger_elemental(point: Vector3, radius: float, element: int) -> void:
	terrain.trigger_elemental_sphere(point, radius, element)

func trailer_undo() -> bool:
	return bool(terrain.call("undo")) if terrain.has_method("undo") else false

func trailer_redo() -> bool:
	return bool(terrain.call("redo")) if terrain.has_method("redo") else false

func trailer_clear_history() -> void:
	_clear_undo_history_to_current_baseline()

func trailer_reset_baseline() -> void:
	if terrain.has_method("undo_all"):
		terrain.call("undo_all")
	_reset_statues()
	_clear_undo_history_to_current_baseline()

func trailer_history_counts() -> Dictionary:
	var undo_count := 0
	var redo_count := 0
	if terrain.has_method("get_undo_count"):
		undo_count = int(terrain.call("get_undo_count"))
	if terrain.has_method("get_redo_count"):
		redo_count = int(terrain.call("get_redo_count"))
	return {"undo": undo_count, "redo": redo_count}

func trailer_statue_center(statue_id: String) -> Vector3:
	match statue_id:
		"easter_opt1":
			return _opt1_world_center
		"easter_opt2":
			return _opt2_world_center
		"atlas_opt1":
			return _atlas_opt1_world_center
		"atlas_opt2":
			return _atlas_opt2_world_center
		"ape_opt1":
			return _ape_opt1_world_center
		"ape_opt2":
			return _ape_opt2_world_center
	return Vector3.ZERO

func trailer_place_companion(pos: Vector3) -> void:
	ai_companion.global_position = pos
	_companion_vertical_vel = 0.0
	_companion_initialized = true
	_active_waypoints.clear()
	_path_immediate_mesh.clear_surfaces()

func trailer_set_path_goal(goal: Vector3) -> void:
	_pending_path_goal = goal
	_goal_mesh_inst.global_position = goal
	_goal_mesh_inst.visible = true
	_has_pending_path_query = true

func trailer_path_count() -> int:
	return _active_waypoints.size()

func trailer_path_ready() -> bool:
	return _active_waypoints.size() > 1 and not _has_pending_path_query

func trailer_waypoints() -> PackedVector3Array:
	return _active_waypoints

func trailer_ground_point(x: float, z: float, y_offset: float = 0.0) -> Vector3:
	var ground := terrain.get_surface_height(x, z)
	if not is_finite(ground):
		ground = 68.0
	return Vector3(x, ground + y_offset, z)

func trailer_save_world() -> void:
	terrain.save_world()

# ==============================================================================
# Automated Multi-Setting & Hardware Benchmark Suite (Key M or F8 / --benchmark)
# ==============================================================================

func _check_cli_benchmark_autostart() -> void:
	var all_args := OS.get_cmdline_args() + OS.get_cmdline_user_args()
	for arg in all_args:
		if arg == "--benchmark-quit":
			_bench_quit_on_finish = true
		if arg == "--benchmark" or arg == "--benchmark-quit":
			call_deferred("_start_benchmark")
			return

func _toggle_benchmark_mode() -> void:
	if _mode == Mode.BENCHMARK:
		_stop_benchmark(true)
	else:
		_start_benchmark()

func _collect_hardware_info() -> Dictionary:
	var mem_info: Dictionary = OS.get_memory_info()
	var phys_bytes: int = int(mem_info.get("physical", 0))
	var free_bytes: int = int(mem_info.get("free", 0))
	var avail_bytes: int = int(mem_info.get("available", 0))
	var win_size: Vector2i = DisplayServer.window_get_size()
	var vsync_mode: int = DisplayServer.window_get_vsync_mode()
	var ver_info: Dictionary = Engine.get_version_info()
	var cpu_threads: int = OS.get_processor_count()
	var worker_threads: int = clampi(cpu_threads / 2, 2, 6)

	var adapter_type_code: int = RenderingServer.get_video_adapter_type()
	var adapter_type_str := "Other"
	match adapter_type_code:
		RenderingDevice.DEVICE_TYPE_DISCRETE_GPU:
			adapter_type_str = "Discrete GPU"
		RenderingDevice.DEVICE_TYPE_INTEGRATED_GPU:
			adapter_type_str = "Integrated GPU"
		RenderingDevice.DEVICE_TYPE_VIRTUAL_GPU:
			adapter_type_str = "Virtual GPU"
		RenderingDevice.DEVICE_TYPE_CPU:
			adapter_type_str = "CPU / Software"

	return {
		"timestamp_utc": Time.get_datetime_string_from_system(true, true),
		"timestamp_local": Time.get_datetime_string_from_system(false, true),
		"os_name": OS.get_name(),
		"os_distro": OS.get_distribution_name(),
		"os_version": OS.get_version(),
		"locale": OS.get_locale(),
		"cpu_name": OS.get_processor_name(),
		"cpu_arch": Engine.get_architecture_name(),
		"cpu_threads": cpu_threads,
		"voxel_worker_threads": worker_threads,
		"ram_physical_mb": roundf(float(phys_bytes) / (1024.0 * 1024.0)) if phys_bytes > 0 else 0.0,
		"ram_free_mb": roundf(float(free_bytes) / (1024.0 * 1024.0)) if free_bytes > 0 else 0.0,
		"ram_available_mb": roundf(float(avail_bytes) / (1024.0 * 1024.0)) if avail_bytes > 0 else 0.0,
		"gpu_name": RenderingServer.get_video_adapter_name(),
		"gpu_vendor": RenderingServer.get_video_adapter_vendor(),
		"gpu_type": adapter_type_str,
		"gpu_api_version": RenderingServer.get_video_adapter_api_version(),
		"rendering_method": RenderingServer.get_current_rendering_method(),
		"rendering_driver": RenderingServer.get_current_rendering_driver_name(),
		"window_resolution": "%dx%d" % [win_size.x, win_size.y],
		"initial_vsync_mode": vsync_mode,
		"godot_version": String(ver_info.get("string", "4.x")),
		"voxel_scale_m": terrain.voxel_scale,
		"world_height_chunks": terrain.get_world_height_chunks() if terrain.has_method("get_world_height_chunks") else 4,
	}

func _start_benchmark() -> void:
	if _mode == Mode.PLAY:
		_exit_play()

	_tool_pressed = false
	_bench_hardware_info = _collect_hardware_info()
	_bench_saved_cam_transform = camera.global_transform
	_bench_saved_yaw = _yaw
	_bench_saved_pitch = _pitch
	_bench_saved_vsync = DisplayServer.window_get_vsync_mode()
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)

	_bench_saved_settings = {
		"view_distance_chunks": terrain.view_distance_chunks,
		"prefetch_distance_chunks": terrain.prefetch_distance_chunks,
		"upload_budget_ms": terrain.upload_budget_ms,
		"resident_memory_budget_mb": terrain.resident_memory_budget_mb,
	}
	if "collision_distance_chunks" in terrain:
		_bench_saved_settings["collision_distance_chunks"] = terrain.get("collision_distance_chunks")

	_bench_results.clear()
	_bench_preset_idx = 0
	_bench_suite_started_utc = Time.get_datetime_string_from_system(true, true)
	_bench_suite_started_local = Time.get_datetime_string_from_system(false, true)
	_bench_suite_started_unix = Time.get_unix_time_from_system()
	_mode = Mode.BENCHMARK

	print("================================================================================")
	print("[VoxelBenchmark] STARTING AUTOMATED BENCHMARK SUITE (%d presets) at %s (UTC: %s)" % [
		BENCHMARK_PRESETS.size(),
		_bench_suite_started_local,
		_bench_suite_started_utc,
	])
	print("[VoxelBenchmark] CPU: %s (%d logical threads, %d voxel workers) | Arch: %s" % [
		_bench_hardware_info["cpu_name"],
		_bench_hardware_info["cpu_threads"],
		_bench_hardware_info["voxel_worker_threads"],
		_bench_hardware_info["cpu_arch"],
	])
	print("[VoxelBenchmark] GPU: %s (%s, %s, API %s, driver=%s/%s)" % [
		_bench_hardware_info["gpu_name"],
		_bench_hardware_info["gpu_vendor"],
		_bench_hardware_info["gpu_type"],
		_bench_hardware_info["gpu_api_version"],
		_bench_hardware_info["rendering_driver"],
		_bench_hardware_info["rendering_method"],
	])
	print("[VoxelBenchmark] OS: %s %s (%s) | System RAM: %.0f MiB total (%.0f MiB free) | Window: %s | voxel_scale=%.2fm" % [
		_bench_hardware_info["os_name"],
		_bench_hardware_info["os_distro"],
		_bench_hardware_info["os_version"],
		_bench_hardware_info["ram_physical_mb"],
		_bench_hardware_info["ram_free_mb"],
		_bench_hardware_info["window_resolution"],
		_bench_hardware_info["voxel_scale_m"],
	])
	print("================================================================================")

	_begin_benchmark_preset(0)

func _begin_benchmark_preset(idx: int) -> void:
	_bench_preset_idx = idx
	var preset: Dictionary = BENCHMARK_PRESETS[idx]

	_bench_preset_started_utc = Time.get_datetime_string_from_system(true, true)
	_bench_preset_started_local = Time.get_datetime_string_from_system(false, true)
	_bench_preset_started_unix = Time.get_unix_time_from_system()

	# Reset all voxel edits, craters, 3-hit Hard Rock damage, Lava->Obsidian quench,
	# Frost Ice bridges, and statues before each pass so every preset runs on
	# 100% identical pristine terrain.
	_reset_terrain_for_benchmark_pass()

	terrain.view_distance_chunks = int(preset["view_distance_chunks"])
	terrain.prefetch_distance_chunks = int(preset["prefetch_distance_chunks"])
	terrain.upload_budget_ms = float(preset["upload_budget_ms"])
	terrain.resident_memory_budget_mb = int(preset["resident_memory_budget_mb"])
	if "collision_distance_chunks" in terrain and preset.has("collision_distance_chunks"):
		terrain.set("collision_distance_chunks", int(preset["collision_distance_chunks"]))

	_bench_phase = BenchmarkPhase.SETTLE
	_bench_phase_elapsed = 0.0
	_bench_preset_elapsed = 0.0
	_bench_settle_time_ms = 0.0
	_bench_frame_times_ms.clear()
	_bench_phase_frame_times = {
		BenchmarkPhase.SETTLE: PackedFloat64Array(),
		BenchmarkPhase.STREAM_FLIGHT: PackedFloat64Array(),
		BenchmarkPhase.DESTRUCTION_CSG: PackedFloat64Array(),
		BenchmarkPhase.NAVIGATION_3D: PackedFloat64Array(),
	}
	_bench_commits_count = 0
	_bench_csg_step_idx = 0
	_bench_csg_next_fire_sec = 0.15
	_bench_pending_blast_start_usec.clear()
	_bench_blast_latencies_ms.clear()
	_bench_affected_chunks_total = 0
	_bench_nav_query_started = false
	_bench_nav_start_usec = 0
	_bench_nav_sync_ms = 0.0
	_bench_nav_query_ms = 0.0
	_bench_nav_waypoints = 0
	_bench_peak_vram_mb = 0.0
	_bench_peak_ram_mb = 0.0
	_bench_peak_draw_calls = 0
	_bench_peak_primitives = 0

	_set_benchmark_camera_pose(Vector3(0.0, 108.0, 38.0), Vector3(0.0, 88.0, -20.0))
	_velocity = Vector3.ZERO
	_update_stream_focus(true)

	print("[VoxelBenchmark] >>> Preset %d/%d: '%s' (view=%d, prefetch=%d, upload_budget=%.1fms, collision_dist=%s, mem=%dMB)" % [
		idx + 1,
		BENCHMARK_PRESETS.size(),
		String(preset["name"]),
		int(preset["view_distance_chunks"]),
		int(preset["prefetch_distance_chunks"]),
		float(preset["upload_budget_ms"]),
		str(preset.get("collision_distance_chunks", "N/A")),
		int(preset["resident_memory_budget_mb"]),
	])

func _set_benchmark_camera_pose(pos: Vector3, look_target: Vector3) -> void:
	camera.global_position = pos
	camera.look_at(look_target, Vector3.UP)
	var euler := camera.global_transform.basis.get_euler()
	_pitch = euler.x
	_yaw = euler.y
	if _fly_pcam != null:
		_fly_pcam.global_transform = camera.global_transform

func _process_benchmark(delta: float) -> void:
	var dt_ms := delta * 1000.0
	_bench_phase_elapsed += delta
	_bench_preset_elapsed += delta

	var phase_arr: PackedFloat64Array = _bench_phase_frame_times[_bench_phase]
	phase_arr.push_back(dt_ms)
	_bench_phase_frame_times[_bench_phase] = phase_arr
	if _bench_phase != BenchmarkPhase.SETTLE:
		_bench_frame_times_ms.push_back(dt_ms)

	var vram_mb := float(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED)) / (1024.0 * 1024.0)
	var ram_mb := float(Performance.get_monitor(Performance.MEMORY_STATIC)) / (1024.0 * 1024.0)
	var draw_calls := int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
	var primitives := int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))
	_bench_peak_vram_mb = maxf(_bench_peak_vram_mb, vram_mb)
	_bench_peak_ram_mb = maxf(_bench_peak_ram_mb, ram_mb)
	_bench_peak_draw_calls = maxi(_bench_peak_draw_calls, draw_calls)
	_bench_peak_primitives = maxi(_bench_peak_primitives, primitives)

	match _bench_phase:
		BenchmarkPhase.SETTLE:
			if (terrain.fully_committed() and _bench_phase_elapsed >= 0.35) or _bench_phase_elapsed >= BENCHMARK_SETTLE_MAX_SEC:
				_bench_settle_time_ms = _bench_phase_elapsed * 1000.0
				_bench_phase = BenchmarkPhase.STREAM_FLIGHT
				_bench_phase_elapsed = 0.0

		BenchmarkPhase.STREAM_FLIGHT:
			var u := clampf(_bench_phase_elapsed / BENCHMARK_FLIGHT_SEC, 0.0, 1.0)
			# Sweeping S-curve flight down the statue corridor (z = +38 -> -148)
			var z_pos := lerpf(38.0, -148.0, u)
			var x_pos := sin(u * PI * 2.0) * 34.0
			var y_pos := 110.0 + cos(u * PI * 2.0) * 8.0
			var next_pos := Vector3(x_pos, y_pos, z_pos)
			_velocity = (next_pos - camera.global_position) / maxf(delta, 0.001)
			var look_ahead := Vector3(sin(minf(1.0, u + 0.08) * PI * 2.0) * 18.0, 90.0, lerpf(10.0, -165.0, u))
			_set_benchmark_camera_pose(next_pos, look_ahead)

			if _bench_phase_elapsed >= BENCHMARK_FLIGHT_SEC:
				_bench_phase = BenchmarkPhase.DESTRUCTION_CSG
				_bench_phase_elapsed = 0.0
				_bench_csg_step_idx = 0
				_bench_csg_next_fire_sec = 0.10
				_velocity = Vector3.ZERO
				_set_benchmark_camera_pose(Vector3(0.0, 96.0, 34.0), Vector3(16.0, 74.0, 12.0))
				_update_stream_focus(true)

		BenchmarkPhase.DESTRUCTION_CSG:
			# Orbit gently around the blast arena while firing deterministic CSG & elemental spheres
			var angle := _bench_phase_elapsed * 0.45
			var cam_pos := Vector3(sin(angle) * 26.0, 96.0, 28.0 + cos(angle) * 8.0)
			_set_benchmark_camera_pose(cam_pos, Vector3(16.0, 72.0, 12.0))

			if _bench_phase_elapsed >= _bench_csg_next_fire_sec and _bench_csg_step_idx < 8:
				_execute_benchmark_csg_step(_bench_csg_step_idx)
				_bench_csg_step_idx += 1
				_bench_csg_next_fire_sec += 0.42

			if _bench_phase_elapsed >= BENCHMARK_DESTRUCTION_SEC:
				_bench_phase = BenchmarkPhase.NAVIGATION_3D
				_bench_phase_elapsed = 0.0
				_bench_nav_query_started = false

		BenchmarkPhase.NAVIGATION_3D:
			_set_benchmark_camera_pose(Vector3(0.0, 102.0, 36.0), Vector3(16.0, 70.0, 16.0))
			if not _bench_nav_query_started and _bench_pending_blast_start_usec.is_empty():
				_bench_nav_query_started = true
				_bench_nav_start_usec = Time.get_ticks_usec()
				var start_y := terrain.get_surface_height(4.0, 20.0) + 0.25
				var goal_y := terrain.get_surface_height(28.0, 20.0) + 0.25
				ai_companion.global_position = Vector3(4.0, start_y, 20.0)
				_companion_initialized = true
				_pending_path_goal = Vector3(28.0, goal_y, 20.0)
				_goal_mesh_inst.global_position = _pending_path_goal
				_goal_mesh_inst.visible = true
				# Prime on-demand corridor groups immediately without waiting for distant horizon chunks
				var _s_ok := ai_companion.is_path_region_synced(ai_companion.global_position)
				var _g_ok := ai_companion.is_path_region_synced(_pending_path_goal)

			elif _bench_nav_query_started and _bench_nav_waypoints == 0:
				var start_pos := ai_companion.global_position
				if ai_companion.is_path_region_synced(start_pos) and ai_companion.is_path_region_synced(_pending_path_goal):
					_bench_nav_sync_ms = float(Time.get_ticks_usec() - _bench_nav_start_usec) / 1000.0
					var q0 := Time.get_ticks_usec()
					var raw_path: PackedVector3Array = ai_companion.get_voxel_path(start_pos, _pending_path_goal)
					_bench_nav_query_ms = float(Time.get_ticks_usec() - q0) / 1000.0
					_bench_nav_waypoints = maxi(1, raw_path.size())
					var draped := _drape_path_onto_terrain(raw_path)
					_active_waypoints = draped
					_current_waypoint_idx = 0
					_draw_path_ribbon(draped)

			var nav_done := (_bench_nav_waypoints > 0 and _bench_phase_elapsed >= BENCHMARK_NAV_SEC) \
				or (_bench_phase_elapsed >= BENCHMARK_NAV_MAX_SEC)
			if nav_done:
				_finalize_current_benchmark_preset()
				if _bench_preset_idx + 1 < BENCHMARK_PRESETS.size():
					_begin_benchmark_preset(_bench_preset_idx + 1)
				else:
					_stop_benchmark(false)

func _execute_benchmark_csg_step(step: int) -> void:
	var surf_y := terrain.get_surface_height(16.0, 16.0)
	_bench_pending_blast_start_usec.push_back(Time.get_ticks_usec())
	match step:
		0:
			# Small precision chip
			terrain.trigger_explosion(Vector3(12.0, surf_y, 16.0), small_hit_radius)
		1:
			# Standard crater blast on soft terrain
			terrain.trigger_explosion(Vector3(8.0, surf_y, 14.0), explosion_radius)
		2:
			# Hit 1 on 3-hit Hard Rock boulder at (16, 70, 24)
			terrain.trigger_explosion(Vector3(16.0, 70.0, 24.0), explosion_radius)
		3:
			# Hit 2 on 3-hit Hard Rock boulder
			terrain.trigger_explosion(Vector3(16.0, 70.0, 24.0), explosion_radius)
		4:
			# Hit 3 on 3-hit Hard Rock boulder (shatters core)
			terrain.trigger_explosion(Vector3(16.0, 70.0, 24.0), explosion_radius)
		5:
			# Water Quench on Molten Lava dome at (30, 69, 24) -> 3-hit Obsidian
			terrain.trigger_elemental_sphere(Vector3(30.0, 69.0, 24.0), elemental_radius, ELEMENTAL_WATER)
		6:
			# Frost / Ice Bridge across crater
			terrain.trigger_elemental_sphere(Vector3(12.0, surf_y + 1.0, 16.0), elemental_radius, ELEMENTAL_FROST)
		7:
			# Large explosion stress test
			terrain.trigger_explosion(Vector3(0.0, surf_y, 18.0), large_explosion_radius)

func _compute_frame_stats(samples_ms: PackedFloat64Array) -> Dictionary:
	if samples_ms.is_empty():
		return {
			"frames": 0,
			"avg_ms": 0.0,
			"min_ms": 0.0,
			"p50_ms": 0.0,
			"p95_ms": 0.0,
			"p99_ms": 0.0,
			"max_ms": 0.0,
			"avg_fps": 0.0,
			"min_fps": 0.0,
			"p1_low_fps": 0.0,
		}
	var sorted: Array[float] = []
	sorted.resize(samples_ms.size())
	var sum_ms := 0.0
	for i in range(samples_ms.size()):
		var v := float(samples_ms[i])
		sorted[i] = v
		sum_ms += v
	sorted.sort()
	var n := sorted.size()
	var avg_ms := sum_ms / float(n)
	var min_ms := sorted[0]
	var p50_ms := sorted[clampi(int(floor(float(n - 1) * 0.50)), 0, n - 1)]
	var p95_ms := sorted[clampi(int(floor(float(n - 1) * 0.95)), 0, n - 1)]
	var p99_ms := sorted[clampi(int(floor(float(n - 1) * 0.99)), 0, n - 1)]
	var max_ms := sorted[n - 1]
	return {
		"frames": n,
		"avg_ms": snappedf(avg_ms, 0.01),
		"min_ms": snappedf(min_ms, 0.01),
		"p50_ms": snappedf(p50_ms, 0.01),
		"p95_ms": snappedf(p95_ms, 0.01),
		"p99_ms": snappedf(p99_ms, 0.01),
		"max_ms": snappedf(max_ms, 0.01),
		"avg_fps": snappedf(1000.0 / maxf(avg_ms, 0.001), 0.1),
		"min_fps": snappedf(1000.0 / maxf(max_ms, 0.001), 0.1),
		"p1_low_fps": snappedf(1000.0 / maxf(p99_ms, 0.001), 0.1),
	}

func _finalize_current_benchmark_preset() -> void:
	var preset: Dictionary = BENCHMARK_PRESETS[_bench_preset_idx]
	var overall := _compute_frame_stats(_bench_frame_times_ms)
	var flight_stats := _compute_frame_stats(_bench_phase_frame_times[BenchmarkPhase.STREAM_FLIGHT])
	var csg_stats := _compute_frame_stats(_bench_phase_frame_times[BenchmarkPhase.DESTRUCTION_CSG])
	var nav_stats := _compute_frame_stats(_bench_phase_frame_times[BenchmarkPhase.NAVIGATION_3D])

	var avg_blast_ms := 0.0
	var max_blast_ms := 0.0
	if not _bench_blast_latencies_ms.is_empty():
		var sum_b := 0.0
		for b in _bench_blast_latencies_ms:
			sum_b += b
			max_blast_ms = maxf(max_blast_ms, b)
		avg_blast_ms = sum_b / float(_bench_blast_latencies_ms.size())

	var completed_utc := Time.get_datetime_string_from_system(true, true)
	var completed_local := Time.get_datetime_string_from_system(false, true)
	var completed_unix := Time.get_unix_time_from_system()
	var active_sec := maxf(0.001, _bench_preset_elapsed - (_bench_settle_time_ms / 1000.0))
	var record := {
		"preset_name": String(preset["name"]),
		"timestamps": {
			"started_at_local": _bench_preset_started_local,
			"started_at_utc": _bench_preset_started_utc,
			"completed_at_local": completed_local,
			"completed_at_utc": completed_utc,
			"duration_sec": snappedf(completed_unix - _bench_preset_started_unix, 0.01),
			"terrain_reset_before_pass": true,
			"terrain_reset_method": _bench_last_reset_method,
		},
		"settings": {
			"view_distance_chunks": terrain.view_distance_chunks,
			"effective_view_distance_chunks": terrain.get_effective_view_distance_chunks(),
			"prefetch_distance_chunks": terrain.prefetch_distance_chunks,
			"upload_budget_ms": snappedf(terrain.upload_budget_ms, 0.1),
			"collision_distance_chunks": terrain.get("collision_distance_chunks") if "collision_distance_chunks" in terrain else -1,
			"resident_memory_budget_mb": terrain.resident_memory_budget_mb,
			"voxel_scale_m": snappedf(terrain.voxel_scale, 0.01),
		},
		"overall": overall,
		"phases": {
			"stream_flight": flight_stats,
			"destruction_csg": csg_stats,
			"navigation_3d": nav_stats,
		},
		"streaming": {
			"settle_time_ms": snappedf(_bench_settle_time_ms, 0.1),
			"chunks_committed": _bench_commits_count,
			"commits_per_sec": snappedf(float(_bench_commits_count) / active_sec, 0.1),
		},
		"destruction": {
			"blasts_completed": _bench_blast_latencies_ms.size(),
			"avg_blast_latency_ms": snappedf(avg_blast_ms, 0.01),
			"max_blast_latency_ms": snappedf(max_blast_ms, 0.01),
			"affected_chunks_total": _bench_affected_chunks_total,
		},
		"navigation": {
			"nav_sync_ms": snappedf(_bench_nav_sync_ms, 0.01),
			"nav_query_ms": snappedf(_bench_nav_query_ms, 0.001),
			"nav_waypoints": _bench_nav_waypoints,
		},
		"resources": {
			"peak_vram_mb": snappedf(_bench_peak_vram_mb, 0.1),
			"peak_static_ram_mb": snappedf(_bench_peak_ram_mb, 0.1),
			"peak_draw_calls": _bench_peak_draw_calls,
			"peak_primitives": _bench_peak_primitives,
		},
	}
	_bench_results.append(record)
	print("[VoxelBenchmark] Completed '%s': Avg %.1f FPS (1%% Low %.1f FPS, p99 %.2f ms) | Commits %.1f/s | Blast Avg %.2f ms | Nav Sync %.1f ms | VRAM %.1f MiB" % [
		record["preset_name"],
		overall["avg_fps"],
		overall["p1_low_fps"],
		overall["p99_ms"],
		record["streaming"]["commits_per_sec"],
		record["destruction"]["avg_blast_latency_ms"],
		record["navigation"]["nav_sync_ms"],
		record["resources"]["peak_vram_mb"],
	])

func _stop_benchmark(aborted: bool) -> void:
	if _mode != Mode.BENCHMARK:
		return
	if aborted and not _bench_frame_times_ms.is_empty():
		_finalize_current_benchmark_preset()

	# Restore pre-benchmark settings, VSync, statues, and camera pose
	if not _bench_saved_settings.is_empty():
		terrain.view_distance_chunks = int(_bench_saved_settings["view_distance_chunks"])
		terrain.prefetch_distance_chunks = int(_bench_saved_settings["prefetch_distance_chunks"])
		terrain.upload_budget_ms = float(_bench_saved_settings["upload_budget_ms"])
		terrain.resident_memory_budget_mb = int(_bench_saved_settings["resident_memory_budget_mb"])
		if "collision_distance_chunks" in terrain and _bench_saved_settings.has("collision_distance_chunks"):
			terrain.set("collision_distance_chunks", int(_bench_saved_settings["collision_distance_chunks"]))

	# Revert all benchmark pass CSG edits & restore statues before returning to interactive Fly mode
	_reset_terrain_for_benchmark_pass()

	DisplayServer.window_set_vsync_mode(_bench_saved_vsync)
	_mode = Mode.FLY
	_velocity = Vector3.ZERO
	camera.global_transform = _bench_saved_cam_transform
	_yaw = _bench_saved_yaw
	_pitch = _bench_saved_pitch
	if _fly_pcam != null:
		_fly_pcam.global_transform = camera.global_transform
	_update_stream_focus(true)

	if not _bench_results.is_empty():
		_write_and_print_benchmark_report(aborted)

	if _bench_quit_on_finish:
		get_tree().quit()

func _write_and_print_benchmark_report(aborted: bool) -> void:
	var completed_utc := Time.get_datetime_string_from_system(true, true)
	var completed_local := Time.get_datetime_string_from_system(false, true)
	var completed_unix := Time.get_unix_time_from_system()
	var total_duration_sec := snappedf(maxf(0.0, completed_unix - _bench_suite_started_unix), 0.01)

	_bench_hardware_info["started_at_local"] = _bench_suite_started_local
	_bench_hardware_info["started_at_utc"] = _bench_suite_started_utc
	_bench_hardware_info["completed_at_local"] = completed_local
	_bench_hardware_info["completed_at_utc"] = completed_utc
	_bench_hardware_info["total_duration_sec"] = total_duration_sec

	var file_stamp := completed_local.replace(":", "-").replace(" ", "_").replace("T", "_")
	var ts_md_user := "user://voxel_benchmark_report_%s.md" % file_stamp
	var ts_json_user := "user://voxel_benchmark_results_%s.json" % file_stamp

	var md := _build_benchmark_markdown_report(aborted)
	var payload := {
		"status": "aborted" if aborted else "completed",
		"timestamp_local": completed_local,
		"timestamp_utc": completed_utc,
		"started_at_local": _bench_suite_started_local,
		"started_at_utc": _bench_suite_started_utc,
		"completed_at_local": completed_local,
		"completed_at_utc": completed_utc,
		"started_unix_sec": _bench_suite_started_unix,
		"completed_unix_sec": completed_unix,
		"total_duration_sec": total_duration_sec,
		"terrain_reset_between_passes": true,
		"terrain_reset_method": _bench_last_reset_method,
		"hardware": _bench_hardware_info,
		"presets": _bench_results,
	}
	var json_text := JSON.stringify(payload, "\t")

	var saved_paths: Array[String] = []
	for path in [BENCHMARK_REPORT_MD_USER, BENCHMARK_REPORT_MD_RES, ts_md_user]:
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f != null:
			f.store_string(md)
			f.close()
			saved_paths.append(ProjectSettings.globalize_path(path))

	for path in [BENCHMARK_REPORT_JSON_USER, BENCHMARK_REPORT_JSON_RES, ts_json_user]:
		var jf := FileAccess.open(path, FileAccess.WRITE)
		if jf != null:
			jf.store_string(json_text)
			jf.close()
			saved_paths.append(ProjectSettings.globalize_path(path))

	print("\n" + md)
	for sp in saved_paths:
		print("[VoxelBenchmark] Saved benchmark log -> ", sp)

	var best_fps := 0.0
	var best_name := ""
	for r in _bench_results:
		var fps := float(r["overall"]["avg_fps"])
		if fps > best_fps:
			best_fps = fps
			best_name = String(r["preset_name"])
	_bench_last_summary_banner = "[Benchmark Saved %s] Best: %s (%.1f FPS) -> %s" % [
		completed_local, best_name, best_fps, ts_md_user
	]
	_bench_banner_timer = 10.0

func _build_benchmark_markdown_report(aborted: bool) -> String:
	var hw := _bench_hardware_info
	var lines := PackedStringArray()
	lines.append("# VoxelTerrain3D Hardware & Multi-Setting Benchmark Report")
	lines.append("")
	lines.append("- **Status**: %s" % ("Aborted Early" if aborted else "Completed Full Suite"))
	lines.append("- **Started (Local / UTC)**: `%s` / `%s`" % [
		hw.get("started_at_local", hw.get("timestamp_local", "")),
		hw.get("started_at_utc", hw.get("timestamp_utc", "")),
	])
	lines.append("- **Completed (Local / UTC)**: `%s` / `%s` (Total Duration: `%.2f s`)" % [
		hw.get("completed_at_local", hw.get("timestamp_local", "")),
		hw.get("completed_at_utc", hw.get("timestamp_utc", "")),
		float(hw.get("total_duration_sec", 0.0)),
	])
	lines.append("- **Pre-Pass Terrain Reset**: `Enabled (%s)` — pristine terrain & statues restored before every pass" % _bench_last_reset_method)
	lines.append("- **CPU**: %s (`%s`, %d logical threads, %d voxel worker threads)" % [
		hw.get("cpu_name", "Unknown"),
		hw.get("cpu_arch", ""),
		int(hw.get("cpu_threads", 0)),
		int(hw.get("voxel_worker_threads", 0)),
	])
	lines.append("- **GPU**: %s — %s (%s, API `%s`, driver `%s` / `%s`)" % [
		hw.get("gpu_name", "Unknown"),
		hw.get("gpu_vendor", ""),
		hw.get("gpu_type", ""),
		hw.get("gpu_api_version", ""),
		hw.get("rendering_driver", ""),
		hw.get("rendering_method", ""),
	])
	lines.append("- **System RAM**: %.0f MiB physical total (%.0f MiB physical free | %.0f MiB OS commit available)" % [
		float(hw.get("ram_physical_mb", 0.0)),
		float(hw.get("ram_free_mb", 0.0)),
		float(hw.get("ram_available_mb", 0.0)),
	])
	lines.append("- **OS & Engine**: %s %s (%s) | Godot `%s`" % [
		hw.get("os_name", ""),
		hw.get("os_distro", ""),
		hw.get("os_version", ""),
		hw.get("godot_version", ""),
	])
	lines.append("- **Viewport & Voxel Config**: `%s` | `voxel_scale = %.2f m` | `world_height_chunks = %d`" % [
		hw.get("window_resolution", ""),
		float(hw.get("voxel_scale_m", 1.0)),
		int(hw.get("world_height_chunks", 4)),
	])
	lines.append("")
	lines.append("## Summary Across Settings Presets")
	lines.append("")
	lines.append("| Preset | Completed (Local) | View / Prefetch | Upload Budget | Avg FPS | 1% Low FPS | Avg / p95 / p99 / Max (ms) | Flight FPS | CSG FPS | Settle (ms) | Commits/s | Avg Blast (ms) | Nav Sync / Query (ms) | Peak VRAM (MiB) | Peak Draw Calls |")
	lines.append("| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

	for r in _bench_results:
		var ts: Dictionary = r.get("timestamps", {})
		var s: Dictionary = r["settings"]
		var o: Dictionary = r["overall"]
		var fl: Dictionary = r["phases"]["stream_flight"]
		var cs: Dictionary = r["phases"]["destruction_csg"]
		var st: Dictionary = r["streaming"]
		var ds: Dictionary = r["destruction"]
		var nv: Dictionary = r["navigation"]
		var rs: Dictionary = r["resources"]
		lines.append("| **%s** | `%s` | `%d / %d` | `%.1f ms` | **%.1f** | %.1f | `%.2f / %.2f / %.2f / %.2f` | %.1f | %.1f | %.0f | %.1f | %.2f | `%.1f / %.3f` | %.1f | %d |" % [
			String(r["preset_name"]),
			String(ts.get("completed_at_local", "")),
			int(s["view_distance_chunks"]),
			int(s["prefetch_distance_chunks"]),
			float(s["upload_budget_ms"]),
			float(o["avg_fps"]),
			float(o["p1_low_fps"]),
			float(o["avg_ms"]),
			float(o["p95_ms"]),
			float(o["p99_ms"]),
			float(o["max_ms"]),
			float(fl["avg_fps"]),
			float(cs["avg_fps"]),
			float(st["settle_time_ms"]),
			float(st["commits_per_sec"]),
			float(ds["avg_blast_latency_ms"]),
			float(nv["nav_sync_ms"]),
			float(nv["nav_query_ms"]),
			float(rs["peak_vram_mb"]),
			int(rs["peak_draw_calls"]),
		])

	lines.append("")
	return "\n".join(lines)
