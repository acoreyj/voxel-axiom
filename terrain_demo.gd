extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D and applies terrain tools on
## left-click: keys 1-6 select add / explode / large-explode / paint / paint-delete /
## place-boulder, where paint and paint-delete are hold-and-drag brushes. R toggles
## the add/paint material between soil and 3-hit rock. Keys 7-9 fire Bananza-style
## elemental reaction spheres:
##   Key 7: Water Quench (turns Lava -> 3-hit Obsidian on contact & cools cracks)
##   Key 8: Frost / Ice Bridge (builds a flat-topped 31-deg ramped Ice Bridge & crater plug; flips Air/Water -> solid Ice & quenches Lava -> Obsidian)
##   Key 9: Lava / Magma (injects Lava, melts Ice -> Air, reacts with Water -> Obsidian)
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

enum Tool { ADD_TERRAIN, EXPLODE, LARGE_EXPLODE, PAINT, PAINT_DELETE, BOULDER, WATER_QUENCH, FROST_ICE, LAVA_MAGMA }

const TOOL_NAMES := ["Add terrain", "Explode", "Large explode", "Paint terrain", "Paint delete", "Place boulder", "Water quench", "Frost / Ice", "Lava / Magma"]
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

@onready var terrain: VoxelTerrain3D = $VoxelTerrain3D
@onready var ai_companion: VoxelAICompanion = $VoxelAICompanion
@onready var camera: Camera3D = $Camera3D
@onready var stats_label: Label = $HUD/StatsLabel

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

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.terrain_add_requested.connect(_on_terrain_add_requested)
	terrain.terrain_add_completed.connect(_on_terrain_add_completed)
	terrain.terrain_add_failed.connect(_on_terrain_add_failed)
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
			KEY_R:
				_toggle_add_material()
			KEY_P:
				_place_companion_at_crosshair()

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
	var cur_chunk := Vector3i(
		int(floor(cur_pos.x / 32.0)),
		int(floor(cur_pos.y / 32.0)),
		int(floor(cur_pos.z / 32.0))
	)
	# Only call into C++ set_stream_focus when the camera crosses into a new 32^3 chunk!
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
		material_state = " | Material: %s (R)" % MATERIAL_NAMES[_add_material - 1]

	_look_at_accum += delta
	if _look_at_accum >= LOOK_AT_REFRESH_INTERVAL:
		_look_at_accum = 0.0
		_refresh_look_at_material()

	stats_label.text = "FPS: %.0f  (%.2f ms)\nView dist: %d / %d chunks  (prefetch %d)\nCommitted: %d chunks  (%.0f/s)\nStreaming: %s\nLooking at: %s\nTool: [%d] %s%s | 1-9 select, LMB use, RMB path, P place" % [
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
		or _is_elemental_tool()

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
