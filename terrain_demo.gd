extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D and applies terrain tools on
## left-click. Tools progress from construction to destruction to elements:
##   Keys 1-4: Add terrain / Paint terrain / Paint delete / Place boulder
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
## H hides / shows the grouped controls overlay.
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

## First-person Play mode (F toggles Fly <-> Play). Play drops an eye-height body
## onto the voxel surface below the camera: WASD runs, Shift sprints, Space jumps,
## and LMB swings the rigged pickaxe arms before chipping the terrain at the crosshair.
@export var play_eye_height: float = 1.6
@export var play_walk_speed: float = 5.0
@export var play_run_speed: float = 9.0
@export var play_jump_speed: float = 6.0
@export var play_gravity: float = 24.0
@export var play_step_height: float = 0.6
@export var play_body_radius: float = 0.4
## Radius of the pickaxe chip carved on the swing's impact frame.
@export var pickaxe_hit_radius: float = 1.5
## Playback multiplier applied to the authored 80-frame PickaxeSwing_R action.
@export var pickaxe_swing_speed: float = 3.0
## Point in PickaxeSwing_R (0-1 of its length) where the tool bites the terrain.
@export var pickaxe_impact_fraction: float = 0.42
const ARMS_SCENE_PATH := "res://models/arms/fp_arms.glb"

## Tool palette in keyboard order: construction (1-4), destruction (5-7),
## elemental reactions (8-0). `TOOL_KEYS` is the keycap shown in the HUD.
enum Tool { ADD_TERRAIN, PAINT, PAINT_DELETE, BOULDER, SMALL_HIT, EXPLODE, LARGE_EXPLODE, WATER_QUENCH, FROST_ICE, LAVA_MAGMA }

## Fly is the original free camera; Play is the first-person pickaxe body.
enum Mode { FLY, PLAY }

const TOOL_NAMES := ["Add terrain", "Paint terrain", "Paint delete", "Place boulder", "Small hit", "Explode", "Large explode", "Water quench", "Frost / Ice", "Lava / Magma"]
const TOOL_KEYS := ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"]
const CONTROLS_TEXT := """CONTROLS  (H to hide)
Mode    F fly / play   Play: WASD run   Shift sprint   Space jump
Move    W/A/S/D   Space or E up   Ctrl or Q down   Shift sprint
Look    Mouse   Esc releases cursor   click recaptures
Build   1 Add terrain   2 Paint   3 Paint delete   4 Boulder
Carve   5 Small hit     6 Explode   7 Large explode
Element 8 Water   9 Frost/Ice   0 Lava/Magma
Stamps  B timed ruin   Shift+B permanent ruin
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
var _player_feet: Vector3 = Vector3.ZERO
var _player_vel_y: float = 0.0
var _player_grounded: bool = false
var _arms_root: Node3D = null
var _arms_player: AnimationPlayer = null
var _arms_current_anim: String = ""
var _swing_active: bool = false
var _swing_hit_done: bool = false
var _swing_elapsed: float = 0.0
var _swing_impact_time: float = 0.0
var _last_focus_chunk: Vector3i = Vector3i(999999, 999999, 999999)
var _committed_chunk_count: int = 0
var _fps_smoothed: float = 0.0
var _commits_in_window: int = 0
var _commits_per_second: float = 0.0
var _stats_accum: float = 0.0
var _active_tool: int = Tool.EXPLODE
var _controls_visible: bool = true
var _add_material: int = MATERIAL_SOIL
var _tool_pressed: bool = false
var _brush_cooldown: float = 0.0
var _last_brush_point: Vector3 = Vector3(INF, INF, INF)
var _last_boom_point: Vector3 = Vector3(INF, INF, INF)
var _last_event_print_ms: int = 0
var _last_fail_warn_ms: int = 0
var _look_at_name: String = "Sky"
var _look_at_accum: float = 0.0

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

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.terrain_add_requested.connect(_on_terrain_add_requested)
	terrain.terrain_add_completed.connect(_on_terrain_add_completed)
	terrain.terrain_add_failed.connect(_on_terrain_add_failed)
	terrain.chunk_committed.connect(_on_chunk_committed)
	terrain.structure_damaged.connect(_on_structure_damaged)
	terrain.structure_recreated.connect(_on_structure_recreated)
	if terrain.has_signal("structure_removed"):
		terrain.connect("structure_removed", Callable(self, "_on_structure_removed"))

	_setup_path_debug_visuals()
	controls_label.text = CONTROLS_TEXT
	controls_label.visible = _controls_visible
	_setup_side_by_side_easter_island_statues()
	_setup_side_by_side_atlas_statues()
	_setup_side_by_side_ape_statues()
	_setup_first_person_arms()
	_update_stream_focus(true)

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

func _setup_first_person_arms() -> void:
	var packed := load(ARMS_SCENE_PATH) as PackedScene
	if packed == null:
		push_warning("[FirstPerson] Missing arms scene: %s" % ARMS_SCENE_PATH)
		return
	_arms_root = packed.instantiate() as Node3D
	if _arms_root == null:
		push_warning("[FirstPerson] Arms scene has no Node3D root: %s" % ARMS_SCENE_PATH)
		return
	_arms_root.name = "FirstPersonArms"
	camera.add_child(_arms_root)
	# The rig origin is at the feet, 0.15 m behind the authored ArmCam eye.
	_arms_root.position = Vector3(0.0, -play_eye_height, 0.15)
	_arms_root.rotation = Vector3(0.0, PI, 0.0)
	_arms_root.visible = false
	_arms_player = _arms_root.find_child("AnimationPlayer", true, false) as AnimationPlayer
	if _arms_player == null:
		push_warning("[FirstPerson] No AnimationPlayer in %s" % ARMS_SCENE_PATH)
		return
	for anim_name in ["Walk_R", "Run_R"]:
		var anim := _arms_player.get_animation(anim_name)
		if anim:
			anim.loop_mode = Animation.LOOP_LINEAR
	_arms_player.animation_finished.connect(_on_arms_animation_finished)

func _toggle_mode() -> void:
	if _mode == Mode.FLY:
		_enter_play()
	else:
		_exit_play()

func _enter_play() -> void:
	_mode = Mode.PLAY
	_tool_pressed = false
	_swing_active = false
	var eye := camera.global_position
	var ground_y := _ground_height_below(eye + Vector3(0.0, play_step_height, 0.0), 400.0)
	if not is_finite(ground_y):
		ground_y = eye.y - play_eye_height
	_player_feet = Vector3(eye.x, ground_y, eye.z)
	_player_vel_y = 0.0
	_player_grounded = true
	camera.global_position = _player_feet + Vector3(0.0, play_eye_height, 0.0)
	if _arms_root:
		_arms_root.visible = true
	_play_arms_idle()
	print("[FirstPerson] Play mode (F to fly)")

func _exit_play() -> void:
	_mode = Mode.FLY
	_swing_active = false
	_player_vel_y = 0.0
	_player_grounded = false
	_velocity = Vector3.ZERO
	if _arms_player:
		_arms_player.stop()
	if _arms_root:
		_arms_root.visible = false
	print("[FirstPerson] Fly mode (F to play)")

func _play_arms_idle() -> void:
	if _arms_player == null:
		return
	_arms_current_anim = "Walk_R"
	_arms_player.speed_scale = 1.0
	_arms_player.play("Walk_R")
	_arms_player.seek(0.0, true)
	_arms_player.pause()

func _update_arms_locomotion(speed: float) -> void:
	if _arms_player == null or _swing_active:
		return
	if speed < 0.05:
		if _arms_player.is_playing():
			_play_arms_idle()
		return
	var want := "Run_R" if speed > play_walk_speed + 0.01 else "Walk_R"
	if _arms_current_anim != want or not _arms_player.is_playing():
		_arms_current_anim = want
		_arms_player.speed_scale = 1.0
		_arms_player.play(want)

func _start_pickaxe_swing() -> void:
	if _arms_player == null or _swing_active:
		return
	_swing_active = true
	_swing_hit_done = false
	_swing_elapsed = 0.0
	_arms_current_anim = "PickaxeSwing_R"
	var swing := _arms_player.get_animation("PickaxeSwing_R")
	_swing_impact_time = swing.length * pickaxe_impact_fraction if swing else 0.4
	_arms_player.speed_scale = pickaxe_swing_speed
	_arms_player.play("PickaxeSwing_R")

func _update_pickaxe_swing(delta: float) -> void:
	if not _swing_active:
		return
	_swing_elapsed += delta * pickaxe_swing_speed
	if not _swing_hit_done and _swing_elapsed >= _swing_impact_time:
		_swing_hit_done = true
		_apply_pickaxe_hit()

func _on_arms_animation_finished(anim_name: StringName) -> void:
	if anim_name != "PickaxeSwing_R" or _arms_player == null:
		return
	_swing_active = false
	_swing_hit_done = false
	_arms_player.speed_scale = 1.0
	_arms_current_anim = ""
	if _mode == Mode.PLAY:
		_update_arms_locomotion(_velocity.length())

func _apply_pickaxe_hit() -> void:
	var point := _pickaxe_aim_point()
	if not is_finite(point.x):
		return
	var hit := _raycast_crosshair()
	var hit_collider: Object = hit.get("collider") if not hit.is_empty() else null
	if _companion_initialized and _goal_mesh_inst.visible:
		_rebake_in_progress = true
	_check_and_swap_opt1_easter_island_on_blast(point, pickaxe_hit_radius, hit_collider)
	_check_and_swap_atlas_opt1_on_blast(point, pickaxe_hit_radius, hit_collider)
	_check_and_swap_ape_opt1_on_blast(point, pickaxe_hit_radius, hit_collider)
	terrain.trigger_explosion(point, pickaxe_hit_radius)

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

func _process_play(delta: float) -> void:
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

	var speed := play_run_speed if Input.is_key_pressed(KEY_SHIFT) else play_walk_speed
	var wish := input_dir.normalized() * speed if input_dir.length_squared() > 0.0 else Vector3.ZERO
	_velocity = wish
	_move_player_horizontal(wish * delta)

	_player_vel_y -= play_gravity * delta
	_player_feet.y += _player_vel_y * delta
	var ground_y := _ground_height_below(_player_feet + Vector3(0.0, play_step_height, 0.0), play_step_height + 5.0)
	if is_finite(ground_y):
		var snap_to_ground := _player_grounded and _player_vel_y <= 0.0 \
			and _player_feet.y - ground_y <= 0.35
		if _player_feet.y <= ground_y or snap_to_ground:
			_player_feet.y = ground_y
			_player_vel_y = 0.0
			_player_grounded = true
		else:
			_player_grounded = false
	else:
		_player_grounded = false

	camera.global_position = _player_feet + Vector3(0.0, play_eye_height, 0.0)
	camera.rotation = Vector3(_pitch, _yaw, 0.0)
	_update_pickaxe_swing(delta)
	_update_arms_locomotion(wish.length())

func _move_player_horizontal(motion: Vector3) -> void:
	if motion.length_squared() < 0.000001:
		return
	if not _player_path_blocked(motion):
		_player_feet += motion
		return
	var slide_x := Vector3(motion.x, 0.0, 0.0)
	var slide_z := Vector3(0.0, 0.0, motion.z)
	if absf(slide_x.x) > 0.0001 and not _player_path_blocked(slide_x):
		_player_feet += slide_x
	if absf(slide_z.z) > 0.0001 and not _player_path_blocked(slide_z):
		_player_feet += slide_z

## Voxel terrain has no physics collider, so walls are found with the engine's
## voxel raycast; pristine statue colliders come from the physics space. Rays sit
## just above the climbable step height so 0.5 m voxel steps do not block walking.
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

func _try_jump() -> void:
	if _mode != Mode.PLAY or not _player_grounded:
		return
	_player_vel_y = play_jump_speed
	_player_grounded = false

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch = clamp(_pitch - event.relative.y * mouse_sensitivity, -1.45, 1.45)
		camera.rotation = Vector3(_pitch, _yaw, 0.0)

	elif event is InputEventKey and event.pressed:
		if _mode == Mode.PLAY and event.keycode in [KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_B]:
			return
		match event.keycode:
			KEY_ESCAPE:
				if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
					_tool_pressed = false
					Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
				else:
					Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			KEY_F:
				_toggle_mode()
			KEY_SPACE:
				_try_jump()
			KEY_1:
				_set_tool(Tool.ADD_TERRAIN)
			KEY_2:
				_set_tool(Tool.PAINT)
			KEY_3:
				_set_tool(Tool.PAINT_DELETE)
			KEY_4:
				_set_tool(Tool.BOULDER)
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
				controls_label.visible = _controls_visible
			KEY_R:
				_reset_statues()
			KEY_T:
				_toggle_add_material()
			KEY_P:
				_place_companion_at_crosshair()

	elif event is InputEventMouseButton:
		# While the cursor is released, a click recaptures instead of firing a
		# tool, so Esc / alt-tab can never carve terrain by accident.
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			_tool_pressed = false
			if event.pressed:
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			return
		if event.button_index == MOUSE_BUTTON_LEFT:
			if _mode == Mode.PLAY:
				_tool_pressed = false
				if event.pressed:
					_start_pickaxe_swing()
				return
			_tool_pressed = event.pressed
			if event.pressed:
				if _is_drag_tool():
					_last_brush_point = Vector3(INF, INF, INF)
					_brush_cooldown = 0.0
					_process_brush(0.0)
				else:
					_apply_click_tool()
		elif event.button_index == MOUSE_BUTTON_RIGHT and event.pressed:
			if Input.is_key_pressed(KEY_SHIFT):
				_place_companion_at_crosshair()
			else:
				_query_companion_cave_path_to_crosshair()

func _process(delta: float) -> void:
	if _mode == Mode.PLAY:
		_process_play(delta)
	else:
		_process_fly(delta)

	if _tool_pressed and _mode == Mode.FLY and _is_drag_tool():
		_process_brush(delta)

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

	_update_stream_focus(false)
	_apply_companion_gravity_and_ground_snap(delta)
	_poll_pending_path_query()
	_step_companion_along_path(delta)
	_update_pending_pristine_hides(delta)
	_update_hud(delta)

func _update_stream_focus(force: bool) -> void:
	var cur_pos := camera.global_position
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
		material_state = " | Material: %s (T)" % MATERIAL_NAMES[_add_material - 1]

	var mode_name := "Play" if _mode == Mode.PLAY else "Fly"
	var tool_text := "Pickaxe (LMB swing)" if _mode == Mode.PLAY \
		else "[%s] %s%s" % [TOOL_KEYS[_active_tool], TOOL_NAMES[_active_tool], material_state]

	_look_at_accum += delta
	if _look_at_accum >= LOOK_AT_REFRESH_INTERVAL:
		_look_at_accum = 0.0
		_refresh_look_at_material()

	stats_label.text = "FPS: %.0f  (%.2f ms)\nView dist: %d / %d chunks  (prefetch %d)\nCommitted: %d chunks  (%.0f/s)\nStreaming: %s\nMode: %s (F)\nLooking at: %s\nTool: %s\nControls: H" % [
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
	]

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

func _set_tool(tool: int) -> void:
	if tool == _active_tool:
		return
	_active_tool = tool
	_last_boom_point = Vector3(INF, INF, INF)
	print("[VoxelTerrain3D] Active tool: [%s] %s" % [TOOL_KEYS[tool], TOOL_NAMES[tool]])

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
		Tool.LARGE_EXPLODE:
			terrain.trigger_explosion(point, large_explosion_radius)
		Tool.PAINT:
			terrain.add_terrain_with_material(point, paint_radius, _add_material)
		Tool.PAINT_DELETE:
			terrain.trigger_explosion(point, paint_delete_radius)
		Tool.BOULDER:
			terrain.place_boulder(point, boulder_radius)
		Tool.SMALL_HIT:
			terrain.trigger_explosion(point, small_hit_radius)
		Tool.WATER_QUENCH:
			terrain.trigger_elemental_sphere(point, elemental_radius, ELEMENTAL_WATER)
		Tool.FROST_ICE:
			terrain.trigger_elemental_sphere(point, elemental_radius, ELEMENTAL_FROST)
		Tool.LAVA_MAGMA:
			terrain.trigger_elemental_sphere(point, elemental_radius, ELEMENTAL_LAVA)
		_:
			terrain.trigger_explosion(point, explosion_radius)

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
	var structure_id := _stamp_structure(stamp_asset_path, Vector3(origin), recreate_delay)
	if structure_id > 0:
		print("[VoxelTerrain3D] Stamped asset '%s' structure_id=%d at chunk %s delay=%.1fs" % [
			stamp_asset_path, structure_id, str(origin), recreate_delay
		])
	else:
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
	if not terrain.fully_committed():
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

func _on_explosion_failed(reason: String) -> void:
	_rebake_in_progress = false
	_warn_throttled("[VoxelTerrain3D] Explosion failed: " + reason)

func _on_terrain_add_requested(pos: Vector3, radius: float) -> void:
	_print_throttled("[VoxelTerrain3D] Add terrain requested at %s radius=%.1f" % [pos, radius])

func _on_terrain_add_completed(pos: Vector3, radius: float, affected_chunks: int) -> void:
	_print_throttled("[VoxelTerrain3D] Add terrain completed at %s radius=%.1f chunks=%d" % [pos, radius, affected_chunks])

func _on_terrain_add_failed(reason: String) -> void:
	_warn_throttled("[VoxelTerrain3D] Add terrain failed: " + reason)

func _on_chunk_committed(_chunk_key: int) -> void:
	_committed_chunk_count += 1
	_commits_in_window += 1

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
