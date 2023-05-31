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

typedef enum {
	kBlasBuildStatic, // builds slow for fast trace
	kBlasBuildDynamicUpdate, // builds if not built, updates if built
	kBlasBuildDynamicFast, // builds fast from scratch (no correlation with previous frame guaranteed, e.g. triapi)
} rt_blas_usage_e;

// Just creates an empty BLAS structure, doesn't alloc anything
// Memory pointed to by name must remain alive until RT_BlasDestroy
struct rt_blas_s* RT_BlasCreate(const char *name, rt_blas_usage_e usage);

// Create an empty BLAS with specified limits
struct rt_blas_s* RT_BlasCreatePreallocated(const char *name, rt_blas_usage_e usage, int max_geometries, const int *max_prims, int max_vertex, uint32_t extra_buffer_offset);

void RT_BlasDestroy(struct rt_blas_s* blas);

// 1. Schedules BLAS build (allocates geoms+ranges from a temp pool, etc).
// 2. Allocates kusochki (if not) and fills them with geom and initial material data
qboolean RT_BlasBuild(struct rt_blas_s *blas, const struct vk_render_geometry_s *geoms, int geoms_count);

VkDeviceAddress RT_BlasGetDeviceAddress(struct rt_blas_s *blas);

typedef struct rt_kusochki_s {
	uint32_t offset;
	int count;
	int internal_index__;
} rt_kusochki_t;

// TODO lifetime arg here is KORYAVY
rt_kusochki_t RT_KusochkiAlloc(int count, r_geometry_lifetime_t lifetime);
void RT_KusochkiFree(const rt_kusochki_t*);

struct vk_render_geometry_s;
qboolean RT_KusochkiUpload(const rt_kusochki_t *kusochki, const struct vk_render_geometry_s *geoms, int geoms_count, int override_texture_id);

// Update animated materials
void RT_KusochkiUploadSubset(rt_kusochki_t *kusochki, const struct vk_render_geometry_s *geoms, const int *geoms_indices, int geoms_indices_count);

typedef struct {
	const struct rt_blas_s* blas;
	uint32_t kusochki_offset;
	int render_type; // TODO material_mode
	const matrix3x4 *transform, *prev_transform;
	const vec4_t *color;
} rt_blas_frame_args_t;

void RT_BlasAddToFrame( rt_blas_frame_args_t args );

struct rt_model_s;

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
} rt_frame_add_model_t;

void RT_FrameAddModel( struct rt_model_s *model, rt_frame_add_model_t args );
