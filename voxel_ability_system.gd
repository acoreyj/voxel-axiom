class_name VoxelAbilitySystem
extends Node
## Data-Driven Hero Ability & Status Effect System (GAS-lite) (`voxel_ability_system.gd`)
## Designed for Deadlock-style heroes operating on destructible VoxelTerrain3D:
##   1. `GameplayTags`: Bitmask state & elemental tags (`STUNNED`, `SILENCED`, `AIRBORNE`,
##      `UNSTOPPABLE`, `CHANNELING`, `BURNING`, `CHILLED`, `FROZEN`, `SOAKED`).
##   2. `HeroAttributes`: Base stats (`max_health`, `spirit_power`, `weapon_damage`,
##      `move_speed_mult`, `cooldown_reduction`, `resist`) with automatic buff/debuff modifier stacking.
##   3. `StatusEffectSpec`: Timed buffs, DoTs (`Burning`), stacking slows (`3x Chilled -> Frozen`
##      with destructible voxel ice encasement), and automatic elemental surface reactions
##      (`Water` quenches `Burning`, `Lava` shatters `Frozen` and applies `Burning`).
##   4. `AbilitySpec`: Data-driven ability definitions with cooldowns, charges, stamina costs,
##      tag requirements/blockers, Spirit Power radius/damage scaling, and 11 Hz throttled
##      voxel channel ticks (so 60+ FPS beams like Bebop's Hyper Beam never flood the worker pool).
##   5. `SubsteppedProjectile`: SDF-normal-bouncing arc projectiles (Viscous / Kelvin orbs)
##      that ricochet off destructible voxel surfaces in O(1) time via `sample_sdf_normal()`.

signal ability_cast_started(ability_id: String, slot_index: int)
signal ability_cast_blocked(ability_id: String, reason: String)
signal ability_channel_ticked(ability_id: String, hit_pos: Vector3, hit_normal: Vector3)
signal ability_completed(ability_id: String)
signal status_effect_applied(target: Node3D, effect_id: String, stacks: int, duration: float)
signal status_effect_removed(target: Node3D, effect_id: String)
signal damage_dealt(target: Node3D, amount: float, is_spirit: bool, source_ability: String)
signal elemental_combo_triggered(combo_name: String, world_pos: Vector3, radius: float)
signal weapon_fired(ammo_remaining: int, infinite_slide_shot: bool, ricochet_count: int, hit_crit: bool)
signal weapon_reload_started(reload_seconds: float)
signal weapon_reload_completed(mag_size: int)

# ============================================================================
# 1. Gameplay Tags Bitmask
# ============================================================================
enum Tag {
	NONE        = 0,
	STUNNED     = 1 << 0,
	SILENCED    = 1 << 1,
	ROOTED      = 1 << 2,
	AIRBORNE    = 1 << 3,
	GROUNDED    = 1 << 4,
	UNSTOPPABLE = 1 << 5,
	CHANNELING  = 1 << 6,
	BURNING     = 1 << 7,
	CHILLED     = 1 << 8,
	FROZEN      = 1 << 9,
	SOAKED      = 1 << 10,
	ICE_SURGE   = 1 << 11,
}

# VoxelTerrain3D Material IDs
const MAT_AIR := 0
const MAT_SOIL := 1
const MAT_HARD_ROCK := 2
const MAT_LAVA := 3
const MAT_WATER := 4
const MAT_OBSIDIAN := 5
const MAT_ICE := 6
const MAT_BOULDER := 7

# ElementalKind wire values
const ELEM_NONE := 0
const ELEM_WATER := 1
const ELEM_FROST := 2
const ELEM_LAVA := 3

# ============================================================================
# 2. Data-Driven Hero Attributes & Modifier Stack
# ============================================================================
class HeroAttributes extends RefCounted:
	var max_health: float = 750.0
	var health: float = 750.0
	var base_spirit_power: float = 42.0
	var bonus_spirit_power: float = 0.0
	var base_weapon_damage: float = 25.0
	var move_speed_multiplier: float = 1.0
	var cooldown_reduction: float = 0.15 # 0.0 .. 0.60
	var spirit_resist: float = 0.10
	var bullet_resist: float = 0.10

	func get_spirit_power() -> float:
		return maxf(0.0, base_spirit_power + bonus_spirit_power)

	func apply_damage(raw_amount: float, is_spirit: bool) -> float:
		var resist := clampf(spirit_resist if is_spirit else bullet_resist, -0.5, 0.85)
		var final_dmg := maxf(0.0, raw_amount * (1.0 - resist))
		health = clampf(health - final_dmg, 0.0, max_health)
		return final_dmg

	func apply_heal(amount: float) -> void:
		health = clampf(health + maxf(0.0, amount), 0.0, max_health)

# ============================================================================
# 3. Status Effect Spec (Buffs, DoTs, Stacking Slows, Voxel Ice Encasement)
# ============================================================================
class StatusEffectSpec extends RefCounted:
	var id: String = ""
	var display_name: String = ""
	var duration: float = 3.0
	var remaining: float = 3.0
	var tick_interval: float = 0.5
	var tick_accum: float = 0.0
	var stacks: int = 1
	var max_stacks: int = 1
	var granted_tags: int = Tag.NONE
	var cleansed_tags_on_apply: int = Tag.NONE
	var speed_multiplier_per_stack: float = 1.0
	var bonus_spirit_per_stack: float = 0.0
	var damage_per_tick: float = 0.0
	var heal_per_tick: float = 0.0
	var encase_in_voxel_ice: bool = false
	var encase_origin: Vector3 = Vector3.ZERO
	var vfx_color: Color = Color(1.0, 1.0, 1.0)

	static func make_burning(dps: float = 36.0, dur: float = 3.5) -> StatusEffectSpec:
		var s := StatusEffectSpec.new()
		s.id = "burning"
		s.display_name = "BURNING"
		s.duration = dur
		s.remaining = dur
		s.tick_interval = 0.5
		s.max_stacks = 3
		s.granted_tags = Tag.BURNING
		s.cleansed_tags_on_apply = Tag.CHILLED | Tag.FROZEN
		s.damage_per_tick = dps * 0.5
		s.vfx_color = Color(1.0, 0.38, 0.08)
		return s

	static func make_chilled(dur: float = 3.0) -> StatusEffectSpec:
		var s := StatusEffectSpec.new()
		s.id = "chilled"
		s.display_name = "CHILLED"
		s.duration = dur
		s.remaining = dur
		s.max_stacks = 3
		s.granted_tags = Tag.CHILLED
		s.cleansed_tags_on_apply = Tag.BURNING
		s.speed_multiplier_per_stack = 0.82
		s.vfx_color = Color(0.35, 0.88, 1.0)
		return s

	static func make_frozen_ice_shell(dur: float = 2.2) -> StatusEffectSpec:
		var s := StatusEffectSpec.new()
		s.id = "frozen"
		s.display_name = "FROZEN"
		s.duration = dur
		s.remaining = dur
		s.max_stacks = 1
		s.granted_tags = Tag.FROZEN | Tag.STUNNED | Tag.ROOTED
		s.cleansed_tags_on_apply = Tag.BURNING | Tag.CHILLED
		s.speed_multiplier_per_stack = 0.0
		s.encase_in_voxel_ice = true
		s.vfx_color = Color(0.15, 0.95, 1.0)
		return s

	static func make_wall_pin_stun(dur: float = 1.5) -> StatusEffectSpec:
		var s := StatusEffectSpec.new()
		s.id = "wall_pin_stun"
		s.display_name = "WALL-PIN STUN"
		s.duration = dur
		s.remaining = dur
		s.max_stacks = 1
		s.granted_tags = Tag.STUNNED | Tag.ROOTED
		s.speed_multiplier_per_stack = 0.0
		s.vfx_color = Color(1.0, 0.55, 0.1)
		return s

	static func make_ice_path_surge(dur: float = 0.6) -> StatusEffectSpec:
		var s := StatusEffectSpec.new()
		s.id = "ice_surge"
		s.display_name = "ICE SURGE"
		s.duration = dur
		s.remaining = dur
		s.max_stacks = 1
		s.granted_tags = Tag.ICE_SURGE
		s.speed_multiplier_per_stack = 1.24
		s.bonus_spirit_per_stack = 18.0
		s.vfx_color = Color(0.25, 0.95, 1.0)
		return s

# ============================================================================
# 4. Data-Driven Ability Spec
# ============================================================================
enum DeliveryMode {
	TOGGLE_CHANNEL,
	MOVEMENT_CHARGE,
	AIRBORNE_SLAM,
	HITSCAN_BEAM,
	ARC_PROJECTILE,
	TEMP_CONSTRUCT,
}

class AbilitySpec extends RefCounted:
	var slot_index: int = 1
	var ability_id: String = ""
	var display_name: String = ""
	var hotkey_label: String = "E"
	var delivery_mode: int = DeliveryMode.TOGGLE_CHANNEL
	var base_cooldown: float = 6.0
	var cooldown_remaining: float = 0.0
	var max_charges: int = 1
	var charges: int = 1
	var charge_progress: float = 0.0
	var stamina_cost: int = 0
	var channel_duration: float = 2.6
	var channel_tick_interval: float = 0.09 # ~11 Hz voxel edit tick so 60 FPS beams never flood worker queues
	var required_tags: int = Tag.NONE
	var blocked_by_tags: int = Tag.STUNNED | Tag.SILENCED | Tag.FROZEN
	var granted_tags_while_active: int = Tag.NONE
	var base_damage: float = 80.0
	var spirit_damage_scale: float = 1.4
	var base_voxel_radius: float = 3.5
	var spirit_radius_scale: float = 0.035
	var is_toggled_on: bool = false
	var construct_duration: float = 6.5
	var active_structure_id: int = 0

	func compute_scaled_damage(spirit_power: float) -> float:
		return base_damage + spirit_power * spirit_damage_scale

	func compute_scaled_radius(spirit_power: float) -> float:
		return base_voxel_radius + spirit_power * spirit_radius_scale

# ============================================================================
# 5. Primary Weapon Spec, SDF Ricochet Raycaster & 3D Tracer Segments
# ============================================================================
class WeaponSpec extends RefCounted:
	var display_name: String = "Pulse Carbine"
	var mag_size: int = 24
	var current_ammo: int = 24
	var fire_interval: float = 0.14
	var fire_cooldown: float = 0.0
	var reload_duration: float = 1.55
	var reload_remaining: float = 0.0
	var is_reloading: bool = false
	var base_bullet_damage: float = 24.0
	var headshot_multiplier: float = 1.85
	var ricochet_bonus_multiplier: float = 1.25
	var max_ricochets: int = 2
	var min_ricochet_cos: float = 0.12
	var bullet_chip_radius: float = 0.95

class BulletTracerSegment extends RefCounted:
	var from_pos: Vector3 = Vector3.ZERO
	var to_pos: Vector3 = Vector3.ZERO
	var bounce_index: int = 0
	var is_crit: bool = false
	var ttl: float = 0.16

# ============================================================================
# 5b. Sub-Stepped Bouncing Voxel Projectile (Viscous / Kelvin Orb)
# ============================================================================
class BouncingVoxelProjectile extends RefCounted:
	var position: Vector3 = Vector3.ZERO
	var velocity: Vector3 = Vector3.ZERO
	var radius: float = 0.42
	var bounces_remaining: int = 2
	var max_lifetime: float = 4.5
	var age: float = 0.0
	var elemental_kind: int = ELEM_FROST
	var splash_radius: float = 3.2
	var direct_damage: float = 65.0
	var mesh_instance: MeshInstance3D = null

# ============================================================================
# 6. Runtime State Managed by VoxelAbilitySystem
# ============================================================================
var terrain: Object = null
var owner_node: Node3D = null
var hero_attributes: HeroAttributes = HeroAttributes.new()
var hero_tags: int = Tag.GROUNDED
var abilities: Array[AbilitySpec] = []
var hero_effects: Dictionary = {} # effect_id -> StatusEffectSpec

## Target entity tracking (e.g., AI Companion or Enemy Heroes)
var target_attributes: Dictionary = {} # int(instance_id) -> HeroAttributes
var target_effects: Dictionary = {}    # int(instance_id) -> Dictionary(effect_id -> StatusEffectSpec)
var target_tags: Dictionary = {}       # int(instance_id) -> int(bitmask)

## Active Channeled Beam (Bebop Hyper Beam)
var active_beam_ability: AbilitySpec = null
var beam_timer: float = 0.0
var beam_voxel_tick_accum: float = 0.0
var beam_last_hit_pos: Vector3 = Vector3(INF, INF, INF)
var beam_current_origin: Vector3 = Vector3.ZERO
var beam_current_end: Vector3 = Vector3.ZERO

## Primary Weapon & Active 3D Bullet/Ricochet Tracers
var primary_weapon: WeaponSpec = WeaponSpec.new()
var active_tracers: Array[BulletTracerSegment] = []

## Active Sub-Stepped Bouncing Projectiles
var active_projectiles: Array[BouncingVoxelProjectile] = []

func _ready() -> void:
	_register_default_deadlock_loadout()

func _register_default_deadlock_loadout() -> void:
	abilities.clear()

	# Ability 1 (Key E): Kelvin "Ice Path" (Toggle Channel + Ice Surge Buff)
	var a_ice := AbilitySpec.new()
	a_ice.slot_index = 1
	a_ice.ability_id = "kelvin_ice_path"
	a_ice.display_name = "Ice Path"
	a_ice.hotkey_label = "E"
	a_ice.delivery_mode = DeliveryMode.TOGGLE_CHANNEL
	a_ice.base_cooldown = 1.0
	a_ice.base_voxel_radius = 3.2
	a_ice.spirit_radius_scale = 0.02
	a_ice.granted_tags_while_active = Tag.ICE_SURGE
	abilities.append(a_ice)

	# Ability 2 (Key G): Abrams "Shoulder Charge" (Swept Wall-Pin + Shatter Bonus)
	var a_charge := AbilitySpec.new()
	a_charge.slot_index = 2
	a_charge.ability_id = "abrams_shoulder_charge"
	a_charge.display_name = "Shoulder Charge"
	a_charge.hotkey_label = "G"
	a_charge.delivery_mode = DeliveryMode.MOVEMENT_CHARGE
	a_charge.base_cooldown = 5.5
	a_charge.base_damage = 110.0
	a_charge.spirit_damage_scale = 1.5
	a_charge.base_voxel_radius = 4.2
	abilities.append(a_charge)

	# Ability 3 (Key Q): Lash "Ground Strike" (Airborne Slam + Elemental Terrain Combo)
	var a_slam := AbilitySpec.new()
	a_slam.slot_index = 3
	a_slam.ability_id = "lash_ground_strike"
	a_slam.display_name = "Ground Strike"
	a_slam.hotkey_label = "Q"
	a_slam.delivery_mode = DeliveryMode.AIRBORNE_SLAM
	a_slam.base_cooldown = 4.5
	a_slam.required_tags = Tag.AIRBORNE
	a_slam.base_damage = 125.0
	a_slam.spirit_damage_scale = 1.8
	a_slam.base_voxel_radius = 5.2
	a_slam.spirit_radius_scale = 0.04
	abilities.append(a_slam)

	# Ability 4 (Key Z): Bebop "Hyper Beam" (Channeled 11 Hz Molten Laser Trench)
	var a_beam := AbilitySpec.new()
	a_beam.slot_index = 4
	a_beam.ability_id = "bebop_hyper_beam"
	a_beam.display_name = "Hyper Beam"
	a_beam.hotkey_label = "Z"
	a_beam.delivery_mode = DeliveryMode.HITSCAN_BEAM
	a_beam.base_cooldown = 9.0
	a_beam.channel_duration = 2.6
	a_beam.channel_tick_interval = 0.09
	a_beam.granted_tags_while_active = Tag.CHANNELING | Tag.UNSTOPPABLE
	a_beam.base_damage = 140.0 # DPS
	a_beam.spirit_damage_scale = 1.6
	a_beam.base_voxel_radius = 2.4
	a_beam.spirit_radius_scale = 0.02
	abilities.append(a_beam)

	# Ability 5 (Key X): Viscous / Kelvin "Cryo-Bounce Orb" (2 Charges, SDF Normal Ricochet -> Freeze)
	var a_orb := AbilitySpec.new()
	a_orb.slot_index = 5
	a_orb.ability_id = "cryo_bounce_orb"
	a_orb.display_name = "Cryo Orb"
	a_orb.hotkey_label = "X"
	a_orb.delivery_mode = DeliveryMode.ARC_PROJECTILE
	a_orb.base_cooldown = 4.0
	a_orb.max_charges = 2
	a_orb.charges = 2
	a_orb.base_damage = 75.0
	a_orb.spirit_damage_scale = 1.2
	a_orb.base_voxel_radius = 3.0
	abilities.append(a_orb)

	# Ability 6 (Key 1 in Play): Kelvin "Frozen Shelter" (Hollow Ice Dome, press 1 again to dismiss)
	var a_dome := AbilitySpec.new()
	a_dome.slot_index = 6
	a_dome.ability_id = "kelvin_frozen_shelter"
	a_dome.display_name = "Ice Dome"
	a_dome.hotkey_label = "1"
	a_dome.delivery_mode = DeliveryMode.TEMP_CONSTRUCT
	a_dome.base_cooldown = 12.0
	a_dome.construct_duration = 6.5
	a_dome.base_voxel_radius = 8.5
	a_dome.spirit_radius_scale = 0.03
	abilities.append(a_dome)

	# Ability 7 (Key 2 in Play): McGinnis "Spectral Wall" (3-Hit Rock Barrier, press 2 again to drop)
	var a_wall := AbilitySpec.new()
	a_wall.slot_index = 7
	a_wall.ability_id = "mcginnis_spectral_wall"
	a_wall.display_name = "Spectral Wall"
	a_wall.hotkey_label = "2"
	a_wall.delivery_mode = DeliveryMode.TEMP_CONSTRUCT
	a_wall.base_cooldown = 9.0
	a_wall.construct_duration = 7.5
	a_wall.base_voxel_radius = 7.0 # Half-width of wall (14 m total)
	a_wall.spirit_radius_scale = 0.04
	abilities.append(a_wall)

func register_target(target: Node3D, max_hp: float = 750.0) -> void:
	if not is_instance_valid(target):
		return
	var id := target.get_instance_id()
	if not target_attributes.has(id):
		var attr := HeroAttributes.new()
		attr.max_health = max_hp
		attr.health = max_hp
		target_attributes[id] = attr
		target_effects[id] = {}
		target_tags[id] = Tag.GROUNDED

func get_ability(ability_id: String) -> AbilitySpec:
	for a in abilities:
		if a.ability_id == ability_id:
			return a
	return null

func can_activate_ability(ability_id: String, current_tags: int) -> Dictionary:
	var a := get_ability(ability_id)
	if a == null:
		return {"ok": false, "reason": "unknown ability"}
	if (current_tags & a.blocked_by_tags) != 0 and (current_tags & Tag.UNSTOPPABLE) == 0:
		return {"ok": false, "reason": "blocked by status effect (Stunned/Silenced/Frozen)"}
	if a.required_tags != Tag.NONE and (current_tags & a.required_tags) != a.required_tags:
		return {"ok": false, "reason": "must be airborne"}
	if a.max_charges > 1:
		if a.charges <= 0:
			return {"ok": false, "reason": "recharging (%.1fs)" % a.cooldown_remaining}
	elif a.cooldown_remaining > 0.0:
		return {"ok": false, "reason": "on cooldown (%.1fs)" % a.cooldown_remaining}
	return {"ok": true, "reason": ""}

func commit_ability_cooldown(ability_id: String) -> AbilitySpec:
	var a := get_ability(ability_id)
	if a == null:
		return null
	var eff_cd := a.base_cooldown * (1.0 - clampf(hero_attributes.cooldown_reduction, 0.0, 0.65))
	if a.max_charges > 1:
		a.charges = maxi(0, a.charges - 1)
		if a.cooldown_remaining <= 0.0:
			a.cooldown_remaining = eff_cd
	else:
		a.cooldown_remaining = eff_cd
	emit_signal("ability_cast_started", a.ability_id, a.slot_index)
	return a

## Applies a StatusEffectSpec to a target (or the hero if `target == owner_node`),
## handling tag cleansing (`Water`/`Frost` cleanses `Burning`), stack promotion
## (`3x Chilled -> Frozen` voxel ice encasement), and stat recalculation.
func apply_status_effect(target: Node3D, spec: StatusEffectSpec) -> void:
	if not is_instance_valid(target):
		return
	var is_self := (target == owner_node)
	var eff_map: Dictionary = hero_effects if is_self else target_effects.get(target.get_instance_id(), {})

	# 1. Cleanse opposing elemental tags (e.g. Chilled/Frozen cleanses Burning; Burning melts Chilled)
	if spec.cleansed_tags_on_apply != Tag.NONE:
		var to_erase: Array[String] = []
		for k in eff_map.keys():
			var existing: StatusEffectSpec = eff_map[k]
			if (existing.granted_tags & spec.cleansed_tags_on_apply) != 0:
				to_erase.append(k)
		for k in to_erase:
			eff_map.erase(k)
			emit_signal("status_effect_removed", target, k)

	# 2. Stack or refresh duration
	if eff_map.has(spec.id):
		var cur: StatusEffectSpec = eff_map[spec.id]
		cur.stacks = mini(cur.max_stacks, cur.stacks + spec.stacks)
		cur.remaining = maxf(cur.remaining, spec.duration)
		# Promote 3x Chilled -> Frozen (Encased in Voxel Ice!)
		if cur.id == "chilled" and cur.stacks >= 3:
			eff_map.erase("chilled")
			emit_signal("status_effect_removed", target, "chilled")
			var frozen := StatusEffectSpec.make_frozen_ice_shell(2.2)
			frozen.encase_origin = target.global_position
			eff_map[frozen.id] = frozen
			if terrain != null and terrain.has_method("trigger_elemental_sphere"):
				terrain.trigger_elemental_sphere(target.global_position + Vector3(0.0, 0.8, 0.0), 2.4, ELEM_FROST)
			emit_signal("status_effect_applied", target, frozen.id, 1, frozen.duration)
			_recompute_entity_tags_and_stats(target)
			return
		emit_signal("status_effect_applied", target, cur.id, cur.stacks, cur.remaining)
	else:
		if spec.encase_in_voxel_ice:
			spec.encase_origin = target.global_position
			if terrain != null and terrain.has_method("trigger_elemental_sphere"):
				terrain.trigger_elemental_sphere(target.global_position + Vector3(0.0, 0.8, 0.0), 2.4, ELEM_FROST)
		eff_map[spec.id] = spec
		if not is_self:
			target_effects[target.get_instance_id()] = eff_map
		emit_signal("status_effect_applied", target, spec.id, spec.stacks, spec.duration)

	_recompute_entity_tags_and_stats(target)

func _recompute_entity_tags_and_stats(target: Node3D) -> void:
	if not is_instance_valid(target):
		return
	var is_self := (target == owner_node)
	var eff_map: Dictionary = hero_effects if is_self else target_effects.get(target.get_instance_id(), {})
	var attr: HeroAttributes = hero_attributes if is_self else target_attributes.get(target.get_instance_id(), null)

	var combined_tags := Tag.NONE
	var speed_mult := 1.0
	var bonus_spirit := 0.0

	for k in eff_map.keys():
		var eff: StatusEffectSpec = eff_map[k]
		combined_tags |= eff.granted_tags
		for _i in range(eff.stacks):
			speed_mult *= eff.speed_multiplier_per_stack
		bonus_spirit += eff.bonus_spirit_per_stack * float(eff.stacks)

	if is_self:
		if active_beam_ability != null:
			combined_tags |= active_beam_ability.granted_tags_while_active
		hero_tags = (hero_tags & (Tag.AIRBORNE | Tag.GROUNDED)) | combined_tags
		hero_attributes.move_speed_multiplier = speed_mult
		hero_attributes.bonus_spirit_power = bonus_spirit
	else:
		var id := target.get_instance_id()
		target_tags[id] = combined_tags
		if attr != null:
			attr.move_speed_multiplier = speed_mult
			attr.bonus_spirit_power = bonus_spirit

func is_target_stunned(target: Node3D) -> bool:
	if not is_instance_valid(target):
		return false
	var t_tags: int = hero_tags if target == owner_node else int(target_tags.get(target.get_instance_id(), 0))
	return (t_tags & (Tag.STUNNED | Tag.FROZEN)) != 0

func format_active_effects(target: Node3D) -> String:
	if not is_instance_valid(target):
		return ""
	var is_self := (target == owner_node)
	var eff_map: Dictionary = hero_effects if is_self else target_effects.get(target.get_instance_id(), {})
	if eff_map.is_empty():
		return "None"
	var parts: PackedStringArray = PackedStringArray()
	for k in eff_map.keys():
		var eff: StatusEffectSpec = eff_map[k]
		if eff.stacks > 1:
			parts.append("%s x%d (%.1fs)" % [eff.display_name, eff.stacks, eff.remaining])
		else:
			parts.append("%s (%.1fs)" % [eff.display_name, eff.remaining])
	return ", ".join(parts)

func format_cooldown_bar() -> String:
	var parts: PackedStringArray = PackedStringArray()
	for a in abilities:
		if a.delivery_mode == DeliveryMode.TOGGLE_CHANNEL:
			parts.append("[%s] %s: %s" % [a.hotkey_label, a.display_name, "ON" if a.is_toggled_on else "OFF"])
		elif a.delivery_mode == DeliveryMode.TEMP_CONSTRUCT and a.active_structure_id > 0:
			var rem: float = terrain.get_structure_remaining_ttl(a.active_structure_id) \
				if (terrain != null and terrain.has_method("get_structure_remaining_ttl")) else 0.0
			if rem > 0.0:
				parts.append("[%s] %s: ACTIVE %.1fs (press %s to drop)" % [a.hotkey_label, a.display_name, rem, a.hotkey_label])
			else:
				a.active_structure_id = 0
				parts.append("[%s] %s: %.1fs" % [a.hotkey_label, a.display_name, a.cooldown_remaining] if a.cooldown_remaining > 0.0 else "[%s] %s: READY" % [a.hotkey_label, a.display_name])
		elif a.max_charges > 1:
			if a.charges < a.max_charges:
				parts.append("[%s] %s: %d/%d (%.1fs)" % [a.hotkey_label, a.display_name, a.charges, a.max_charges, a.cooldown_remaining])
			else:
				parts.append("[%s] %s: %d/%d" % [a.hotkey_label, a.display_name, a.charges, a.max_charges])
		elif a.cooldown_remaining > 0.0:
			parts.append("[%s] %s: %.1fs" % [a.hotkey_label, a.display_name, a.cooldown_remaining])
		else:
			parts.append("[%s] %s: READY" % [a.hotkey_label, a.display_name])
	return " | ".join(parts)

## Evaluates Elemental & Terrain Slam Combos when Lash's Ground Strike impacts `hit_pos`.
func resolve_ground_strike_combo(hit_pos: Vector3, targets: Array[Node3D]) -> Dictionary:
	var a := get_ability("lash_ground_strike")
	var spirit := hero_attributes.get_spirit_power()
	var radius := a.compute_scaled_radius(spirit) if a != null else 5.2
	var dmg := a.compute_scaled_damage(spirit) if a != null else 140.0

	var dom_mat := MAT_SOIL
	if terrain != null and terrain.has_method("get_dominant_material_in_sphere"):
		dom_mat = terrain.get_dominant_material_in_sphere(hit_pos, radius * 0.7)

	var combo_name := "Seismic Crater"
	if dom_mat == MAT_LAVA:
		# Combo 1: Slamming into Lava triggers a Magma Eruption (+35% blast radius & Burning stacks)
		combo_name = "MAGMA ERUPTION COMBO"
		radius *= 1.35
		dmg *= 1.30
		if terrain != null:
			terrain.trigger_elemental_sphere(hit_pos, radius * 0.9, ELEM_LAVA)
		emit_signal("elemental_combo_triggered", combo_name, hit_pos, radius)
	elif dom_mat == MAT_ICE:
		# Combo 2: Slamming onto Kelvin Ice shatters the ice into a Cryo Nova (Chills/Freezes targets)
		combo_name = "CRYO SHATTER NOVA"
		radius *= 1.20
		emit_signal("elemental_combo_triggered", combo_name, hit_pos, radius)
	elif dom_mat == MAT_WATER:
		# Combo 3: Slamming into Water triggers a Quench Geyser
		combo_name = "HYDRO GEYSER"
		if terrain != null:
			terrain.trigger_elemental_sphere(hit_pos, radius, ELEM_WATER)
		emit_signal("elemental_combo_triggered", combo_name, hit_pos, radius)

	if terrain != null:
		terrain.trigger_explosion(hit_pos, radius)

	for t in targets:
		if not is_instance_valid(t):
			continue
		if t.global_position.distance_to(hit_pos) <= radius + 2.5:
			deal_ability_damage(t, dmg, true, "lash_ground_strike")
			if dom_mat == MAT_LAVA:
				apply_status_effect(t, StatusEffectSpec.make_burning(40.0, 4.0))
			elif dom_mat == MAT_ICE:
				apply_status_effect(t, StatusEffectSpec.make_frozen_ice_shell(2.2))

	return {"combo": combo_name, "radius": radius, "damage": dmg, "material": dom_mat}

func deal_ability_damage(target: Node3D, raw_damage: float, is_spirit: bool, ability_id: String) -> float:
	if not is_instance_valid(target):
		return 0.0
	register_target(target)
	var attr: HeroAttributes = hero_attributes if target == owner_node else target_attributes.get(target.get_instance_id(), null)
	if attr == null:
		return 0.0
	var dealt := attr.apply_damage(raw_damage, is_spirit)
	# Refill target HP after 1s if it hits 0 in the sandbox demo so you can keep testing combos
	if attr.health <= 0.0:
		attr.health = attr.max_health
	emit_signal("damage_dealt", target, dealt, is_spirit, ability_id)
	return dealt

## Starts Bebop's Channeled Hyper Beam (`Key Z`).
func start_hyper_beam(current_tags: int) -> bool:
	var check := can_activate_ability("bebop_hyper_beam", current_tags)
	if not bool(check.get("ok", false)):
		emit_signal("ability_cast_blocked", "bebop_hyper_beam", String(check.get("reason", "")))
		return false
	active_beam_ability = commit_ability_cooldown("bebop_hyper_beam")
	beam_timer = 0.0
	beam_voxel_tick_accum = 0.0
	beam_last_hit_pos = Vector3(INF, INF, INF)
	_recompute_entity_tags_and_stats(owner_node)
	return true

## Casts or dismisses Kelvin's Frozen Shelter (Hollow Voxel Ice Dome, `Key 1` in Play Mode).
func cast_or_dismiss_frozen_shelter(center: Vector3, current_tags: int) -> Dictionary:
	var a := get_ability("kelvin_frozen_shelter")
	if a == null or terrain == null or not terrain.has_method("spawn_temporary_dome"):
		return {"action": "failed", "reason": "missing spawn_temporary_dome binding"}

	# If dome is currently active, pressing Key 1 again dismisses it early!
	if a.active_structure_id > 0:
		var rem: float = terrain.get_structure_remaining_ttl(a.active_structure_id)
		if rem > 0.0:
			var old_id := a.active_structure_id
			a.active_structure_id = 0
			terrain.remove_structure(old_id)
			return {"action": "dismissed", "structure_id": old_id}
		a.active_structure_id = 0

	var check := can_activate_ability("kelvin_frozen_shelter", current_tags)
	if not bool(check.get("ok", false)):
		emit_signal("ability_cast_blocked", "kelvin_frozen_shelter", String(check.get("reason", "")))
		return {"action": "blocked", "reason": String(check.get("reason", ""))}

	var spirit := hero_attributes.get_spirit_power()
	var dome_r := a.compute_scaled_radius(spirit)
	var struct_id: int = terrain.spawn_temporary_dome(center, dome_r, 2.0, MAT_ICE, a.construct_duration)
	if struct_id > 0:
		commit_ability_cooldown("kelvin_frozen_shelter")
		a.active_structure_id = struct_id
		return {"action": "spawned", "structure_id": struct_id, "radius": dome_r, "duration": a.construct_duration}
	return {"action": "failed", "reason": "no chunks in range"}

## Casts or drops McGinnis's Spectral Wall (3-Hit Destructible Stone Barrier, `Key 2` in Play Mode).
func cast_or_dismiss_spectral_wall(center: Vector3, forward_dir: Vector3, current_tags: int) -> Dictionary:
	var a := get_ability("mcginnis_spectral_wall")
	if a == null or terrain == null or not terrain.has_method("spawn_temporary_wall"):
		return {"action": "failed", "reason": "missing spawn_temporary_wall binding"}

	# If wall is currently active, pressing Key 2 again drops it early!
	if a.active_structure_id > 0:
		var rem: float = terrain.get_structure_remaining_ttl(a.active_structure_id)
		if rem > 0.0:
			var old_id := a.active_structure_id
			a.active_structure_id = 0
			terrain.remove_structure(old_id)
			return {"action": "dismissed", "structure_id": old_id}
		a.active_structure_id = 0

	var check := can_activate_ability("mcginnis_spectral_wall", current_tags)
	if not bool(check.get("ok", false)):
		emit_signal("ability_cast_blocked", "mcginnis_spectral_wall", String(check.get("reason", "")))
		return {"action": "blocked", "reason": String(check.get("reason", ""))}

	var spirit := hero_attributes.get_spirit_power()
	var half_width := a.compute_scaled_radius(spirit)
	var right_dir := Vector3(-forward_dir.z, 0.0, forward_dir.x).normalized()
	if right_dir.length_squared() < 1e-4:
		right_dir = Vector3.RIGHT
	var wall_a := center - right_dir * half_width
	var wall_b := center + right_dir * half_width
	var struct_id: int = terrain.spawn_temporary_wall(wall_a, wall_b, 6.5, 2.2, MAT_HARD_ROCK, a.construct_duration)
	if struct_id > 0:
		commit_ability_cooldown("mcginnis_spectral_wall")
		a.active_structure_id = struct_id
		return {"action": "spawned", "structure_id": struct_id, "width": half_width * 2.0, "duration": a.construct_duration}
	return {"action": "failed", "reason": "no chunks in range"}

## Spawns a Sub-Stepped SDF-Bouncing Cryo Orb (`Key X`).
func spawn_bouncing_cryo_orb(origin: Vector3, aim_dir: Vector3, current_tags: int, parent_for_mesh: Node3D) -> bool:
	var check := can_activate_ability("cryo_bounce_orb", current_tags)
	if not bool(check.get("ok", false)):
		emit_signal("ability_cast_blocked", "cryo_bounce_orb", String(check.get("reason", "")))
		return false
	var a := commit_ability_cooldown("cryo_bounce_orb")
	var spirit := hero_attributes.get_spirit_power()

	var proj := BouncingVoxelProjectile.new()
	proj.position = origin + aim_dir * 0.9
	proj.velocity = aim_dir * 26.0 + Vector3(0.0, 3.2, 0.0)
	proj.bounces_remaining = 2
	proj.elemental_kind = ELEM_FROST
	proj.splash_radius = a.compute_scaled_radius(spirit)
	proj.direct_damage = a.compute_scaled_damage(spirit)

	if is_instance_valid(parent_for_mesh):
		var mi := MeshInstance3D.new()
		var sph := SphereMesh.new()
		sph.radius = proj.radius
		sph.height = proj.radius * 2.0
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color(0.2, 0.92, 1.0)
		mat.emission_enabled = true
		mat.emission = Color(0.15, 0.85, 1.0) * 1.8
		sph.material = mat
		mi.mesh = sph
		parent_for_mesh.add_child(mi)
		mi.global_position = proj.position
		proj.mesh_instance = mi

	active_projectiles.append(proj)
	return true

## Ticks all cooldowns, status effects (DoTs, voxel-ice break checks), channeled beams,
## and sub-stepped bouncing projectiles.
func start_weapon_reload() -> bool:
	if primary_weapon.is_reloading or primary_weapon.current_ammo >= primary_weapon.mag_size:
		return false
	primary_weapon.is_reloading = true
	primary_weapon.reload_remaining = primary_weapon.reload_duration
	emit_signal("weapon_reload_started", primary_weapon.reload_duration)
	return true

func format_weapon_hud(is_sliding: bool, is_ads: bool) -> String:
	var w := primary_weapon
	var ads_tag := " [ADS]" if is_ads else ""
	if w.is_reloading:
		return "%s%s: RELOADING (%.1fs)" % [w.display_name, ads_tag, w.reload_remaining]
	var slide_tag := " [SLIDE: INFINITE AMMO!]" if is_sliding else ""
	return "%s%s: %d/%d%s" % [w.display_name, ads_tag, w.current_ammo, w.mag_size, slide_tag]

## Ray-cylinder/capsule intersection for enemy/companion hitboxes with Headshot weakpoint detection.
## Returns {"hit": bool, "t": float, "point": Vector3, "headshot": bool}
func _intersect_ray_with_target(ray_from: Vector3, ray_to: Vector3, target: Node3D, radius: float = 0.72, height: float = 1.95) -> Dictionary:
	if not is_instance_valid(target):
		return {"hit": false}
	var base := target.global_position
	var seg := ray_to - ray_from
	var seg_len := seg.length()
	if seg_len <= 1e-4:
		return {"hit": false}
	var dir := seg / seg_len

	# Closest approach in 3D to the vertical target spine [base + (0, 0.25, 0) .. base + (0, height, 0)]
	var center := base + Vector3(0.0, height * 0.5, 0.0)
	var to_center := center - ray_from
	var proj := clampf(to_center.dot(dir), 0.0, seg_len)
	var closest_ray_pt := ray_from + dir * proj

	if closest_ray_pt.y < base.y - 0.15 or closest_ray_pt.y > base.y + height + 0.20:
		return {"hit": false}
	var horiz_dist := Vector2(closest_ray_pt.x - base.x, closest_ray_pt.z - base.z).length()
	if horiz_dist > radius:
		return {"hit": false}

	# Top 32% of capsule is the Headshot Weakpoint zone!
	var rel_y := clampf((closest_ray_pt.y - base.y) / height, 0.0, 1.0)
	var is_headshot := (rel_y >= 0.68)
	return {
		"hit": true,
		"dist": proj,
		"point": closest_ray_pt,
		"headshot": is_headshot,
	}

## Fires a primary round with Deadlock Slide-Infinite-Ammo, Headshot Crits,
## Voxel Micro-Chipping, and up to `max_ricochets` SDF Surface-Normal Bounces!
func fire_ricochet_weapon(
	muzzle_pos: Vector3,
	aim_origin: Vector3,
	aim_dir: Vector3,
	max_dist: float,
	is_sliding_infinite_ammo: bool,
	targets: Array[Node3D]
) -> Dictionary:
	var w := primary_weapon
	if (hero_tags & (Tag.STUNNED | Tag.FROZEN)) != 0 and (hero_tags & Tag.UNSTOPPABLE) == 0:
		return {"fired": false, "reason": "stunned"}
	if w.fire_cooldown > 0.0:
		return {"fired": false, "reason": "cooldown"}
	if w.is_reloading:
		if not is_sliding_infinite_ammo:
			return {"fired": false, "reason": "reloading"}
		# Sliding cancels reload stall and fires with infinite slide ammo!
		w.is_reloading = false
		w.reload_remaining = 0.0

	if w.current_ammo <= 0 and not is_sliding_infinite_ammo:
		start_weapon_reload()
		return {"fired": false, "reason": "empty_reloading"}

	w.fire_cooldown = w.fire_interval
	if not is_sliding_infinite_ammo:
		w.current_ammo = maxi(0, w.current_ammo - 1)
		if w.current_ammo == 0:
			start_weapon_reload()

	var cur_from := aim_origin
	var cur_dir := aim_dir.normalized()
	var rem_dist := max_dist
	var bounce_count := 0
	var any_crit := false
	var total_damage_dealt := 0.0
	var hit_target_node: Node3D = null
	var chipped_points: PackedVector3Array = PackedVector3Array()

	for seg_idx in range(w.max_ricochets + 1):
		var seg_end := cur_from + cur_dir * rem_dist
		var vox_hit := Vector3(INF, INF, INF)
		if terrain != null:
			vox_hit = terrain.raycast_terrain(cur_from, seg_end)
		var vox_dist := cur_from.distance_to(vox_hit) if is_finite(vox_hit.x) else INF

		# Check if any target is hit before the voxel wall on this segment
		var best_target: Node3D = null
		var best_target_hit: Dictionary = {}
		var best_target_dist := vox_dist
		for t in targets:
			var th := _intersect_ray_with_target(cur_from, seg_end, t)
			if bool(th.get("hit", false)) and float(th.get("dist", INF)) < best_target_dist:
				best_target = t
				best_target_hit = th
				best_target_dist = float(th.get("dist", INF))

		var tracer_start := muzzle_pos if seg_idx == 0 else cur_from
		if best_target != null:
			var hit_pt: Vector3 = best_target_hit.get("point", seg_end)
			var is_headshot := bool(best_target_hit.get("headshot", false))
			any_crit = any_crit or is_headshot

			var t_Seg := BulletTracerSegment.new()
			t_Seg.from_pos = tracer_start
			t_Seg.to_pos = hit_pt
			t_Seg.bounce_index = seg_idx
			t_Seg.is_crit = is_headshot
			active_tracers.append(t_Seg)

			var dmg := w.base_bullet_damage
			if is_headshot:
				dmg *= w.headshot_multiplier
			if seg_idx > 0:
				dmg *= w.ricochet_bonus_multiplier
			# Bonus 25% shatter damage against Frozen or Wall-Pin Stunned targets!
			if is_target_stunned(best_target):
				dmg *= 1.25

			total_damage_dealt += deal_ability_damage(best_target, dmg, false, "pulse_carbine_ricochet" if seg_idx > 0 else "pulse_carbine")
			hit_target_node = best_target
			break

		if is_finite(vox_hit.x):
			var t_seg := BulletTracerSegment.new()
			t_seg.from_pos = tracer_start
			t_seg.to_pos = vox_hit
			t_seg.bounce_index = seg_idx
			t_seg.is_crit = false
			active_tracers.append(t_seg)

			chipped_points.push_back(vox_hit)
			if terrain != null:
				terrain.trigger_explosion(vox_hit, w.bullet_chip_radius)

			# Compute analytical O(1) SDF normal for Ricochet bounce!
			if seg_idx < w.max_ricochets and terrain != null and terrain.has_method("sample_sdf_normal"):
				var surf_n: Vector3 = terrain.sample_sdf_normal(vox_hit)
				if surf_n.length_squared() > 1e-4:
					cur_dir = cur_dir.bounce(surf_n).normalized()
					cur_from = vox_hit + surf_n * 0.35
					rem_dist = maxf(8.0, (rem_dist - vox_dist) * 0.65)
					bounce_count += 1
					continue
			break
		else:
			var t_seg := BulletTracerSegment.new()
			t_seg.from_pos = tracer_start
			t_seg.to_pos = seg_end
			t_seg.bounce_index = seg_idx
			t_seg.is_crit = false
			active_tracers.append(t_seg)
			break

	emit_signal("weapon_fired", w.current_ammo, is_sliding_infinite_ammo, bounce_count, any_crit)
	return {
		"fired": true,
		"ammo": w.current_ammo,
		"infinite_slide": is_sliding_infinite_ammo,
		"ricochets": bounce_count,
		"crit": any_crit,
		"damage": total_damage_dealt,
		"target": hit_target_node,
		"chipped_points": chipped_points,
	}

func tick_ability_system(
	delta: float,
	beam_origin: Vector3,
	beam_aim_dir: Vector3,
	max_beam_dist: float,
	targets: Array[Node3D]
) -> void:
	# 0. Tick Primary Weapon fire cooldown, reload timer & 3D bullet tracers
	if primary_weapon.fire_cooldown > 0.0:
		primary_weapon.fire_cooldown = maxf(0.0, primary_weapon.fire_cooldown - delta)
	if primary_weapon.is_reloading:
		primary_weapon.reload_remaining -= delta
		if primary_weapon.reload_remaining <= 0.0:
			primary_weapon.is_reloading = false
			primary_weapon.reload_remaining = 0.0
			primary_weapon.current_ammo = primary_weapon.mag_size
			emit_signal("weapon_reload_completed", primary_weapon.mag_size)

	for i in range(active_tracers.size() - 1, -1, -1):
		active_tracers[i].ttl -= delta
		if active_tracers[i].ttl <= 0.0:
			active_tracers.remove_at(i)

	# 1. Tick ability cooldowns & charge regeneration
	var eff_cdr := clampf(hero_attributes.cooldown_reduction, 0.0, 0.65)
	for a in abilities:
		if a.cooldown_remaining > 0.0:
			a.cooldown_remaining = maxf(0.0, a.cooldown_remaining - delta)
			if a.cooldown_remaining <= 0.0 and a.max_charges > 1 and a.charges < a.max_charges:
				a.charges += 1
				if a.charges < a.max_charges:
					a.cooldown_remaining = a.base_cooldown * (1.0 - eff_cdr)

	# 2. Tick Hero & Target Status Effects
	if is_instance_valid(owner_node):
		_tick_entity_effects(owner_node, hero_effects, delta)
	for t in targets:
		if is_instance_valid(t):
			register_target(t)
			var t_effs: Dictionary = target_effects.get(t.get_instance_id(), {})
			_tick_entity_effects(t, t_effs, delta)

	# 3. Tick Active Channeled Bebop Hyper Beam (60+ FPS laser + 11 Hz throttled voxel carves)
	if active_beam_ability != null:
		beam_timer += delta
		beam_voxel_tick_accum += delta
		beam_current_origin = beam_origin
		var ray_end := beam_origin + beam_aim_dir * max_beam_dist
		var hit_pos := ray_end
		var hit_voxel := false
		if terrain != null:
			var v_hit: Vector3 = terrain.raycast_terrain(beam_origin, ray_end)
			if is_finite(v_hit.x):
				hit_pos = v_hit
				hit_voxel = true
		beam_current_end = hit_pos

		if beam_voxel_tick_accum >= active_beam_ability.channel_tick_interval:
			beam_voxel_tick_accum = 0.0
			var spirit := hero_attributes.get_spirit_power()
			var r := active_beam_ability.compute_scaled_radius(spirit)
			var tick_dmg := active_beam_ability.compute_scaled_damage(spirit) * active_beam_ability.channel_tick_interval
			if hit_voxel and terrain != null:
				if is_finite(beam_last_hit_pos.x) and beam_last_hit_pos.distance_to(hit_pos) <= r * 3.5:
					terrain.carve_capsule(beam_last_hit_pos, hit_pos, r)
				else:
					terrain.trigger_explosion(hit_pos, r)
				terrain.trigger_elemental_sphere(hit_pos, r * 1.15, ELEM_LAVA)
				var hit_n: Vector3 = terrain.sample_sdf_normal(hit_pos)
				beam_last_hit_pos = hit_pos
				emit_signal("ability_channel_ticked", active_beam_ability.ability_id, hit_pos, hit_n)

			for t in targets:
				if not is_instance_valid(t):
					continue
				if t.global_position.distance_to(hit_pos) <= r + 2.2:
					deal_ability_damage(t, tick_dmg, true, active_beam_ability.ability_id)
					apply_status_effect(t, StatusEffectSpec.make_burning(36.0, 3.5))

		if beam_timer >= active_beam_ability.channel_duration:
			var done_id := active_beam_ability.ability_id
			active_beam_ability = null
			beam_last_hit_pos = Vector3(INF, INF, INF)
			_recompute_entity_tags_and_stats(owner_node)
			emit_signal("ability_completed", done_id)

	# 4. Step SDF-Bouncing Projectiles (Cryo Orbs)
	for i in range(active_projectiles.size() - 1, -1, -1):
		var proj := active_projectiles[i]
		proj.age += delta
		if proj.age >= proj.max_lifetime:
			_destroy_projectile(i)
			continue

		proj.velocity.y -= 18.0 * delta
		var next_pos := proj.position + proj.velocity * delta

		# Check direct hit on targets first
		var hit_target: Node3D = null
		for t in targets:
			if is_instance_valid(t) and (t.global_position + Vector3(0.0, 0.9, 0.0)).distance_to(next_pos) <= proj.radius + 1.1:
				hit_target = t
				break

		if hit_target != null:
			if terrain != null:
				terrain.trigger_elemental_sphere(hit_target.global_position, proj.splash_radius, proj.elemental_kind)
			deal_ability_damage(hit_target, proj.direct_damage, true, "cryo_bounce_orb")
			apply_status_effect(hit_target, StatusEffectSpec.make_chilled(3.5))
			# Direct orb hit grants 2 stacks of Chilled (2nd hit promotes straight to Frozen ice shell!)
			apply_status_effect(hit_target, StatusEffectSpec.make_chilled(3.5))
			_destroy_projectile(i)
			continue

		# Check O(1) swept sphere / raycast against voxel terrain
		var hit_pt := Vector3(INF, INF, INF)
		if terrain != null:
			hit_pt = terrain.sweep_sphere(proj.position, next_pos, proj.radius) if terrain.has_method("sweep_sphere") else terrain.raycast_terrain(proj.position, next_pos)

		if is_finite(hit_pt.x):
			var surf_n: Vector3 = terrain.sample_sdf_normal(hit_pt) if terrain.has_method("sample_sdf_normal") else Vector3.UP
			terrain.trigger_elemental_sphere(hit_pt, proj.splash_radius * 0.82, proj.elemental_kind)
			for t in targets:
				if is_instance_valid(t) and t.global_position.distance_to(hit_pt) <= proj.splash_radius + 1.2:
					deal_ability_damage(t, proj.direct_damage * 0.6, true, "cryo_bounce_orb")
					apply_status_effect(t, StatusEffectSpec.make_chilled(3.5))

			if proj.bounces_remaining > 0:
				proj.bounces_remaining -= 1
				proj.position = hit_pt + surf_n * (proj.radius + 0.15)
				proj.velocity = proj.velocity.bounce(surf_n) * 0.78
				if is_instance_valid(proj.mesh_instance):
					proj.mesh_instance.global_position = proj.position
			else:
				_destroy_projectile(i)
		else:
			proj.position = next_pos
			if is_instance_valid(proj.mesh_instance):
				proj.mesh_instance.global_position = proj.position

func _tick_entity_effects(entity: Node3D, eff_map: Dictionary, delta: float) -> void:
	if eff_map.is_empty():
		return
	var expired: Array[String] = []
	for k in eff_map.keys():
		var eff: StatusEffectSpec = eff_map[k]
		eff.remaining -= delta
		eff.tick_accum += delta

		if eff.tick_interval > 0.0 and eff.tick_accum >= eff.tick_interval:
			eff.tick_accum -= eff.tick_interval
			if eff.damage_per_tick > 0.0:
				deal_ability_damage(entity, eff.damage_per_tick * float(eff.stacks), true, eff.id)
			if eff.heal_per_tick > 0.0:
				var attr: HeroAttributes = hero_attributes if entity == owner_node else target_attributes.get(entity.get_instance_id(), null)
				if attr != null:
					attr.apply_heal(eff.heal_per_tick * float(eff.stacks))

		# Early break check for Voxel Ice Encasement: if an explosion or Lava destroys the Ice around the target, break free early!
		if eff.encase_in_voxel_ice and eff.remaining > 0.25 and (eff.duration - eff.remaining) > 0.35 and terrain != null:
			var dom: int = terrain.get_dominant_material_in_sphere(entity.global_position + Vector3(0.0, 0.8, 0.0), 2.2)
			if dom != MAT_ICE:
				expired.append(k)
				continue

		if eff.remaining <= 0.0:
			expired.append(k)

	if not expired.is_empty():
		for k in expired:
			eff_map.erase(k)
			emit_signal("status_effect_removed", entity, k)
		_recompute_entity_tags_and_stats(entity)

func _destroy_projectile(idx: int) -> void:
	var proj := active_projectiles[idx]
	if is_instance_valid(proj.mesh_instance):
		proj.mesh_instance.queue_free()
	active_projectiles.remove_at(idx)
