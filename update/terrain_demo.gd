extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D only when crossing a chunk boundary,
## fires asynchronous R+delta sphere explosions or elemental reactions on left-click,
## and tests 3D `4x2x4` cave navigation pathing with visual 3D path ribbons & moving companion:
##   Key 1: Explode (CSG blast: 1-hit Soil/Lava/Ice, 3-hit Hard Rock & Obsidian)
##   Key 2: Water Quench (turns Lava -> 3-hit Obsidian on contact & cools cracks)
##   Key 3: Frost / Ice Bridge (flips Air/Water -> solid Ice & quenches Lava -> Obsidian)
##   Key 4: Lava / Magma (injects Lava, melts Ice -> Air, reacts with Water -> Obsidian)
##   Key P (or Shift + RMB): Place / teleport AI Companion start marker at crosshair hit point
##   RMB (Right-Click): Set path goal at crosshair hit point, bake/query 3D path, draw path & walk!

@export var move_speed: float = 24.0
@export var fast_multiplier: float = 3.0
@export var mouse_sensitivity: float = 0.0025
@export var explosion_radius: float = 8.5
@export var max_ray_distance: float = 250.0
@export var companion_walk_speed: float = 10.0

@onready var terrain: VoxelTerrain3D = $VoxelTerrain3D
@onready var ai_companion: VoxelAICompanion = $VoxelAICompanion
@onready var camera: Camera3D = $Camera3D

var _yaw: float = 0.0
var _pitch: float = -0.25
var _velocity: Vector3 = Vector3.ZERO
var _last_focus_chunk: Vector3i = Vector3i(999999, 999999, 999999)
var _committed_chunk_count: int = 0
var _selected_element_kind: int = 0 # 0=Explode, 1=Water, 2=Frost/Ice, 3=Lava

var _companion_initialized: bool = false
var _pending_path_goal: Vector3 = Vector3.ZERO
var _has_pending_path_query: bool = false
var _active_waypoints: PackedVector3Array = PackedVector3Array()
var _current_waypoint_idx: int = 0

var _companion_mesh_inst: MeshInstance3D = null
var _goal_mesh_inst: MeshInstance3D = null
var _path_mesh_inst: MeshInstance3D = null
var _path_immediate_mesh: ImmediateMesh = null

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.chunk_committed.connect(_on_chunk_committed)

	_setup_path_debug_visuals()
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

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch = clamp(_pitch - event.relative.y * mouse_sensitivity, -1.45, 1.45)
		camera.rotation = Vector3(_pitch, _yaw, 0.0)

	elif event is InputEventKey and event.pressed:
		if event.keycode == KEY_ESCAPE:
			if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
				Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
			else:
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		elif event.keycode == KEY_1:
			_selected_element_kind = 0
			print("[Tool] Selected 1: Explode (CSG Carve)")
		elif event.keycode == KEY_2:
			_selected_element_kind = 1
			print("[Tool] Selected 2: Water Quench (Lava -> 3-Hit Obsidian)")
		elif event.keycode == KEY_3:
			_selected_element_kind = 2
			print("[Tool] Selected 3: Frost / Ice (Air -> Solid Ice, Lava -> Obsidian)")
		elif event.keycode == KEY_4:
			_selected_element_kind = 3
			print("[Tool] Selected 4: Lava / Magma (Soil -> Lava, Melts Ice -> Air)")
		elif event.keycode == KEY_P:
			_place_companion_at_crosshair()

	elif event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_fire_raycast_action()
		elif event.button_index == MOUSE_BUTTON_RIGHT:
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

	_update_stream_focus(false)
	_poll_pending_path_query()
	_step_companion_along_path(delta)

func _update_stream_focus(force: bool) -> void:
	var cur_pos := camera.global_position
	var cur_chunk := Vector3i(
		int(floor(cur_pos.x / 32.0)),
		int(floor(cur_pos.y / 32.0)),
		int(floor(cur_pos.z / 32.0))
	)
	if force or cur_chunk != _last_focus_chunk:
		_last_focus_chunk = cur_chunk
		terrain.set_stream_focus(cur_pos, _velocity)

func _raycast_crosshair() -> Dictionary:
	var space_state := get_world_3d().direct_space_state
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * max_ray_distance
	var query := PhysicsRayQueryParameters3D.create(from, to)
	return space_state.intersect_ray(query)

func _fire_raycast_action() -> void:
	var hit := _raycast_crosshair()
	var target_pos: Vector3 = hit.position if not hit.is_empty() else (camera.global_position - camera.global_transform.basis.z * 24.0)
	if _selected_element_kind == 0:
		terrain.trigger_explosion(target_pos, explosion_radius)
	else:
		terrain.trigger_elemental_sphere(target_pos, explosion_radius, _selected_element_kind)

func _place_companion_at_crosshair() -> void:
	var hit := _raycast_crosshair()
	if hit.is_empty():
		print("[Nav3D] Aim at terrain surface and press P (or Shift+RMB) to place AI Companion.")
		return
	ai_companion.global_position = hit.position + Vector3(0.0, 0.5, 0.0)
	_companion_initialized = true
	_active_waypoints.clear()
	print("[Nav3D] Placed AI Companion start at ", ai_companion.global_position)

func _query_companion_cave_path_to_crosshair() -> void:
	var hit := _raycast_crosshair()
	var goal_pos: Vector3 = (hit.position + Vector3(0.0, 0.5, 0.0)) if not hit.is_empty() else camera.global_position

	# First RMB click places the companion if it hasn't been placed on the surface yet
	if not _companion_initialized:
		ai_companion.global_position = goal_pos
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
	var path: PackedVector3Array = ai_companion.get_voxel_path(start_pos, _pending_path_goal)
	print("[Nav3D] Companion 3D path from ", start_pos, " to ", _pending_path_goal, " -> waypoints: ", path.size())
	_active_waypoints = path
	_current_waypoint_idx = 0
	_draw_path_ribbon(path)

func _draw_path_ribbon(path: PackedVector3Array) -> void:
	_path_immediate_mesh.clear_surfaces()
	if path.size() < 2:
		return
	_path_immediate_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for pt in path:
		_path_immediate_mesh.surface_add_vertex(pt + Vector3(0.0, 0.35, 0.0))
	_path_immediate_mesh.surface_end()

func _step_companion_along_path(delta: float) -> void:
	if _current_waypoint_idx >= _active_waypoints.size():
		return
	var target := _active_waypoints[_current_waypoint_idx]
	var to_target := target - ai_companion.global_position
	var dist := to_target.length()
	var step := companion_walk_speed * delta
	if dist <= step or dist < 0.15:
		ai_companion.global_position = target
		_current_waypoint_idx += 1
	else:
		ai_companion.global_position += (to_target / dist) * step

func _on_explosion_requested(pos: Vector3, radius: float) -> void:
	print("[VoxelTerrain3D] Action requested at ", pos, " radius=", radius, " element=", _selected_element_kind)

func _on_explosion_completed(pos: Vector3, radius: float, affected_chunks: int) -> void:
	print("[VoxelTerrain3D] Action completed at ", pos, " radius=", radius, " chunks=", affected_chunks)
	# Automatically re-query path if a goal is active so you can watch paths reroute around craters or across Ice bridges!
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _on_explosion_failed(reason: String) -> void:
	push_warning("[VoxelTerrain3D] Action failed: " + reason)

func _on_chunk_committed(_chunk_key: int) -> void:
	_committed_chunk_count += 1
