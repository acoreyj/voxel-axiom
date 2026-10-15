shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec3 grass_color : source_color = vec3(0.24, 0.56, 0.20);
uniform vec3 dirt_color  : source_color = vec3(0.45, 0.34, 0.23);
uniform vec3 rock_color  : source_color = vec3(0.39, 0.40, 0.43);
uniform vec3 hard_rock_color : source_color = vec3(0.22, 0.25, 0.33);
uniform vec3 crack_glow_color : source_color = vec3(1.0, 0.42, 0.08);
uniform float slope_sharpness : hint_range(1.0, 16.0) = 6.0;
uniform float noise_scale : hint_range(0.01, 1.0) = 0.14;

varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_damage_ratio;
varying float v_is_hard_rock;

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

	// Gate progressive crack network and magma glow strictly to Hard Rock (v_is_hard_rock)
	// so grass and dirt never render hard-rock cracks or glow.
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

		// Darken fractured crevices and blend warm stress-glow at Stage 2 (2nd hit before shatter)
		base_col = mix(base_col, vec3(0.04, 0.03, 0.03), crack_mask * 0.88);
	}

	ALBEDO = base_col;
	EMISSION = crack_glow_color * (crack_mask * smoothstep(0.25, 0.75, rock_damage) * 1.6);
	ROUGHNESS = mix(0.90, mix(0.75, 0.52, hard_rock_weight), flat_mask);
	METALLIC = 0.18 * hard_rock_weight;
}
