extends Node3D
## HLD v4.0 GDScript Demo Controller (`terrain_demo.gd`)
## Streams 32^3 chunks around the active Camera3D, fires asynchronous R+delta
## sphere explosions on left-click raycast, and queries 3D cave navigation paths on right-click.

@export var move_speed: float = 24.0
@export var fast_multiplier: float = 3.0
@export var mouse_sensitivity: float = 0.0025
@export var explosion_radius: float = 8.5
@export var max_ray_distance: float = 250.0

@onready var terrain: VoxelTerrain3D = $VoxelTerrain3D
@onready var ai_companion: VoxelAICompanion = $VoxelAICompanion
@onready var camera: Camera3D = $Camera3D

var _yaw: float = 0.0
var _pitch: float = -0.25
var _velocity: Vector3 = Vector3.ZERO
var _committed_chunk_count: int = 0

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	terrain.explosion_requested.connect(_on_explosion_requested)
	terrain.explosion_completed.connect(_on_explosion_completed)
	terrain.explosion_failed.connect(_on_explosion_failed)
	terrain.chunk_committed.connect(_on_chunk_committed)

	# Prime initial streaming focus around the camera's starting world position.
	terrain.set_stream_focus(camera.global_position, Vector3.ZERO)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch = clamp(_pitch - event.relative.y * mouse_sensitivity, -1.45, 1.45)
		camera.rotation = Vector3(_pitch, _yaw, 0.0)

	elif event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		else:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

	elif event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_fire_raycast_explosion()
		elif event.button_index == MOUSE_BUTTON_RIGHT:
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

	# Update streaming center + velocity bias every frame (Section 3.3, 4.2)
	terrain.set_stream_focus(camera.global_position, _velocity)

func _fire_raycast_explosion() -> void:
	var space_state := get_world_3d().direct_space_state
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * max_ray_distance
	var query := PhysicsRayQueryParameters3D.create(from, to)
	var hit := space_state.intersect_ray(query)
	if not hit.is_empty():
		terrain.trigger_explosion(hit.position, explosion_radius)
	else:
		# If looking into open cave air, detonate 24 m ahead of the camera
		terrain.trigger_explosion(from - camera.global_transform.basis.z * 24.0, explosion_radius)

func _query_companion_cave_path() -> void:
	var target_pos := camera.global_position
	if not ai_companion.is_path_region_synced(target_pos):
		print("[Nav3D] 3D 4x2x4 cave navigation group still baking async...")
		return
	var path: PackedVector3Array = ai_companion.get_voxel_path(ai_companion.global_position, target_pos)
	print("[Nav3D] Companion 3D cave path waypoints: ", path.size())

func _on_explosion_requested(pos: Vector3, radius: float) -> void:
	print("[VoxelTerrain3D] Explosion requested at ", pos, " radius=", radius)

func _on_explosion_completed(pos: Vector3, radius: float, affected_chunks: int) -> void:
	print("[VoxelTerrain3D] Explosion completed at ", pos, " radius=", radius, " chunks=", affected_chunks)

func _on_explosion_failed(reason: String) -> void:
	push_warning("[VoxelTerrain3D] Explosion failed: " + reason)

func _on_chunk_committed(_chunk_key: int) -> void:
	_committed_chunk_count += 1
