shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec3 grass_color : source_color = vec3(0.24, 0.56, 0.20);
uniform vec3 dirt_color  : source_color = vec3(0.45, 0.34, 0.23);
uniform vec3 rock_color  : source_color = vec3(0.39, 0.40, 0.43);
uniform vec3 hard_rock_color : source_color = vec3(0.34, 0.32, 0.30);
uniform vec3 lava_color      : source_color = vec3(1.00, 0.32, 0.04);
uniform vec3 water_color     : source_color = vec3(0.10, 0.45, 0.85);
uniform vec3 obsidian_color  : source_color = vec3(0.08, 0.06, 0.14);
uniform vec3 ice_color       : source_color = vec3(0.62, 0.88, 0.98);
uniform vec3 crack_glow_color : source_color = vec3(1.0, 0.42, 0.08);
uniform float slope_sharpness : hint_range(1.0, 16.0) = 6.0;
uniform float noise_scale : hint_range(0.01, 1.0) = 0.14;
uniform float chisel_normal_strength : hint_range(0.0, 1.0) = 0.24;

varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_damage_ratio;
varying float v_is_hard_rock;
varying float v_material_code;
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

void vertex() {
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_NORMAL_MATRIX * NORMAL).xyz);
	v_damage_ratio = COLOR.r;
	v_is_hard_rock = COLOR.g;
	v_material_code = COLOR.b;
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
	vec3 emission_col = vec3(0.0);
	float roughness_val = mix(0.90, 0.75, flat_mask);
	float metallic_val = 0.0;

	// Blend in volcanic basalt/tuff tint for unpainted 3-hit Hard Rock formations & interior craters
	float hard_rock_weight = smoothstep(0.35, 0.75, clamp(v_is_hard_rock, 0.0, 1.0));
	if (hard_rock_weight > 0.01) {
		float vein_tex = value_noise3d(v_world_pos * 0.45);
		vec3 hr_col = hard_rock_color * mix(0.85, 1.20, vein_tex);
		base_col = mix(base_col, hr_col, hard_rock_weight);
		roughness_val = mix(roughness_val, 0.56, hard_rock_weight);
		metallic_val = 0.12 * hard_rock_weight;
	}

	// Blend in per-voxel baked RGB5551 albedo (passed via UV.xy + UV2.x, gated by COLOR.a = v_baked_weight).
	// Un-premultiply v_baked_albedo by max(paint_w, 0.001) so triangles straddling painted (w=1, UV=RGB)
	// and unpainted (w=0, UV=0) vertices never darken toward black at the base or crater rim!
	float paint_w = clamp(v_baked_weight, 0.0, 1.0);
	if (paint_w > 0.01) {
		vec3 unpremult_albedo = clamp(v_baked_albedo / max(paint_w, 0.001), vec3(0.0), vec3(1.0));
		vec3 painted_col = unpremult_albedo * mix(0.92, 1.08, n_detail);
		float blend_alpha = smoothstep(0.04, 0.80, paint_w);
		base_col = mix(base_col, painted_col, blend_alpha);
		roughness_val = mix(roughness_val, 0.76, blend_alpha);
		metallic_val = mix(metallic_val, 0.03, blend_alpha);
	}

	// Chiseled Geometric Facet Normals for Hard Rock & Painted Stone Monuments:
	// Blends view-space triangle face normal with the smooth TSDF normal so 1.0m Surface Nets
	// meshes display crisp, monolithic chiseled planes on brow, nose bridge, lips, and jaw.
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

	// Elemental Material Shading (COLOR.b = v_material_code:
	//   0.25 = Lava, 0.50 = Water, 0.75 = Obsidian, 1.00 = Ice)
	float lava_w     = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 0.25));
	float water_w    = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 0.50));
	float obsidian_w = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 0.75));
	float ice_w      = 1.0 - smoothstep(0.04, 0.12, abs(v_material_code - 1.00));

	if (lava_w > 0.01) {
		float magma_crust = crack_voronoi_edge(v_world_pos * 0.55);
		float molten_vein = 1.0 - smoothstep(0.0, 0.28, magma_crust);
		vec3 crust_col = mix(vec3(0.18, 0.05, 0.03), lava_color, molten_vein);
		base_col = mix(base_col, crust_col, lava_w);
		emission_col += lava_color * (0.65 + 1.35 * molten_vein) * lava_w;
		roughness_val = mix(roughness_val, 0.65, lava_w);
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

	// Gate progressive crack network and stress glow strictly to 3-hit Hard Rock & Obsidian
	// (including painted Moai stone statues, so hits 1 & 2 fracture across the painted face!)
	float crack_mask = 0.0;
	float rock_damage = v_damage_ratio * hard_rock_weight;
	if (rock_damage > 0.02) {
		float edge_coarse = crack_voronoi_edge(v_world_pos * 0.85);
		float edge_fine   = crack_voronoi_edge(v_world_pos * 2.10);
		float width_coarse = mix(0.0, 0.16, clamp(rock_damage * 1.4, 0.0, 1.0));
		float width_fine   = mix(0.0, 0.14, clamp((rock_damage - 0.35) * 1.8, 0.0, 1.0));
		float c1 = 1.0 - smoothstep(0.0, max(width_coarse, 0.001), edge_coarse);
		float c2 = 1.0 - smoothstep(0.0, max(width_fine,   0.001), edge_fine);
		crack_mask = clamp(max(c1, c2 * 0.85), 0.0, 1.0) * hard_rock_weight;

		base_col = mix(base_col, vec3(0.04, 0.03, 0.03), crack_mask * 0.88);
	}

	ALBEDO = base_col;
	EMISSION = emission_col + crack_glow_color * (crack_mask * smoothstep(0.25, 0.75, rock_damage) * 1.6);
	ROUGHNESS = roughness_val;
	METALLIC = metallic_val;
}
