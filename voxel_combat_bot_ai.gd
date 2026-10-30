class_name VoxelCombatBotAI
extends Node
## Deadlock-Style Combat AI Bot & Trooper State Machine (`voxel_combat_bot_ai.gd`)
## Operates on `VoxelTerrain3D` + `VoxelAICompanion` + `VoxelAbilitySystem`:
##   - 5 Tactical States: `IDLE_FOLLOW`, `PURSUE_3D_NAV`, `STRAFE_ENGAGE`,
##     `HAZARD_ESCAPE` (leaps out of Lava / Hyper Beam fire), and `BREACH_COVER`
##     (chips through Kelvin Ice Domes or McGinnis Spectral Walls blocking LoS).
##   - Uses O(1) `sample_sdf`, `sample_sdf_normal`, and `get_material_at` for
##     cliff-safe strafing, wall-avoidance steering, and Lava hazard detection.
##   - Uses `raycast_terrain(bot_eye, hero_eye)` for true voxel Line-of-Sight (automatically
##     blocked when the player raises an Ice Dome or Spectral Wall).
##   - Fires sub-stepped Plasma Bolts and arcing Magma Mortars at the hero's predicted velocity.

signal bot_state_changed(new_state: int, state_name: String)
signal bot_fired_bolt(from_pos: Vector3, to_pos: Vector3, hit_hero: bool, damage: float)
signal bot_lobbed_mortar(origin: Vector3, target_pos: Vector3)
signal bot_breached_cover(wall_hit_pos: Vector3)
signal bot_escaped_hazard(hazard_material: int, jump_velocity: Vector3)

enum BotState {
	IDLE_FOLLOW,
	PURSUE_3D_NAV,
	STRAFE_ENGAGE,
	HAZARD_ESCAPE,
	BREACH_COVER,
}

const BOT_STATE_NAMES := [
	"Path Follow",
	"Flanking (3D Nav)",
	"Strafe & Engage",
	"Hazard Escape!",
	"Breaching Cover!",
]

const MAT_AIR := 0
const MAT_SOIL := 1
const MAT_HARD_ROCK := 2
const MAT_LAVA := 3
const MAT_WATER := 4
const MAT_OBSIDIAN := 5
const MAT_ICE := 6

class BotPlasmaBolt extends RefCounted:
	var position: Vector3 = Vector3.ZERO
	var velocity: Vector3 = Vector3.ZERO
	var radius: float = 0.28
	var damage: float = 18.0
	var is_mortar: bool = false
	var age: float = 0.0
	var max_lifetime: float = 3.2
	var mesh_instance: MeshInstance3D = null

@export var combat_enabled: bool = false
@export var move_speed: float = 7.8
@export var strafe_speed: float = 6.2
@export var preferred_min_range: float = 7.5
@export var preferred_max_range: float = 24.0
@export var repath_interval: float = 1.25
@export var primary_fire_interval: float = 0.75
@export var mortar_cooldown: float = 6.5
@export var breach_cooldown: float = 4.0
@export var bolt_speed: float = 34.0
@export var bolt_damage: float = 16.0
@export var mortar_damage: float = 45.0
@export var mortar_splash_radius: float = 3.6

var terrain: Object = null
var companion_node: Node3D = null
var ability_system: VoxelAbilitySystem = null
var projectile_parent: Node3D = null

var state: int = BotState.IDLE_FOLLOW
var has_line_of_sight: bool = false
var last_los_blocker_pos: Vector3 = Vector3(INF, INF, INF)
var vertical_vel: float = 0.0
var horizontal_vel: Vector3 = Vector3.ZERO
var active_bolts: Array[BotPlasmaBolt] = []

var _strafe_sign: float = 1.0
var _strafe_flip_timer: float = 0.0
var _repath_timer: float = 0.0
var _fire_timer: float = 0.4
var _mortar_timer: float = 2.5
var _breach_timer: float = 0.0
var _escape_timer: float = 0.0
var _nav_waypoints: PackedVector3Array = PackedVector3Array()
var _nav_index: int = 0

func toggle_combat_ai() -> bool:
	combat_enabled = not combat_enabled
	if not combat_enabled:
		_set_state(BotState.IDLE_FOLLOW)
		_nav_waypoints.clear()
	else:
		_repath_timer = 0.0
		_set_state(BotState.PURSUE_3D_NAV)
	return combat_enabled

func _set_state(new_state: int) -> void:
	if state == new_state:
		return
	state = new_state
	emit_signal("bot_state_changed", state, BOT_STATE_NAMES[state])

func format_bot_hud() -> String:
	if not combat_enabled:
		return "OFF (Press K to enable Sparring Bot)"
	var los_txt := "CLEAR" if has_line_of_sight else "BLOCKED"
	return "%s | LoS: %s" % [BOT_STATE_NAMES[state], los_txt]

## Steps the Combat AI Bot for `delta` seconds against `hero_feet` and `hero_vel`.
func step_bot_ai(delta: float, hero_feet: Vector3, hero_vel: Vector3, is_stunned_or_pinned: bool) -> void:
	_step_active_bolts(delta, hero_feet)
	if not combat_enabled or terrain == null or not is_instance_valid(companion_node):
		return
	if is_stunned_or_pinned:
		horizontal_vel = Vector3.ZERO
		return

	_fire_timer = maxf(0.0, _fire_timer - delta)
	_mortar_timer = maxf(0.0, _mortar_timer - delta)
	_breach_timer = maxf(0.0, _breach_timer - delta)
	_repath_timer = maxf(0.0, _repath_timer - delta)
	_strafe_flip_timer += delta
	if _strafe_flip_timer >= 2.2:
		_strafe_flip_timer = 0.0
		_strafe_sign = -_strafe_sign

	var bot_pos := companion_node.global_position
	var bot_eye := bot_pos + Vector3(0.0, 1.35, 0.0)
	var hero_chest := hero_feet + Vector3(0.0, 1.15, 0.0)
	var dist_to_hero := bot_pos.distance_to(hero_feet)

	# 1. Check Voxel Line-of-Sight (`raycast_terrain`)
	var los_hit: Vector3 = terrain.raycast_terrain(bot_eye, hero_chest)
	has_line_of_sight = not is_finite(los_hit.x)
	last_los_blocker_pos = los_hit

	# 2. Check Underfoot Material Hazard (`Lava` -> immediate Hazard Escape Jump!)
	var foot_mat: int = terrain.get_material_at(bot_pos - Vector3(0.0, 0.25, 0.0)) if terrain.has_method("get_material_at") else MAT_SOIL
	if foot_mat == MAT_LAVA and state != BotState.HAZARD_ESCAPE:
		_set_state(BotState.HAZARD_ESCAPE)
		_escape_timer = 0.75
		vertical_vel = 10.5
		var away := (bot_pos - hero_feet)
		away.y = 0.0
		var esc_dir := away.normalized() if away.length_squared() > 1e-4 else Vector3.RIGHT
		horizontal_vel = esc_dir * (move_speed * 1.35)
		emit_signal("bot_escaped_hazard", foot_mat, horizontal_vel + Vector3(0.0, vertical_vel, 0.0))

	if state == BotState.HAZARD_ESCAPE:
		_escape_timer -= delta
		_apply_bot_motion_and_sdf_constraints(delta)
		if _escape_timer <= 0.0 and vertical_vel <= 0.1:
			_set_state(BotState.STRAFE_ENGAGE if has_line_of_sight else BotState.PURSUE_3D_NAV)
		return

	# 3. Choose Tactical State based on Line-of-Sight, Cover Blockers & Range
	if not has_line_of_sight:
		# If blocked by a close-range construct (e.g. Kelvin Ice Dome or McGinnis Spectral Wall), breach it!
		var blocker_dist := bot_eye.distance_to(los_hit) if is_finite(los_hit.x) else INF
		var blocker_mat: int = terrain.get_material_at(los_hit) if (is_finite(los_hit.x) and terrain.has_method("get_material_at")) else MAT_SOIL
		if blocker_dist < 11.0 and (blocker_mat == MAT_ICE or blocker_mat == MAT_HARD_ROCK) and _breach_timer <= 0.0:
			_set_state(BotState.BREACH_COVER)
			_breach_timer = breach_cooldown
			terrain.trigger_explosion(los_hit, 3.8)
			emit_signal("bot_breached_cover", los_hit)
		else:
			_set_state(BotState.PURSUE_3D_NAV)
	elif dist_to_hero > preferred_max_range:
		_set_state(BotState.PURSUE_3D_NAV)
	else:
		_set_state(BotState.STRAFE_ENGAGE)

	# 4. Execute Active Tactical State
	if state == BotState.PURSUE_3D_NAV:
		_step_pursue_nav(delta, bot_pos, hero_feet)
	elif state == BotState.STRAFE_ENGAGE or state == BotState.BREACH_COVER:
		_step_strafe_and_shoot(delta, bot_pos, bot_eye, hero_feet, hero_chest, hero_vel, dist_to_hero)

func _step_pursue_nav(delta: float, bot_pos: Vector3, hero_feet: Vector3) -> void:
	if _repath_timer <= 0.0 and companion_node.has_method("get_voxel_path"):
		if companion_node.is_path_region_synced(bot_pos) and companion_node.is_path_region_synced(hero_feet):
			_nav_waypoints = companion_node.get_voxel_path(bot_pos, hero_feet)
			_nav_index = 0
			_repath_timer = repath_interval

	var move_dir := Vector3.ZERO
	if _nav_index < _nav_waypoints.size():
		var wp := _nav_waypoints[_nav_index]
		var to_wp := Vector3(wp.x - bot_pos.x, 0.0, wp.z - bot_pos.z)
		if to_wp.length() < 0.85:
			_nav_index += 1
		else:
			move_dir = to_wp.normalized()
	else:
		var direct := Vector3(hero_feet.x - bot_pos.x, 0.0, hero_feet.z - bot_pos.z)
		if direct.length_squared() > 1e-4:
			move_dir = direct.normalized()

	horizontal_vel = move_dir * move_speed
	_apply_bot_motion_and_sdf_constraints(delta)

func _step_strafe_and_shoot(
	delta: float,
	bot_pos: Vector3,
	bot_eye: Vector3,
	hero_feet: Vector3,
	hero_chest: Vector3,
	hero_vel: Vector3,
	dist_to_hero: float
) -> void:
	var to_hero_xz := Vector3(hero_feet.x - bot_pos.x, 0.0, hero_feet.z - bot_pos.z)
	var fwd := to_hero_xz.normalized() if to_hero_xz.length_squared() > 1e-4 else Vector3.FORWARD
	var right := Vector3(-fwd.z, 0.0, fwd.x)

	var radial_move := 0.0
	if dist_to_hero < preferred_min_range:
		radial_move = -0.65
	elif dist_to_hero > preferred_max_range * 0.85:
		radial_move = 0.65

	var wish_dir := (right * _strafe_sign + fwd * radial_move).normalized()

	# Check ahead for Lava or steep drop-offs before committing to the strafe direction
	var probe_ahead := bot_pos + wish_dir * 1.8
	if terrain.has_method("get_material_at"):
		var ahead_mat: int = terrain.get_material_at(probe_ahead - Vector3(0.0, 0.3, 0.0))
		var ahead_sdf: float = terrain.sample_sdf(probe_ahead + Vector3(0.0, 0.8, 0.0))
		if ahead_mat == MAT_LAVA or ahead_sdf < 0.45:
			_strafe_sign = -_strafe_sign
			wish_dir = (right * _strafe_sign + fwd * radial_move).normalized()

	horizontal_vel = wish_dir * strafe_speed
	_apply_bot_motion_and_sdf_constraints(delta)

	# Fire Primary Plasma Bolt at led hero target position
	if has_line_of_sight and _fire_timer <= 0.0:
		_fire_timer = primary_fire_interval
		var flight_time := clampf(dist_to_hero / bolt_speed, 0.05, 0.65)
		var aim_pt := hero_chest + Vector3(hero_vel.x, hero_vel.y * 0.35, hero_vel.z) * flight_time
		_spawn_bot_bolt(bot_eye, (aim_pt - bot_eye).normalized(), false)

	# Lob Arcing Magma Mortar if off cooldown
	if _mortar_timer <= 0.0 and dist_to_hero >= 6.0 and dist_to_hero <= 32.0:
		_mortar_timer = mortar_cooldown
		var pred_feet := hero_feet + Vector3(hero_vel.x, 0.0, hero_vel.z) * 0.45
		var launch_vel := (pred_feet - bot_eye) * 0.95 + Vector3(0.0, 9.5, 0.0)
		_spawn_bot_bolt(bot_eye, launch_vel.normalized(), true, launch_vel.length())
		emit_signal("bot_lobbed_mortar", bot_eye, pred_feet)

func _apply_bot_motion_and_sdf_constraints(delta: float) -> void:
	var cur := companion_node.global_position
	var next_pos := cur + horizontal_vel * delta

	# Horizontal SDF wall depenetration
	if terrain.has_method("sample_sdf"):
		var waist := next_pos + Vector3(0.0, 0.95, 0.0)
		var d: float = terrain.sample_sdf(waist)
		if d < 0.55:
			var n: Vector3 = terrain.sample_sdf_normal(waist)
			var push := Vector3(n.x, 0.0, n.z)
			if push.length_squared() > 1e-4:
				next_pos += push.normalized() * (0.55 - d)

	# Vertical gravity & floor snap
	if vertical_vel > 0.0:
		next_pos.y += vertical_vel * delta
		vertical_vel -= 24.0 * delta
	else:
		var floor_hit: Vector3 = terrain.raycast_terrain(
			next_pos + Vector3(0.0, 2.2, 0.0),
			next_pos - Vector3(0.0, 18.0, 0.0)
		)
		if is_finite(floor_hit.y):
			if next_pos.y > floor_hit.y + 0.45:
				vertical_vel = maxf(vertical_vel - 24.0 * delta, -35.0)
				next_pos.y = maxf(floor_hit.y + 0.15, next_pos.y + vertical_vel * delta)
			else:
				next_pos.y = floor_hit.y + 0.15
				vertical_vel = 0.0

	companion_node.global_position = next_pos

func _spawn_bot_bolt(origin: Vector3, dir: Vector3, is_mortar: bool, custom_speed: float = -1.0) -> void:
	var bolt := BotPlasmaBolt.new()
	bolt.position = origin + dir * 0.75
	var spd := custom_speed if custom_speed > 0.0 else bolt_speed
	bolt.velocity = dir * spd
	bolt.is_mortar = is_mortar
	bolt.radius = 0.42 if is_mortar else 0.24
	bolt.damage = mortar_damage if is_mortar else bolt_damage

	if is_instance_valid(projectile_parent):
		var mi := MeshInstance3D.new()
		var sph := SphereMesh.new()
		sph.radius = bolt.radius
		sph.height = bolt.radius * 2.0
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color(1.0, 0.35, 0.08) if is_mortar else Color(1.0, 0.15, 0.65)
		mat.emission_enabled = true
		mat.emission = (Color(1.0, 0.3, 0.05) * 2.2) if is_mortar else (Color(1.0, 0.1, 0.55) * 2.0)
		sph.material = mat
		mi.mesh = sph
		projectile_parent.add_child(mi)
		mi.global_position = bolt.position
		bolt.mesh_instance = mi

	active_bolts.append(bolt)

func _step_active_bolts(delta: float, hero_feet: Vector3) -> void:
	var hero_center := hero_feet + Vector3(0.0, 0.95, 0.0)
	for i in range(active_bolts.size() - 1, -1, -1):
		var b := active_bolts[i]
		b.age += delta
		if b.age >= b.max_lifetime:
			_free_bolt(i)
			continue

		if b.is_mortar:
			b.velocity.y -= 16.0 * delta
		var next_pos := b.position + b.velocity * delta

		# 1. Check hit against Hero capsule
		if next_pos.distance_to(hero_center) <= b.radius + 0.65:
			if ability_system != null and is_instance_valid(ability_system.owner_node):
				var dealt := ability_system.deal_ability_damage(ability_system.owner_node, b.damage, b.is_mortar, "bot_mortar" if b.is_mortar else "bot_plasma_bolt")
				if b.is_mortar:
					ability_system.apply_status_effect(ability_system.owner_node, VoxelAbilitySystem.StatusEffectSpec.make_burning(24.0, 2.5))
				emit_signal("bot_fired_bolt", b.position, next_pos, true, dealt)
			if b.is_mortar and terrain != null:
				terrain.trigger_elemental_sphere(next_pos, mortar_splash_radius, 3) # Lava splash
			_free_bolt(i)
			continue

		# 2. Check Voxel Terrain / Ice Dome / Spectral Wall collision (cover blocks bot fire!)
		if terrain != null:
			var v_hit: Vector3 = terrain.raycast_terrain(b.position, next_pos)
			if is_finite(v_hit.x):
				if b.is_mortar:
					terrain.trigger_elemental_sphere(v_hit, mortar_splash_radius, 3)
					if v_hit.distance_to(hero_center) <= mortar_splash_radius + 1.0 and ability_system != null:
						ability_system.deal_ability_damage(ability_system.owner_node, b.damage * 0.65, true, "bot_mortar_splash")
				else:
					terrain.trigger_explosion(v_hit, 0.75)
				emit_signal("bot_fired_bolt", b.position, v_hit, false, 0.0)
				_free_bolt(i)
				continue

		b.position = next_pos
		if is_instance_valid(b.mesh_instance):
			b.mesh_instance.global_position = b.position

func _free_bolt(idx: int) -> void:
	var b := active_bolts[idx]
	if is_instance_valid(b.mesh_instance):
		b.mesh_instance.queue_free()
	active_bolts.remove_at(idx)
