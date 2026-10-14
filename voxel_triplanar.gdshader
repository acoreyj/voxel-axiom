shader_type spatial;
render_mode specular_schlick_ggx, cull_back;

// HLD v4.0 Section 8.2: World-Space Triplanar Cave & Surface Terrain Shader
uniform vec3 top_grass_color : source_color = vec3(0.24, 0.48, 0.22);
uniform vec3 cliff_rock_color : source_color = vec3(0.42, 0.39, 0.36);
uniform vec3 deep_cave_color : source_color = vec3(0.22, 0.24, 0.29);
uniform float surface_height_threshold = 56.0;
uniform float triplanar_scale = 0.25;

varying vec3 v_world_pos;
varying vec3 v_world_normal;

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Smooth trilinear value noise (eliminates blocky floor() checkerboard artifacts)
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

void vertex() {
    v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    v_world_normal = normalize((MODEL_NORMAL_MATRIX * NORMAL));
}

void fragment() {
    vec3 n = normalize(v_world_normal);
    float up_factor = clamp(n.y, 0.0, 1.0);
    float cave_depth_factor = clamp((surface_height_threshold - v_world_pos.y) / 20.0, 0.0, 1.0);

    vec3 surface_tint = mix(cliff_rock_color, top_grass_color, smoothstep(0.55, 0.85, up_factor));
    vec3 base_color = mix(surface_tint, deep_cave_color, cave_depth_factor);

    float grain = (triplanar_detail(v_world_pos, n) - 0.5) * 0.12;
    ALBEDO = clamp(base_color + vec3(grain), vec3(0.0), vec3(1.0));
    ROUGHNESS = mix(0.88, 0.72, cave_depth_factor);
    METALLIC = 0.04;
}
