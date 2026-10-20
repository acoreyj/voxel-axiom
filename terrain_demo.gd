extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D and applies terrain tools on
## left-click: keys 1-6 select add / explode / large-explode / paint / paint-delete /
## place-boulder, where paint and paint-delete are hold-and-drag brushes. Key C
## selects a much smaller chip blast (small_hit_radius) for sculpting fine craters.
## T toggles the add/paint material between soil and 3-hit rock. Keys 7-9 fire Bananza-style
## elemental reaction spheres:
##   Key 7: Water Quench (turns Lava -> 3-hit Obsidian on contact & cools cracks)
##   Key 8: Frost / Ice Bridge (builds a flat-topped 31-deg ramped Ice Bridge & crater plug; flips Air/Water -> solid Ice & quenches Lava -> Obsidian)
##   Key 9: Lava / Magma (injects Lava, melts Ice -> Air, reacts with Water -> Obsidian)
## Key 0 stamps an offline-baked `.vreg` ruin asset at the crosshair with the
## reconstitution timer below; Key - (minus) stamps the same asset permanently
## destroyed (no timer). Blast the stamped structure and it is either restored
## after the quiet period or stays destroyed forever.
## Side-by-side 2x-scale Moai statues are seeded on startup (see `_setup_side_by_side_moai_statues`):
##   Option 1 `MoaiOption1_MeshSwap` shows a pristine 2x moai_statue.obj mesh + StaticBody3D
##   until the first blast nearby, then swaps to a stamped moai_statue.vreg voxel structure.
##   Option 2 `MoaiOption2_BakedVoxel` is stamped directly into VoxelTerrain3D with baked
##   RGB5551 colors, 3-hit crack damage, raw interior craters and a 15 s reconstitution timer.
## A second Atlas statue pair at z=-64 uses the same two styles, baked from atlas.glb:
##   Option 1 `AtlasOption1_MeshSwap` swaps atlas.obj for models/atlas/atlas_statue.vreg.
##   Option 2 `AtlasOption2_BakedVoxel` is stamped from the same watertight .vreg.
## Key R resets all four statues (restores Option 1's pre-stamp chunks and pristine mesh);
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
## Small-chip blast (Key C): a much smaller carve for sculpting fine craters
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
## Offline-baked ruin asset stamped by Key 0 / Key - (see the `voxel_baker` CLI:
## `voxel_baker ancient_ruin.obj ancient_ruin.vreg`). Place the .vreg next to
## this scene or point this at any other exported region file.
@export var stamp_asset_path: String = "res://ancient_ruin.vreg"
## Reconstitution quiet period for Key 0 stamps, in seconds. Key - stamps the
## same region with 0.0 so it stays permanently destroyed once blasted.
@export var stamp_recreate_delay_seconds: float = 15.0
## Moai statue assets for the side-by-side demo. `moai_vreg_path` is produced by
## the voxel_baker CLI from the 2x-scale (48 m) OBJ + albedo at 0.5 m voxels, and
## is stamped directly into the terrain for both demo options.
@export var moai_mesh_path: String = "res://models/moai_statue.obj"
@export var moai_albedo_path: String = "res://models/moai_albedo.png"
@export var moai_vreg_path: String = "moai_statue.vreg"
## Atlas statue assets for the second side-by-side demo pair. `atlas_statue.vreg`
## is baked from models/atlas/atlas.glb's 5.333x (48 m tall) OBJ + albedo at
## 0.5 m voxels with `--watertight --color-depth 2.8`.
@export var atlas_mesh_path: String = "res://models/atlas/atlas.obj"
@export var atlas_albedo_path: String = "res://models/atlas/atlas_albedo.png"
@export var atlas_vreg_path: String = "models/atlas/atlas_statue.vreg"

enum Tool { ADD_TERRAIN, EXPLODE, LARGE_EXPLODE, PAINT, PAINT_DELETE, BOULDER, WATER_QUENCH, FROST_ICE, LAVA_MAGMA, SMALL_HIT }

const TOOL_NAMES := ["Add terrain", "Explode", "Large explode", "Paint terrain", "Paint delete", "Place boulder", "Water quench", "Frost / Ice", "Lava / Magma", "Small hit"]
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

## Side-by-side 2x Moai statue placement. `moai_statue.vreg` is baked at
## voxel_scale = 0.5 m with target region center (160, 160, 128) at 48 m tall
## x 23.1 m wide x 21.2 m deep, so stamping at `origin_chunk` puts the statue
## center at `(origin_chunk * 32 + MOAI_REGION_CENTER) * voxel_scale` and its
## base at region sample 112 (= 160 - 24 m half height / 0.5 m per voxel).
const MOAI_REGION_CENTER := Vector3(160.0, 160.0, 128.0)
const OPT1_ORIGIN_CHUNK := Vector3(-5.0, -2.0, -4.0) # World center: (-16, 96, 0), base at y=72 buried in the ground
const OPT1_WORLD_CENTER := Vector3(-16.0, 96.0, 0.0)
const OPT2_ORIGIN_CHUNK := Vector3(-4.0, -2.0, -4.0) # World center: (+16, 96, 0), base at y=72 buried in the ground
const OPT2_WORLD_CENTER := Vector3(16.0, 96.0, 0.0)
const MOAI_HALF_EXTENTS := Vector3(13.0, 25.0, 12.0)
## Model base in region-sample coords for the (160,160,128)-centred 0.5 m bake;
## used to seat the statue on the terrain at the 32-sample chunk quantisation.
const MOAI_BASE_LOCAL_Y := 112.0
## The exported 2x OBJ has its origin at the statue mid-point (y -24..24 m), so
## the model center used for blast-proximity tests sits 24 m above the base.
const MOAI_MESH_CENTER_OFFSET_Y := 24.0
## Because the 2x OBJ origin already coincides with the model center, the pristine
## MeshInstance3D root is placed directly at `_opt1_world_center` (no base/rebase
## subtraction the way a base-origin OBJ would need).
const MOAI_ROOT_OFFSET_Y := 0.0

## Second side-by-side pair: the Atlas statue at z=-64. The source atlas.glb is
## 9 m tall, so it is baked at 5.333x to match the 48 m Moai pair and shares the
## same region center/base placement. Its exported OBJ origin is at the feet
## (y 0..48), so the pristine mesh root is seated 24 m above the statue center.
const ATLAS_REGION_CENTER := Vector3(160.0, 160.0, 128.0)
const ATLAS_BASE_LOCAL_Y := 112.0
const ATLAS_HALF_EXTENTS := Vector3(13.0, 25.0, 13.0)
const ATLAS_ROOT_OFFSET_Y := 24.0
const ATLAS_OPT1_WORLD_CENTER := Vector3(-16.0, 96.0, -64.0)
const ATLAS_OPT2_WORLD_CENTER := Vector3(16.0, 96.0, -64.0)

@onready var terrain: VoxelTerrain3D = $VoxelTerrain3D
@onready var ai_companion: VoxelAICompanion = $VoxelAICompanion
@onready var camera: Camera3D = $Camera3D
@onready var stats_label: Label = $HUD/StatsLabel
@onready var opt1_root: Node3D = $MoaiOption1_MeshSwap
@onready var opt1_mesh_inst: MeshInstance3D = $MoaiOption1_MeshSwap/PristineMesh
@onready var opt1_static_body: StaticBody3D = $MoaiOption1_MeshSwap/StaticBody3D
@onready var opt1_col_shape: CollisionShape3D = $MoaiOption1_MeshSwap/StaticBody3D/CollisionShape3D
@onready var opt1_label: Label3D = $MoaiOption1_MeshSwap/Label3D
@onready var atlas_opt1_root: Node3D = $AtlasOption1_MeshSwap
@onready var atlas_opt1_mesh_inst: MeshInstance3D = $AtlasOption1_MeshSwap/PristineMesh
@onready var atlas_opt1_static_body: StaticBody3D = $AtlasOption1_MeshSwap/StaticBody3D
@onready var atlas_opt1_col_shape: CollisionShape3D = $AtlasOption1_MeshSwap/StaticBody3D/CollisionShape3D
@onready var atlas_opt1_label: Label3D = $AtlasOption1_MeshSwap/Label3D

var _yaw: float = 0.0
var _pitch: float = -0.25
var _velocity: Vector3 = Vector3.ZERO
var _last_focus_chunk: Vector3i = Vector3i(999999, 999999, 999999)
var _committed_chunk_count: int = 0
var _fps_smoothed: float = 0.0
var _commits_in_window: int = 0
var _commits_per_second: float = 0.0
var _stats_accum: float = 0.0
var _active_tool: int = Tool.EXPLODE
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
## Surface-resolved placement (computed on startup / reset, not fixed in the scene).
var _opt1_world_center: Vector3 = OPT1_WORLD_CENTER
var _opt2_world_center: Vector3 = OPT2_WORLD_CENTER
var _opt1_origin_chunk: Vector3 = OPT1_ORIGIN_CHUNK
var _opt2_origin_chunk: Vector3 = OPT2_ORIGIN_CHUNK

var _atlas_opt1_swapped_to_voxels: bool = false
var _atlas_opt1_structure_id: int = 0
var _atlas_opt2_structure_id: int = 0
var _atlas_opt1_world_center: Vector3 = ATLAS_OPT1_WORLD_CENTER
var _atlas_opt2_world_center: Vector3 = ATLAS_OPT2_WORLD_CENTER
var _atlas_opt1_origin_chunk: Vector3 = Vector3.ZERO
var _atlas_opt2_origin_chunk: Vector3 = Vector3.ZERO

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
	_setup_side_by_side_moai_statues()
	_setup_side_by_side_atlas_statues()
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

func _moai_origin_for_surface(world_x: float, world_z: float) -> Vector3:
	return _statue_origin_for_surface(MOAI_REGION_CENTER, MOAI_BASE_LOCAL_Y, world_x, world_z)

func _atlas_origin_for_surface(world_x: float, world_z: float) -> Vector3:
	return _statue_origin_for_surface(ATLAS_REGION_CENTER, ATLAS_BASE_LOCAL_Y, world_x, world_z)

func _setup_side_by_side_moai_statues() -> void:
	# 1. Option 1 (Left at x=-16, z=0): Load the 2x (48 m) moai_statue.obj +
	#    albedo onto PristineMesh. It stays a normal mesh + StaticBody3D until the
	#    first nearby blast, then swaps to a stamped voxel structure. Its height is
	#    matched to Option 2's chunk-quantized base below so both stand level.
	_load_pristine_moai_mesh_and_collider()
	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0
	_opt1_origin_chunk = _moai_origin_for_surface(OPT1_WORLD_CENTER.x, OPT1_WORLD_CENTER.z)

	# 2. Option 2 (Right at x=+16, z=0): Stamp moai_statue.vreg directly into
	#    VoxelTerrain3D with per-voxel RGB5551 baked colors and a 15-second
	#    auto-reconstitution timer, seated on the surface. A region's vertical
	#    origin is quantized to a whole 32 m chunk, so its base can sit up to half
	#    a chunk off the terrain; compute it first and seat Option 1 to match, so
	#    the pristine .obj and the voxel statue stand at exactly the same level.
	_opt2_origin_chunk = _moai_origin_for_surface(OPT2_WORLD_CENTER.x, OPT2_WORLD_CENTER.z)
	_opt2_world_center = (_opt2_origin_chunk * 32.0 + MOAI_REGION_CENTER) * terrain.voxel_scale
	_opt1_world_center = Vector3(OPT1_WORLD_CENTER.x, _opt2_world_center.y, OPT1_WORLD_CENTER.z)
	opt1_root.global_position = _opt1_world_center - Vector3(0.0, MOAI_ROOT_OFFSET_Y, 0.0)

	_opt2_structure_id = _stamp_structure(moai_vreg_path, _opt2_origin_chunk, 15.0)
	if _opt2_structure_id <= 0:
		push_warning("[MoaiDemo] Failed to stamp '%s' — bake it first: voxel_baker models/moai_statue.obj %s --voxel-scale 0.5 --watertight --texture models/moai_albedo.ppm --color-depth 2.8 --center-x 160 --center-y 160 --center-z 128" % [
			moai_vreg_path, moai_vreg_path
		])
	print("[MoaiDemo] Option 1 (Pristine Mesh -> Voxel Swap) ready at ", _opt1_world_center,
		" | Option 2 (Baked Per-Voxel Albedo, id=#", _opt2_structure_id, ") stamped at ", _opt2_world_center)

func _setup_side_by_side_atlas_statues() -> void:
	# Same two styles as the Moai pair, one row further down the view direction
	# (z=-64). atlas.glb is baked at 5.333x so the statue is 48 m tall and shares
	# the Moai region center/base, letting both pairs seat at the same terrain level.
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

func _load_pristine_moai_mesh_and_collider() -> void:
	var mesh_candidates: Array[String] = [
		moai_mesh_path,
		"res://models/moai_statue.obj",
		"res://moai_statue.obj",
		"res://update/moai_statue.obj",
		"moai_statue.obj",
	]
	var albedo_candidates: Array[String] = [
		moai_albedo_path,
		"res://models/moai_albedo.png",
		"res://moai_albedo.png",
		"res://update/moai_albedo.png",
		"moai_albedo.png",
	]
	_load_pristine_statue_mesh(mesh_candidates, albedo_candidates, opt1_mesh_inst, opt1_col_shape, "MoaiDemo")

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
## hides the mesh/collider and stamps the exact same Moai into the voxel terrain,
## immediately followed by the normal crater carve in the same frame.
func _check_and_swap_opt1_moai_on_blast(blast_pos: Vector3, blast_radius: float, hit_collider: Object) -> void:
	if _opt1_swapped_to_voxels:
		return
	var direct_hit := (hit_collider == opt1_static_body)
	var dx: float = max(0.0, abs(blast_pos.x - _opt1_world_center.x) - MOAI_HALF_EXTENTS.x)
	var dy: float = max(0.0, abs(blast_pos.y - _opt1_world_center.y) - MOAI_HALF_EXTENTS.y)
	var dz: float = max(0.0, abs(blast_pos.z - _opt1_world_center.z) - MOAI_HALF_EXTENTS.z)
	var dist_to_box := sqrt(dx * dx + dy * dy + dz * dz)

	if direct_hit or dist_to_box <= blast_radius + 2.0:
		opt1_mesh_inst.visible = false
		opt1_col_shape.disabled = true
		_opt1_structure_id = _stamp_structure(moai_vreg_path, _opt1_origin_chunk, 0.0)
		_opt1_swapped_to_voxels = true
		opt1_label.text = "Option 1 (2x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _opt1_structure_id
		print("[MoaiSwap] Option 1 Moai hit at ", blast_pos, " -> Hid PristineMesh & stamped Voxel Structure #", _opt1_structure_id, "!")

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
		atlas_opt1_mesh_inst.visible = false
		atlas_opt1_col_shape.disabled = true
		_atlas_opt1_structure_id = _stamp_structure(atlas_vreg_path, _atlas_opt1_origin_chunk, 0.0)
		_atlas_opt1_swapped_to_voxels = true
		atlas_opt1_label.text = "Atlas Option 1 (5.33x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _atlas_opt1_structure_id
		print("[AtlasSwap] Option 1 Atlas hit at ", blast_pos, " -> Hid PristineMesh & stamped Voxel Structure #", _atlas_opt1_structure_id, "!")

## Key R reset loop. Option 1 is removed via terrain.remove_structure(), which
## restores the pre-stamp chunks bit-exactly and brings back the pristine mesh +
## collider. Option 2 is re-stamped if it was removed successfully.
func _reset_statues() -> void:
	_reset_moai_statues()
	_reset_atlas_statues()

func _reset_moai_statues() -> void:
	if _opt1_swapped_to_voxels and _opt1_structure_id > 0:
		var ok := _remove_structure(_opt1_structure_id)
		print("[MoaiReset] Removed Option 1 voxel structure #", _opt1_structure_id, " (restored pre-stamp chunks: ", ok, ")")
	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0
	opt1_mesh_inst.visible = true
	opt1_col_shape.disabled = false
	opt1_label.text = "Option 1 (2x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"

	var can_restamp_opt2 := true
	if _opt2_structure_id > 0:
		can_restamp_opt2 = _remove_structure(_opt2_structure_id)
	_opt2_structure_id = 0
	if can_restamp_opt2:
		_opt2_structure_id = _stamp_structure(moai_vreg_path, _opt2_origin_chunk, 15.0)
	print("[MoaiReset] Both Moai statues restored to pristine state! (Option 2 id=#", _opt2_structure_id, ")")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _reset_atlas_statues() -> void:
	if _atlas_opt1_swapped_to_voxels and _atlas_opt1_structure_id > 0:
		var ok := _remove_structure(_atlas_opt1_structure_id)
		print("[AtlasReset] Removed Option 1 voxel structure #", _atlas_opt1_structure_id, " (restored pre-stamp chunks: ", ok, ")")
	_atlas_opt1_swapped_to_voxels = false
	_atlas_opt1_structure_id = 0
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

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch = clamp(_pitch - event.relative.y * mouse_sensitivity, -1.45, 1.45)
		camera.rotation = Vector3(_pitch, _yaw, 0.0)

	elif event is InputEventKey and event.pressed:
		match event.keycode:
			KEY_ESCAPE:
				if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
					Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
				else:
					Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			KEY_1:
				_set_tool(Tool.ADD_TERRAIN)
			KEY_2:
				_set_tool(Tool.EXPLODE)
			KEY_3:
				_set_tool(Tool.LARGE_EXPLODE)
			KEY_4:
				_set_tool(Tool.PAINT)
			KEY_5:
				_set_tool(Tool.PAINT_DELETE)
			KEY_6:
				_set_tool(Tool.BOULDER)
			KEY_7:
				_set_tool(Tool.WATER_QUENCH)
			KEY_8:
				_set_tool(Tool.FROST_ICE)
			KEY_9:
				_set_tool(Tool.LAVA_MAGMA)
			KEY_C:
				_set_tool(Tool.SMALL_HIT)
			KEY_R:
				_reset_statues()
			KEY_T:
				_toggle_add_material()
			KEY_P:
				_place_companion_at_crosshair()
			KEY_0:
				_stamp_asset_at_crosshair(stamp_recreate_delay_seconds)
			KEY_MINUS:
				_stamp_asset_at_crosshair(0.0)

	elif event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
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

	if _tool_pressed and _is_drag_tool():
		_process_brush(delta)

	_update_stream_focus(false)
	_apply_companion_gravity_and_ground_snap(delta)
	_poll_pending_path_query()
	_step_companion_along_path(delta)
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

	_look_at_accum += delta
	if _look_at_accum >= LOOK_AT_REFRESH_INTERVAL:
		_look_at_accum = 0.0
		_refresh_look_at_material()

	stats_label.text = "FPS: %.0f  (%.2f ms)\nView dist: %d / %d chunks  (prefetch %d)\nCommitted: %d chunks  (%.0f/s)\nStreaming: %s\nLooking at: %s\nTool: [%d] %s%s | 1-9 select, C small hit, 0 stamp ruin, - permanent ruin, R reset statues, T material, LMB use, RMB path, P place" % [
		_fps_smoothed,
		1000.0 / max(_fps_smoothed, 1.0),
		terrain.get_effective_view_distance_chunks(),
		terrain.view_distance_chunks,
		terrain.prefetch_distance_chunks,
		_committed_chunk_count,
		_commits_per_second,
		streaming_state,
		_look_at_name,
		_active_tool + 1,
		TOOL_NAMES[_active_tool],
		material_state,
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
	print("[VoxelTerrain3D] Active tool: [%d] %s" % [tool + 1, TOOL_NAMES[tool]])

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
	if _is_boom_tool() and (not _opt1_swapped_to_voxels or not _atlas_opt1_swapped_to_voxels):
		var phys_hit := _raycast_crosshair()
		if not phys_hit.is_empty():
			var hit_collider: Object = phys_hit.get("collider")
			if not _opt1_swapped_to_voxels and hit_collider == opt1_static_body:
				point = phys_hit.position
			elif not _atlas_opt1_swapped_to_voxels and hit_collider == atlas_opt1_static_body:
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
	if _is_boom_tool() and (not _opt1_swapped_to_voxels or not _atlas_opt1_swapped_to_voxels):
		var hit := _raycast_crosshair()
		var hit_collider: Object = hit.get("collider") if not hit.is_empty() else null
		_check_and_swap_opt1_moai_on_blast(point, _active_radius(), hit_collider)
		_check_and_swap_atlas_opt1_on_blast(point, _active_radius(), hit_collider)
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
