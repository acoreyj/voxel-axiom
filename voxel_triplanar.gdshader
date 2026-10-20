shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec3 grass_color : source_color = vec3(0.24, 0.56, 0.20);
uniform vec3 dirt_color  : source_color = vec3(0.45, 0.34, 0.23);
uniform vec3 rock_color  : source_color = vec3(0.39, 0.40, 0.43);
uniform vec3 hard_rock_color : source_color = vec3(0.22, 0.25, 0.33);
uniform vec3 boulder_color : source_color = vec3(0.55, 0.48, 0.40);
uniform vec3 lava_color     : source_color = vec3(1.00, 0.32, 0.04);
uniform vec3 water_color    : source_color = vec3(0.10, 0.45, 0.85);
uniform vec3 obsidian_color : source_color = vec3(0.08, 0.06, 0.14);
uniform vec3 ice_color      : source_color = vec3(0.62, 0.88, 0.98);
uniform vec3 crack_glow_color : source_color = vec3(1.0, 0.42, 0.08);
// Debug overlay: tints accumulated damage light-yellow -> bright red and glows along
// fracture lines. Uncheck for the realistic crack-only look used in normal play.
uniform bool debug_damage_indicator = true;
uniform float slope_sharpness : hint_range(1.0, 16.0) = 6.0;
uniform float noise_scale : hint_range(0.01, 1.0) = 0.14;
// Chiseled geometric facet normals: blends the screen-space triangle face normal
// into Hard Rock and painted monument surfaces so 1.0 m Surface Nets meshes show
// crisp monolithic planes on brow, nose bridge, lips and jaw without making
// curved cheeks look blocky.
uniform float chisel_normal_strength : hint_range(0.0, 1.0) = 0.24;

varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_damage_ratio;
varying float v_is_hard_rock;
// COLOR.b carries two disjoint domains: elemental materials use shader_material_code
// >= 0.25 (Lava 0.25, Water 0.50, Obsidian 0.75, Ice 1.00), while rock materials stay
// below 0.25 carrying hits-to-break / 255 (1/255, 3/255, 5/255).
varying float v_hits_to_break;
varying float v_material_code;
// Per-voxel baked RGB5551 albedo, packed by ServerSlotManager into UV.xy + UV2.x
// and gated by COLOR.a (0 = unpainted interior, 1 = painted surface).
varying vec3 v_baked_albedo;
varying float v_baked_weight;

float hash31(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float value_noise3d(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(hash31(i + vec3(0, 0, 0)), hash31(i + vec3(1, 0, 0)), f.x),
		    mix(hash31(i + vec3(0, 1, 0)), hash31(i + vec3(1, 1, 0)), f.x), f.y),
		mix(mix(hash31(i + vec3(0, 0, 1)), hash31(i + vec3(1, 0, 1)), f.x),
		    mix(hash31(i + vec3(0, 1, 1)), hash31(i + vec3(1, 1, 1)), f.x), f.y),
		f.z);
}

// 3D Voronoi cell-edge distance for progressive Bananza-style fracture lines
float crack_voronoi_edge(vec3 p) {
	vec3 ip = floor(p);
	vec3 fp = fract(p);
	float d1 = 8.0;
	float d2 = 8.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 offset = vec3(float(x), float(y), float(z));
				vec3 h = vec3(
					hash31(ip + offset),
					hash31(ip + offset + vec3(17.0, 31.0, 47.0)),
					hash31(ip + offset + vec3(59.0, 83.0, 97.0))
				);
				vec3 r = offset + h - fp;
				float d = dot(r, r);
				if (d < d1) {
					d2 = d1;
					d1 = d;
				} else if (d < d2) {
					d2 = d;
				}
			}
		}
	}
	return sqrt(d2) - sqrt(d1);
}

// Same cell-edge distance as crack_voronoi_edge, plus the random id (0..1) of the
// nearest cell. Callers use the id as the line's onset so each fracture line only
// appears once the damage level passes it: higher damage adds cracks rather than
// just darkening the ones already there.
vec2 crack_voronoi_edge_rnd(vec3 p) {
	vec3 ip = floor(p);
	vec3 fp = fract(p);
	float d1 = 8.0;
	float d2 = 8.0;
	float id1 = 0.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 offset = vec3(float(x), float(y), float(z));
				vec3 h = vec3(
					hash31(ip + offset),
					hash31(ip + offset + vec3(17.0, 31.0, 47.0)),
					hash31(ip + offset + vec3(59.0, 83.0, 97.0))
				);
				vec3 r = offset + h - fp;
				float d = dot(r, r);
				if (d < d1) {
					d2 = d1;
					d1 = d;
					id1 = hash31(ip + offset + vec3(7.0, 13.0, 29.0));
				} else if (d < d2) {
					d2 = d;
				}
			}
		}
	}
	return vec2(sqrt(d2) - sqrt(d1), id1);
}

// Five discrete damage colors, light yellow through bright red. The caller picks the
// index by hits remaining, so every material turns bright red on its final intact state.
vec3 damage_level_color(float level_index) {
	if (level_index < 0.5) return vec3(1.00, 0.96, 0.55);
	if (level_index < 1.5) return vec3(1.00, 0.85, 0.25);
	if (level_index < 2.5) return vec3(1.00, 0.55, 0.08);
	if (level_index < 3.5) return vec3(1.00, 0.28, 0.04);
	return vec3(1.00, 0.07, 0.02);
}

// Baked RGB5551 albedo arrives sRGB-encoded (the offline baker samples source
// textures directly); ALBEDO expects linear, so convert before mixing.
vec3 baked_srgb_to_linear(vec3 c) {
	c = clamp(c, 0.0, 1.0);
	return mix(c / 12.92, pow((c + vec3(0.055)) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void vertex() {
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_NORMAL_MATRIX * NORMAL).xyz);
	v_damage_ratio = COLOR.r;
	v_is_hard_rock = COLOR.g;
	v_hits_to_break = COLOR.b * 255.0;
	v_material_code = (COLOR.b >= 0.2) ? COLOR.b : 0.0;
	v_baked_weight = COLOR.a;
	v_baked_albedo = vec3(UV.x, UV.y, UV2.x);
}

void fragment() {
	vec3 n = normalize(v_world_normal);
	float up_factor = clamp(n.y, 0.0, 1.0);
	float flat_mask = pow(up_factor, slope_sharpness);
	float height_mask = clamp((v_world_pos.y - 45.0) / 20.0, 0.0, 1.0);

	float n_macro = value_noise3d(v_world_pos * noise_scale);
	float n_detail = value_noise3d(v_world_pos * (noise_scale * 3.7));
	float tint = mix(0.82, 1.18, 0.65 * n_macro + 0.35 * n_detail);

	vec3 top_col = mix(dirt_color, grass_color, height_mask);
	vec3 base_col = mix(rock_color, top_col, flat_mask) * tint;

	// Blend in dark metallic-basalt tint for 3-hit Hard Rock formations
	float hard_rock_weight = smoothstep(0.35, 0.75, clamp(v_is_hard_rock, 0.0, 1.0));
	if (hard_rock_weight > 0.01) {
		float vein_tex = value_noise3d(v_world_pos * 0.45);
		vec3 hr_col = hard_rock_color * mix(0.85, 1.20, vein_tex);
		base_col = mix(base_col, hr_col, hard_rock_weight);
	}

	// Rock-domain flag: v_hits_to_break is only meaningful below the elemental band.
	float is_elemental = step(0.2, v_material_code);

	// Placed 5-hit boulders ride the same persistent-damage ramp as 3-hit Hard Rock, so they
	// are separated here by hits-to-break (COLOR.b): a lighter, warmer speckled granite instead
	// of dark metallic basalt. Boulder weighs 1 only where the 5-hit material was tagged, and
	// never on elemental materials (their COLOR.b is a material code, not a hit count).
	float boulder_weight = hard_rock_weight * step(3.5, v_hits_to_break) * (1.0 - is_elemental);
	if (boulder_weight > 0.01) {
		float speck_coarse = value_noise3d(v_world_pos * 1.6);
		float speck_fine   = value_noise3d(v_world_pos * 7.3);
		float speck = 0.60 * speck_coarse + 0.40 * speck_fine;
		vec3 boulder_col = boulder_color * mix(0.72, 1.25, speck);
		base_col = mix(base_col, boulder_col, boulder_weight);
	}

	// Elemental Material Shading (v_material_code: 0.25 = Lava, 0.50 = Water,
	// 0.75 = Obsidian, 1.00 = Ice). Weights come from the disjoint COLOR.b band.
	float lava_w     = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 0.25));
	float water_w    = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 0.50));
	float obsidian_w = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 0.75));
	float ice_w      = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 1.00));
	vec3 emission_col = vec3(0.0);
	float roughness_val = mix(0.90, mix(mix(0.75, 0.52, hard_rock_weight * (1.0 - boulder_weight)),
		0.88, boulder_weight), flat_mask);
	float metallic_val = 0.18 * hard_rock_weight * (1.0 - boulder_weight);

	// Blend in per-voxel baked RGB5551 albedo (UV.xy + UV2.x, gated by COLOR.a).
	// Unpainted interior voxels (v_baked_weight == 0.0) fall back to the procedural
	// Hard Rock / Soil / boulder albedo above; elemental shading below still wins.
	// Un-premultiply v_baked_albedo by max(paint_w, 0.001) so triangles straddling
	// painted (w=1, UV=RGB) and unpainted (w=0, UV=0) vertices never darken toward
	// black at the statue base or crater rim; the sRGB->linear conversion is applied
	// after un-premultiplying since the baker samples source textures directly.
	float paint_w = clamp(v_baked_weight, 0.0, 1.0);
	if (paint_w > 0.01) {
		vec3 unpremult_albedo = clamp(v_baked_albedo / max(paint_w, 0.001), vec3(0.0), vec3(1.0));
		vec3 painted_col = baked_srgb_to_linear(unpremult_albedo) * mix(0.92, 1.08, n_detail);
		float blend_alpha = smoothstep(0.04, 0.80, paint_w);
		base_col = mix(base_col, painted_col, blend_alpha);
		roughness_val = mix(roughness_val, 0.76, blend_alpha);
		metallic_val = mix(metallic_val, 0.03, blend_alpha);
	}

	// Chiseled Geometric Facet Normals for Hard Rock & Painted Stone Monuments:
	// Blends view-space triangle face normal with the smooth TSDF normal so 1.0 m
	// Surface Nets meshes display crisp, monolithic chiseled planes on brow, nose
	// bridge, lips, and jaw.
	float stone_facet_mask = max(hard_rock_weight * 0.80, paint_w);
	if (stone_facet_mask > 0.01 && chisel_normal_strength > 0.001) {
		vec3 geom_n = cross(dFdx(VERTEX), dFdy(VERTEX));
		float geom_len = length(geom_n);
		if (geom_len > 1e-6) {
			geom_n /= geom_len;
			if (dot(geom_n, NORMAL) < 0.0) {
				geom_n = -geom_n;
			}
			float facet_blend = clamp(stone_facet_mask * chisel_normal_strength, 0.0, 0.85);
			NORMAL = normalize(mix(NORMAL, geom_n, facet_blend));
		}
	}

	if (is_elemental > 0.5) {
		if (lava_w > 0.01) {
			float magma_crust = crack_voronoi_edge(v_world_pos * 0.55);
			float molten_vein = 1.0 - smoothstep(0.0, 0.28, magma_crust);
			vec3 crust_col = mix(vec3(0.18, 0.05, 0.03), lava_color, molten_vein);
			base_col = mix(base_col, crust_col, lava_w);
			emission_col += lava_color * (0.65 + 1.35 * molten_vein) * lava_w;
			roughness_val = mix(roughness_val, 0.65, lava_w);
			metallic_val = mix(metallic_val, 0.0, lava_w);
		}
		if (water_w > 0.01) {
			base_col = mix(base_col, water_color * tint, water_w);
			roughness_val = mix(roughness_val, 0.15, water_w);
			metallic_val = mix(metallic_val, 0.10, water_w);
		}
		if (obsidian_w > 0.01) {
			float glass_sheen = value_noise3d(v_world_pos * 0.90);
			vec3 obs_col = obsidian_color * mix(0.80, 1.35, glass_sheen);
			base_col = mix(base_col, obs_col, obsidian_w);
			roughness_val = mix(roughness_val, 0.18, obsidian_w);
			metallic_val = mix(metallic_val, 0.45, obsidian_w);
		}
		if (ice_w > 0.01) {
			float frost_crystal = value_noise3d(v_world_pos * 1.40);
			vec3 frost_col = mix(ice_color, vec3(0.88, 0.96, 1.0), frost_crystal * 0.5);
			base_col = mix(base_col, frost_col, ice_w);
			emission_col += vec3(0.08, 0.22, 0.30) * ice_w;
			roughness_val = mix(roughness_val, 0.12, ice_w);
			metallic_val = mix(metallic_val, 0.25, ice_w);
		}
	}

	// Gate the progressive crack network strictly to Hard Rock and placed boulders
	// (v_is_hard_rock) so grass and dirt never render rock cracks.
	float crack_mask = 0.0;
	float rock_damage = v_damage_ratio * hard_rock_weight;
	// Damage level from hits remaining: 3-hit rocks and 5-hit boulders traverse the same
	// five-step ramp and both reach their final cracked state with one hit left to shatter.
	// Obsidian rides the 3-hit ramp (its COLOR.b carries the 0.75 material code, not a count).
	float max_hits = mix(max(floor(v_hits_to_break + 0.5), 1.0), 3.0, step(0.5, obsidian_w));
	float hits_left = max_hits * (1.0 - rock_damage);
	float dmg_level = clamp(floor(6.0 - hits_left + 0.5) - 1.0, 0.0, 4.0);
	float level01 = clamp((dmg_level + 1.0) / 5.0, 0.0, 1.0);
	// Realistic crack severity. Boulders walk the five-step ramp directly; the shorter
	// 3-hit rock instead tracks progress across its own hits, so its first hit is a
	// moderate split and the amount of cracks grows into the full web before shatter.
	float crack_level = level01;
	if (max_hits < 4.0) {
		crack_level = clamp((max_hits - hits_left) / (max_hits - 1.0), 0.0, 1.0);
	}
	vec3 damage_col = damage_level_color(dmg_level);
	float damage_presence = smoothstep(0.02, 0.30, rock_damage);
	if (rock_damage > 0.02) {
		if (debug_damage_indicator) {
			// Debug indicator: five-step yellow -> red tint marks each cumulative hit and
			// the fracture lines carry the matching glow.
			float edge_coarse = crack_voronoi_edge(v_world_pos * 0.85);
			float edge_fine   = crack_voronoi_edge(v_world_pos * 2.10);
			float width_coarse = mix(0.0, 0.16, clamp(rock_damage * 1.4, 0.0, 1.0));
			float width_fine   = mix(0.0, 0.14, clamp((rock_damage - 0.35) * 1.8, 0.0, 1.0));
			float c1 = 1.0 - smoothstep(0.0, max(width_coarse, 0.001), edge_coarse);
			float c2 = 1.0 - smoothstep(0.0, max(width_fine,   0.001), edge_fine);
			crack_mask = clamp(max(c1, c2 * 0.85), 0.0, 1.0) * hard_rock_weight;

			// Five-step yellow -> red tint marks each cumulative hit on the intact rock.
			base_col = mix(base_col, damage_col, damage_presence * 0.60);
			// Darken fractured crevices so the crack network still reads over the tint.
			base_col = mix(base_col, vec3(0.04, 0.03, 0.03), crack_mask * 0.60);
		} else {
			// Realistic mode: no colour code and no glow. A domain-warped fracture network
			// keeps the lines irregular like real rock, and every hit opens MORE lines:
			// each fracture line has its own random onset and only appears once the damage
			// level passes it, so the rock goes from a few cracks to a shattered web.
			vec3 warp = vec3(
				value_noise3d(v_world_pos * 0.65),
				value_noise3d(v_world_pos * 0.65 + vec3(31.7, 0.0, 0.0)),
				value_noise3d(v_world_pos * 0.65 + vec3(0.0, 71.3, 0.0))
			) - 0.5;
			vec2 plate_e  = crack_voronoi_edge_rnd(v_world_pos * 0.52 + warp * 0.85 + vec3(23.0, 41.0, 7.0));
			vec2 coarse_e = crack_voronoi_edge_rnd(v_world_pos * 0.85 + warp * 0.60);
			vec2 fine_e   = crack_voronoi_edge_rnd(v_world_pos * 2.10 + warp * 0.25);
			vec2 micro_e  = crack_voronoi_edge_rnd(v_world_pos * 4.30 + warp * 0.15 + vec3(11.0, 5.0, 3.0));

			// Per-network open-in ramps: big plates split first, then the coarse, fine and
			// micro webs fill in as destruction nears.
			float ramp_plate  = clamp((crack_level - 0.30) / 0.45, 0.0, 1.0);
			float ramp_coarse = clamp((crack_level - 0.45) / 0.50, 0.0, 1.0);
			float ramp_fine   = clamp((crack_level - 0.55) / 0.45, 0.0, 1.0);
			float ramp_micro  = clamp((crack_level - 0.72) / 0.28, 0.0, 1.0);
			float open_plate  = smoothstep(plate_e.y * 0.8,  plate_e.y * 0.8 + 0.06,  ramp_plate);
			float open_coarse = smoothstep(coarse_e.y * 0.8, coarse_e.y * 0.8 + 0.06, ramp_coarse);
			float open_fine   = smoothstep(fine_e.y * 0.8,   fine_e.y * 0.8 + 0.06,   ramp_fine);
			float open_micro  = smoothstep(micro_e.y * 0.8,  micro_e.y * 0.8 + 0.06,  ramp_micro);
			// Cracks widen only slightly with damage; the ramp is mostly new lines opening.
			float c4 = (1.0 - smoothstep(0.0, mix(0.16, 0.24, crack_level), plate_e.x)) * open_plate;
			float c1 = (1.0 - smoothstep(0.0, mix(0.12, 0.20, crack_level), coarse_e.x)) * open_coarse;
			float c2 = (1.0 - smoothstep(0.0, mix(0.07, 0.12, crack_level), fine_e.x)) * open_fine;
			float c3 = (1.0 - smoothstep(0.0, mix(0.05, 0.09, crack_level), micro_e.x)) * open_micro;
			crack_mask = clamp(max(c4, max(c1, max(c2 * 0.90, c3 * 0.85))), 0.0, 1.0)
				* hard_rock_weight;

			// Fractured crevices sit in shadow; constant depth, so damage reads as cracks added.
			base_col = mix(base_col, vec3(0.035, 0.028, 0.022), crack_mask * 0.85);
			// Soft occlusion around the fracture lines keeps them reading as grooves.
			float halo = 1.0 - smoothstep(0.0, 0.40, coarse_e.x);
			base_col *= mix(1.0, 0.80, halo * halo * 0.6 * hard_rock_weight);
		}
	}

	ALBEDO = base_col;
	EMISSION = emission_col
		+ (debug_damage_indicator
			? damage_col * (crack_mask * damage_presence * (0.65 + 0.34 * dmg_level))
			: vec3(0.0))
		// Fractured obsidian smoulders along its crack network (elemental glow, always on).
		+ crack_glow_color * (crack_mask * smoothstep(0.25, 0.75, rock_damage) * 1.6 * obsidian_w);
	// Freshly fractured rock is rougher than the intact finish; elemental surfaces keep
	// their own finish (shiny obsidian stays shiny, molten lava governs its own roughness).
	float crack_rough = debug_damage_indicator
		? 0.0
		: damage_presence * hard_rock_weight * 0.35 * (1.0 - is_elemental);
	ROUGHNESS = clamp(mix(roughness_val, 0.97, crack_rough), 0.02, 1.0);
	METALLIC = metallic_val;
}
