#pragma once

#include "vk_geometry.h"
#include "vk_core.h"

struct vk_render_model_s;
struct vk_ray_model_s;

typedef struct {
	struct vk_render_model_s *model;
} vk_ray_model_init_t;

struct vk_ray_model_s *VK_RayModelCreate( vk_ray_model_init_t model_init );
void VK_RayModelDestroy( struct vk_ray_model_s *model );

void VK_RayFrameBegin( void );

// TODO how to improve this render vs ray model storage/interaction?
void VK_RayFrameAddModel(struct vk_ray_model_s *model, const struct vk_render_model_s *render_model);

typedef struct {
	VkBuffer buffer;
	uint32_t offset;
	uint64_t size;
} vk_buffer_region_t;

typedef struct {
	struct vk_combuf_s *combuf;

	struct {
		VkImageView image_view;
		VkImage image;
		uint32_t width, height;
	} dst;

	const matrix4x4 *projection, *view;

	// Buffer holding vertex and index data
	// TODO remove
	struct {
		VkBuffer buffer; // must be the same as in vk_ray_model_create_t TODO: validate or make impossible to specify incorrectly
		uint64_t size;
	} geometry_data;

	float fov_angle_y;
} vk_ray_frame_render_args_t;
void VK_RayFrameEnd(const vk_ray_frame_render_args_t* args);

void VK_RayNewMap( void );
void VK_RayMapLoadEnd( void );

qboolean VK_RayInit( void );
void VK_RayShutdown( void );

struct rt_blas_s;
struct vk_render_geometry_s;

struct rt_model_s;

typedef enum {
	kBlasBuildStatic, // builds slow for fast trace
	kBlasBuildDynamicUpdate, // builds if not built, updates if built
	kBlasBuildDynamicFast, // builds fast from scratch (no correlation with previous frame guaranteed, e.g. triapi)
} rt_blas_usage_e;

typedef struct {
	const char *debug_name; // Must remain alive until RT_ModelDestroy
	const struct vk_render_geometry_s *geometries;
	int geometries_count;
	rt_blas_usage_e usage;
} rt_model_create_t;
struct rt_model_s *RT_ModelCreate(rt_model_create_t args);
void RT_ModelDestroy(struct rt_model_s *model);

typedef struct {
	int render_type; // TODO material_mode
	const matrix3x4 *transform, *prev_transform;
	const vec4_t *color;
	int textures_override; // Override kusochki/material textures if > 0
} rt_frame_add_model_t;

void RT_FrameAddModel( struct rt_model_s *model, rt_frame_add_model_t args );
