#version 460 core
#extension GL_EXT_ray_tracing: require

#include "ray_primary_common.glsl"
#include "color_spaces.glsl"

layout(set = 0, binding = 7) uniform samplerCube skybox;

layout(location = PAYLOAD_LOCATION_PRIMARY) rayPayloadEXT RayPayloadPrimary payload;

void main() {
	// TODO payload.prev_pos_t = payload.hit_t = vec4(geom.pos, gl_HitTEXT);
	payload.emissive.rgb = SRGBtoLINEAR(texture(skybox, gl_WorldRayDirectionEXT).rgb);
}
