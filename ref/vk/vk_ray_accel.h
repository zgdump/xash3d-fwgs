#pragma once

#include "vk_core.h"
#include "vk_buffer.h"
#include "vk_math.h"
#include "ray_resources.h"

qboolean RT_VkAccelInit(void);
void RT_VkAccelShutdown(void);

void RT_VkAccelNewMap(void);

struct rt_blas_s;
struct vk_render_geometry_s;

typedef enum {
	kBlasBuildStatic, // builds slow for fast trace
	kBlasBuildDynamicUpdate, // builds if not built, updates if built
	kBlasBuildDynamicFast, // builds fast from scratch (no correlation with previous frame guaranteed, e.g. triapi)
} rt_blas_usage_e;

// Just creates an empty BLAS structure, doesn't alloc anything
struct rt_blas_s* RT_BlasCreate(rt_blas_usage_e usage);

// Create an empty BLAS with specified limits
struct rt_blas_s* RT_BlasCreatePreallocated(rt_blas_usage_e usage, int max_geometries, const int *max_prims, int max_vertex, uint32_t extra_buffer_offset);

void RT_BlasDestroy(struct rt_blas_s* blas);

// 1. Schedules BLAS build (allocates geoms+ranges from a temp pool, etc).
// 2. Allocates kusochki (if not) and fills them with geom and initial material data
qboolean RT_BlasBuild(struct rt_blas_s *blas, const struct vk_render_geometry_s *geoms, int geoms_count);

typedef struct {
	const struct rt_blas_s* blas;
	int material_mode;
	matrix3x4 *transform, *prev_transform;
	vec4_t *color;
	uint32_t material_override;
} rt_frame_add_model_args_t;

void RT_VkAccelFrameBegin(void);
void RT_VkAccelFrameAddBlas( rt_frame_add_model_args_t args );

struct vk_combuf_s;
vk_resource_t RT_VkAccelPrepareTlas(struct vk_combuf_s *combuf);
