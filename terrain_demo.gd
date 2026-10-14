extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D and applies terrain tools on
## left-click: keys 1-5 select add / explode / large-explode / paint / paint-delete,
## where paint and paint-delete are hold-and-drag brushes. Right-click queries
## 3D cave navigation paths.

@export var move_speed: float = 24.0
@export var fast_multiplier: float = 3.0
@export var mouse_sensitivity: float = 0.0025
@export var explosion_radius: float = 8.5
@export var large_explosion_radius: float = 18.0
@export var add_radius: float = 6.0
@export var paint_radius: float = 3.5
@export var paint_delete_radius: float = 3.5
@export var brush_spacing_factor: float = 0.6
@export var brush_interval: float = 0.05
@export var max_ray_distance: float = 250.0

enum Tool { ADD_TERRAIN, EXPLODE, LARGE_EXPLODE, PAINT, PAINT_DELETE }

const TOOL_NAMES := ["Add terrain", "Explode", "Large explode", "Paint terrain", "Paint delete"]

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
var _tool_pressed: bool = false
var _brush_cooldown: float = 0.0
var _last_brush_point: Vector3 = Vector3(INF, INF, INF)
var _last_event_print_ms: int = 0
var _last_fail_warn_ms: int = 0

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.terrain_add_requested.connect(_on_terrain_add_requested)
	terrain.terrain_add_completed.connect(_on_terrain_add_completed)
	terrain.terrain_add_failed.connect(_on_terrain_add_failed)
	terrain.chunk_committed.connect(_on_chunk_committed)

	_update_stream_focus(true)

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

	elif event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_tool_pressed = event.pressed
			if event.pressed:
				if _is_drag_tool():
					_last_brush_point = Vector3(INF, INF, INF)
					_brush_cooldown = 0.0
					_process_brush(0.0)
				else:
					_apply_tool_at(_aim_point())
		elif event.button_index == MOUSE_BUTTON_RIGHT and event.pressed:
			_query_companion_cave_path()

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

	stats_label.text = "FPS: %.0f  (%.2f ms)\nView dist: %d / %d chunks  (prefetch %d)\nCommitted: %d chunks  (%.0f/s)\nStreaming: %s\nTool: [%d] %s | 1-5 select, LMB use, RMB cave path" % [
		_fps_smoothed,
		1000.0 / max(_fps_smoothed, 1.0),
		terrain.get_effective_view_distance_chunks(),
		terrain.view_distance_chunks,
		terrain.prefetch_distance_chunks,
		_committed_chunk_count,
		_commits_per_second,
		streaming_state,
		_active_tool + 1,
		TOOL_NAMES[_active_tool],
	]

func _set_tool(tool: int) -> void:
	if tool == _active_tool:
		return
	_active_tool = tool
	print("[VoxelTerrain3D] Active tool: [%d] %s" % [tool + 1, TOOL_NAMES[tool]])

func _is_drag_tool() -> bool:
	return _active_tool == Tool.PAINT or _active_tool == Tool.PAINT_DELETE

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
	match _active_tool:
		Tool.ADD_TERRAIN:
			terrain.add_terrain(point, add_radius)
		Tool.LARGE_EXPLODE:
			terrain.trigger_explosion(point, large_explosion_radius)
		Tool.PAINT:
			terrain.add_terrain(point, paint_radius)
		Tool.PAINT_DELETE:
			terrain.trigger_explosion(point, paint_delete_radius)
		_:
			terrain.trigger_explosion(point, explosion_radius)

func _query_companion_cave_path() -> void:
	var target_pos := camera.global_position
	if not ai_companion.is_path_region_synced(target_pos):
		print("[Nav3D] 3D 4x2x4 cave navigation group still baking async...")
		return
	var path: PackedVector3Array = ai_companion.get_voxel_path(ai_companion.global_position, target_pos)
	print("[Nav3D] Companion 3D cave path waypoints: ", path.size())

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

func _on_explosion_failed(reason: String) -> void:
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
