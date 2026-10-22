extends Node
## Deterministic shot player for the trailer capture pipeline.
##
## Invoked by terrain_demo.gd when the project is launched with:
##   -- --trailer-shot=<id> --trailer-save-dir=<absolute path>
##
## Every shot starts from a known baseline, hides the gameplay HUD and 3D labels,
## builds only the set it needs, waits for streaming/commits, flashes a one-frame
## magenta capture marker, then plays its timed action list.

const SHOTS_PATH := "res://trailer/shots.json"
const MATERIAL_BAY_SCRIPT := preload("res://trailer/material_bay.gd")
const CAVE_SET_SCRIPT := preload("res://trailer/cave_set.gd")

var demo: Node3D
var shot_id: String = ""

var _manifest: Dictionary = {}
var _shot: Dictionary = {}
var _fps := 60
var _duration := 3.0
var _lead := 0.35
var _tail := 0.35
var _elapsed := 0.0
var _start_frame := 0
var _started := false
var _events_done: Dictionary = {}
var _camera_keys: Array = []
var _material_bay: Node3D
var _cave_set: Node3D
var _save_dir := ""
var _marker_layer: CanvasLayer
var _marker_rect: ColorRect
var _overlay_layer: CanvasLayer
var _tool_panel: ColorRect
var _tool_swatch: ColorRect
var _tool_label: Label
var _target_dot_outer: TextureRect
var _target_dot_inner: TextureRect
var _ui_scale := 1.0
var _shake_time := 0.0
var _shake_duration := 0.0
var _shake_amp := 0.0
var _headlamp: SpotLight3D

func _ready() -> void:
	process_priority = 100
	print("[Trailer] director ready shot=", shot_id)
	_manifest = _load_json(SHOTS_PATH)
	var shots: Dictionary = _manifest.get("shots", {})
	_shot = shots.get(shot_id, {})
	if _shot.is_empty():
		push_error("[Trailer] Unknown shot id: " + shot_id)
		get_tree().quit()
		return
	_fps = int(_shot.get("fps", _manifest.get("fps", 60)))
	_duration = float(_shot.get("duration", 3.0))
	_lead = float(_shot.get("lead", 0.35))
	_tail = float(_shot.get("tail", 0.35))
	_camera_keys = _shot.get("camera_path", [])
	_save_dir = _get_cli_arg("--trailer-save-dir=")
	if _save_dir.is_empty():
		_save_dir = ProjectSettings.globalize_path("res://trailer_saves/" + shot_id)
	DirAccess.make_dir_recursive_absolute(_save_dir)
	_create_marker()
	_create_overlay()
	if bool(_shot.get("headlamp", false)):
		_create_headlamp()
	demo.trailer_hide_ui(true)
	_apply_initial_camera()
	call_deferred("_run")

func _process(_delta: float) -> void:
	if not _started:
		return
	_elapsed = float(Engine.get_process_frames() - _start_frame) / float(_fps)
	_update_camera(_elapsed)
	_fire_events()
	if _elapsed >= _duration:
		_started = false
		_finish()

func _load_json(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("[Trailer] Cannot open " + path)
		return {}
	var parsed = JSON.parse_string(file.get_as_text())
	if typeof(parsed) != TYPE_DICTIONARY:
		push_error("[Trailer] Invalid JSON in " + path)
		return {}
	return parsed

func _get_cli_arg(prefix: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return arg.substr(prefix.length())
	return ""

func _create_marker() -> void:
	_marker_layer = CanvasLayer.new()
	_marker_layer.name = "TrailerMarkerLayer"
	_marker_layer.layer = 100
	add_child(_marker_layer)
	_marker_rect = ColorRect.new()
	_marker_rect.name = "CaptureMarker"
	_marker_rect.position = Vector2.ZERO
	_marker_rect.size = Vector2(16, 16)
	_marker_rect.color = Color(1.0, 0.0, 1.0, 1.0)
	_marker_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_marker_rect.visible = false
	_marker_layer.add_child(_marker_rect)

func _create_overlay() -> void:
	_overlay_layer = CanvasLayer.new()
	_overlay_layer.name = "TrailerOverlay"
	_overlay_layer.layer = 95
	add_child(_overlay_layer)

	_ui_scale = maxf(0.35, get_viewport().get_visible_rect().size.y / 1080.0)
	var s := _ui_scale

	_tool_panel = ColorRect.new()
	_tool_panel.name = "ToolPanel"
	var _vp := get_viewport().get_visible_rect().size
	_tool_panel.position = Vector2(_vp.x * 0.5 - 215.0 * s, 48.0 * s)
	_tool_panel.size = Vector2(430, 64) * s
	_tool_panel.color = Color(0.02, 0.025, 0.03, 0.72)
	_tool_panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_tool_panel.visible = false
	_overlay_layer.add_child(_tool_panel)

	_tool_swatch = ColorRect.new()
	_tool_swatch.position = Vector2(14, 14) * s
	_tool_swatch.size = Vector2(36, 36) * s
	_tool_swatch.color = Color(1.0, 0.5, 0.1, 1.0)
	_tool_swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_tool_panel.add_child(_tool_swatch)

	_tool_label = Label.new()
	_tool_label.position = Vector2(64, 12) * s
	_tool_label.size = Vector2(350, 40) * s
	_tool_label.add_theme_font_size_override("font_size", int(maxf(9.0, 26.0 * s)))
	_tool_label.add_theme_color_override("font_color", Color(0.94, 0.98, 0.95, 1.0))
	_tool_label.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.8))
	_tool_label.add_theme_constant_override("shadow_offset_x", int(maxf(1.0, 2.0 * s)))
	_tool_label.add_theme_constant_override("shadow_offset_y", int(maxf(1.0, 2.0 * s)))
	_tool_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_tool_panel.add_child(_tool_label)

	_target_dot_outer = TextureRect.new()
	_target_dot_outer.name = "TargetDotOuter"
	_target_dot_outer.anchor_left = 0.5
	_target_dot_outer.anchor_top = 0.5
	_target_dot_outer.anchor_right = 0.5
	_target_dot_outer.anchor_bottom = 0.5
	_target_dot_outer.offset_left = -11.0 * s
	_target_dot_outer.offset_top = -11.0 * s
	_target_dot_outer.offset_right = 11.0 * s
	_target_dot_outer.offset_bottom = 11.0 * s
	_target_dot_outer.texture = _make_dot_texture(64, Color(0.0, 0.0, 0.0, 0.78))
	_target_dot_outer.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_target_dot_outer.stretch_mode = TextureRect.STRETCH_SCALE
	_target_dot_outer.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
	_target_dot_outer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_target_dot_outer.visible = false
	_overlay_layer.add_child(_target_dot_outer)

	_target_dot_inner = TextureRect.new()
	_target_dot_inner.name = "TargetDotInner"
	_target_dot_inner.anchor_left = 0.5
	_target_dot_inner.anchor_top = 0.5
	_target_dot_inner.anchor_right = 0.5
	_target_dot_inner.anchor_bottom = 0.5
	_target_dot_inner.offset_left = -5.0 * s
	_target_dot_inner.offset_top = -5.0 * s
	_target_dot_inner.offset_right = 5.0 * s
	_target_dot_inner.offset_bottom = 5.0 * s
	_target_dot_inner.texture = _make_dot_texture(64, Color(1.0, 1.0, 1.0, 0.95))
	_target_dot_inner.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_target_dot_inner.stretch_mode = TextureRect.STRETCH_SCALE
	_target_dot_inner.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
	_target_dot_inner.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_target_dot_inner.visible = false
	_overlay_layer.add_child(_target_dot_inner)

func _create_headlamp() -> void:
	_headlamp = SpotLight3D.new()
	_headlamp.name = "TrailerHeadlamp"
	_headlamp.spot_range = 45.0
	_headlamp.spot_angle = 58.0
	_headlamp.spot_angle_attenuation = 0.8
	_headlamp.light_energy = 1.5
	_headlamp.light_color = Color(1.0, 0.93, 0.82)
	_headlamp.shadow_enabled = false
	demo.camera.add_child(_headlamp)

	var fill := OmniLight3D.new()
	fill.name = "TrailerHeadlampFill"
	fill.omni_range = 14.0
	fill.light_energy = 0.3
	fill.light_color = Color(0.7, 0.82, 1.0)
	fill.shadow_enabled = false
	demo.camera.add_child(fill)

func _make_dot_texture(size: int, color: Color) -> ImageTexture:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	var radius := float(size) * 0.5
	var center := Vector2(radius, radius)
	for y in range(size):
		for x in range(size):
			if Vector2(float(x) + 0.5, float(y) + 0.5).distance_to(center) <= radius - 0.8:
				image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)

func _show_tool_ui(label_text: String, swatch_color: Color, key: String) -> void:
	if _tool_panel == null:
		return
	_tool_panel.visible = true
	_tool_swatch.color = swatch_color
	if key.is_empty():
		_tool_label.text = label_text
	else:
		_tool_label.text = "[%s]  %s" % [key, label_text]

func _hide_tool_ui() -> void:
	if _tool_panel != null:
		_tool_panel.visible = false

func _set_target_dot(visible: bool) -> void:
	if _target_dot_outer != null:
		_target_dot_outer.visible = visible
	if _target_dot_inner != null:
		_target_dot_inner.visible = visible

func _run() -> void:
	print("[Trailer] run start shot=", shot_id)
	await get_tree().process_frame
	# Capture profile: spend main-thread budget on streaming instead of gating
	# collision to the whole resident world. Visual output is unchanged; the
	# separate benchmark suite remains the performance-of-record.
	demo.terrain.upload_budget_ms = 16.0
	demo.terrain.collision_distance_chunks = 6 if shot_id == "debug_xray" else 2
	if bool(_shot.get("reset_baseline", false)):
		demo.trailer_reset_baseline()
	await _wait_for_world(30.0)

	if bool(_shot.get("material_bay", false)):
		_material_bay = MATERIAL_BAY_SCRIPT.new()
		_material_bay.name = "TrailerMaterialBay"
		add_child(_material_bay)
		_material_bay.setup(demo.terrain)
		await _wait_for_world(30.0)

	if bool(_shot.get("cave_set", false)):
		_cave_set = CAVE_SET_SCRIPT.new()
		_cave_set.name = "TrailerCaveSet"
		add_child(_cave_set)
		await _cave_set.setup(demo.terrain)

	demo.trailer_clear_history()

	_apply_mode_and_camera()
	await _wait_for_world(30.0)
	await _run_pre_actions()
	await _wait_for_world(30.0)

	if bool(_shot.get("wait_path", false)):
		await _wait_for_path(20.0)

	await _wait_for_world(10.0)
	_set_target_dot(bool(_shot.get("target_dot", false)))
	print("[Trailer] pre-marker shot=", shot_id, " frame=", Engine.get_process_frames())
	_write_marker()
	_show_marker()
	await get_tree().process_frame
	_hide_marker()
	_start_frame = Engine.get_process_frames()
	_elapsed = 0.0
	_events_done.clear()
	_started = true

func _finish() -> void:
	print("[Trailer] finish shot=", shot_id, " elapsed=", _elapsed, " duration=", _duration)
	if bool(_shot.get("save_on_finish", false)):
		demo.trailer_save_world()
	await get_tree().create_timer(0.2).timeout
	get_tree().quit()

func _wait_for_world(timeout: float) -> void:
	var elapsed := 0.0
	while elapsed < timeout:
		if demo.terrain.core_idle() and demo.terrain.fully_committed():
			await get_tree().create_timer(0.35).timeout
			if demo.terrain.core_idle() and demo.terrain.fully_committed():
				return
		await get_tree().process_frame
		elapsed += get_process_delta_time()
	push_warning("[Trailer] World wait timed out for " + shot_id)

func _wait_for_path(timeout: float) -> void:
	var elapsed := 0.0
	while elapsed < timeout:
		if demo.trailer_path_ready():
			return
		await get_tree().process_frame
		elapsed += get_process_delta_time()
	push_warning("[Trailer] Nav path wait timed out for " + shot_id)

func _apply_initial_camera() -> void:
	var mode := String(_shot.get("mode", "fly"))
	if mode == "play":
		var play: Dictionary = _shot.get("play", {})
		var stand: Array = play.get("stand", [16.0, 64.0])
		var target := _resolve_target(play.get("look_at", [16.0, 80.0, 0.0]))
		if target == Vector3.ZERO:
			target = Vector3(16.0, 80.0, 0.0)
		var ground: float = float(demo.terrain.get_surface_height(float(stand[0]), float(stand[1])))
		if not is_finite(ground):
			ground = 70.0
		demo.trailer_set_fly_camera(
			Vector3(float(stand[0]), ground + 2.2, float(stand[1])),
			target,
			float(play.get("fov", 75.0))
		)
	elif not _camera_keys.is_empty():
		_apply_camera_key(0.0)

func _apply_mode_and_camera() -> void:
	var mode := String(_shot.get("mode", "fly"))
	if mode == "play":
		var play: Dictionary = _shot.get("play", {})
		var stand: Array = play.get("stand", [16.0, 64.0])
		var target := _resolve_target(play.get("look_at", [16.0, 80.0, 0.0]))
		if target == Vector3.ZERO:
			target = Vector3(16.0, 80.0, 0.0)
		demo.trailer_enter_play_on_surface(float(stand[0]), float(stand[1]), target)
		demo.trailer_set_fov(float(play.get("fov", 75.0)))
	else:
		_apply_camera_key(0.0)

func _run_pre_actions() -> void:
	for action in _shot.get("pre_actions", []):
		_execute_action(action)
		var delay := float(action.get("delay", 0.0))
		if delay > 0.0:
			await get_tree().create_timer(delay).timeout

func _fire_events() -> void:
	var events: Array = _shot.get("events", [])
	for i in range(events.size()):
		if _events_done.has(i):
			continue
		var event: Dictionary = events[i]
		if _elapsed >= float(event.get("t", 0.0)):
			_events_done[i] = true
			_execute_action(event)

func _execute_action(action: Dictionary) -> void:
	var kind := String(action.get("action", ""))
	match kind:
		"aim":
			demo.trailer_aim_at(_resolve_target(action.get("target", [0, 0, 0])))
		"swing":
			demo.trailer_swing_pickaxe()
		"tool":
			var tool := _tool_id(String(action.get("tool", "explode")))
			demo.trailer_apply_tool(tool, _resolve_target(action.get("target", [0, 0, 0])))
		"explode":
			demo.trailer_trigger_explosion(
				_resolve_target(action.get("target", [0, 0, 0])),
				float(action.get("radius", 8.5))
			)
		"large_explode":
			demo.trailer_trigger_explosion(
				_resolve_target(action.get("target", [0, 0, 0])),
				float(action.get("radius", 18.0))
			)
		"add_material":
			demo.trailer_add_material(
				_resolve_target(action.get("target", [0, 0, 0])),
				float(action.get("radius", 6.0)),
				int(action.get("material", 1))
			)
		"elemental":
			demo.trailer_trigger_elemental(
				_resolve_target(action.get("target", [0, 0, 0])),
				float(action.get("radius", 8.5)),
				int(action.get("element", 1))
			)
		"undo":
			demo.trailer_undo()
		"redo":
			demo.trailer_redo()
		"save":
			demo.trailer_save_world()
		"reset_baseline":
			demo.trailer_reset_baseline()
		"clear_history":
			demo.trailer_clear_history()
		"place_companion":
			demo.trailer_place_companion(_resolve_target(action.get("target", [0, 0, 0])))
		"path_goal":
			demo.trailer_set_path_goal(_resolve_target(action.get("target", [0, 0, 0])))
		"set_fov":
			demo.trailer_set_fov(float(action.get("value", 75.0)))
		"set_tool":
			var tool_name := String(action.get("tool", ""))
			if not tool_name.is_empty():
				demo.trailer_set_active_tool(_tool_id(tool_name))
			var swatch: Array = action.get("color", [1.0, 1.0, 1.0, 1.0])
			_show_tool_ui(
				String(action.get("label", "")),
				Color(float(swatch[0]), float(swatch[1]), float(swatch[2]), float(swatch[3])),
				String(action.get("key", ""))
			)
		"hide_tool":
			_hide_tool_ui()
		"target_dot":
			_set_target_dot(bool(action.get("visible", true)))
		"shake":
			_shake_time = float(action.get("time", 0.35))
			_shake_duration = maxf(0.001, _shake_time)
			_shake_amp = float(action.get("amplitude", 0.18))
		_:
			pass

func _update_camera(t: float) -> void:
	var mode := String(_shot.get("mode", "fly"))
	if mode == "fly":
		_apply_camera_at(t)
		if _shake_time > 0.0:
			_shake_time = maxf(0.0, _shake_time - get_process_delta_time())
			var amp := _shake_amp * (_shake_time / _shake_duration)
			demo.camera.global_position += Vector3(
				sin(t * 91.0) * amp,
				cos(t * 73.0) * amp * 0.6,
				sin(t * 57.0) * amp * 0.4
			)
			if demo._fly_pcam != null:
				demo._fly_pcam.global_transform = demo.camera.global_transform
	else:
		var play: Dictionary = _shot.get("play", {})
		if play.has("look_path"):
			var lp: Array = play["look_path"]
			var target := _sample_camera_target(lp, t)
			if target != Vector3.ZERO:
				demo.trailer_aim_at(target)

func _apply_camera_at(t: float) -> void:
	if _camera_keys.is_empty():
		return
	if _camera_keys.size() == 1:
		_apply_camera_key(0.0)
		return
	for i in range(_camera_keys.size() - 1):
		var a: Dictionary = _camera_keys[i]
		var b: Dictionary = _camera_keys[i + 1]
		var ta := float(a.get("t", 0.0))
		var tb := float(b.get("t", ta + 1.0))
		if t >= ta and t <= tb:
			var u := clampf((t - ta) / maxf(0.001, tb - ta), 0.0, 1.0)
			u = u * u * (3.0 - 2.0 * u)
			var pa := _resolve_vector(a.get("pos", [0, 0, 0]))
			var pb := _resolve_vector(b.get("pos", pa))
			var la := _resolve_target(a.get("look", [0, 0, 0]))
			var lb := _resolve_target(b.get("look", la))
			var fa := float(a.get("fov", 75.0))
			var fb := float(b.get("fov", fa))
			demo.trailer_set_fly_camera(pa.lerp(pb, u), la.lerp(lb, u), lerpf(fa, fb, u))
			return
	_apply_camera_key(_camera_keys.size() - 1)

func _apply_camera_key(index: int) -> void:
	if _camera_keys.is_empty():
		return
	var key: Dictionary = _camera_keys[index]
	demo.trailer_set_fly_camera(
		_resolve_vector(key.get("pos", [0, 0, 0])),
		_resolve_target(key.get("look", [0, 0, 0])),
		float(key.get("fov", 75.0))
	)

func _sample_camera_target(keys: Array, t: float) -> Vector3:
	if keys.is_empty():
		return Vector3.ZERO
	if keys.size() == 1:
		return _resolve_target(keys[0].get("look", [0, 0, 0]))
	for i in range(keys.size() - 1):
		var a: Dictionary = keys[i]
		var b: Dictionary = keys[i + 1]
		var ta := float(a.get("t", 0.0))
		var tb := float(b.get("t", ta + 1.0))
		if t >= ta and t <= tb:
			var u := clampf((t - ta) / maxf(0.001, tb - ta), 0.0, 1.0)
			return _resolve_target(a.get("look", [0, 0, 0])).lerp(
				_resolve_target(b.get("look", [0, 0, 0])), u
			)
	return _resolve_target(keys[keys.size() - 1].get("look", [0, 0, 0]))

func _resolve_vector(value) -> Vector3:
	if value is String:
		return _resolve_target(value)
	if value is Array and value.size() >= 3:
		return Vector3(float(value[0]), float(value[1]), float(value[2]))
	if value is Dictionary:
		return _resolve_target(value)
	return Vector3.ZERO

func _resolve_target(value) -> Vector3:
	if value is String:
		if value.begins_with("material:") and _material_bay != null:
			return _material_bay.target(value.substr(9))
		if value.begins_with("cave:") and _cave_set != null:
			return _cave_set.target(value.substr(5))
		if value.begins_with("statue:"):
			return demo.trailer_statue_center(value.substr(7))
		if value == "companion":
			return demo.ai_companion.global_position
		return Vector3.ZERO
	if value is Array and value.size() >= 3:
		return Vector3(float(value[0]), float(value[1]), float(value[2]))
	if value is Dictionary:
		if value.has("ground"):
			var xz: Array = value["ground"]
			return demo.trailer_ground_point(
				float(xz[0]),
				float(xz[1]),
				float(value.get("y_offset", 0.0))
			)
		if value.has("pos"):
			return _resolve_vector(value["pos"])
	return Vector3.ZERO

func _tool_id(tool_name: String) -> int:
	match tool_name:
		"add":
			return 0
		"paint":
			return 1
		"paint_delete":
			return 2
		"boulder":
			return 3
		"small":
			return 4
		"explode":
			return 5
		"large":
			return 6
		"water":
			return 7
		"frost":
			return 8
		"lava":
			return 9
	return 5

func _write_marker() -> void:
	var frame := Engine.get_process_frames()
	var path := _save_dir.path_join("marker.txt")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_line(str(frame))
		file.store_line(shot_id)
		file.store_line(str(_fps))
		file.store_line(str(_duration))
		file.store_line(str(_lead))
		file.store_line(str(_tail))

func _show_marker() -> void:
	if _marker_rect != null:
		_marker_rect.visible = true

func _hide_marker() -> void:
	if _marker_rect != null:
		_marker_rect.visible = false
