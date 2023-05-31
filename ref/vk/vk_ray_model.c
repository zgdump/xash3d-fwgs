#include "vk_ray_internal.h"

#include "vk_rtx.h"
#include "vk_textures.h"
#include "vk_materials.h"
#include "vk_geometry.h"
#include "vk_render.h"
#include "vk_staging.h"
#include "vk_light.h"
#include "vk_math.h"
#include "vk_combuf.h"

#include "eiface.h"
#include "xash3d_mathlib.h"

#include <string.h>

xvk_ray_model_state_t g_ray_model_state;

typedef struct rt_model_s {
	struct rt_blas_s *blas;
	VkDeviceAddress blas_addr;
	rt_kusochki_t kusochki;
} rt_model_t;

static void returnModelToCache(vk_ray_model_t *model) {
	ASSERT(model->cache_toremove.taken);
	model->cache_toremove.taken = false;
}

static vk_ray_model_t *getModelFromCache(int num_geoms, int max_prims, const VkAccelerationStructureGeometryKHR *geoms) { //}, int size) {
	vk_ray_model_t *model = NULL;
	int i;
	for (i = 0; i < ARRAYSIZE(g_ray_model_state.models_cache); ++i)
	{
		int j;
	 	model = g_ray_model_state.models_cache + i;
		if (model->cache_toremove.taken)
			continue;

		if (!model->blas)
			break;

		if (model->cache_toremove.num_geoms != num_geoms)
			continue;

		if (model->cache_toremove.max_prims != max_prims)
			continue;

		for (j = 0; j < num_geoms; ++j) {
			if (model->cache_toremove.geoms[j].geometryType != geoms[j].geometryType)
				break;

			if (model->cache_toremove.geoms[j].flags != geoms[j].flags)
				break;

			if (geoms[j].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
				// TODO what else should we compare?
				if (model->cache_toremove.geoms[j].geometry.triangles.maxVertex != geoms[j].geometry.triangles.maxVertex)
					break;

				ASSERT(model->cache_toremove.geoms[j].geometry.triangles.vertexStride == geoms[j].geometry.triangles.vertexStride);
				ASSERT(model->cache_toremove.geoms[j].geometry.triangles.vertexFormat == geoms[j].geometry.triangles.vertexFormat);
				ASSERT(model->cache_toremove.geoms[j].geometry.triangles.indexType == geoms[j].geometry.triangles.indexType);
			} else {
				PRINT_NOT_IMPLEMENTED_ARGS("Non-tri geometries are not implemented");
				break;
			}
		}

		if (j == num_geoms)
			break;
	}

	if (i == ARRAYSIZE(g_ray_model_state.models_cache))
		return NULL;

	// if (model->size > 0)
	// 	ASSERT(model->size >= size);

	if (!model->cache_toremove.geoms) {
		const size_t size = sizeof(*geoms) * num_geoms;
		model->cache_toremove.geoms = Mem_Malloc(vk_core.pool, size);
		memcpy(model->cache_toremove.geoms, geoms, size);
		model->cache_toremove.num_geoms = num_geoms;
		model->cache_toremove.max_prims = max_prims;
	}

	model->cache_toremove.taken = true;
	return model;
}

static void applyMaterialToKusok(vk_kusok_data_t* kusok, const vk_render_geometry_t *geom) {
	const xvk_material_t *const mat = XVK_GetMaterialForTextureIndex( geom->texture );
	ASSERT(mat);

	// TODO split kusochki into static geometry data and potentially dynamic material data
	// This data is static, should never change
	kusok->vertex_offset = geom->vertex_offset;
	kusok->index_offset = geom->index_offset;
	kusok->triangles = geom->element_count / 3;

	// Material data itself is mostly static. Except for animated textures, which just get a new material slot for each frame.
	kusok->material = (struct Material){
		.tex_base_color = mat->tex_base_color,
		.tex_roughness = mat->tex_roughness,
		.tex_metalness = mat->tex_metalness,
		.tex_normalmap = mat->tex_normalmap,

		.roughness = mat->roughness,
		.metalness = mat->metalness,
		.normal_scale = mat->normal_scale,
	};

	// TODO emissive is potentially "dynamic", not tied to the material directly, as it is specified per-surface in rad files
	VectorCopy(geom->emissive, kusok->emissive);
	Vector4Copy(mat->base_color, kusok->material.base_color);

	// TODO should be patched by the Chrome material source itself to generate a static chrome material
	const qboolean HACK_chrome = geom->material == kXVkMaterialChrome;
	if (!mat->set && HACK_chrome)
		kusok->material.tex_roughness = tglob.grayTexture;

	// Purely static. Once a sky forever a sky.
	if (geom->material == kXVkMaterialSky)
		kusok->material.tex_base_color = TEX_BASE_SKYBOX;
}


vk_ray_model_t* VK_RayModelCreate( vk_ray_model_init_t args ) {
	VkAccelerationStructureGeometryKHR *geoms;
	uint32_t *geom_max_prim_counts;
	VkAccelerationStructureBuildRangeInfoKHR *geom_build_ranges;
	const VkBuffer geometry_buffer = R_GeometryBuffer_Get();
	const VkDeviceAddress buffer_addr = R_VkBufferGetDeviceAddress(geometry_buffer);
	vk_ray_model_t *ray_model;
	int max_prims = 0;

	ASSERT(vk_core.rtx);

	// FIXME don't touch allocator each frame many times pls
	geoms = Mem_Calloc(vk_core.pool, args.model->num_geometries * sizeof(*geoms));
	geom_max_prim_counts = Mem_Malloc(vk_core.pool, args.model->num_geometries * sizeof(*geom_max_prim_counts));
	geom_build_ranges = Mem_Calloc(vk_core.pool, args.model->num_geometries * sizeof(*geom_build_ranges));

	/* gEngine.Con_Reportf("Loading model %s, geoms: %d\n", args.model->debug_name, args.model->num_geometries); */

	for (int i = 0; i < args.model->num_geometries; ++i) {
		vk_render_geometry_t *mg = args.model->geometries + i;
		const uint32_t prim_count = mg->element_count / 3;

		max_prims += prim_count;
		geom_max_prim_counts[i] = prim_count;
		geoms[i] = (VkAccelerationStructureGeometryKHR)
			{
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
				.flags = VK_GEOMETRY_OPAQUE_BIT_KHR, // FIXME this is not true. incoming mode might have transparency eventually (and also dynamically)
				.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
				.geometry.triangles =
					(VkAccelerationStructureGeometryTrianglesDataKHR){
						.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
						.indexType = VK_INDEX_TYPE_UINT16,
						.maxVertex = mg->max_vertex,
						.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
						.vertexStride = sizeof(vk_vertex_t),
						.vertexData.deviceAddress = buffer_addr,
						.indexData.deviceAddress = buffer_addr,
					},
			};

		geom_build_ranges[i] = (VkAccelerationStructureBuildRangeInfoKHR) {
			.primitiveCount = prim_count,
			.primitiveOffset = mg->index_offset * sizeof(uint16_t),
			.firstVertex = mg->vertex_offset,
		};
	}

	// FIXME this is definitely not the right place. We should upload everything in bulk, and only then build blases in bulk too
	vk_combuf_t *const combuf = R_VkStagingCommit();
	{
		const VkBufferMemoryBarrier bmb[] = { {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			//.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, // FIXME
			.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT, // FIXME
			.buffer = geometry_buffer,
			.offset = 0, // FIXME
			.size = VK_WHOLE_SIZE, // FIXME
		} };
		vkCmdPipelineBarrier(combuf->cmdbuf,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			//VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0, 0, NULL, ARRAYSIZE(bmb), bmb, 0, NULL);
	}

	{
		as_build_args_t asrgs = {
			.geoms = geoms,
			.max_prim_counts = geom_max_prim_counts,
			.build_ranges = geom_build_ranges,
			.n_geoms = args.model->num_geometries,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			.dynamic = args.model->dynamic,
			.debug_name = args.model->debug_name,
		};
		ray_model = getModelFromCache(args.model->num_geometries, max_prims, geoms); //, build_size.accelerationStructureSize);
		if (!ray_model) {
			gEngine.Con_Printf(S_ERROR "Ran out of model cache slots\n");
		} else {
			qboolean result;
			asrgs.p_accel = &ray_model->blas;
			asrgs.out_accel_addr = &ray_model->blas_addr;
			asrgs.inout_size = &ray_model->cache_toremove.size;

			DEBUG_BEGINF(combuf->cmdbuf, "build blas for %s", args.model->debug_name);
			result = createOrUpdateAccelerationStructure(combuf, &asrgs);
			DEBUG_END(combuf->cmdbuf);

			if (!result)
			{
				gEngine.Con_Printf(S_ERROR "Could not build BLAS for %s\n", args.model->debug_name);
				returnModelToCache(ray_model);
				ray_model = NULL;
			} else {
				ray_model->kusochki_offset = ALO_ALLOC_FAILED;
				ray_model->dynamic = args.model->dynamic;
			}
		}
	}

	Mem_Free(geom_build_ranges);
	Mem_Free(geom_max_prim_counts);
	Mem_Free(geoms); // TODO this can be cached within models_cache ??

	//gEngine.Con_Reportf("Model %s (%p) created blas %p\n", args.model->debug_name, args.model, args.model->rtx.blas);

	return ray_model;
}

void VK_RayModelDestroy( struct vk_ray_model_s *model ) {
	ASSERT(vk_core.rtx);
	if (model->blas != VK_NULL_HANDLE) {
		//gEngine.Con_Reportf("Model %s destroying AS=%p blas_index=%d\n", model->debug_name, model->rtx.blas, blas_index);

		vkDestroyAccelerationStructureKHR(vk_core.device, model->blas, NULL);
		Mem_Free(model->cache_toremove.geoms);
		memset(model, 0, sizeof(*model));
	}
}

// TODO move this to vk_brush
static void computeConveyorSpeed(const color24 rendercolor, int tex_index, vec2_t speed) {
	float sy, cy;
	float flConveyorSpeed = 0.0f;
	float flRate, flAngle;
	vk_texture_t *texture = findTexture( tex_index );
	//gl_texture_t	*texture;

	// FIXME
	/* if( ENGINE_GET_PARM( PARM_QUAKE_COMPATIBLE ) && RI.currententity == gEngfuncs.GetEntityByIndex( 0 ) ) */
	/* { */
	/* 	// same as doom speed */
	/* 	flConveyorSpeed = -35.0f; */
	/* } */
	/* else */
	{
		flConveyorSpeed = (rendercolor.g<<8|rendercolor.b) / 16.0f;
		if( rendercolor.r ) flConveyorSpeed = -flConveyorSpeed;
	}
	//texture = R_GetTexture( glState.currentTextures[glState.activeTMU] );

	flRate = fabs( flConveyorSpeed ) / (float)texture->width;
	flAngle = ( flConveyorSpeed >= 0 ) ? 180 : 0;

	SinCos( flAngle * ( M_PI_F / 180.0f ), &sy, &cy );
	speed[0] = cy * flRate;
	speed[1] = sy * flRate;
}

// TODO utilize uploadKusochki([1]) to avoid 2 copies of staging code
static qboolean uploadKusochkiSubset(const vk_ray_model_t *const model, const vk_render_model_t *const render_model,  const int *geom_indexes, int geom_indexes_count) {
	// TODO can we sort all animated geometries (in brush) to have only a single range here?
	for (int i = 0; i < geom_indexes_count; ++i) {
		const int index = geom_indexes[i];

		const vk_staging_buffer_args_t staging_args = {
			.buffer = g_ray_model_state.kusochki_buffer.buffer,
			.offset = (model->kusochki_offset + index) * sizeof(vk_kusok_data_t),
			.size = sizeof(vk_kusok_data_t),
			.alignment = 16,
		};
		const vk_staging_region_t kusok_staging = R_VkStagingLockForBuffer(staging_args);

		if (!kusok_staging.ptr) {
			gEngine.Con_Printf(S_ERROR "Couldn't allocate staging for %d kusochek for model %s\n", 1, render_model->debug_name);
			return false;
		}

		vk_kusok_data_t *const kusochki = kusok_staging.ptr;

		vk_render_geometry_t *geom = render_model->geometries + index;
		applyMaterialToKusok(kusochki + 0, geom);

		/* gEngine.Con_Reportf("model %s: geom=%d kuoffs=%d kustoff=%d kustsz=%d sthndl=%d\n", */
		/* 		render_model->debug_name, */
		/* 		render_model->num_geometries, */
		/* 		model->kusochki_offset, */
		/* 		staging_args.offset, staging_args.size, */
		/* 		kusok_staging.handle */
		/* 		); */

		R_VkStagingUnlock(kusok_staging.handle);
	}
	return true;
}

static qboolean uploadKusochki(const vk_ray_model_t *const model, const vk_render_model_t *const render_model) {
	const vk_staging_buffer_args_t staging_args = {
		.buffer = g_ray_model_state.kusochki_buffer.buffer,
		.offset = model->kusochki_offset * sizeof(vk_kusok_data_t),
		.size = render_model->num_geometries * sizeof(vk_kusok_data_t),
		.alignment = 16,
	};
	const vk_staging_region_t kusok_staging = R_VkStagingLockForBuffer(staging_args);

	if (!kusok_staging.ptr) {
		gEngine.Con_Printf(S_ERROR "Couldn't allocate staging for %d kusochkov for model %s\n", render_model->num_geometries, render_model->debug_name);
		return false;
	}

	vk_kusok_data_t *const kusochki = kusok_staging.ptr;

	for (int i = 0; i < render_model->num_geometries; ++i) {
		vk_render_geometry_t *geom = render_model->geometries + i;
		applyMaterialToKusok(kusochki + i, geom);
	}

	/* gEngine.Con_Reportf("model %s: geom=%d kuoffs=%d kustoff=%d kustsz=%d sthndl=%d\n", */
	/* 		render_model->debug_name, */
	/* 		render_model->num_geometries, */
	/* 		model->kusochki_offset, */
	/* 		staging_args.offset, staging_args.size, */
	/* 		kusok_staging.handle */
	/* 		); */

	R_VkStagingUnlock(kusok_staging.handle);
	return true;
}

// TODO this material mapping is context dependent. I.e. different entity types might need different ray tracing behaviours for
// same render_mode/type and even texture.
static uint32_t materialModeFromRenderType(vk_render_type_e render_type) {
	switch (render_type) {
		case kVkRenderTypeSolid:
			return MATERIAL_MODE_OPAQUE;
			break;
		case kVkRenderType_A_1mA_RW: // blend: scr*a + dst*(1-a), depth: RW
		case kVkRenderType_A_1mA_R:  // blend: scr*a + dst*(1-a), depth test
			// FIXME where is MATERIAL_MODE_TRANSLUCENT??1
			return MATERIAL_MODE_BLEND_MIX;
			break;
		case kVkRenderType_A_1:   // blend: scr*a + dst, no depth test or write; sprite:kRenderGlow only
			return MATERIAL_MODE_BLEND_GLOW;
			break;
		case kVkRenderType_A_1_R: // blend: scr*a + dst, depth test
		case kVkRenderType_1_1_R: // blend: scr + dst, depth test
			return MATERIAL_MODE_BLEND_ADD;
			break;
		case kVkRenderType_AT: // no blend, depth RW, alpha test
			return MATERIAL_MODE_OPAQUE_ALPHA_TEST;
			break;

		default:
			gEngine.Host_Error("Unexpected render type %d\n", render_type);
	}

	return MATERIAL_MODE_OPAQUE;
}

void VK_RayFrameAddModel( vk_ray_model_t *model, const vk_render_model_t *render_model) {
	rt_draw_instance_t* draw_instance = g_ray_model_state.frame.instances + g_ray_model_state.frame.instances_count;

	ASSERT(vk_core.rtx);
	ASSERT(g_ray_model_state.frame.instances_count <= ARRAYSIZE(g_ray_model_state.frame.instances));
	ASSERT(model->cache_toremove.num_geoms == render_model->num_geometries);

	if (g_ray_model_state.frame.instances_count == ARRAYSIZE(g_ray_model_state.frame.instances)) {
		gEngine.Con_Printf(S_ERROR "Ran out of AccelerationStructure slots\n");
		return;
	}

	ASSERT(model->blas != VK_NULL_HANDLE);

	// Upload kusochki for the first time
	if (ALO_ALLOC_FAILED == model->kusochki_offset) {
		const r_lifetime_t lifetime = model->dynamic ? LifetimeDynamic : LifetimeStatic;
		const int count = render_model->num_geometries;
		model->kusochki_offset = R_DEBuffer_Alloc(&g_ray_model_state.kusochki_alloc, lifetime, count, 1);

		if (model->kusochki_offset == ALO_ALLOC_FAILED) {
			gEngine.Con_Printf(S_ERROR "Maximum number of kusochki exceeded on model %s\n", render_model->debug_name);
			return;
		}

		if (!uploadKusochki(model, render_model))
			return;
	} else {
		/* FIXME move to RT_FrameAddModel if (!uploadKusochkiSubset(model, render_model, render_model->geometries_changed, render_model->geometries_changed_count)) */
		/* 	return; */
	}

	// TODO needed for brush models only
	// (? TODO studio models?)
	for (int i = 0; i < render_model->dynamic_polylights_count; ++i) {
		rt_light_add_polygon_t *const polylight = render_model->dynamic_polylights + i;
		polylight->transform_row = (const matrix3x4*)render_model->deprecate.transform;
		polylight->dynamic = true;
		RT_LightAddPolygon(polylight);
	}

	draw_instance->model_toremove = model;
	draw_instance->blas_addr = model->blas_addr;
	draw_instance->kusochki_offset = model->kusochki_offset;
	draw_instance->material_mode = materialModeFromRenderType(render_model->deprecate.render_type);
	Vector4Copy(render_model->deprecate.color, draw_instance->color);
	Matrix3x4_Copy(draw_instance->transform_row, render_model->deprecate.transform);
	Matrix4x4_Copy(draw_instance->prev_transform_row, render_model->deprecate.prev_transform);

	g_ray_model_state.frame.instances_count++;
}

void RT_RayModel_Clear(void) {
	R_DEBuffer_Init(&g_ray_model_state.kusochki_alloc, MAX_KUSOCHKI / 2, MAX_KUSOCHKI / 2);
}

void XVK_RayModel_ClearForNextFrame( void ) {
	// FIXME we depend on the fact that only a single frame can be in flight
	// currently framectl waits for the queue to complete before returning
	// so we can be sure here that previous frame is complete and we're free to
	// destroy/reuse dynamic ASes from previous frame
	for (int i = 0; i < g_ray_model_state.frame.instances_count; ++i) {
		rt_draw_instance_t *instance = g_ray_model_state.frame.instances + i;
		ASSERT(instance->blas_addr);

		if (!instance->model_toremove)
			continue;

		if (!instance->model_toremove->dynamic)
			continue;

		returnModelToCache(instance->model_toremove);
		instance->model_toremove = NULL;
	}

	g_ray_model_state.frame.instances_count = 0;

	// TODO N frames in flight
	// HACK: blas caching requires persistent memory
	// proper fix would need some other memory allocation strategy
	// VK_RingBuffer_ClearFrame(&g_rtx.accels_buffer_alloc);
	//VK_RingBuffer_ClearFrame(&g_ray_model_state.kusochki_alloc);
	R_DEBuffer_Flip(&g_ray_model_state.kusochki_alloc);
}

rt_kusochki_t RT_KusochkiAlloc(int count, r_geometry_lifetime_t lifetime) {
	// TODO Proper block allocator
	const r_lifetime_t rlifetime = lifetime == LifetimeSingleFrame ? LifetimeDynamic : LifetimeStatic;
	uint32_t kusochki_offset = R_DEBuffer_Alloc(&g_ray_model_state.kusochki_alloc, rlifetime, count, 1);

	if (kusochki_offset == ALO_ALLOC_FAILED) {
		gEngine.Con_Printf(S_ERROR "Maximum number of kusochki exceeded\n");
		return (rt_kusochki_t){0,0,-1};
	}

	return (rt_kusochki_t){
		.offset = kusochki_offset,
		.count = count,
		.internal_index__ = 0, // ???
	};
}

void RT_KusochkiFree(const rt_kusochki_t *kusochki) {
	// TODO block alloc
	PRINT_NOT_IMPLEMENTED();
}

qboolean RT_KusochkiUpload(const rt_kusochki_t *kusochki, const struct vk_render_geometry_s *geoms, int geoms_count, int override_texture_id) {
	ASSERT(kusochki->count == geoms_count);

	// TODO not implemented yet
	ASSERT(override_texture_id < 0);

	const vk_staging_buffer_args_t staging_args = {
		.buffer = g_ray_model_state.kusochki_buffer.buffer,
		.offset = kusochki->offset * sizeof(vk_kusok_data_t),
		.size = geoms_count * sizeof(vk_kusok_data_t),
		.alignment = 16,
	};
	const vk_staging_region_t kusok_staging = R_VkStagingLockForBuffer(staging_args);

	if (!kusok_staging.ptr) {
		gEngine.Con_Printf(S_ERROR "Couldn't allocate staging for %d kusochkov\n", geoms_count);
		return false;
	}

	vk_kusok_data_t *const p = kusok_staging.ptr;
	for (int i = 0; i < geoms_count; ++i) {
		const vk_render_geometry_t *geom = geoms + i;
		applyMaterialToKusok(p + i, geom);
	}

	R_VkStagingUnlock(kusok_staging.handle);
	return true;
}

struct rt_model_s *RT_ModelCreate(rt_model_create_t args) {
	const rt_kusochki_t kusochki = RT_KusochkiAlloc(args.geometries_count, LifetimeLong);
	if (kusochki.count == 0) {
		gEngine.Con_Printf(S_ERROR "Cannot allocate kusochki for %s\n", args.debug_name);
		return NULL;
	}

	struct rt_blas_s* blas = RT_BlasCreate(args.usage);
	if (!blas) {
		gEngine.Con_Printf(S_ERROR "Cannot create BLAS for %s\n", args.debug_name);
		goto fail;
	}

	if (!RT_BlasBuild(blas, args.geometries, args.geometries_count)) {
		gEngine.Con_Printf(S_ERROR "Cannot build BLAS for %s\n", args.debug_name);
		goto fail;
	}

	RT_KusochkiUpload(&kusochki, args.geometries, args.geometries_count, -1);

	{
		rt_model_t *const ret = Mem_Malloc(vk_core.pool, sizeof(*ret));
		ret->blas = blas;
		ret->blas_addr = RT_BlasGetDeviceAddress(ret->blas);
		ret->kusochki = kusochki;
		return ret;
	}

fail:
	if (blas)
		RT_BlasDestroy(blas);

	if (kusochki.count)
		RT_KusochkiFree(&kusochki);

	return NULL;
}

void RT_ModelDestroy(struct rt_model_s* model) {
	if (!model)
		return;

	if (model->blas)
		RT_BlasDestroy(model->blas);

	if (model->kusochki.count)
		RT_KusochkiFree(&model->kusochki);

	Mem_Free(model);
}

void RT_FrameAddModel( struct rt_model_s *model, rt_frame_add_model_t args ) {
	if (!model || !model->blas)
		return;

	ASSERT(g_ray_model_state.frame.instances_count <= ARRAYSIZE(g_ray_model_state.frame.instances));

	rt_draw_instance_t* draw_instance = g_ray_model_state.frame.instances + g_ray_model_state.frame.instances_count;

	draw_instance->model_toremove = NULL;
	draw_instance->blas_addr = model->blas_addr;
	draw_instance->kusochki_offset = model->kusochki.offset;
	draw_instance->material_mode = materialModeFromRenderType(args.render_type);
	Vector4Copy(*args.color, draw_instance->color);
	Matrix3x4_Copy(draw_instance->transform_row, args.transform);
	Matrix4x4_Copy(draw_instance->prev_transform_row, args.prev_transform);
	g_ray_model_state.frame.instances_count++;
}

