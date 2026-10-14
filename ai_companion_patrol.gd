extends VoxelAICompanion
## Demo gameplay driver for the pill companion (`voxel_terrain_demo.tscn`).
##
## Loops the node around a patrol route laid out as XZ points on the terrain
## surface (a triangle by default) and pins it to the ground every frame via
## VoxelTerrain3D.get_surface_height(x, z). Waypoint Y values are ignored — the
## height under the pill always comes from the terrain itself, so the companion
## walks down into craters and over new terrain as the world is edited.
##
## The node keeps its `VoxelAICompanion` role as a navigation query facade; this
## script is the gameplay that drives it, as documented in
## `docs/AI Companion & Navigation.md`.

@export var patrol_points: PackedVector2Array = [
	Vector2(16.0, 44.0), Vector2(44.0, 8.0), Vector2(-12.0, 8.0),
]
@export var patrol_speed: float = 6.0
@export var ground_offset: float = 0.0
@export var yaw_turn_speed: float = 8.0
@export var terrain_path: NodePath = ^"../VoxelTerrain3D"

@onready var _terrain: VoxelTerrain3D = get_node_or_null(terrain_path) as VoxelTerrain3D

var _edge: int = 0
var _pos: Vector2 = Vector2.ZERO

func _ready() -> void:
	if patrol_points.size() >= 2:
		_pos = patrol_points[0]
		_apply_transform(0.0, true)
		_face_edge(0.0)

func _process(delta: float) -> void:
	if _terrain == null or patrol_points.size() < 2:
		return

	var a := patrol_points[_edge]
	var b := patrol_points[(_edge + 1) % patrol_points.size()]
	var seg := b - a
	var seg_len := seg.length()
	if seg_len > 0.0001:
		var step := patrol_speed * delta
		if a.distance_to(_pos) + step >= seg_len:
			_pos = b
			_edge = (_edge + 1) % patrol_points.size()
			print("[Pill] waypoint %d/%d reached at %s (y=%.1f)" % [
				_edge, patrol_points.size(), _pos,
				_terrain.get_surface_height(_pos.x, _pos.y),
			])
		else:
			_pos += seg / seg_len * step

	_apply_transform(delta, false)
	_face_edge(delta)

func _apply_transform(delta: float, snap: bool) -> void:
	var target_y := _terrain.get_surface_height(_pos.x, _pos.y) + ground_offset
	var y := target_y if snap else lerpf(global_position.y, target_y, 1.0 - exp(-8.0 * delta))
	global_position = Vector3(_pos.x, y, _pos.y)

func _face_edge(delta: float) -> void:
	var a := patrol_points[_edge]
	var b := patrol_points[(_edge + 1) % patrol_points.size()]
	var dir := b - a
	if dir.length_squared() < 0.0001:
		return
	var target_yaw := atan2(-dir.x, -dir.y)
	if delta <= 0.0:
		rotation.y = target_yaw
	else:
		rotation.y = lerp_angle(rotation.y, target_yaw, minf(1.0, yaw_turn_speed * delta))
