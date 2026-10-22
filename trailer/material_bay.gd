extends Node3D
## Deterministic play-mode material set for the trailer.
##
## The bay is built from real VoxelTerrain3D material fills, not preview meshes.
## Each pillar is a short stack of spheres seated on the procedural surface so the
## pickaxe hits the actual TSDF material and the shader damage ramp.

const BAY_CENTER_X := 16.0
const BAY_Z := 44.5
const PILLARS := [
	{"name": "soil", "material": 1, "offset": -9.0, "radius": 2.55, "height": 8.4},
	{"name": "hard_rock", "material": 2, "offset": -4.5, "radius": 2.55, "height": 8.4},
	{"name": "boulder", "material": 7, "offset": 0.0, "radius": 2.65, "height": 8.8},
	{"name": "obsidian", "material": 5, "offset": 4.5, "radius": 2.55, "height": 8.4},
	{"name": "ice", "material": 6, "offset": 9.0, "radius": 2.55, "height": 8.4},
]

var _targets: Dictionary = {}
var _built := false

func setup(terrain: VoxelTerrain3D) -> void:
	if _built:
		return
	_built = true
	for pillar in PILLARS:
		var x := BAY_CENTER_X + float(pillar["offset"])
		var surface := terrain.get_surface_height(x, BAY_Z)
		if not is_finite(surface):
			surface = 70.0
		var base := Vector3(x, surface, BAY_Z)
		var radius := float(pillar["radius"])
		var material := int(pillar["material"])
		var levels := [
			{"y": 2.0, "scale": 1.12},
			{"y": 5.2, "scale": 0.86},
		]
		for level in levels:
			var pos: Vector3 = base + Vector3(0.0, float(level["y"]), 0.0)
			terrain.add_terrain_with_material(pos, radius * float(level["scale"]), material)
		_targets[String(pillar["name"])] = base + Vector3(0.0, float(pillar["height"]) * 0.58, 0.0)
		_targets[String(pillar["name"]) + "_base"] = base

	print("[Trailer] Material bay built at x=%.1f z=%.1f" % [BAY_CENTER_X, BAY_Z])

func target(name: String) -> Vector3:
	return _targets.get(name, Vector3.ZERO)

func center() -> Vector3:
	return Vector3(BAY_CENTER_X, 0.0, BAY_Z)

func play_stand_point(terrain: VoxelTerrain3D) -> Vector3:
	var stand_z := BAY_Z + 20.0
	var surface := terrain.get_surface_height(BAY_CENTER_X, stand_z)
	if not is_finite(surface):
		surface = 70.0
	return Vector3(BAY_CENTER_X, surface, stand_z)
