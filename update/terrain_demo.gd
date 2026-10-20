extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Showcases 2x-scale side-by-side Moai Statues (Option 1: Pristine Mesh -> Voxel Swap vs Option 2: 100% Voxel + Baked RGB5551 Albedo),
## asynchronous R+delta sphere explosions, elemental reactions, `remove_structure` reset loop, and 3D `4x2x4` cave navigation:
##   Key 1: Explode (CSG blast: 1-hit Soil/Lava/Ice, 3-hit Hard Rock & Obsidian)
##   Key 2: Water Quench (turns Lava -> 3-hit Obsidian on contact & cools cracks)
##   Key 3: Frost / Ice Bridge (builds a flat-topped 31-deg ramped Ice Bridge & quenches Lava -> Obsidian)
##   Key 4: Lava / Magma (injects Lava, melts Ice -> Air, reacts with Water -> Obsidian)
##   Key 5: Stamp Ancient Ruin (.vreg) at crosshair with 15s Reconstitution Timer
##   Key 6: Stamp 2x Baked-Color Moai Statue (.vreg) at crosshair
##   Key R: Reset both Moai Statues (calls terrain.remove_structure() to restore pre-stamp chunks & unhide Option 1 mesh!)
##   Key P (or Shift + RMB): Place / teleport AI Companion start marker at crosshair hit point
##   RMB (Right-Click): Set path goal at crosshair hit point, bake/query 3D path, draw path & walk!

@export var move_speed: float = 24.0
@export var fast_multiplier: float = 3.0
@export var mouse_sensitivity: float = 0.0025
@export var explosion_radius: float = 8.5
@export var max_ray_distance: float = 250.0
@export var companion_walk_speed: float = 10.0
@export var companion_gravity: float = 28.0

# 2x-Scale Side-by-Side Moai Statue coordinates on the y=78.0..78.6 hilltop plateau:
# moai_statue.vreg (48m tall x 23m wide x 21m deep) is baked with target_region_center = (144, 160, 128),
# so stamping at origin_chunk places the statue center at (origin_chunk * 32 + Vector3(144, 160, 128)).
const MOAI_REGION_CENTER := Vector3(144.0, 160.0, 128.0)
const OPT1_ORIGIN_CHUNK := Vector3(-5.0, -2.0, -4.0) # World center: (-16, 96, 0), base at y=72 buried in y=78.5 ground
const OPT1_WORLD_CENTER := Vector3(-16.0, 96.0, 0.0)
const OPT2_ORIGIN_CHUNK := Vector3(-4.0, -2.0, -4.0) # World center: (+16, 96, 0), base at y=72 buried in y=78.2 ground
const OPT2_WORLD_CENTER := Vector3(16.0, 96.0, 0.0)
const MOAI_HALF_EXTENTS := Vector3(13.0, 25.0, 12.0)

@onready var terrain: VoxelTerrain3D = $VoxelTerrain3D
@onready var ai_companion: VoxelAICompanion = $VoxelAICompanion
@onready var camera: Camera3D = $Camera3D
@onready var opt1_root: Node3D = $MoaiOption1_MeshSwap
@onready var opt1_mesh_inst: MeshInstance3D = $MoaiOption1_MeshSwap/PristineMesh
@onready var opt1_static_body: StaticBody3D = $MoaiOption1_MeshSwap/StaticBody3D
@onready var opt1_col_shape: CollisionShape3D = $MoaiOption1_MeshSwap/StaticBody3D/CollisionShape3D
@onready var opt1_label: Label3D = $MoaiOption1_MeshSwap/Label3D

var _yaw: float = 0.0
var _pitch: float = -0.10
var _velocity: Vector3 = Vector3.ZERO
var _last_focus_chunk: Vector3i = Vector3i(999999, 999999, 999999)
var _committed_chunk_count: int = 0
var _selected_element_kind: int = 0 # 0=Explode, 1=Water, 2=Frost/Ice Bridge, 3=Lava

var _opt1_swapped_to_voxels: bool = false
var _opt1_structure_id: int = 0
var _opt2_structure_id: int = 0

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

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.chunk_committed.connect(_on_chunk_committed)
	terrain.structure_damaged.connect(_on_structure_damaged)
	terrain.structure_recreated.connect(_on_structure_recreated)
	if terrain.has_signal("structure_removed"):
		terrain.structure_removed.connect(_on_structure_removed)

	_setup_path_debug_visuals()
	_setup_side_by_side_moai_statues()
	_update_stream_focus(true)

func _setup_side_by_side_moai_statues() -> void:
	# 1. Option 1 (Left at x=-16, y=96, z=0): Load 2x high-res moai_statue.obj + moai_albedo.png onto PristineMesh
	_load_pristine_moai_mesh_and_collider()
	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0

	# 2. Option 2 (Right at x=+16, y=96, z=0): Stamp 2x moai_statue.vreg directly into VoxelTerrain3D
	#    with per-voxel RGB5551 baked colors and a 15-second auto-reconstitution timer!
	_opt2_structure_id = terrain.stamp_asset("moai_statue.vreg", OPT2_ORIGIN_CHUNK, 15.0)
	print("[MoaiDemo] 2x Option 1 (Pristine Mesh -> Voxel Swap) ready at ", OPT1_WORLD_CENTER,
		" | 2x Option 2 (Baked Per-Voxel Albedo, id=#", _opt2_structure_id, ") stamped at ", OPT2_WORLD_CENTER)

func _load_pristine_moai_mesh_and_collider() -> void:
	var f := FileAccess.open("res://moai_statue.obj", FileAccess.READ)
	if f == null:
		f = FileAccess.open("moai_statue.obj", FileAccess.READ)
	if f == null:
		push_warning("[MoaiDemo] Could not open moai_statue.obj for Option 1 PristineMesh")
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
	var img := Image.new()
	var err := img.load("res://moai_albedo.png")
	if err != OK:
		err = img.load("moai_albedo.png")
	if err == OK:
		mat.albedo_texture = ImageTexture.create_from_image(img)
	else:
		mat.albedo_color = Color(0.57, 0.52, 0.45)

	opt1_mesh_inst.mesh = arr_mesh
	opt1_mesh_inst.material_override = mat
	opt1_mesh_inst.visible = true

	var shape := ConcavePolygonShape3D.new()
	shape.set_faces(out_verts)
	opt1_col_shape.shape = shape
	opt1_col_shape.disabled = false

func _check_and_swap_opt1_moai_on_blast(blast_pos: Vector3, blast_radius: float, hit_collider: Object) -> void:
	if _opt1_swapped_to_voxels:
		return
	var direct_hit := (hit_collider == opt1_static_body)
	var dx: float = max(0.0, abs(blast_pos.x - OPT1_WORLD_CENTER.x) - MOAI_HALF_EXTENTS.x)
	var dy: float = max(0.0, abs(blast_pos.y - OPT1_WORLD_CENTER.y) - MOAI_HALF_EXTENTS.y)
	var dz: float = max(0.0, abs(blast_pos.z - OPT1_WORLD_CENTER.z) - MOAI_HALF_EXTENTS.z)
	var dist_to_box := sqrt(dx * dx + dy * dy + dz * dz)

	if direct_hit or dist_to_box <= blast_radius + 2.0:
		opt1_mesh_inst.visible = false
		opt1_col_shape.disabled = true
		_opt1_structure_id = terrain.stamp_asset("moai_statue.vreg", OPT1_ORIGIN_CHUNK, 0.0)
		_opt1_swapped_to_voxels = true
		opt1_label.text = "Option 1 (2x Scale): SWAPPED TO VOXELS (#%d)\n(Carve freely | Press R to restore Pristine Mesh)" % _opt1_structure_id
		print("[MoaiSwap] Option 1 Moai hit at ", blast_pos, " -> Hid PristineMesh & stamped Voxel Structure #", _opt1_structure_id, "!")

func _reset_moai_statues() -> void:
	if _opt1_swapped_to_voxels and _opt1_structure_id > 0:
		var ok := terrain.remove_structure(_opt1_structure_id)
		print("[MoaiReset] Removed Option 1 voxel structure #", _opt1_structure_id, " (restored pre-stamp chunks: ", ok, ")")
	_opt1_swapped_to_voxels = false
	_opt1_structure_id = 0
	opt1_mesh_inst.visible = true
	opt1_col_shape.disabled = false
	opt1_label.text = "Option 1 (2x Scale): Pristine Mesh -> Voxel Swap\n(Shoot to fracture into voxels | Press R to Reset)"

	if _opt2_structure_id > 0:
		terrain.remove_structure(_opt2_structure_id)
	_opt2_structure_id = terrain.stamp_asset("moai_statue.vreg", OPT2_ORIGIN_CHUNK, 15.0)
	print("[MoaiReset] Both 2x Moai statues restored to pristine state! (Option 2 id=#", _opt2_structure_id, ")")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _setup_path_debug_visuals() -> void:
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
			print("[Tool] Selected 3: Frost / Ice Bridge (Flat-Topped 31-deg Ramped Bridge, Lava -> Obsidian)")
		elif event.keycode == KEY_4:
			_selected_element_kind = 3
			print("[Tool] Selected 4: Lava / Magma (Soil -> Lava, Melts Ice -> Air)")
		elif event.keycode == KEY_5:
			_stamp_asset_at_crosshair("ancient_ruin.vreg", 15.0, Vector3(128.0, 128.0, 128.0))
		elif event.keycode == KEY_6:
			_stamp_asset_at_crosshair("moai_statue.vreg", 15.0, MOAI_REGION_CENTER)
		elif event.keycode == KEY_R:
			_reset_moai_statues()
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
	_apply_companion_gravity_and_ground_snap(delta)
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

func _raycast_ground_at(pos: Vector3, clearance_up: float = 3.5, down_dist: float = 64.0) -> Dictionary:
	var space_state := get_world_3d().direct_space_state
	var from := pos + Vector3(0.0, clearance_up, 0.0)
	var to := pos - Vector3(0.0, down_dist, 0.0)
	var query := PhysicsRayQueryParameters3D.create(from, to)
	return space_state.intersect_ray(query)

func _apply_companion_gravity_and_ground_snap(delta: float) -> void:
	if not _companion_initialized:
		return
	var cur := ai_companion.global_position
	var hit := _raycast_ground_at(cur, 4.5, 80.0)
	if not hit.is_empty():
		var ground_y: float = hit.position.y + 0.15
		if cur.y > ground_y + 0.35:
			_companion_vertical_vel = min(_companion_vertical_vel + companion_gravity * delta, 40.0)
			cur.y = max(ground_y, cur.y - _companion_vertical_vel * delta)
			ai_companion.global_position = cur
			if cur.y <= ground_y + 0.05 and _goal_mesh_inst.visible:
				_companion_vertical_vel = 0.0
				_has_pending_path_query = true
		else:
			_companion_vertical_vel = 0.0
			cur.y = ground_y
			ai_companion.global_position = cur

func _fire_raycast_action() -> void:
	var hit := _raycast_crosshair()
	var target_pos: Vector3 = hit.position if not hit.is_empty() else (camera.global_position - camera.global_transform.basis.z * 24.0)
	var hit_collider: Object = hit.get("collider") if not hit.is_empty() else null

	_check_and_swap_opt1_moai_on_blast(target_pos, explosion_radius, hit_collider)

	if _companion_initialized and _goal_mesh_inst.visible:
		_rebake_in_progress = true
	if _selected_element_kind == 0:
		terrain.trigger_explosion(target_pos, explosion_radius)
	else:
		terrain.trigger_elemental_sphere(target_pos, explosion_radius, _selected_element_kind)

func _stamp_asset_at_crosshair(vreg_path: String, recreate_delay: float, region_center: Vector3) -> void:
	var hit := _raycast_crosshair()
	var stamp_world_pos: Vector3 = hit.position if not hit.is_empty() else (camera.global_position - camera.global_transform.basis.z * 30.0)
	var origin_chunk := Vector3(
		floor((stamp_world_pos.x - region_center.x) / 32.0),
		floor((stamp_world_pos.y - region_center.y) / 32.0),
		floor((stamp_world_pos.z - region_center.z) / 32.0)
	)
	var struct_id := terrain.stamp_asset(vreg_path, origin_chunk, recreate_delay)
	if struct_id > 0:
		print("[AssetStamp] Stamped '", vreg_path, "' #", struct_id, " at chunk ", origin_chunk,
			" (recreate_delay=", recreate_delay, "s)")
		if _companion_initialized and _goal_mesh_inst.visible:
			_has_pending_path_query = true
	else:
		push_warning("[AssetStamp] Failed to stamp asset from " + vreg_path)

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

func _on_explosion_requested(pos: Vector3, radius: float) -> void:
	print("[VoxelTerrain3D] Action requested at ", pos, " radius=", radius, " element=", _selected_element_kind)

func _on_explosion_completed(pos: Vector3, radius: float, affected_chunks: int) -> void:
	print("[VoxelTerrain3D] Action completed at ", pos, " radius=", radius, " chunks=", affected_chunks)
	if _goal_mesh_inst.visible:
		var g_hit := _raycast_ground_at(_pending_path_goal, 6.0, 32.0)
		if not g_hit.is_empty():
			_pending_path_goal.y = g_hit.position.y + 0.15
			_goal_mesh_inst.global_position = _pending_path_goal
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _on_explosion_failed(reason: String) -> void:
	_rebake_in_progress = false
	push_warning("[VoxelTerrain3D] Action failed: " + reason)

func _on_chunk_committed(_chunk_key: int) -> void:
	_committed_chunk_count += 1

func _on_structure_damaged(structure_id: int, struct_name: String) -> void:
	print("[Structure] Stamped structure #", structure_id, " ('", struct_name, "') damaged!")

func _on_structure_recreated(structure_id: int, struct_name: String) -> void:
	print("[Structure] Stamped structure #", structure_id, " ('", struct_name, "') timer elapsed: FULLY RECONSTITUTED from original TSDF + RGB5551 colors!")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true

func _on_structure_removed(structure_id: int, struct_name: String) -> void:
	print("[Structure] Stamped structure #", structure_id, " ('", struct_name, "') removed -> pre-stamp chunk baseline restored!")
	if _companion_initialized and _goal_mesh_inst.visible:
		_has_pending_path_query = true
