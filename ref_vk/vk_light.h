#pragma once

#include "vk_const.h"

#include "xash3d_types.h"
#include "protocol.h"
#include "const.h"
#include "bspfile.h"

typedef struct {
	vec3_t emissive;
	qboolean set;
} vk_emissive_texture_t;

typedef struct {
	uint8_t num_dlights;
	uint8_t num_emissive_surfaces;
	uint8_t dlights[MAX_VISIBLE_DLIGHTS];
	uint8_t emissive_surfaces[MAX_VISIBLE_SURFACE_LIGHTS];
} vk_lights_cell_t;

typedef struct {
	vec3_t emissive;
	uint32_t kusok_index;
	matrix3x4 transform;
} vk_emissive_surface_t;

typedef struct {
	struct {
		int grid_min_cell[3];
		int grid_size[3];
		int grid_cells;

		vk_emissive_texture_t emissive_textures[MAX_TEXTURES];
	} map;

	int num_emissive_surfaces;
	vk_emissive_surface_t emissive_surfaces[255]; // indexed by uint8_t

	vk_lights_cell_t cells[MAX_LIGHT_CLUSTERS];
} vk_lights_t;

extern vk_lights_t g_lights;

void VK_LightsShutdown( void );

void VK_LightsNewMap( void );
void VK_LightsLoadMapStaticLights( void );

void VK_LightsFrameInit( void );

// TODO there is an arguably better way to organize this.
// a. this only belongs to ray tracing mode
// b. kusochki now have emissive color, so it probably makes more sense to not store emissive
//    separately in emissive surfaces.
struct vk_render_geometry_s;
const vk_emissive_surface_t *VK_LightsAddEmissiveSurface( const struct vk_render_geometry_s *geom, const matrix3x4 *transform_row );

void VK_LightsFrameFinalize( void );
