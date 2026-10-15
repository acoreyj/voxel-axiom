shader_type spatial;
render_mode specular_schlick_ggx, cull_back;

// HLD v4.0 Section 8.2 + Persistent Damage & 3-Hit Hard Rock Crack Visualization
uniform vec3 top_grass_color : source_color = vec3(0.24, 0.48, 0.22);
uniform vec3 cliff_rock_color : source_color = vec3(0.42, 0.39, 0.36);
uniform vec3 deep_cave_color : source_color = vec3(0.22, 0.24, 0.29);
uniform vec3 hard_rock_color : source_color = vec3(0.16, 0.18, 0.24);
uniform vec3 crack_glow_stage1 : source_color = vec3(0.95, 0.48, 0.08);
uniform vec3 crack_glow_stage2 : source_color = vec3(1.00, 0.18, 0.04);
uniform float surface_height_threshold = 56.0;
uniform float triplanar_scale = 0.25;

varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_damage_ratio;
varying float v_is_hard_rock;

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float value_noise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    vec3 u = f * f * (3.0 - 2.0 * f);

    return mix(
        mix(mix(hash31(i + vec3(0.0, 0.0, 0.0)), hash31(i + vec3(1.0, 0.0, 0.0)), u.x),
            mix(hash31(i + vec3(0.0, 1.0, 0.0)), hash31(i + vec3(1.0, 1.0, 0.0)), u.x), u.y),
        mix(mix(hash31(i + vec3(0.0, 0.0, 1.0)), hash31(i + vec3(1.0, 0.0, 1.0)), u.x),
            mix(hash31(i + vec3(0.0, 1.0, 1.0)), hash31(i + vec3(1.0, 1.0, 1.0)), u.x), u.y),
        u.z);
}

float triplanar_detail(vec3 wpos, vec3 wnorm) {
    vec3 blend = abs(wnorm);
    blend = pow(blend, vec3(4.0));
    blend /= max(dot(blend, vec3(1.0)), 1e-5);

    vec3 s = wpos * triplanar_scale;
    float nx = value_noise3(vec3(s.yz, 1.3)) * 0.65 + value_noise3(vec3(s.yz * 2.7, 4.1)) * 0.35;
    float ny = value_noise3(vec3(s.xz, 2.7)) * 0.65 + value_noise3(vec3(s.xz * 2.7, 5.2)) * 0.35;
    float nz = value_noise3(vec3(s.xy, 3.9)) * 0.65 + value_noise3(vec3(s.xy * 2.7, 6.4)) * 0.35;
    return nx * blend.x + ny * blend.y + nz * blend.z;
}

// 3D fracture ridge pattern for progressive 1st-hit and 2nd-hit crack visualization
float crack_pattern_3d(vec3 wpos) {
    float n1 = abs(value_noise3(wpos * 0.85) - 0.5) * 2.0;
    float n2 = abs(value_noise3(wpos * 2.10 + vec3(7.3, 2.9, 5.1)) - 0.5) * 2.0;
    return min(n1, n2);
}

void vertex() {
    v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    v_world_normal = normalize((MODEL_NORMAL_MATRIX * NORMAL));
    v_damage_ratio = COLOR.r;
    v_is_hard_rock = COLOR.g;
}

void fragment() {
    vec3 n = normalize(v_world_normal);
    float up_factor = clamp(n.y, 0.0, 1.0);
    float cave_depth_factor = clamp((surface_height_threshold - v_world_pos.y) / 20.0, 0.0, 1.0);

    vec3 surface_tint = mix(cliff_rock_color, top_grass_color, smoothstep(0.55, 0.85, up_factor));
    vec3 base_color = mix(surface_tint, deep_cave_color, cave_depth_factor);

    // Blend in dark basalt/obsidian tint for 3-hit Hard Rock formations
    base_color = mix(base_color, hard_rock_color, clamp(v_is_hard_rock, 0.0, 1.0));

    float grain = (triplanar_detail(v_world_pos, n) - 0.5) * 0.12;
    vec3 albedo = clamp(base_color + vec3(grain), vec3(0.0), vec3(1.0));
    float rough = mix(mix(0.88, 0.72, cave_depth_factor), 0.42, clamp(v_is_hard_rock, 0.0, 1.0));
    float metal = mix(0.04, 0.28, clamp(v_is_hard_rock, 0.0, 1.0));
    vec3 emission = vec3(0.0);

    // Gate progressive cracks and magma glow strictly to Hard Rock / Boulder
    // (v_is_hard_rock) so grass and dirt never render hard-rock cracks or glow.
    float hard_rock_weight = smoothstep(0.35, 0.75, clamp(v_is_hard_rock, 0.0, 1.0));
    float rock_damage = v_damage_ratio * hard_rock_weight;
    if (rock_damage > 0.02) {
        float ridge = crack_pattern_3d(v_world_pos);
        float crack_width = mix(0.08, 0.24, rock_damage);
        float crack_mask = 1.0 - smoothstep(0.0, crack_width, ridge);

        // Darken fractured crevices and emit glowing stress lines that intensify from Hit 1 -> Hit 2
        albedo = mix(albedo * (1.0 - 0.35 * rock_damage), vec3(0.05, 0.03, 0.02), crack_mask);
        vec3 glow_col = mix(crack_glow_stage1, crack_glow_stage2, clamp((rock_damage - 0.30) * 2.5, 0.0, 1.0));
        emission = glow_col * crack_mask * (1.5 * rock_damage);
        rough = mix(rough, 0.95, crack_mask);
    }

    ALBEDO = albedo;
    ROUGHNESS = rough;
    METALLIC = metal;
    EMISSION = emission;
}
