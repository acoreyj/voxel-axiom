class_name VoxelHeroController
extends Node3D
## Deadlock-Style Third-Person Hero Movement, Ledge-Mantling & Swept Wall-Pin Controller (`voxel_hero_controller.gd`)
## Powered by VoxelTerrain3D's O(1) C++ SDF & Surface Queries:
##   - `sample_sdf(pos)` & `sample_sdf_normal(pos)` for sub-voxel capsule collide-and-slide,
##     downhill slope-slide tangent acceleration, and camera spring-arm anti-clipping.
##   - `sweep_sphere(from, to, radius)` & `measure_wall_thickness(hit, dir, max_t)` for
##     Abrams-style Shoulder Charge with Swept Wall-Pin Detection, Glancing Wall-Slide,
##     and Thin-Wall Breaching (`carve_capsule`).
##   - 3-probe SDF + raycast Ledge Mantling (wall face, ledge top slope, capsule headroom).
##   - 3-Stamina Dash (`Shift`), Dash-Jump momentum window (`Shift` -> `Space`),
##     and Crouch/Slope-Slide (`Ctrl` / `C`).
##   - Live elemental surface reactions via `get_material_at(feet)` and `trigger_elemental_capsule()`.

signal dashed(direction: Vector3, stamina_remaining: int)
signal dash_jumped(velocity: Vector3)
signal mantle_started(ledge_target: Vector3)
signal mantle_completed(landed_feet: Vector3)
signal ground_slam_impacted(impact_point: Vector3, impact_normal: Vector3, dominant_material: int)
signal shoulder_charge_started(direction: Vector3)
signal target_captured_by_charge(target: Node3D)
signal wall_pin_impacted(impact_point: Vector3, wall_normal: Vector3, pinned_target: Node3D, wall_material: int, stun_duration: float)
signal wall_breached(breach_point: Vector3, wall_thickness: float, wall_material: int)

enum LocomotionState {
	GROUNDED,
	AIRBORNE,
	DASHING,
	SLIDING,
	MANTLING,
	GROUND_SLAM,
	SHOULDER_CHARGE,
}

const STATE_NAMES := [
	"Grounded",
	"Airborne",
	"Dashing",
	"Sliding",
	"Mantling",
	"Ground Slam",
	"Shoulder Charge",
]

const MATERIAL_AIR := 0
const MATERIAL_SOIL := 1
const MATERIAL_HARD_ROCK := 2
const MATERIAL_LAVA := 3
const MATERIAL_WATER := 4
const MATERIAL_OBSIDIAN := 5
const MATERIAL_ICE := 6
const MATERIAL_BOULDER := 7

@export_group("Body & Capsule")
@export var body_radius: float = 0.45
@export var body_height: float = 1.85
@export var eye_height: float = 1.60
@export var step_height: float = 0.60
@export var max_walkable_slope_deg: float = 48.0

@export_group("Locomotion & Gravity")
@export var walk_speed: float = 6.5
@export var sprint_speed: float = 9.5
@export var ground_accel: float = 42.0
@export var ground_friction: float = 14.0
@export var air_accel: float = 16.0
@export var air_drag: float = 1.2
@export var jump_speed: float = 8.2
@export var gravity: float = 24.0
@export var max_fall_speed: float = 65.0

@export_group("Deadlock Stamina, Dash & Slide")
@export var max_stamina: int = 3
@export var stamina_regen_seconds: float = 2.4
@export var dash_speed: float = 18.5
@export var dash_duration: float = 0.26
@export var dash_jump_min_time: float = 0.05
@export var dash_jump_max_time: float = 0.24
@export var dash_jump_horiz_boost: float = 15.5
@export var dash_jump_vert_speed: float = 8.8
@export var slide_initial_boost: float = 12.0
@export var slide_max_speed: float = 17.5
@export var slide_slope_accel: float = 26.0
@export var slide_friction: float = 2.8

@export_group("Ledge Mantling")
@export var mantle_reach_distance: float = 0.85
@export var mantle_min_height: float = 0.65
@export var mantle_max_height: float = 2.35
@export var mantle_duration: float = 0.34
@export var mantle_exit_forward_speed: float = 4.8

@export_group("Swept Wall-Pin & Shoulder Charge (Abrams Style)")
@export var charge_speed: float = 16.0
@export var charge_duration: float = 1.15
@export var charge_steer_rate: float = 2.2
@export var charge_capture_radius: float = 1.65
@export var charge_pin_min_cos: float = 0.60 # ~53 deg max angle from wall normal for a direct pin
@export var charge_breach_max_thickness: float = 2.2
@export var charge_wall_pin_blast_radius: float = 4.2
@export var charge_pin_stun_seconds: float = 1.5

@export_group("Third-Person Camera")
@export var third_person_enabled: bool = true
@export var cam_arm_length: float = 4.2
@export var cam_shoulder_offset: Vector3 = Vector3(0.65, 0.35, 0.0)
@export var cam_collision_margin: float = 0.32

@export_group("Voxel Abilities (Kelvin Ice Path & Lash Slam)")
@export var ice_path_radius: float = 3.2
@export var ice_path_step_distance: float = 1.1
@export var ground_slam_speed: float = 30.0
@export var ground_slam_crater_radius: float = 5.5

var terrain: Object = null
var chargeable_targets: Array[Node3D] = []
var state: int = LocomotionState.GROUNDED
var feet_position: Vector3 = Vector3.ZERO
var velocity: Vector3 = Vector3.ZERO
var ground_normal: Vector3 = Vector3.UP
var surface_material: int = MATERIAL_SOIL
var surface_damage: float = 0.0
var yaw: float = 0.0
var pitch: float = -0.15

var stamina_charges: int = 3
var stamina_regen_progress: float = 0.0
var ice_path_active: bool = false

var _dash_timer: float = 0.0
var _dash_dir: Vector3 = Vector3.FORWARD
var _mantle_timer: float = 0.0
var _mantle_start_pos: Vector3 = Vector3.ZERO
var _mantle_target_pos: Vector3 = Vector3.ZERO
var _mantle_wall_normal: Vector3 = Vector3.BACK
var _charge_timer: float = 0.0
var _charge_dir: Vector3 = Vector3.FORWARD
var _pinned_target: Node3D = null
var _last_ice_path_anchor: Vector3 = Vector3(INF, INF, INF)
var _last_safe_feet: Vector3 = Vector3.ZERO
var _coyote_timer: float = 0.0

## Steps the full Deadlock-style hero controller for `delta` seconds.
func step_controller(
	delta: float,
	input_move: Vector2,
	want_jump: bool,
	want_dash: bool,
	want_crouch_slide: bool,
	want_shoulder_charge: bool = false
) -> void:
	if terrain == null:
		return

	_tick_stamina(delta)
	_refresh_surface_queries()

	if state == LocomotionState.MANTLING:
		_step_mantle(delta)
		_update_kelvin_ice_path()
		return

	if state == LocomotionState.GROUND_SLAM:
		_step_ground_slam(delta)
		return

	if want_shoulder_charge and state != LocomotionState.SHOULDER_CHARGE:
		start_shoulder_charge()

	if state == LocomotionState.SHOULDER_CHARGE:
		_step_shoulder_charge(delta, input_move.x)
		_update_kelvin_ice_path()
		return

	var forward := Vector3(-sin(yaw), 0.0, -cos(yaw))
	var right := Vector3(cos(yaw), 0.0, -sin(yaw))
	var wish_dir := (right * input_move.x + forward * input_move.y)
	if wish_dir.length_squared() > 1e-4:
		wish_dir = wish_dir.normalized()

	# 1. Trigger Dash if requested and stamina is available
	if want_dash and state != LocomotionState.DASHING and stamina_charges > 0:
		stamina_charges -= 1
		state = LocomotionState.DASHING
		_dash_timer = 0.0
		_dash_dir = wish_dir if wish_dir.length_squared() > 1e-4 else forward
		velocity = _dash_dir * dash_speed
		emit_signal("dashed", _dash_dir, stamina_charges)

	# 2. Handle active Dash window & Dash-Jump combo
	if state == LocomotionState.DASHING:
		_dash_timer += delta
		if want_jump and _dash_timer >= dash_jump_min_time and _dash_timer <= dash_jump_max_time:
			state = LocomotionState.AIRBORNE
			_coyote_timer = 0.0
			velocity = _dash_dir * dash_jump_horiz_boost + Vector3(0.0, dash_jump_vert_speed, 0.0)
			emit_signal("dash_jumped", velocity)
		elif _dash_timer >= dash_duration:
			state = LocomotionState.GROUNDED if _is_on_ground() else LocomotionState.AIRBORNE
		else:
			velocity.x = _dash_dir.x * dash_speed
			velocity.z = _dash_dir.z * dash_speed
			velocity.y = maxf(velocity.y - gravity * 0.25 * delta, -4.0)

	# 3. Handle Jump or Airborne Ledge Mantle grab
	elif want_jump:
		if state == LocomotionState.GROUNDED or state == LocomotionState.SLIDING or _coyote_timer > 0.0:
			var boost := 1.15 if state == LocomotionState.SLIDING else 1.0
			velocity.x *= boost
			velocity.z *= boost
			velocity.y = jump_speed
			state = LocomotionState.AIRBORNE
			_coyote_timer = 0.0
		elif _try_start_ledge_mantle(wish_dir if wish_dir.length_squared() > 1e-4 else forward):
			return

	# 4. Horizontal acceleration, Slope Sliding, and Elemental Surface Friction
	if state != LocomotionState.DASHING:
		_apply_horizontal_locomotion(delta, wish_dir, want_crouch_slide)

	# 5. Apply Gravity & Swept SDF Collide-and-Slide
	_apply_vertical_and_sdf_collision(delta, wish_dir, want_jump)
	_update_kelvin_ice_path()

## Starts an Abrams-style Shoulder Charge with Swept Wall-Pin Detection.
func start_shoulder_charge() -> bool:
	if state == LocomotionState.MANTLING or state == LocomotionState.SHOULDER_CHARGE:
		return false
	state = LocomotionState.SHOULDER_CHARGE
	_charge_timer = 0.0
	_pinned_target = null
	_charge_dir = Vector3(-sin(yaw), 0.0, -cos(yaw)).normalized()
	velocity = _charge_dir * charge_speed
	emit_signal("shoulder_charge_started", _charge_dir)
	return true

## Steps the Abrams-style Shoulder Charge:
##   1. Applies limited yaw steering.
##   2. Sweeps a capture corridor to impale/drag any target in `chargeable_targets`.
##   3. Runs `sweep_sphere` + `sample_sdf_normal` + `measure_wall_thickness` to resolve:
##      - Glancing Wall-Slide (`impact_cos < charge_pin_min_cos`)
##      - Thin-Wall Breach (`wall_thickness <= charge_breach_max_thickness` -> `carve_capsule` through!)
##      - Solid Wall-Pin & Stun (`impact_cos >= charge_pin_min_cos` -> crater fracture + stun target!)
func _step_shoulder_charge(delta: float, steer_input: float) -> void:
	_charge_timer += delta
	if _charge_timer >= charge_duration:
		_pinned_target = null
		state = LocomotionState.GROUNDED if _is_on_ground() else LocomotionState.AIRBORNE
		return

	# Smoothly steer toward camera yaw (with clamped turn rate)
	var desired_dir := Vector3(-sin(yaw), 0.0, -cos(yaw)).normalized()
	var turn_weight := clampf((charge_steer_rate - absf(steer_input) * 0.4) * delta, 0.0, 1.0)
	_charge_dir = _charge_dir.lerp(desired_dir, turn_weight).normalized()

	var horiz_step := _charge_dir * (charge_speed * delta)
	var next_feet := feet_position + horiz_step

	# 1. Sweep target capture corridor if we haven't impaled a target yet
	if _pinned_target == null:
		for candidate in chargeable_targets:
			if not is_instance_valid(candidate):
				continue
			var to_cand := candidate.global_position - feet_position
			if absf(to_cand.y) > 2.2:
				continue
			var horiz_cand := Vector3(to_cand.x, 0.0, to_cand.z)
			var forward_proj := horiz_cand.dot(_charge_dir)
			if forward_proj >= -0.3 and forward_proj <= horiz_step.length() + charge_capture_radius:
				var lateral_dist := (horiz_cand - _charge_dir * forward_proj).length()
				if lateral_dist <= charge_capture_radius:
					_pinned_target = candidate
					emit_signal("target_captured_by_charge", _pinned_target)
					break

	# 2. Swept Sphere Wall-Pin Detection at waist & chest height
	var lead_extra := 0.85 if is_instance_valid(_pinned_target) else 0.25
	var sweep_radius := body_radius * 0.92
	var hit_pos := Vector3(INF, INF, INF)
	for h in [0.85, 1.35]:
		var from_pt := feet_position + Vector3(0.0, h, 0.0)
		var to_pt := next_feet + Vector3(0.0, h, 0.0) + _charge_dir * (body_radius + lead_extra)
		var contact: Vector3 = terrain.sweep_sphere(from_pt, to_pt, sweep_radius) if terrain.has_method("sweep_sphere") else terrain.raycast_terrain(from_pt, to_pt)
		if is_finite(contact.x):
			hit_pos = contact
			break

	if is_finite(hit_pos.x):
		var wall_normal: Vector3 = terrain.sample_sdf_normal(hit_pos)
		# Only treat steep surfaces as walls; walkable ramps are climbed below
		if absf(wall_normal.y) < 0.62:
			var wall_n_xz := Vector3(wall_normal.x, 0.0, wall_normal.z).normalized()
			var impact_cos := -_charge_dir.dot(wall_n_xz)

			# Case A: Glancing angle -> slide smoothly along the wall tangent without stopping
			if impact_cos < charge_pin_min_cos:
				var slide_dir := (_charge_dir - wall_n_xz * _charge_dir.dot(wall_n_xz)).normalized()
				if slide_dir.length_squared() > 1e-4:
					_charge_dir = slide_dir
					next_feet = feet_position + _charge_dir * (charge_speed * delta * maxf(0.45, 1.0 - impact_cos))
			else:
				# Direct head-on impact! Query material, fracture state, and wall thickness
				var wall_mat: int = terrain.get_material_at(hit_pos)
				var wall_dmg: float = terrain.get_damage_at(hit_pos)
				var wall_thick: float = terrain.measure_wall_thickness(
					hit_pos, _charge_dir, charge_breach_max_thickness + 1.0
				) if terrain.has_method("measure_wall_thickness") else 99.0

				var is_brittle_or_cracked := (
					wall_mat == MATERIAL_SOIL or
					wall_mat == MATERIAL_ICE or
					(wall_mat == MATERIAL_HARD_ROCK and wall_dmg >= 0.60)
				)

				# Case B: Thin-Wall Breach (when not pinning a target, or smashing through a thin partition)
				if not is_instance_valid(_pinned_target) and is_brittle_or_cracked and wall_thick > 0.05 and wall_thick <= charge_breach_max_thickness:
					var tunnel_end := hit_pos + _charge_dir * (wall_thick + 0.9)
					terrain.carve_capsule(hit_pos - _charge_dir * 0.4, tunnel_end, 2.3)
					emit_signal("wall_breached", hit_pos, wall_thick, wall_mat)
					feet_position = next_feet
					return

				# Case C: Solid Wall-Pin & Stun!
				terrain.trigger_explosion(hit_pos, charge_wall_pin_blast_radius)
				if is_instance_valid(_pinned_target):
					_pinned_target.global_position = Vector3(
						hit_pos.x + wall_n_xz.x * 0.65,
						feet_position.y + 0.15,
						hit_pos.z + wall_n_xz.z * 0.65
					)
				emit_signal("wall_pin_impacted", hit_pos, wall_normal, _pinned_target, wall_mat, charge_pin_stun_seconds)
				_pinned_target = null
				velocity = wall_n_xz * 3.8 + Vector3(0.0, 3.2, 0.0)
				state = LocomotionState.AIRBORNE
				return

	# Advance hero and keep any captured target pinned to the front bumper
	feet_position = next_feet
	var ground_hit: Vector3 = terrain.raycast_terrain(
		feet_position + Vector3(0.0, step_height + 0.4, 0.0),
		feet_position - Vector3(0.0, 1.2, 0.0)
	)
	if is_finite(ground_hit.y):
		feet_position.y = ground_hit.y
		velocity.y = 0.0
	else:
		velocity.y = maxf(velocity.y - gravity * delta, -max_fall_speed)
		feet_position.y += velocity.y * delta

	if is_instance_valid(_pinned_target):
		_pinned_target.global_position = feet_position + _charge_dir * 1.15 + Vector3(0.0, 0.15, 0.0)

func trigger_ground_slam() -> bool:
	if state == LocomotionState.GROUNDED or state == LocomotionState.MANTLING:
		return false
	state = LocomotionState.GROUND_SLAM
	var aim_forward := Vector3(-sin(yaw) * cos(pitch), sin(pitch), -cos(yaw) * cos(pitch)).normalized()
	var dive_dir := (Vector3(aim_forward.x * 0.35, -1.0, aim_forward.z * 0.35)).normalized()
	velocity = dive_dir * ground_slam_speed
	return true

func _tick_stamina(delta: float) -> void:
	if stamina_charges >= max_stamina:
		stamina_regen_progress = 0.0
		return
	stamina_regen_progress += delta / maxf(0.1, stamina_regen_seconds)
	if stamina_regen_progress >= 1.0:
		stamina_regen_progress -= 1.0
		stamina_charges = mini(max_stamina, stamina_charges + 1)

func _refresh_surface_queries() -> void:
	var probe_pt := feet_position + Vector3(0.0, 0.2, 0.0)
	ground_normal = terrain.sample_sdf_normal(probe_pt)
	surface_material = terrain.get_material_at(feet_position - Vector3(0.0, 0.15, 0.0))
	surface_damage = terrain.get_damage_at(feet_position - Vector3(0.0, 0.15, 0.0))

func _apply_horizontal_locomotion(delta: float, wish_dir: Vector3, want_crouch_slide: bool) -> void:
	var horiz_vel := Vector3(velocity.x, 0.0, velocity.z)
	var horiz_speed := horiz_vel.length()
	var on_ice := (surface_material == MATERIAL_ICE)
	var in_water := (surface_material == MATERIAL_WATER)

	if state == LocomotionState.GROUNDED or state == LocomotionState.SLIDING:
		var slope_steepness := clampf(1.0 - ground_normal.y, 0.0, 1.0)
		var can_enter_slide := want_crouch_slide and (horiz_speed > 5.0 or slope_steepness > 0.05 or on_ice)
		if can_enter_slide:
			if state != LocomotionState.SLIDING and horiz_speed < slide_initial_boost:
				var boost_dir := wish_dir if wish_dir.length_squared() > 1e-4 else horiz_vel.normalized()
				if boost_dir.length_squared() > 1e-4:
					horiz_vel = boost_dir * slide_initial_boost
			state = LocomotionState.SLIDING
		elif state == LocomotionState.SLIDING and (not want_crouch_slide or horiz_speed < 2.0):
			state = LocomotionState.GROUNDED

		if state == LocomotionState.SLIDING:
			var downhill := Vector3(ground_normal.x, 0.0, ground_normal.z)
			if downhill.length_squared() > 1e-5:
				horiz_vel += downhill.normalized() * (slide_slope_accel * (slope_steepness * 2.5 + 0.15) * delta)
			var eff_slide_friction := slide_friction * (0.22 if on_ice else 1.0)
			horiz_vel = horiz_vel.move_toward(Vector3.ZERO, eff_slide_friction * delta)
			var cap := slide_max_speed * (1.2 if on_ice else 1.0)
			if horiz_vel.length() > cap:
				horiz_vel = horiz_vel.normalized() * cap
		else:
			var target_speed := walk_speed * (0.76 if in_water else (1.18 if on_ice else 1.0))
			var target_vel := wish_dir * target_speed
			var eff_friction := ground_friction * (0.18 if on_ice else 1.0)
			var rate := ground_accel if wish_dir.length_squared() > 1e-4 else eff_friction
			horiz_vel = horiz_vel.move_toward(target_vel, rate * delta)

		if surface_material == MATERIAL_LAVA:
			velocity.y = maxf(velocity.y, 9.2)
			state = LocomotionState.AIRBORNE
	else:
		if wish_dir.length_squared() > 1e-4:
			var proj := horiz_vel.dot(wish_dir)
			var max_air_wish := walk_speed * 1.15
			if proj < max_air_wish:
				horiz_vel += wish_dir * minf(air_accel * delta, max_air_wish - proj)
		horiz_vel = horiz_vel.move_toward(Vector3.ZERO, air_drag * delta)

	velocity.x = horiz_vel.x
	velocity.z = horiz_vel.z

func _apply_vertical_and_sdf_collision(delta: float, wish_dir: Vector3, want_jump: bool) -> void:
	var prev_feet := feet_position
	var horiz_step := Vector3(velocity.x, 0.0, velocity.z) * delta

	feet_position += horiz_step
	var blocked_horiz := _depenetrate_capsule_using_sdf()

	if blocked_horiz and (state == LocomotionState.AIRBORNE or want_jump) and wish_dir.length_squared() > 1e-4:
		if _try_start_ledge_mantle(wish_dir):
			return

	if state != LocomotionState.DASHING:
		velocity.y = maxf(velocity.y - gravity * delta, -max_fall_speed)
	feet_position.y += velocity.y * delta

	var head_pos := feet_position + Vector3(0.0, body_height, 0.0)
	var head_sdf: float = terrain.sample_sdf(head_pos)
	if head_sdf < body_radius and velocity.y > 0.0:
		var head_n: Vector3 = terrain.sample_sdf_normal(head_pos)
		if head_n.y < -0.25:
			feet_position.y -= (body_radius - head_sdf)
			velocity.y = 0.0

	var probe_top := maxf(prev_feet.y, feet_position.y) + step_height + 0.25
	var hit: Vector3 = terrain.raycast_terrain(
		Vector3(feet_position.x, probe_top, feet_position.z),
		Vector3(feet_position.x, feet_position.y - 0.65, feet_position.z)
	)
	var min_walkable_ny := cos(deg_to_rad(max_walkable_slope_deg))
	if is_finite(hit.y):
		var hit_normal: Vector3 = terrain.sample_sdf_normal(hit)
		var within_step := (hit.y - prev_feet.y) <= step_height + 0.1
		var descending := velocity.y <= 0.05
		if within_step and descending and feet_position.y <= hit.y + 0.32 and hit_normal.y >= min_walkable_ny:
			feet_position.y = hit.y
			velocity.y = 0.0
			ground_normal = hit_normal
			_last_safe_feet = feet_position
			_coyote_timer = 0.14
			if state == LocomotionState.AIRBORNE:
				state = LocomotionState.GROUNDED
			return

	_coyote_timer = maxf(0.0, _coyote_timer - delta)
	if state == LocomotionState.GROUNDED or state == LocomotionState.SLIDING:
		state = LocomotionState.AIRBORNE

func _depenetrate_capsule_using_sdf() -> bool:
	var touched_wall := false
	for h in [step_height + 0.15, 1.15, 1.55]:
		var sample_pt := feet_position + Vector3(0.0, h, 0.0)
		var d: float = terrain.sample_sdf(sample_pt)
		if d < body_radius:
			var n: Vector3 = terrain.sample_sdf_normal(sample_pt)
			var push_horiz := Vector3(n.x, 0.0, n.z)
			if push_horiz.length_squared() > 1e-4:
				push_horiz = push_horiz.normalized()
				feet_position += push_horiz * (body_radius - d)
				var vn := velocity.x * push_horiz.x + velocity.z * push_horiz.z
				if vn < 0.0:
					velocity.x -= push_horiz.x * vn
					velocity.z -= push_horiz.z * vn
				touched_wall = true
	return touched_wall

func _try_start_ledge_mantle(move_dir: Vector3) -> bool:
	var forward_dir := Vector3(move_dir.x, 0.0, move_dir.z).normalized()
	if forward_dir.length_squared() < 1e-4:
		return false

	var chest_origin := feet_position + Vector3(0.0, 1.05, 0.0)
	var chest_end := chest_origin + forward_dir * (body_radius + mantle_reach_distance)
	var wall_hit: Vector3 = terrain.raycast_terrain(chest_origin, chest_end)
	if not is_finite(wall_hit.x):
		return false

	var wall_normal: Vector3 = terrain.sample_sdf_normal(wall_hit)
	if absf(wall_normal.y) > 0.60:
		return false

	var ledge_probe_xz := wall_hit + forward_dir * (body_radius + 0.25)
	var probe_from := Vector3(ledge_probe_xz.x, feet_position.y + mantle_max_height + 0.3, ledge_probe_xz.z)
	var probe_to := Vector3(ledge_probe_xz.x, feet_position.y + mantle_min_height, ledge_probe_xz.z)
	var ledge_hit: Vector3 = terrain.raycast_terrain(probe_from, probe_to)
	if not is_finite(ledge_hit.y):
		return false

	var climb_height := ledge_hit.y - feet_position.y
	if climb_height < mantle_min_height or climb_height > mantle_max_height:
		return false

	var ledge_normal: Vector3 = terrain.sample_sdf_normal(ledge_hit)
	if ledge_normal.y < cos(deg_to_rad(max_walkable_slope_deg)):
		return false

	var waist_clearance: float = terrain.sample_sdf(ledge_hit + Vector3(0.0, 0.90, 0.0))
	var head_clearance: float = terrain.sample_sdf(ledge_hit + Vector3(0.0, 1.65, 0.0))
	if waist_clearance < body_radius * 0.75 or head_clearance < body_radius * 0.75:
		return false

	state = LocomotionState.MANTLING
	_mantle_timer = 0.0
	_mantle_start_pos = feet_position
	_mantle_target_pos = ledge_hit
	_mantle_wall_normal = Vector3(wall_normal.x, 0.0, wall_normal.z).normalized()
	velocity = Vector3.ZERO
	emit_signal("mantle_started", _mantle_target_pos)
	return true

func _step_mantle(delta: float) -> void:
	_mantle_timer += delta
	var t := clampf(_mantle_timer / maxf(0.08, mantle_duration), 0.0, 1.0)
	if t < 0.55:
		var u := smoothstep(0.0, 1.0, t / 0.55)
		feet_position.x = lerpf(_mantle_start_pos.x, _mantle_target_pos.x, u * 0.25)
		feet_position.z = lerpf(_mantle_start_pos.z, _mantle_target_pos.z, u * 0.25)
		feet_position.y = lerpf(_mantle_start_pos.y, _mantle_target_pos.y + 0.12, u)
	else:
		var u := smoothstep(0.0, 1.0, (t - 0.55) / 0.45)
		feet_position.x = lerpf(_mantle_start_pos.x, _mantle_target_pos.x, 0.25 + 0.75 * u)
		feet_position.z = lerpf(_mantle_start_pos.z, _mantle_target_pos.z, 0.25 + 0.75 * u)
		feet_position.y = lerpf(_mantle_target_pos.y + 0.12, _mantle_target_pos.y, u)

	if t >= 1.0:
		feet_position = _mantle_target_pos
		var exit_dir := (_mantle_target_pos - _mantle_start_pos)
		exit_dir.y = 0.0
		if exit_dir.length_squared() > 1e-4:
			velocity = exit_dir.normalized() * mantle_exit_forward_speed
		state = LocomotionState.GROUNDED
		emit_signal("mantle_completed", feet_position)

func _step_ground_slam(delta: float) -> void:
	var next_pos := feet_position + velocity * delta
	var hit: Vector3 = terrain.raycast_terrain(feet_position + Vector3(0.0, 0.8, 0.0), next_pos - Vector3(0.0, 0.4, 0.0))
	if is_finite(hit.y) and next_pos.y <= hit.y + 0.2:
		feet_position = hit
		var n: Vector3 = terrain.sample_sdf_normal(hit)
		var dom_mat: int = terrain.get_dominant_material_in_sphere(hit, ground_slam_crater_radius * 0.65)
		terrain.trigger_explosion(hit, ground_slam_crater_radius)
		velocity = Vector3.ZERO
		state = LocomotionState.GROUNDED
		emit_signal("ground_slam_impacted", hit, n, dom_mat)
		return
	feet_position = next_pos

func _update_kelvin_ice_path() -> void:
	if not ice_path_active:
		_last_ice_path_anchor = Vector3(INF, INF, INF)
		return
	var pitch_offset := clampf(sin(pitch) * 0.85, -0.55, 0.65)
	var cur_anchor := feet_position + Vector3(0.0, pitch_offset - 0.35, 0.0)
	if not is_finite(_last_ice_path_anchor.x):
		_last_ice_path_anchor = cur_anchor
		terrain.trigger_elemental_sphere(cur_anchor, ice_path_radius, 2)
		return
	if cur_anchor.distance_to(_last_ice_path_anchor) >= ice_path_step_distance:
		terrain.trigger_elemental_capsule(_last_ice_path_anchor, cur_anchor, ice_path_radius, 2)
		_last_ice_path_anchor = cur_anchor

func _is_on_ground() -> bool:
	var d: float = terrain.sample_sdf(feet_position)
	return d <= 0.35

func compute_camera_transform() -> Transform3D:
	var cam_rot := Basis.from_euler(Vector3(pitch, yaw, 0.0))
	var slide_crouch := -0.45 if state == LocomotionState.SLIDING else 0.0
	var pivot := feet_position + Vector3(0.0, eye_height + slide_crouch, 0.0)
	if not third_person_enabled:
		return Transform3D(cam_rot, pivot)

	var desired_pos := pivot + cam_rot * Vector3(cam_shoulder_offset.x, cam_shoulder_offset.y, cam_arm_length)
	var hit: Vector3 = terrain.raycast_terrain(pivot, desired_pos)
	var final_pos := desired_pos
	if is_finite(hit.x):
		var to_desired := desired_pos - pivot
		var dist := maxf(0.45, pivot.distance_to(hit) - cam_collision_margin)
		if to_desired.length_squared() > 1e-5:
			final_pos = pivot + to_desired.normalized() * dist

	var cam_sdf: float = terrain.sample_sdf(final_pos)
	if cam_sdf < cam_collision_margin:
		var cam_n: Vector3 = terrain.sample_sdf_normal(final_pos)
		final_pos += cam_n * (cam_collision_margin - cam_sdf)

	return Transform3D(cam_rot, final_pos)
