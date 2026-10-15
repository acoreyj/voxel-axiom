extends VoxelAICompanion
## Demo gameplay driver for the pill companion (`voxel_terrain_demo.tscn`).
##
## Loops the node around a patrol route laid out as XZ points on the terrain
## surface (a triangle by default) and pins it to the ground every frame via
## VoxelTerrain3D.get_surface_height(x, z). Waypoint Y values are ignored — the
## height under the pill always comes from the terrain itself, so the companion
## walks down into craters and over new terrain as the world is edited.
##
## Placed boulders (and any other unclimbable rise) are steered around: the pill
## probes the ground a step ahead and, when it rises more than `max_step_height`,
## slides tangentially instead of climbing on top.
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
## Maximum ground rise the pill will climb before steering around it (boulders,
## cliff bases). Boulders are placed as near-vertical spheres, so their base
## always exceeds this and the pill walks around them instead of on top.
@export var max_step_height: float = 1.2
## How far ahead the pill probes the ground for an unclimbable rise.
@export var avoid_probe_distance: float = 1.6
@export var terrain_path: NodePath = ^"../VoxelTerrain3D"

@onready var _terrain: VoxelTerrain3D = get_node_or_null(terrain_path) as VoxelTerrain3D

var _edge: int = 0
var _pos: Vector2 = Vector2.ZERO
var _move_dir: Vector2 = Vector2.ZERO

func _ready() -> void:
	if patrol_points.size() >= 2:
		_pos = patrol_points[0]
		_apply_transform(0.0, true)
		_face_movement(0.0)

func _process(delta: float) -> void:
	if _terrain == null or patrol_points.size() < 2:
		return

	var b := patrol_points[(_edge + 1) % patrol_points.size()]
	var to_b := b - _pos
	var seg_dist := to_b.length()
	var step := patrol_speed * delta
	if seg_dist <= maxf(step, 0.0001):
		_pos = b
		_edge = (_edge + 1) % patrol_points.size()
		_move_dir = Vector2.ZERO
		print("[Pill] waypoint %d/%d reached at %s (y=%.1f)" % [
			_edge, patrol_points.size(), _pos,
			_terrain.get_surface_height(_pos.x, _pos.y),
		])
	else:
		var desired := to_b / seg_dist
		_move_dir = _steer_around_obstacles(desired)
		_pos += _move_dir * step

	_apply_transform(delta, false)
	_face_movement(delta)

## Returns `desired`, or the best tangential alternative when the ground ahead
## rises more than `max_step_height` (a placed boulder or a cliff base). This is
## what keeps the pill walking around boulders instead of climbing over them.
func _steer_around_obstacles(desired: Vector2) -> Vector2:
	if not _is_blocked_ahead(desired):
		return desired
	for angle in [0.96, -0.96, 1.57, -1.57]: # ~55 deg, then 90 deg, both sides
		var candidate: Vector2 = desired.rotated(angle)
		if not _is_blocked_ahead(candidate):
			return candidate
	return desired # boxed in on all sides: push forward and follow the ground

func _is_blocked_ahead(dir: Vector2) -> bool:
	var probe := _pos + dir * avoid_probe_distance
	var ground_here := _terrain.get_surface_height(_pos.x, _pos.y)
	var ground_ahead := _terrain.get_surface_height(probe.x, probe.y)
	return ground_ahead - ground_here > max_step_height

func _apply_transform(delta: float, snap: bool) -> void:
	var target_y := _terrain.get_surface_height(_pos.x, _pos.y) + ground_offset
	var y := target_y if snap else lerpf(global_position.y, target_y, 1.0 - exp(-8.0 * delta))
	global_position = Vector3(_pos.x, y, _pos.y)

func _face_movement(delta: float) -> void:
	var dir := _move_dir
	if dir.length_squared() < 0.0001:
		var a := patrol_points[_edge]
		var b := patrol_points[(_edge + 1) % patrol_points.size()]
		dir = b - a
	if dir.length_squared() < 0.0001:
		return
	var target_yaw := atan2(-dir.x, -dir.y)
	if delta <= 0.0:
		rotation.y = target_yaw
	else:
		rotation.y = lerp_angle(rotation.y, target_yaw, minf(1.0, yaw_turn_speed * delta))
