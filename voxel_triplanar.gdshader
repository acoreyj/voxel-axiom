shader_type spatial;
render_mode specular_schlick_ggx, cull_back;

// HLD v4.0 Section 8.2: World-Space Triplanar Cave & Surface Terrain Shader
uniform vec3 top_grass_color : source_color = vec3(0.24, 0.48, 0.22);
uniform vec3 cliff_rock_color : source_color = vec3(0.38, 0.36, 0.34);
uniform vec3 deep_cave_color : source_color = vec3(0.18, 0.20, 0.25);
uniform vec3 crater_scorch_color : source_color = vec3(0.28, 0.22, 0.18);
uniform float surface_height_threshold = 72.0;
uniform float triplanar_scale = 0.125;

varying vec3 v_world_pos;
varying vec3 v_world_normal;

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float triplanar_detail(vec3 wpos, vec3 wnorm) {
    vec3 blend = abs(wnorm);
    blend = pow(blend, vec3(4.0));
    blend /= max(dot(blend, vec3(1.0)), 1e-5);

    vec3 scaled = wpos * triplanar_scale;
    float nx = hash31(floor(vec3(scaled.yz, 1.0)));
    float ny = hash31(floor(vec3(scaled.xz, 2.0)));
    float nz = hash31(floor(vec3(scaled.xy, 3.0)));
    return nx * blend.x + ny * blend.y + nz * blend.z;
}

void vertex() {
    v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    v_world_normal = normalize((MODEL_NORMAL_MATRIX * NORMAL));
}

void fragment() {
    vec3 n = normalize(v_world_normal);
    float up_factor = clamp(n.y, 0.0, 1.0);
    float cave_depth_factor = clamp((surface_height_threshold - v_world_pos.y) / 24.0, 0.0, 1.0);

    vec3 surface_tint = mix(cliff_rock_color, top_grass_color, smoothstep(0.55, 0.85, up_factor));
    vec3 base_color = mix(surface_tint, deep_cave_color, cave_depth_factor);

    // Subtle procedural triplanar rock grain so overhangs and craters read clearly in 3D
    float grain = (triplanar_detail(v_world_pos, n) - 0.5) * 0.10;
    ALBEDO = clamp(base_color + vec3(grain), vec3(0.0), vec3(1.0));
    ROUGHNESS = mix(0.88, 0.72, cave_depth_factor);
    METALLIC = 0.04;
}
