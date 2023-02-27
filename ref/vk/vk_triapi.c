#include "vk_triapi.h"
#include "vk_geometry.h"
#include "vk_render.h"

#include "vk_textures.h" // FIXME temp

#include "xash3d_mathlib.h"

#define MAX_TRIAPI_VERTICES 1024
#define MAX_TRIAPI_INDICES 1024

static struct {
	vk_vertex_t vertices[MAX_TRIAPI_VERTICES];
	uint16_t indices[MAX_TRIAPI_INDICES];

	int num_vertices;
	int mode;
} g_triapi = {0};

void TriBegin( int mode ) {
	ASSERT(!g_triapi.mode);

	switch(mode) {
		case TRI_TRIANGLES: break;
		case TRI_TRIANGLE_STRIP: break;
		default:
			gEngine.Con_Printf(S_ERROR "TriBegin: unsupported mode %d\n", mode);
			return;
	}

	g_triapi.mode = mode + 1;
	g_triapi.num_vertices = 0;

	vk_vertex_t *const ve = g_triapi.vertices + 0;
	memset(ve, 0, sizeof *ve);
	Vector4Set(ve->color, 255, 255, 255, 255);
}

/* static int genTrianglesIndices(void) { */
/* 	return 0; */
/* } */

static int genTriangleStripIndices(void) {
	int num_indices = 0;
	uint16_t *const dst_idx = g_triapi.indices;
	for (int i = 2; i < g_triapi.num_vertices; ++i) {
		if( i & 1 )
		{
			// draw triangle [n-1 n-2 n]
			dst_idx[num_indices++] = i - 1;
			dst_idx[num_indices++] = i - 2;
			dst_idx[num_indices++] = i;
		}
		else
		{
			// draw triangle [n-2 n-1 n]
			dst_idx[num_indices++] = i - 2;
			dst_idx[num_indices++] = i - 1;
			dst_idx[num_indices++] = i;
		}
	}
	return num_indices;
}

static void emitDynamicGeometry(int num_indices) {
	if (!num_indices)
		return;

	r_geometry_buffer_lock_t buffer;
	if (!R_GeometryBufferAllocAndLock( &buffer, g_triapi.num_vertices, num_indices, LifetimeSingleFrame )) {
		gEngine.Con_Printf(S_ERROR "Cannot allocate geometry for tri api\n");
		return;
	}

	memcpy(buffer.vertices.ptr, g_triapi.vertices, sizeof(vk_vertex_t) * g_triapi.num_vertices);
	memcpy(buffer.indices.ptr, g_triapi.indices, sizeof(uint16_t) * num_indices);

	R_GeometryBufferUnlock( &buffer );

	{
	// FIXME pass these properly
		const int texture = tglob.whiteTexture;
		const vec4_t color = {1, 1, 1, 1};
		const vk_render_type_e render_type = kVkRenderType_A_1_R;
		const char* name = "FIXME triapi";

		const vk_render_geometry_t geometry = {
			.texture = texture,
			.material = kXVkMaterialEmissive,

			.max_vertex = g_triapi.num_vertices,
			.vertex_offset = buffer.vertices.unit_offset,

			.element_count = num_indices,
			.index_offset = buffer.indices.unit_offset,

			.emissive = { color[0], color[1], color[2] },
		};

		VK_RenderModelDynamicBegin( render_type, color, name );
		VK_RenderModelDynamicAddGeometry( &geometry );
		VK_RenderModelDynamicCommit();
	}
}

void TriEnd( void ) {
	if (!g_triapi.mode)
		return;

	if (!g_triapi.num_vertices)
		return;

	int num_indices = 0;
	switch(g_triapi.mode - 1) {
		/* case TRI_TRIANGLES: */
		/* 	num_indices = genTrianglesIndices(); */
		/* 	break; */
		case TRI_TRIANGLE_STRIP:
			num_indices = genTriangleStripIndices();
			break;
		default:
			gEngine.Con_Printf(S_ERROR "TriEnd: unsupported mode %d\n", g_triapi.mode - 1);
			break;
	}

	emitDynamicGeometry(num_indices);

	g_triapi.num_vertices = 0;
	g_triapi.mode = 0;
}

void TriTexCoord2f( float u, float v ) {
	vk_vertex_t *const ve = g_triapi.vertices + g_triapi.num_vertices;
	Vector2Set(ve->gl_tc, u, v);
}

void TriVertex3fv( const float *v ) {
	TriVertex3f(v[0], v[1], v[2]);
}

void TriVertex3f( float x, float y, float z ) {
	if (g_triapi.num_vertices == MAX_TRIAPI_VERTICES - 1) {
		gEngine.Con_Printf(S_ERROR "vk TriApi: trying to emit more than %d vertices in one batch\n", MAX_TRIAPI_VERTICES);
		return;
	}

	vk_vertex_t *const ve = g_triapi.vertices + g_triapi.num_vertices;
	VectorSet(ve->pos, x, y, z);

	// Emit vertex preserving previous vertex values
	++g_triapi.num_vertices;
	g_triapi.vertices[g_triapi.num_vertices] = g_triapi.vertices[g_triapi.num_vertices-1];
}

static int clampi32(int v, int min, int max) {
	if (v < min) return min;
	if (v > max) return max;
	return v;
}

void TriColor4ub_( byte r, byte g, byte b, byte a ) {
	Vector4Set(g_triapi.vertices[g_triapi.num_vertices].color, r, g, b, a);
}

void TriColor4f( float r, float g, float b, float a ) {
	TriColor4ub_(clampi32(r*255.f, 0, 255),clampi32(g*255.f, 0, 255),clampi32(b*255.f, 0, 255),clampi32(a*255.f, 0, 255));
}

