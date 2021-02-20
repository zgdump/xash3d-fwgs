#include "vk_render.h"

#include "vk_core.h"
#include "vk_buffer.h"
#include "vk_const.h"
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_textures.h"
#include "vk_math.h"

#include "eiface.h"
#include "xash3d_mathlib.h"

#include <memory.h>

#define MAX_UNIFORM_SLOTS (MAX_SCENE_ENTITIES * 2 /* solid + trans */ + 1)

typedef struct {
	matrix4x4 mvp;
	vec4_t color;
} uniform_data_t;

static struct {
	VkPipelineLayout pipeline_layout;
	VkPipeline pipelines[kRenderTransAdd + 1];

	vk_buffer_t buffer;
	uint32_t buffer_free_offset;

	vk_buffer_t uniform_buffer;
	uint32_t uniform_unit_size;

	struct {
		int align_holes_size;
	} stat;

	uint32_t temp_buffer_offset;
} g_render;

static qboolean createPipelines( void )
{
	/* VkPushConstantRange push_const = { */
	/* 	.offset = 0, */
	/* 	.size = sizeof(AVec3f), */
	/* 	.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, */
	/* }; */

	VkDescriptorSetLayout descriptor_layouts[] = {
		vk_core.descriptor_pool.one_uniform_buffer_layout,
		vk_core.descriptor_pool.one_texture_layout,
		vk_core.descriptor_pool.one_texture_layout,
	};

	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = ARRAYSIZE(descriptor_layouts),
		.pSetLayouts = descriptor_layouts,
		/* .pushConstantRangeCount = 1, */
		/* .pPushConstantRanges = &push_const, */
	};

	// FIXME store layout separately
	XVK_CHECK(vkCreatePipelineLayout(vk_core.device, &plci, NULL, &g_render.pipeline_layout));

	{
		struct ShaderSpec {
			float alpha_test_threshold;
		} spec_data = { .25f };
		const VkSpecializationMapEntry spec_map[] = {
			{.constantID = 0, .offset = offsetof(struct ShaderSpec, alpha_test_threshold), .size = sizeof(float) },
		};

		VkSpecializationInfo alpha_test_spec = {
			.mapEntryCount = ARRAYSIZE(spec_map),
			.pMapEntries = spec_map,
			.dataSize = sizeof(struct ShaderSpec),
			.pData = &spec_data
		};

		VkVertexInputAttributeDescription attribs[] = {
			{.binding = 0, .location = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(vk_vertex_t, pos)},
			{.binding = 0, .location = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vk_vertex_t, gl_tc)},
			{.binding = 0, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(vk_vertex_t, lm_tc)},
		};

		VkPipelineShaderStageCreateInfo shader_stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = loadShader("brush.vert.spv"),
			.pName = "main",
		}, {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = loadShader("brush.frag.spv"),
			.pName = "main",
		}};

		vk_pipeline_create_info_t ci = {
			.layout = g_render.pipeline_layout,
			.attribs = attribs,
			.num_attribs = ARRAYSIZE(attribs),

			.stages = shader_stages,
			.num_stages = ARRAYSIZE(shader_stages),

			.vertex_stride = sizeof(vk_vertex_t),

			.depthTestEnable = VK_TRUE,
			.depthWriteEnable = VK_TRUE,
			.depthCompareOp = VK_COMPARE_OP_LESS,

			.blendEnable = VK_FALSE,

			.cullMode = VK_CULL_MODE_FRONT_BIT,
		};

		for (int i = 0; i < ARRAYSIZE(g_render.pipelines); ++i)
		{
			const char *name = "UNDEFINED";
			switch (i)
			{
				case kRenderNormal:
					ci.stages[1].pSpecializationInfo = NULL;
					ci.blendEnable = VK_FALSE;
					ci.depthWriteEnable = VK_TRUE;
					ci.depthTestEnable = VK_TRUE;
					name = "brush kRenderNormal";
					break;

				case kRenderTransColor:
					ci.stages[1].pSpecializationInfo = NULL;
					ci.depthWriteEnable = VK_TRUE;
					ci.depthTestEnable = VK_TRUE;
					ci.blendEnable = VK_TRUE;
					ci.colorBlendOp = VK_BLEND_OP_ADD; // TODO check
					ci.srcAlphaBlendFactor = ci.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					ci.dstAlphaBlendFactor = ci.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					name = "brush kRenderTransColor";
					break;

				case kRenderTransAdd:
					ci.stages[1].pSpecializationInfo = NULL;
					ci.depthWriteEnable = VK_FALSE;
					ci.depthTestEnable = VK_TRUE;
					ci.blendEnable = VK_TRUE;
					ci.colorBlendOp = VK_BLEND_OP_ADD; // TODO check

					// sprites do SRC_ALPHA
					ci.srcAlphaBlendFactor = ci.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;// TODO ? FACTOR_ONE;
					ci.dstAlphaBlendFactor = ci.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
					name = "brush kRenderTransAdd";
					break;

				case kRenderTransAlpha:
					ci.stages[1].pSpecializationInfo = &alpha_test_spec;
					ci.depthWriteEnable = VK_TRUE;
					ci.depthTestEnable = VK_TRUE;
					ci.blendEnable = VK_FALSE;
					name = "brush kRenderTransAlpha(test)";
					break;

				case kRenderGlow:
					ci.stages[1].pSpecializationInfo = NULL;
					ci.depthWriteEnable = VK_FALSE;
					ci.depthTestEnable = VK_FALSE;
					ci.blendEnable = VK_TRUE;
					ci.colorBlendOp = VK_BLEND_OP_ADD; // TODO check
					ci.srcAlphaBlendFactor = ci.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					ci.dstAlphaBlendFactor = ci.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
					break;

				case kRenderTransTexture:
					ci.stages[1].pSpecializationInfo = NULL;
					ci.depthWriteEnable = VK_FALSE;
					ci.depthTestEnable = VK_TRUE;
					ci.blendEnable = VK_TRUE;
					ci.colorBlendOp = VK_BLEND_OP_ADD; // TODO check
					ci.srcAlphaBlendFactor = ci.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					ci.dstAlphaBlendFactor = ci.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					name = "brush kRenderTransTexture/Glow";
					break;

				default:
					ASSERT(!"Unreachable");
			}

			g_render.pipelines[i] = createPipeline(&ci);

			if (!g_render.pipelines[i])
			{
				// TODO complain
				return false;
			}

			if (vk_core.debug)
			{
				VkDebugUtilsObjectNameInfoEXT debug_name = {
					.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
					.objectHandle = (uint64_t)g_render.pipelines[i],
					.objectType = VK_OBJECT_TYPE_PIPELINE,
					.pObjectName = name,
				};
				XVK_CHECK(vkSetDebugUtilsObjectNameEXT(vk_core.device, &debug_name));
			}
		}

		for (int i = 0; i < (int)ARRAYSIZE(shader_stages); ++i)
			vkDestroyShaderModule(vk_core.device, shader_stages[i].module, NULL);
	}

	return true;
}

qboolean VK_RenderInit( void )
{
	// TODO Better estimates
	const uint32_t vertex_buffer_size = MAX_BUFFER_VERTICES * sizeof(float) * (3 + 3 + 2 + 2);
	const uint32_t index_buffer_size = MAX_BUFFER_INDICES * sizeof(uint16_t);
	const uint32_t ubo_align = Q_max(4, vk_core.physical_device.properties.limits.minUniformBufferOffsetAlignment);

	g_render.uniform_unit_size = ((sizeof(uniform_data_t) + ubo_align - 1) / ubo_align) * ubo_align;

	// TODO device memory and friends (e.g. handle mobile memory ...)

	if (!createBuffer(&g_render.buffer, vertex_buffer_size + index_buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		return false;

	if (!createBuffer(&g_render.uniform_buffer, g_render.uniform_unit_size * MAX_UNIFORM_SLOTS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		return false;

	{
		VkDescriptorBufferInfo dbi = {
			.buffer = g_render.uniform_buffer.buffer,
			.offset = 0,
			.range = sizeof(uniform_data_t),
		};
		VkWriteDescriptorSet wds[] = { {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.pBufferInfo = &dbi,
			.dstSet = vk_core.descriptor_pool.ubo_sets[0], // FIXME
		}};
		vkUpdateDescriptorSets(vk_core.device, ARRAYSIZE(wds), wds, 0, NULL);
	}

	if (!createPipelines())
		return false;

	return true;
}

void VK_RenderShutdown( void )
{
	for (int i = 0; i < ARRAYSIZE(g_render.pipelines); ++i)
		vkDestroyPipeline(vk_core.device, g_render.pipelines[i], NULL);
	vkDestroyPipelineLayout( vk_core.device, g_render.pipeline_layout, NULL );

	destroyBuffer( &g_render.buffer );
	destroyBuffer( &g_render.uniform_buffer );
}

static vk_buffer_alloc_t renderBufferAlloc( uint32_t unit_size, uint32_t count )
{
	const uint32_t offset = ALIGN_UP(g_render.buffer_free_offset, unit_size);
	const uint32_t alloc_size = unit_size * count;
	vk_buffer_alloc_t ret = {0};
	if (offset + alloc_size > g_render.buffer.size) {
		gEngine.Con_Printf(S_ERROR "Cannot allocate %u bytes aligned at %u from buffer; only %u are left",
				alloc_size, unit_size, g_render.buffer.size - offset);
		return (vk_buffer_alloc_t){0};
	}

	ret.buffer_offset_in_units = offset / unit_size;
	ret.ptr = g_render.buffer.mapped + offset;

	g_render.stat.align_holes_size += offset - g_render.buffer_free_offset;
	g_render.buffer_free_offset = offset + alloc_size;
	return ret;
}

vk_buffer_alloc_t VK_RenderBufferAlloc( uint32_t unit_size, uint32_t count )
{
	// FIXME this is not correct: on the very first map load it will trigger a lot
	if (g_render.buffer_free_offset >= g_render.temp_buffer_offset)
		gEngine.Con_Printf(S_ERROR "Trying to allocate map-permanent storage in temp buffer context\n");

	return renderBufferAlloc( unit_size, count );
}

void VK_RenderBufferClearAll( void )
{
	g_render.buffer_free_offset = 0;
	g_render.stat.align_holes_size = 0;
}

void VK_RenderBufferPrintStats( void )
{
	gEngine.Con_Reportf("Buffer usage: %uKiB of (%uKiB); holes: %u bytes\n",
		g_render.buffer_free_offset / 1024,
		g_render.buffer.size / 1024,
		g_render.stat.align_holes_size);
}

void VK_RenderTempBufferBegin( void )
{
	g_render.temp_buffer_offset = g_render.buffer_free_offset;
}

vk_buffer_alloc_t VK_RenderTempBufferAlloc( uint32_t unit_size, uint32_t count )
{
	return renderBufferAlloc( unit_size, count );
}

void VK_RenderTempBufferEnd( void )
{
	g_render.buffer_free_offset = g_render.temp_buffer_offset;
}

#define MAX_DRAW_COMMANDS 8192 // TODO estimate
#define MAX_DEBUG_NAME_LENGTH 32

typedef struct {
	render_draw_t draw;
	uint32_t ubo_offset;
	//char debug_name[MAX_DEBUG_NAME_LENGTH];
} draw_command_t;

static struct {
	int uniform_data_set_mask;
	int next_free_uniform_slot;
	uniform_data_t current_uniform_data;
	uniform_data_t dirty_uniform_data;

	draw_command_t draw_commands[MAX_DRAW_COMMANDS];
	int num_draw_commands;
} g_render_state;

enum {
	UNIFORM_UNSET = 0,
	UNIFORM_SET_COLOR = 1,
	UNIFORM_SET_MATRIX = 2,
	UNIFORM_SET_ALL = UNIFORM_SET_COLOR | UNIFORM_SET_MATRIX,
	UNIFORM_UPLOADED = 4,
};

void VK_RenderBegin( void ) {
	g_render_state.next_free_uniform_slot = 0;
	g_render_state.uniform_data_set_mask = UNIFORM_UNSET;

	memset(&g_render_state.current_uniform_data, 0, sizeof(g_render_state.current_uniform_data));
	memset(&g_render_state.dirty_uniform_data, 0, sizeof(g_render_state.dirty_uniform_data));

	g_render_state.num_draw_commands = 0;
}

void VK_RenderStateSetColor( float r, float g, float b, float a )
{
	g_render_state.uniform_data_set_mask |= UNIFORM_SET_COLOR;
	g_render_state.dirty_uniform_data.color[0] = r;
	g_render_state.dirty_uniform_data.color[1] = g;
	g_render_state.dirty_uniform_data.color[2] = b;
	g_render_state.dirty_uniform_data.color[3] = a;
}

void VK_RenderStateSetMatrix( const matrix4x4 mvp )
{
	g_render_state.uniform_data_set_mask |= UNIFORM_SET_MATRIX;
	Matrix4x4_ToArrayFloatGL( mvp, (float*)g_render_state.dirty_uniform_data.mvp );
}

static uniform_data_t *getUniformSlot(int index)
{
	ASSERT(index >= 0);
	ASSERT(index < MAX_UNIFORM_SLOTS);
	return (uniform_data_t*)(((uint8_t*)g_render.uniform_buffer.mapped) + (g_render.uniform_unit_size * index));
}

static int allocUniformSlot( void ) {
	if (g_render_state.next_free_uniform_slot == MAX_UNIFORM_SLOTS)
		return -1;

	return g_render_state.next_free_uniform_slot++;
}

void VK_RenderScheduleDraw( const render_draw_t *draw )
{
	int ubo_index = g_render_state.next_free_uniform_slot - 1;
	draw_command_t *draw_command;

	ASSERT(draw->render_mode >= 0);
	ASSERT(draw->render_mode < ARRAYSIZE(g_render.pipelines));
	ASSERT(draw->lightmap >= 0);
	ASSERT(draw->texture >= 0);

	if ((g_render_state.uniform_data_set_mask & UNIFORM_SET_ALL) != UNIFORM_SET_ALL) {
		gEngine.Con_Printf( S_ERROR "Not all uniform state was initialized prior to rendering\n" );
		return;
	}

	if (g_render_state.num_draw_commands >= ARRAYSIZE(g_render_state.draw_commands)) {
		gEngine.Con_Printf( S_ERROR "Maximum number of draw commands reached\n" );
		return;
	}

	// Figure out whether we need to update UBO data, and upload new data if we do
	// TODO generally it's not safe to do memcmp for structures comparison
	if (((g_render_state.uniform_data_set_mask & UNIFORM_UPLOADED) == 0) || memcmp(&g_render_state.current_uniform_data, &g_render_state.dirty_uniform_data, sizeof(g_render_state.current_uniform_data)) != 0) {
		uniform_data_t *ubo;
		ubo_index = allocUniformSlot();
		if (ubo_index < 0) {
			gEngine.Con_Printf( S_ERROR "Ran out of uniform slots\n" );
			return;
		}

		ubo = getUniformSlot( ubo_index );
		memcpy(&g_render_state.current_uniform_data, &g_render_state.dirty_uniform_data, sizeof(g_render_state.dirty_uniform_data));
		memcpy(ubo, &g_render_state.current_uniform_data, sizeof(*ubo));
		g_render_state.uniform_data_set_mask |= UNIFORM_UPLOADED;
	}

	draw_command = g_render_state.draw_commands + (g_render_state.num_draw_commands++);
	draw_command->draw = *draw;
	draw_command->ubo_offset = g_render.uniform_unit_size * ubo_index;
}

void VK_RenderEnd( VkCommandBuffer cmdbuf )
{
	// TODO we can sort collected draw commands for more efficient and correct rendering
	// that requires adding info about distance to camera for correct order-dependent blending

	int pipeline = -1;
	int texture = -1;
	int lightmap = -1;
	uint32_t ubo_offset = -1;

	{
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmdbuf, 0, 1, &g_render.buffer.buffer, &offset);
		vkCmdBindIndexBuffer(cmdbuf, g_render.buffer.buffer, 0, VK_INDEX_TYPE_UINT16);
	}

	for (int i = 0; i < g_render_state.num_draw_commands; ++i) {
		const draw_command_t *const draw = g_render_state.draw_commands + i;

		if (ubo_offset != draw->ubo_offset)
		{
			ubo_offset = draw->ubo_offset;
			vkCmdBindDescriptorSets(vk_core.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render.pipeline_layout, 0, 1, vk_core.descriptor_pool.ubo_sets, 1., &ubo_offset);
		}

		if (pipeline != draw->draw.render_mode) {
			pipeline = draw->draw.render_mode;
			vkCmdBindPipeline(vk_core.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render.pipelines[pipeline]);
		}

		if (lightmap != draw->draw.lightmap) {
			lightmap = draw->draw.lightmap;
			vkCmdBindDescriptorSets(vk_core.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render.pipeline_layout, 2, 1, &findTexture(lightmap)->vk.descriptor, 0, NULL);
		}

		if (texture != draw->draw.texture)
		{
			texture = draw->draw.texture;
			// TODO names/enums for binding points
			vkCmdBindDescriptorSets(vk_core.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render.pipeline_layout, 1, 1, &findTexture(texture)->vk.descriptor, 0, NULL);
		}

		if (draw->draw.index_offset == UINT32_MAX) {
			/*gEngine.Con_Reportf( "Draw("
				"ubo_index=%d, "
				"lightmap=%d, "
				"texture=%d, "
				"render_mode=%d, "
				"element_count=%u, "
				"index_offset=%u, "
				"vertex_offset=%u)\n",
			draw->ubo_index,
			draw->lightmap,
			draw->texture,
			draw->render_mode,
			draw->element_count,
			draw->index_offset,
			draw->vertex_offset );
			*/

			vkCmdDraw(vk_core.cb, draw->draw.element_count, 1, draw->draw.vertex_offset, 0);
		} else {
			vkCmdDrawIndexed(vk_core.cb, draw->draw.element_count, 1, draw->draw.index_offset, draw->draw.vertex_offset, 0);
		}
	}
}

void VK_RenderDebugLabelBegin( const char *name )
{
	if (vk_core.debug) {
		VkDebugUtilsLabelEXT label = {
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			.pLabelName = name,
		};
		vkCmdBeginDebugUtilsLabelEXT(vk_core.cb, &label);
	}
}

void VK_RenderDebugLabelEnd( void )
{
	if (vk_core.debug)
		vkCmdEndDebugUtilsLabelEXT(vk_core.cb);
}
