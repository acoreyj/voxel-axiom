extends Node3D
## Trailer-only underground cave tunnel built from real terrain CSG.
##
## A long covered tunnel is carved below the surface, with a lava pool at the
## far end and a few lights along the route. The camera stays on the tunnel
## axis, so the shot has a clear vanishing point instead of a wall close-up.

const CENTER_X := 88.0
const CENTER_Z := 20.0

var _targets: Dictionary = {}

func setup(terrain: VoxelTerrain3D) -> void:
	var surface := terrain.get_surface_height(CENTER_X, CENTER_Z)
	if not is_finite(surface):
		surface = 70.0
	var tunnel_y := surface - 16.0

	# Carve a long covered tunnel with heavily overlapping spheres.
	for z in range(42, -42, -4):
		terrain.trigger_explosion(Vector3(CENTER_X, tunnel_y, float(z)), 12.0)
	# Entrance shaft.
	for y in [surface - 2.0, surface - 6.0, surface - 10.0]:
		terrain.trigger_explosion(Vector3(CENTER_X, y, 42.0), 6.0)
	await _wait_for_world(terrain, 45.0)

	# Lava at the far end gives the tunnel a strong glow and a clear depth cue.
	terrain.add_terrain_with_material(Vector3(CENTER_X, surface - 29.0, -34.0), 5.0, 3)
	terrain.add_terrain_with_material(Vector3(CENTER_X, surface - 29.0, -8.0), 3.0, 3)
	terrain.add_terrain_with_material(Vector3(CENTER_X, surface - 29.0, 14.0), 2.5, 6)
	await _wait_for_world(terrain, 25.0)

	_add_light(Vector3(CENTER_X, surface - 24.0, -34.0), Color(1.0, 0.42, 0.16), 42.0, 40.0)
	_add_light(Vector3(CENTER_X, surface - 24.0, -8.0), Color(1.0, 0.36, 0.12), 22.0, 30.0)
	_add_light(Vector3(CENTER_X, surface - 24.0, 18.0), Color(0.45, 0.72, 1.0), 14.0, 28.0)

	_targets["outside"] = Vector3(CENTER_X, surface + 6.0, 58.0)
	_targets["entrance"] = Vector3(CENTER_X, tunnel_y, 40.0)
	_targets["inside_start"] = Vector3(CENTER_X, tunnel_y, 36.0)
	_targets["inside_mid"] = Vector3(CENTER_X, tunnel_y, 10.0)
	_targets["inside_end"] = Vector3(CENTER_X, tunnel_y, -30.0)
	_targets["inner"] = _targets["inside_mid"]
	_targets["goal"] = _targets["inside_end"]
	_targets["look_in"] = _targets["inside_mid"]
	print("[Trailer] Underground cave tunnel carved at x=%.1f z=%.1f" % [CENTER_X, CENTER_Z])

func _wait_for_world(terrain: VoxelTerrain3D, timeout: float) -> void:
	var t := 0.0
	while t < timeout:
		if terrain.core_idle() and terrain.fully_committed():
			await get_tree().create_timer(0.35).timeout
			if terrain.core_idle() and terrain.fully_committed():
				return
		await get_tree().process_frame
		t += get_process_delta_time()

func target(name: String) -> Vector3:
	return _targets.get(name, Vector3.ZERO)

func _add_light(pos: Vector3, color: Color, energy: float, range_m: float) -> void:
	var light := OmniLight3D.new()
	light.position = pos
	light.light_color = color
	light.light_energy = energy
	light.omni_range = range_m
	light.shadow_enabled = false
	add_child(light)
