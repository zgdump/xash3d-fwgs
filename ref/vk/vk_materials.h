#pragma once

#include "xash3d_types.h"

typedef struct r_vk_material_s {
	int tex_base_color;
	int tex_roughness;
	int tex_metalness;
	int tex_normalmap;

	vec4_t base_color;
	float roughness;
	float metalness;
	float normal_scale;

	// TODO this should be internal
	qboolean set;
} r_vk_material_t;

typedef struct { int index; } r_vk_material_ref_t;

// Note: invalidates all previously issued material refs
// TODO: track "version" in high bits?
void R_VkMaterialsReload( void );

struct model_s;
void R_VkMaterialsLoadForModel( const struct model_s* mod );

r_vk_material_t R_VkMaterialGetForTexture( int tex_id );

r_vk_material_ref_t R_VkMaterialGetForName( const char *name );
r_vk_material_t R_VkMaterialGetForRef( r_vk_material_ref_t ref );
