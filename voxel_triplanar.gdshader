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
// Dynamic props (rolling boulders) sample the rock pattern in object space so the
// granite stays locked to the tumbling mesh instead of swimming through world space.
// Terrain leaves this false and keeps its world-space pattern.
uniform bool pattern_in_object_space = false;
// Chiseled geometric facet normals: blends the screen-space triangle face normal
// into Hard Rock and painted monument surfaces so 1.0 m Surface Nets meshes show
// crisp monolithic planes on brow, nose bridge, lips and jaw without making
// curved cheeks look blocky.
uniform float chisel_normal_strength : hint_range(0.0, 1.0) = 0.24;

// Brittle fracture tuning uniforms for realistic rock breaking:
// - crack_warp_strength: controls macro angular deflection of cleavage planes (keep <= 0.25 to avoid wavy ribbons)
// - crack_jaggedness: high-frequency micro-serration / chatter along fracture lines
// - crack_width_scale: overall width multiplier for primary/secondary/hairline fissures
// - crack_normal_depth: analytical 3D normal beveling along fracture lips so light catches chipped edges
uniform float crack_warp_strength : hint_range(0.0, 0.6) = 0.16;
uniform float crack_jaggedness : hint_range(0.0, 0.25) = 0.055;
uniform float crack_width_scale : hint_range(0.25, 2.5) = 1.0;
uniform float crack_normal_depth : hint_range(0.0, 1.0) = 0.45;

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

vec3 hash33(vec3 p) {
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.xxy + p.yxx) * p.zyx);
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

// Signed [-0.5, +0.5]^3 3D vector noise for angular domain warping & micro-chatter
vec3 noise3d_vec(vec3 p) {
	return vec3(
		value_noise3d(p),
		value_noise3d(p + vec3(31.7, 19.3, 47.1)),
		value_noise3d(p + vec3(71.3, 53.9, 13.7))
	) - vec3(0.5);
}

// True planar bisector 3D Voronoi edge distance (debug overlay & lava crust):
// Projects onto the exact perpendicular bisector plane between the nearest seed and
// its neighbors so cell edges are straight, flat cleavage planes with sharp angular
// trihedral junctions rather than pinched hyperbolic bubbles.
float crack_voronoi_edge(vec3 p) {
	vec3 ip = floor(p);
	vec3 fp = fract(p);

	vec3 mr = vec3(0.0);
	vec3 mg = vec3(0.0);
	float md = 8.0;

	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 g = vec3(float(x), float(y), float(z));
				vec3 o = 0.12 + 0.76 * hash33(ip + g);
				vec3 r = g + o - fp;
				float d = dot(r, r);
				if (d < md) {
					md = d;
					mr = r;
					mg = g;
				}
			}
		}
	}

	float res = 8.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 g = vec3(float(x), float(y), float(z));
				vec3 diff = g - mg;
				if (dot(diff, diff) > 0.05) {
					vec3 o = 0.12 + 0.76 * hash33(ip + g);
					vec3 r = g + o - fp;
					vec3 d_vec = r - mr;
					float d_len = length(d_vec);
					if (d_len > 1e-5) {
						float plane_dist = dot(0.5 * (mr + r), d_vec / d_len);
						res = min(res, plane_dist);
					}
				}
			}
		}
	}
	return max(res, 0.0);
}

// Realistic brittle rock Voronoi fracture evaluator:
// Returns vec2(planar_edge_dist, symmetric_edge_onset_id) and writes the analytical 3D
// cleavage-plane unit normal into `out_plane_normal`:
//  - x: true perpendicular distance to the closest cleavage plane (straight, angular facets)
//  - y: SYMMETRIC onset ID shared by both cells on either side of the cleavage plane
//  - out_plane_normal: exact 3D unit vector perpendicular to the fracture plane, allowing
//    crack lips to be beveled analytically without calling dFdx/dFdy/fwidth inside a branch!
vec2 crack_voronoi_brittle(vec3 p, out vec3 out_plane_normal) {
	vec3 ip = floor(p);
	vec3 fp = fract(p);

	vec3 mr = vec3(0.0);
	vec3 mg = vec3(0.0);
	float md = 8.0;

	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 g = vec3(float(x), float(y), float(z));
				vec3 o = 0.12 + 0.76 * hash33(ip + g);
				vec3 r = g + o - fp;
				float d = dot(r, r);
				if (d < md) {
					md = d;
					mr = r;
					mg = g;
				}
			}
		}
	}

	float min_plane_dist = 8.0;
	vec3 best_neighbor_g = mg;
	vec3 best_plane_n = vec3(0.0, 1.0, 0.0);

	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 g = vec3(float(x), float(y), float(z));
				vec3 diff = g - mg;
				if (dot(diff, diff) > 0.05) {
					vec3 o = 0.12 + 0.76 * hash33(ip + g);
					vec3 r = g + o - fp;
					vec3 d_vec = r - mr;
					float d_len = length(d_vec);
					if (d_len > 1e-5) {
						vec3 unit_n = d_vec / d_len;
						float plane_dist = dot(0.5 * (mr + r), unit_n);
						if (plane_dist < min_plane_dist) {
							min_plane_dist = plane_dist;
							best_neighbor_g = g;
							best_plane_n = unit_n;
						}
					}
				}
			}
		}
	}

	// Order-independent symmetric hash of the two cells (cell_a, cell_b) sharing this edge
	// so both sides of a fracture line always open at the exact same damage threshold.
	vec3 cell_a = ip + mg;
	vec3 cell_b = ip + best_neighbor_g;
	vec3 c_min = min(cell_a, cell_b);
	vec3 c_max = max(cell_a, cell_b);
	float edge_id = hash31(c_min * 1.31 + c_max * 2.73 + vec3(7.0, 13.0, 29.0));

	out_plane_normal = best_plane_n;
	return vec2(max(min_plane_dist, 0.0), edge_id);
}

// Pure analytical V-groove fissure profile (NO fwidth/dFdx/dFdy):
// Avoids non-uniform control-flow quad-derivative NaNs that draw a 1-pixel dotted black
// border along the `rock_damage > threshold` isoline in Vulkan/Forward+.
float brittle_fissure_profile(float edge_dist, float half_width) {
	if (half_width <= 0.0001) {
		return 0.0;
	}
	float core = 1.0 - smoothstep(half_width * 0.18, half_width, edge_dist);
	return core * core;
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
	// World-space by default (terrain chunks); object-space for props that rotate,
	// so the procedural rock pattern does not slide across a tumbling boulder.
	v_world_pos = pattern_in_object_space ? VERTEX : (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_NORMAL_MATRIX * NORMAL).xyz);
	v_damage_ratio = COLOR.r;
	v_is_hard_rock = COLOR.g;
	v_hits_to_break = COLOR.b * 255.0;
	v_material_code = (COLOR.b >= 0.2) ? COLOR.b : 0.0;
	v_baked_weight = COLOR.a;
	v_baked_albedo = vec3(UV.x, UV.y, UV2.x);
}

void fragment() {
	// Evaluate screen-space vertex derivatives in uniform control flow at the top of
	// fragment() so quad derivatives never straddle a conditional branch boundary.
	vec3 dpos_dx = dFdx(VERTEX);
	vec3 dpos_dy = dFdy(VERTEX);

	vec3 n = normalize(v_world_normal);
	float up_factor = clamp(n.y, 0.0, 1.0);
	float flat_mask = pow(up_factor, slope_sharpness);
	float height_mask = clamp((v_world_pos.y - 45.0) / 20.0, 0.0, 1.0);

	float n_macro = value_noise3d(v_world_pos * noise_scale);
	float n_detail = value_noise3d(v_world_pos * (noise_scale * 3.7));
	float tint = mix(0.82, 1.18, 0.65 * n_macro + 0.35 * n_detail);

	// Blend in dark metallic-basalt tint for 3-hit Hard Rock formations.
	// Suppress grass/dirt flat_mask on Hard Rock so horizontal rock tops and freshly
	// carved ledges stay stone instead of showing brown dirt contour bands.
	float hard_rock_weight = smoothstep(0.35, 0.75, clamp(v_is_hard_rock, 0.0, 1.0));
	float soil_flat_mask = flat_mask * (1.0 - hard_rock_weight);

	vec3 top_col = mix(dirt_color, grass_color, height_mask);
	vec3 base_col = mix(rock_color, top_col, soil_flat_mask) * tint;

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
		0.88, boulder_weight), soil_flat_mask);
	float metallic_val = 0.18 * hard_rock_weight * (1.0 - boulder_weight);

	// Blend in per-voxel baked RGB5551 albedo (UV.xy + UV2.x, gated by COLOR.a).
	float paint_w = clamp(v_baked_weight, 0.0, 1.0);
	if (paint_w > 0.01) {
		vec3 unpremult_albedo = clamp(v_baked_albedo / max(paint_w, 0.001), vec3(0.0), vec3(1.0));
		vec3 painted_col = baked_srgb_to_linear(unpremult_albedo) * mix(0.92, 1.08, n_detail);
		float blend_alpha = smoothstep(0.04, 0.80, paint_w);
		base_col = mix(base_col, painted_col, blend_alpha);
		roughness_val = mix(roughness_val, 0.76, blend_alpha);
		metallic_val = mix(metallic_val, 0.03, blend_alpha);
	}

	// Chiseled Geometric Facet Normals for Hard Rock & Painted Stone Monuments
	float stone_facet_mask = max(hard_rock_weight * 0.80, paint_w);
	if (stone_facet_mask > 0.01 && chisel_normal_strength > 0.001) {
		vec3 geom_n = cross(dpos_dx, dpos_dy);
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
	float spall_rim = 0.0;
	float rock_damage = v_damage_ratio * hard_rock_weight;
	// Default rock to 3-hit if COLOR.b is 0 (standard C++ shader_material_code), or use
	// the explicit hit count when encoded in COLOR.b (3-hit rock vs 5-hit boulder).
	float raw_hits = floor(v_hits_to_break + 0.5);
	float max_hits = (raw_hits >= 2.0) ? raw_hits : 3.0;
	max_hits = mix(max_hits, 3.0, step(0.5, obsidian_w));
	float hits_left = max_hits * (1.0 - rock_damage);
	float dmg_level = clamp(floor(6.0 - hits_left + 0.5) - 1.0, 0.0, 4.0);
	float level01 = clamp((dmg_level + 1.0) / 5.0, 0.0, 1.0);

	float radial_taper = smoothstep(0.01, 0.22, rock_damage);
	float crack_level = level01;
	if (max_hits < 4.0) {
		crack_level = clamp((max_hits - hits_left) / max(max_hits - 1.0, 1.0), 0.0, 1.0);
	}
	crack_level *= radial_taper;

	vec3 damage_col = damage_level_color(dmg_level);
	float damage_presence = smoothstep(0.02, 0.30, rock_damage);
	if (rock_damage > 0.005) {
		if (debug_damage_indicator) {
			// Debug indicator: five-step yellow -> red tint marks each cumulative hit and
			// the fracture lines carry the matching glow.
			float edge_coarse = crack_voronoi_edge(v_world_pos * 0.85);
			float edge_fine   = crack_voronoi_edge(v_world_pos * 2.10);
			float width_coarse = mix(0.0, 0.12, clamp(rock_damage * 1.4, 0.0, 1.0));
			float width_fine   = mix(0.0, 0.09, clamp((rock_damage - 0.35) * 1.8, 0.0, 1.0));
			float c1 = 1.0 - smoothstep(0.0, max(width_coarse, 0.001), edge_coarse);
			float c2 = 1.0 - smoothstep(0.0, max(width_fine,   0.001), edge_fine);
			crack_mask = clamp(max(c1, c2 * 0.85), 0.0, 1.0) * hard_rock_weight;

			base_col = mix(base_col, damage_col, damage_presence * 0.60);
			base_col = mix(base_col, vec3(0.04, 0.03, 0.03), crack_mask * 0.60);
		} else {
			// Realistic Brittle Fracture Network (Zero screen-space derivative calls inside branch):
			// 1. Two-band brittle warp: restrained angular deflection + high-frequency micro-chatter.
			vec3 warp_angular = noise3d_vec(v_world_pos * 1.35) * crack_warp_strength;
			vec3 warp_chatter = noise3d_vec(v_world_pos * 5.80) * crack_jaggedness;
			vec3 total_warp   = warp_angular + warp_chatter;

			// 2. Evaluate true planar bisector Voronoi + analytical 3D cleavage-plane normals.
			vec3 n_plate, n_coarse, n_fine, n_micro;
			vec2 plate_e  = crack_voronoi_brittle(v_world_pos * 0.55 + total_warp * 1.00 + vec3(23.0, 41.0, 7.0), n_plate);
			vec2 coarse_e = crack_voronoi_brittle(v_world_pos * 1.05 + total_warp * 0.85, n_coarse);
			vec2 fine_e   = crack_voronoi_brittle(v_world_pos * 2.35 + total_warp * 0.65, n_fine);
			vec2 micro_e  = crack_voronoi_brittle(v_world_pos * 4.80 + total_warp * 0.45 + vec3(11.0, 5.0, 3.0), n_micro);

			// 3. Progressive hierarchy with wide smooth open ramps so cracks physically taper
			//    to needle-sharp points at the outer perimeter of the blast rather than
			//    terminating in blunt stubs along the boundary triangle ring.
			float ramp_plate  = clamp((crack_level - 0.04) / 0.55, 0.0, 1.0);
			float ramp_coarse = clamp((crack_level - 0.18) / 0.55, 0.0, 1.0);
			float ramp_fine   = clamp((crack_level - 0.36) / 0.50, 0.0, 1.0);
			float ramp_micro  = clamp((crack_level - 0.58) / 0.40, 0.0, 1.0);

			float open_plate  = smoothstep(plate_e.y * 0.72,  plate_e.y * 0.72 + 0.22,  ramp_plate);
			float open_coarse = smoothstep(coarse_e.y * 0.72, coarse_e.y * 0.72 + 0.22, ramp_coarse);
			float open_fine   = smoothstep(fine_e.y * 0.72,   fine_e.y * 0.72 + 0.22,   ramp_fine);
			float open_micro  = smoothstep(micro_e.y * 0.72,  micro_e.y * 0.72 + 0.22,  ramp_micro);

			// Pinch-and-swell along crack length; multiplying width by open_* makes every
			// crack narrow to a razor point at its tip and at the outer blast boundary.
			float pinch = mix(0.65, 1.25, value_noise3d(v_world_pos * 3.4));
			float w_scale = crack_width_scale * pinch * radial_taper;

			float w_plate  = mix(0.026, 0.050, crack_level) * w_scale * open_plate;
			float w_coarse = mix(0.018, 0.036, crack_level) * w_scale * open_coarse;
			float w_fine   = mix(0.012, 0.022, crack_level) * w_scale * open_fine;
			float w_micro  = mix(0.007, 0.014, crack_level) * w_scale * open_micro;

			float c_plate  = brittle_fissure_profile(plate_e.x,  w_plate)  * open_plate;
			float c_coarse = brittle_fissure_profile(coarse_e.x, w_coarse) * open_coarse;
			float c_fine   = brittle_fissure_profile(fine_e.x,   w_fine)   * open_fine;
			float c_micro  = brittle_fissure_profile(micro_e.x,  w_micro)  * open_micro;

			crack_mask = clamp(
				max(c_plate, max(c_coarse * 0.92, max(c_fine * 0.78, c_micro * 0.62))),
				0.0, 1.0
			) * hard_rock_weight;

			// 4. Tightly gated bevel shadow along active crack lips (no pale gray halo):
			float spall_plate  = (1.0 - smoothstep(w_plate,  w_plate * 2.1 + 0.001,  plate_e.x))  * open_plate;
			float spall_coarse = (1.0 - smoothstep(w_coarse, w_coarse * 2.0 + 0.001, coarse_e.x)) * open_coarse;
			spall_rim = max(spall_plate, spall_coarse * 0.8) * hard_rock_weight * radial_taper;

			// Subtle recessed shadow bevel right at the lip + deep dark fissure interior:
			base_col *= mix(1.0, 0.78, spall_rim * (1.0 - crack_mask));
			base_col = mix(base_col, vec3(0.025, 0.022, 0.020), crack_mask * 0.92);

			// 5. Analytical 3D normal beveling along the cleavage plane lips:
			//    Uses the exact Voronoi bisector normal (n_plate / n_coarse) transformed into
			//    view space, avoiding dFdx/dFdy quad-derivative border artifacts completely!
			if (crack_normal_depth > 0.001 && spall_rim > 0.001) {
				vec3 world_lip_n = normalize(n_plate * spall_plate + n_coarse * (spall_coarse * 0.7) + vec3(1e-6));
				vec3 view_lip_n  = (VIEW_MATRIX * vec4(world_lip_n, 0.0)).xyz;
				float lip_weight = spall_rim * (1.0 - crack_mask * 0.5) * crack_normal_depth * 0.55;
				NORMAL = normalize(NORMAL + view_lip_n * lip_weight);
			}
		}
	}

	ALBEDO = base_col;
	EMISSION = emission_col
		+ (debug_damage_indicator
			? damage_col * (crack_mask * damage_presence * (0.65 + 0.34 * dmg_level))
			: vec3(0.0))
		// Fractured obsidian smoulders along its crack network (elemental glow, always on).
		+ crack_glow_color * (crack_mask * smoothstep(0.25, 0.75, rock_damage) * 1.6 * obsidian_w);
	// Scope realistic roughness increase strictly to the cracks and their immediate lips
	// (never across uncracked `damage_presence`), so the outer triangle ring never shows
	// a matte polygon border around the damaged zone.
	float crack_rough = debug_damage_indicator
		? 0.0
		: clamp(crack_mask * 0.85 + spall_rim * 0.35, 0.0, 1.0) * hard_rock_weight * (1.0 - is_elemental);
	ROUGHNESS = clamp(mix(roughness_val, 0.98, crack_rough), 0.02, 1.0);
	METALLIC = metallic_val * (1.0 - 0.80 * crack_mask);
}
