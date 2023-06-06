#pragma once

#include "vk_core.h"
#include "vk_buffer.h"
#include "vk_const.h"
#include "vk_rtx.h"

#define MAX_INSTANCES 2048
#define MAX_KUSOCHKI 32768
#define MODEL_CACHE_SIZE 2048

#include "shaders/ray_interop.h"

typedef struct vk_ray_model_s {
	VkAccelerationStructureKHR blas;
	VkDeviceAddress blas_addr;

	uint32_t kusochki_offset;
	qboolean dynamic;

	// TODO remove
	struct {
		VkAccelerationStructureGeometryKHR *geoms;
		int max_prims;
		int num_geoms;
		uint32_t size;
		qboolean taken;
	} cache_toremove;
} vk_ray_model_t;

typedef struct Kusok vk_kusok_data_t;

typedef struct rt_draw_instance_s {
	VkDeviceAddress blas_addr;
	uint32_t kusochki_offset;
	matrix3x4 transform_row;
	matrix4x4 prev_transform_row;
	vec4_t color;
	uint32_t material_mode; // MATERIAL_MODE_ from ray_interop.h
} rt_draw_instance_t;

typedef struct {
	const char *debug_name;
	VkAccelerationStructureKHR *p_accel;
	const VkAccelerationStructureGeometryKHR *geoms;
	const uint32_t *max_prim_counts;
	const VkAccelerationStructureBuildRangeInfoKHR *build_ranges;
	uint32_t n_geoms;
	VkAccelerationStructureTypeKHR type;
	qboolean dynamic;

	VkDeviceAddress *out_accel_addr;
	uint32_t *inout_size;
} as_build_args_t;

struct vk_combuf_s;
qboolean createOrUpdateAccelerationStructure(struct vk_combuf_s *combuf, const as_build_args_t *args);

#define MAX_SCRATCH_BUFFER (32*1024*1024)
#define MAX_ACCELS_BUFFER (64*1024*1024)

typedef struct {
	// Geometry metadata. Lifetime is similar to geometry lifetime itself.
	// Semantically close to render buffer (describes layout for those objects)
	// TODO unify with render buffer?
	// Needs: STORAGE_BUFFER
	vk_buffer_t kusochki_buffer;
	r_debuffer_t kusochki_alloc;
	// TODO when fully rt_model: r_blocks_t alloc;

	// Model header
	// Array of struct ModelHeader: color, material_mode, prev_transform
	vk_buffer_t model_headers_buffer;

	// Per-frame data that is accumulated between RayFrameBegin and End calls
	struct {
		rt_draw_instance_t instances[MAX_INSTANCES];
		int instances_count;

		uint32_t scratch_offset; // for building dynamic blases
	} frame;
} xvk_ray_model_state_t;

extern xvk_ray_model_state_t g_ray_model_state;

void XVK_RayModel_ClearForNextFrame( void );
void XVK_RayModel_Validate(void);

void RT_RayModel_Clear(void);

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

rt_kusochki_t RT_KusochkiAllocLong(int count);
uint32_t RT_KusochkiAllocOnce(int count);
void RT_KusochkiFree(const rt_kusochki_t*);

struct vk_render_geometry_s;
qboolean RT_KusochkiUpload(uint32_t kusochki_offset, const struct vk_render_geometry_s *geoms, int geoms_count, int override_texture_id);

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
