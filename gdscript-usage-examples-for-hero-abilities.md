# 1. Ricochet Projectile / Viscous Goo Ball Bounce (O(1) SDF normal, zero trimesh edge snagging)
var hit := terrain.raycast_terrain(prev_pos, next_pos)
if is_finite(hit.x):
	var normal := terrain.sample_sdf_normal(hit)
	velocity = velocity.bounce(normal) * 0.85

# 2. Kelvin-Style Continuous Sloped Ice Path Ribbon
terrain.trigger_elemental_capsule(prev_hero_feet, cur_hero_feet, 4.5, ELEMENTAL_FROST)

# 3. Channeled Laser / Drill Dash Tunnel Carve
terrain.carve_capsule(beam_origin, beam_hit_point, 2.5)

# 4. Elemental Spell Combo Check on Impact
var dominant_mat := terrain.get_dominant_material_in_sphere(impact_pos, 4.0)
if dominant_mat == 4: # Water -> Steam explosion bonus
	_spawn_steam_cloud(impact_pos)
elif dominant_mat == 2 and terrain.get_damage_at(impact_pos) >= 0.6:
	# Rock is already on its 2nd crack stage -> shatter bonus!
	_apply_shatter_bonus_damage(impact_pos)