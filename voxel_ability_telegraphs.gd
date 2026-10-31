class_name VoxelAbilityTelegraphs
extends Node3D
## Terrain-Draped 3D Ability Telegraphs & Expanding Impact Decals (`voxel_ability_telegraphs.gd`)
## Uses `VoxelTerrain3D`'s O(1) `raycast_terrain`, `sample_sdf_normal`, `get_material_at`,
## `get_dominant_material_in_sphere`, and `measure_wall_thickness` to render:
##   1. Surface-Draped AoE Rings & Spokes that conform to jagged craters, hills, and slopes
##      by offsetting every perimeter vertex along its analytical SDF normal (`+0.14 m`).
##   2. Live Lash "Ground Strike" Dive Line + Dynamic Elemental Combo Ring (changes color
##      automatically when aiming at Lava -> Magma Eruption, Ice -> Cryo Shatter, Water -> Hydro Geyser).
##   3. Live Abrams "Shoulder Charge" Wall-Pin / Thin-Wall Breach / Glancing Slide Indicator
##      (color-coded by `measure_wall_thickness` and `impact_cos`).
##   4. McGinnis "Spectral Wall" 3D Curtain Footprint & Kelvin "Ice Dome" Hemisphere Arches.
##   5. Expanding Terrain-Draped Post-Impact Shockwave Rings on ability/mortar detonations.

enum TelegraphMode {
	AUTO_CONTEXT,   # Automatically shows Ground Strike in air, Wall-Pin during charge, or aimed construct preview
	SPECTRAL_WALL,  # Always previews the 14m McGinnis Spectral Wall footprint at crosshair
	ICE_DOME,       # Always previews the 8.5m Kelvin Frozen Shelter dome around hero
	OFF,            # Hides live telegraphs (expanding impact shockwaves still render)
}

const TELEGRAPH_MODE_NAMES := [
	"Auto Context",
	"Spectral Wall Preview",
	"Ice Dome Preview",
	"Off",
]

const MAT_AIR := 0
const MAT_SOIL := 1
const MAT_HARD_ROCK := 2
const MAT_LAVA := 3
const MAT_WATER := 4
const MAT_OBSIDIAN := 5
const MAT_ICE := 6

class ExpandingImpactDecal extends RefCounted:
	var center: Vector3 = Vector3.ZERO
	var normal: Vector3 = Vector3.UP
	var max_radius: float = 5.0
	var color: Color = Color(1.0, 0.55, 0.1)
	var duration: float = 0.65
	var age: float = 0.0

@export var telegraph_mode: int = TelegraphMode.AUTO_CONTEXT
@export var fly_mode_tool_ring_enabled: bool = true
@export var ring_segments: int = 32
@export var surface_normal_offset: float = 0.15

var terrain: Object = null
var active_combo_preview_text: String = ""

var _mesh_inst: MeshInstance3D = null
var _imm_mesh: ImmediateMesh = null
var _impact_decals: Array[ExpandingImpactDecal] = []
var _pulse_phase: float = 0.0

func _ready() -> void:
	_mesh_inst = MeshInstance3D.new()
	_mesh_inst.name = "AbilityTelegraphImmediateMesh"
	_imm_mesh = ImmediateMesh.new()
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.no_depth_test = false
	_mesh_inst.mesh = _imm_mesh
	_mesh_inst.material_override = mat
	add_child(_mesh_inst)

func cycle_telegraph_mode() -> String:
	telegraph_mode = (telegraph_mode + 1) % TELEGRAPH_MODE_NAMES.size()
	return TELEGRAPH_MODE_NAMES[telegraph_mode]

## Spawns an expanding, terrain-draped post-impact shockwave decal ring.
func spawn_impact_ring_decal(
	center: Vector3,
	normal: Vector3,
	max_radius: float,
	color: Color = Color(1.0, 0.55, 0.15),
	duration: float = 0.65
) -> void:
	var d := ExpandingImpactDecal.new()
	d.center = center
	d.normal = normal.normalized() if normal.length_squared() > 1e-4 else Vector3.UP
	d.max_radius = maxf(1.0, max_radius)
	d.color = color
	d.duration = maxf(0.15, duration)
	_impact_decals.append(d)

## Updates and renders all active 3D telegraphs and expanding impact decals for the frame.
func update_telegraphs(
	delta: float,
	is_play_mode: bool,
	hero_feet: Vector3,
	hero_yaw: float,
	hero_state: int,
	is_grounded: bool,
	crosshair_hit: Vector3,
	fly_tool_radius: float,
	slam_radius: float,
	dome_radius: float,
	wall_half_width: float
) -> void:
	_pulse_phase = fmod(_pulse_phase + delta * 4.5, TAU)
	active_combo_preview_text = ""
	if _imm_mesh == null or terrain == null:
		return

	_imm_mesh.clear_surfaces()
	_imm_mesh.surface_begin(Mesh.PRIMITIVE_LINES)

	var drew_any := false
	var pulse := 0.78 + 0.22 * sin(_pulse_phase)

	# 1. Step and draw expanding post-impact shockwave decals
	for i in range(_impact_decals.size() - 1, -1, -1):
		var dec := _impact_decals[i]
		dec.age += delta
		if dec.age >= dec.duration:
			_impact_decals.remove_at(i)
			continue
		var u := clampf(dec.age / dec.duration, 0.0, 1.0)
		var cur_r := lerpf(0.6, dec.max_radius, 1.0 - pow(1.0 - u, 2.2))
		var alpha := clampf(1.0 - u, 0.0, 1.0)
		var col := Color(dec.color.r, dec.color.g, dec.color.b, alpha)
		_draw_draped_circle(dec.center, cur_r, col, 24)
		_draw_draped_circle(dec.center, cur_r * 0.72, Color(col.r, col.g, col.b, alpha * 0.55), 16)
		drew_any = true

	# 2. Fly Mode: subtle 3D draped tool radius indicator at crosshair
	if not is_play_mode:
		if fly_mode_tool_ring_enabled and is_finite(crosshair_hit.x):
			var fly_col := Color(0.25, 0.95, 1.0, 0.65 * pulse)
			_draw_draped_circle(crosshair_hit, fly_tool_radius, fly_col, 28)
			_draw_surface_normal_pin(crosshair_hit, fly_col, 1.2)
			drew_any = true
		if drew_any:
			_imm_mesh.surface_end()
		else:
			_imm_mesh.clear_surfaces()
		return

	# 3. Play Mode Telegraphs
	if telegraph_mode != TelegraphMode.OFF:
		var fwd := Vector3(-sin(hero_yaw), 0.0, -cos(hero_yaw)).normalized()

		# A. Airborne Ground Strike Dive Line & Elemental Combo Landing Ring (hero_state == AIRBORNE or GROUND_SLAM)
		if not is_grounded and (telegraph_mode == TelegraphMode.AUTO_CONTEXT):
			var dive_vel := Vector3(fwd.x * 8.0, -28.0, fwd.z * 8.0)
			var dive_dir := dive_vel.normalized()
			var dive_origin := hero_feet + Vector3(0.0, 0.8, 0.0)
			var dive_hit: Vector3 = terrain.raycast_terrain(dive_origin, dive_origin + dive_dir * 95.0)
			if is_finite(dive_hit.x):
				var dom_mat: int = terrain.get_dominant_material_in_sphere(dive_hit, slam_radius * 0.7) if terrain.has_method("get_dominant_material_in_sphere") else MAT_SOIL
				var ring_col := Color(1.0, 0.82, 0.18, 0.90 * pulse)
				var eff_r := slam_radius
				if dom_mat == MAT_LAVA:
					ring_col = Color(1.0, 0.30, 0.08, 0.95 * pulse)
					eff_r *= 1.35
					active_combo_preview_text = "Slam Preview: MAGMA ERUPTION (+35% Radius + Burn)"
				elif dom_mat == MAT_ICE:
					ring_col = Color(0.20, 0.95, 1.0, 0.95 * pulse)
					eff_r *= 1.20
					active_combo_preview_text = "Slam Preview: CRYO SHATTER NOVA (Freeze)"
				elif dom_mat == MAT_WATER:
					ring_col = Color(0.20, 0.60, 1.0, 0.95 * pulse)
					active_combo_preview_text = "Slam Preview: HYDRO GEYSER"
				else:
					active_combo_preview_text = "Slam Preview: Seismic Crater (%.1fm)" % eff_r

				_add_line(dive_origin, dive_hit + Vector3(0.0, 0.18, 0.0), ring_col)
				_draw_draped_circle(dive_hit, eff_r, ring_col, ring_segments)
				_draw_draped_circle(dive_hit, eff_r * 0.5, Color(ring_col.r, ring_col.g, ring_col.b, 0.55), 18)
				_draw_draped_spokes(dive_hit, eff_r, ring_col, 8)
				drew_any = true

		# B. Shoulder Charge Swept Wall-Pin / Thin-Wall Breach Preview (hero_state == 6 -> SHOULDER_CHARGE, or AUTO near wall)
		if hero_state == 6 or telegraph_mode == TelegraphMode.AUTO_CONTEXT:
			var probe_dist := 14.0 if hero_state == 6 else 5.5
			var chest_pt := hero_feet + Vector3(0.0, 1.20, 0.0)
			var wall_hit: Vector3 = terrain.sweep_sphere(chest_pt, chest_pt + fwd * probe_dist, 0.42) if terrain.has_method("sweep_sphere") else terrain.raycast_terrain(chest_pt, chest_pt + fwd * probe_dist)
			if is_finite(wall_hit.x):
				var wall_n: Vector3 = terrain.sample_sdf_normal(wall_hit) if terrain.has_method("sample_sdf_normal") else -fwd
				if absf(wall_n.y) < 0.62:
					var wall_n_xz := Vector3(wall_n.x, 0.0, wall_n.z).normalized()
					var impact_cos := -fwd.dot(wall_n_xz)
					var wall_thick: float = terrain.measure_wall_thickness(wall_hit, fwd, 3.2) if terrain.has_method("measure_wall_thickness") else 99.0
					var wall_mat: int = terrain.get_material_at(wall_hit) if terrain.has_method("get_material_at") else MAT_SOIL
					var wall_dmg: float = terrain.get_damage_at(wall_hit) if terrain.has_method("get_damage_at") else 0.0
					var is_breachable := (wall_mat == MAT_SOIL or wall_mat == MAT_ICE or (wall_mat == MAT_HARD_ROCK and wall_dmg >= 0.60)) and wall_thick > 0.05 and wall_thick <= 2.2

					var pin_col := Color(1.0, 0.55, 0.10, 0.90 * pulse) # Solid Wall-Pin (Orange-Gold)
					if impact_cos < 0.60:
						pin_col = Color(0.30, 0.75, 1.0, 0.80 * pulse)  # Glancing Wall-Slide (Blue)
					elif is_breachable:
						pin_col = Color(0.20, 1.0, 0.45, 0.95 * pulse)  # Thin-Wall Breachable (Emerald Green)
						if active_combo_preview_text.is_empty():
							active_combo_preview_text = "Wall Ahead: BREACHABLE (%.1fm thin)" % wall_thick
					elif active_combo_preview_text.is_empty():
						active_combo_preview_text = "Wall Ahead: SOLID PIN ZONE (cos=%.2f)" % impact_cos

					_draw_oriented_wall_disc(wall_hit + wall_n * 0.14, wall_n, 1.35, pin_col)
					drew_any = true

		# C. Explicit Spectral Wall Curtain Telegraph (`telegraph_mode == SPECTRAL_WALL`)
		if telegraph_mode == TelegraphMode.SPECTRAL_WALL:
			var wall_center := crosshair_hit if (is_finite(crosshair_hit.x) and crosshair_hit.distance_to(hero_feet) <= 28.0) else (hero_feet + fwd * 10.0)
			_draw_spectral_wall_curtain_telegraph(wall_center, fwd, wall_half_width, 6.5, Color(1.0, 0.78, 0.22, 0.88 * pulse))
			active_combo_preview_text = "Telegraph: McGinnis Spectral Wall (Press 2 to Raise)"
			drew_any = true

		# D. Explicit Kelvin Ice Dome Hemisphere Telegraph (`telegraph_mode == ICE_DOME`)
		elif telegraph_mode == TelegraphMode.ICE_DOME:
			_draw_ice_dome_hemisphere_telegraph(hero_feet, dome_radius, Color(0.22, 0.95, 1.0, 0.85 * pulse))
			active_combo_preview_text = "Telegraph: Kelvin Frozen Shelter (Press 1 to Cast)"
			drew_any = true

	if drew_any:
		_imm_mesh.surface_end()
	else:
		_imm_mesh.clear_surfaces()

## Samples the terrain surface height near `sample_xz` and offsets along the SDF normal so
## rings hug craters, slopes, and ledges without clipping.
func _drape_point_on_surface(sample_pos: Vector3, max_up: float = 5.5, max_down: float = 9.0) -> Vector3:
	var from := sample_pos + Vector3(0.0, max_up, 0.0)
	var to := sample_pos - Vector3(0.0, max_down, 0.0)
	var hit: Vector3 = terrain.raycast_terrain(from, to)
	if is_finite(hit.x):
		var n: Vector3 = terrain.sample_sdf_normal(hit) if terrain.has_method("sample_sdf_normal") else Vector3.UP
		return hit + n * surface_normal_offset
	return sample_pos + Vector3(0.0, surface_normal_offset, 0.0)

func _draw_draped_circle(center: Vector3, radius: float, color: Color, segments: int = 28) -> void:
	var prev_pt := Vector3.ZERO
	for i in range(segments + 1):
		var angle := (float(i) / float(segments)) * TAU
		var raw_pt := center + Vector3(cos(angle) * radius, 0.0, sin(angle) * radius)
		var draped := _drape_point_on_surface(raw_pt)
		if i > 0:
			_add_line(prev_pt, draped, color)
		prev_pt = draped

func _draw_draped_spokes(center: Vector3, radius: float, color: Color, num_spokes: int = 8) -> void:
	var center_draped := _drape_point_on_surface(center)
	for i in range(num_spokes):
		var angle := (float(i) / float(num_spokes)) * TAU
		var rim_raw := center + Vector3(cos(angle) * radius, 0.0, sin(angle) * radius)
		var rim_draped := _drape_point_on_surface(rim_raw)
		_add_line(center_draped, rim_draped, Color(color.r, color.g, color.b, color.a * 0.6))

func _draw_surface_normal_pin(hit_pos: Vector3, color: Color, length: float = 1.2) -> void:
	var n: Vector3 = terrain.sample_sdf_normal(hit_pos) if terrain.has_method("sample_sdf_normal") else Vector3.UP
	var base := hit_pos + n * surface_normal_offset
	_add_line(base, base + n * length, color)

func _draw_oriented_wall_disc(center: Vector3, normal: Vector3, radius: float, color: Color, segments: int = 20) -> void:
	var n := normal.normalized() if normal.length_squared() > 1e-4 else Vector3.UP
	var tangent := n.cross(Vector3.UP)
	if tangent.length_squared() < 1e-4:
		tangent = n.cross(Vector3.RIGHT)
	tangent = tangent.normalized()
	var bitangent := n.cross(tangent).normalized()

	var prev := Vector3.ZERO
	for i in range(segments + 1):
		var angle := (float(i) / float(segments)) * TAU
		var pt := center + (tangent * cos(angle) + bitangent * sin(angle)) * radius
		if i > 0:
			_add_line(prev, pt, color)
		prev = pt
	_add_line(center, center + n * 1.1, color)

func _draw_spectral_wall_curtain_telegraph(center: Vector3, forward_dir: Vector3, half_width: float, height: float, color: Color) -> void:
	var right_dir := Vector3(-forward_dir.z, 0.0, forward_dir.x).normalized()
	if right_dir.length_squared() < 1e-4:
		right_dir = Vector3.RIGHT
	var cols := 10
	var prev_bot := Vector3.ZERO
	var prev_top := Vector3.ZERO
	for i in range(cols + 1):
		var u := (float(i) / float(cols)) * 2.0 - 1.0
		var raw_pt := center + right_dir * (u * half_width)
		var bot := _drape_point_on_surface(raw_pt, 6.0, 12.0)
		var top := bot + Vector3(0.0, height, 0.0)
		_add_line(bot, top, Color(color.r, color.g, color.b, color.a * 0.65))
		if i > 0:
			_add_line(prev_bot, bot, color)
			_add_line(prev_top, top, color)
		prev_bot = bot
		prev_top = top

func _draw_ice_dome_hemisphere_telegraph(center: Vector3, radius: float, color: Color) -> void:
	_draw_draped_circle(center, radius, color, ring_segments)
	# Two orthogonal overhead hemisphere arches
	var arch_segs := 18
	for axis in [Vector3.RIGHT, Vector3.FORWARD]:
		var prev := Vector3.ZERO
		for i in range(arch_segs + 1):
			var theta := (float(i) / float(arch_segs)) * PI
			var pt: Vector3 = center + axis * (cos(theta) * radius) + Vector3(0.0, sin(theta) * radius, 0.0)
			if i > 0:
				_add_line(prev, pt, Color(color.r, color.g, color.b, color.a * 0.55))
			prev = pt

func _add_line(a: Vector3, b: Vector3, color: Color) -> void:
	_imm_mesh.surface_set_color(color)
	_imm_mesh.surface_add_vertex(a)
	_imm_mesh.surface_set_color(color)
	_imm_mesh.surface_add_vertex(b)
