#include "vk_materials.h"
#include "vk_textures.h"
#include "vk_mapents.h"
#include "vk_const.h"
#include "profiler.h"
#include "vk_logs.h"

#include <stdio.h>

#define LOG_MODULE LogModule_Material

#define MAX_INCLUDE_DEPTH 4

static r_vk_material_t k_default_material = {
		.tex_base_color = -1,
		.tex_metalness = 0,
		.tex_roughness = 0,
		.tex_normalmap = 0,

		.metalness = 0.f,
		.roughness = 1.f,
		.normal_scale = 1.f,
		.base_color = { 1.f, 1.f, 1.f, 1.f },

		.set = false,
};

#define MAX_RENDERMODE_MATERIALS 32
typedef struct {
		struct {
			int tex_id;
			r_vk_material_t mat;
		} materials[MAX_RENDERMODE_MATERIALS];
		int count;
} r_vk_material_per_mode_t;

static struct {
	r_vk_material_t materials[MAX_TEXTURES];

	r_vk_material_per_mode_t rendermode[kRenderTransAdd+1];
} g_materials;

static struct {
	int mat_files_read;
	int texture_lookups;
	int texture_loads;
	uint64_t material_file_read_duration_ns;
	uint64_t texture_lookup_duration_ns;
	uint64_t texture_load_duration_ns;
} g_stats;

static int loadTexture( const char *filename, qboolean force_reload ) {
	const uint64_t load_begin_ns = aprof_time_now_ns();
	const int tex_id = force_reload ? XVK_LoadTextureReplace( filename, NULL, 0, 0 ) : VK_LoadTexture( filename, NULL, 0, 0 );
	DEBUG("Loaded texture %s => %d", filename, tex_id);
	g_stats.texture_loads++;
	g_stats.texture_load_duration_ns += aprof_time_now_ns() - load_begin_ns;
	return tex_id ? tex_id : -1;
}

static void makePath(char *out, size_t out_size, const char *value, const char *path_begin, const char *path_end) {
	if (value[0] == '/') {
		// Path relative to valve/pbr dir
		Q_snprintf(out, out_size, "pbr%s", value);
	} else {
		// Path relative to current material.mat file
		Q_snprintf(out, out_size, "%.*s%s", (int)(path_end - path_begin), path_begin, value);
	}
}

#define MAKE_PATH(out, value) \
	makePath(out, sizeof(out), value, path_begin, path_end)

static void loadMaterialsFromFile( const char *filename, int depth ) {
	fs_offset_t size;
	const char *const path_begin = filename;
	const char *path_end = Q_strrchr(filename, '/');

	const uint64_t load_file_begin_ns = aprof_time_now_ns();
	byte *data = gEngine.fsapi->LoadFile( filename, 0, false );
	g_stats.material_file_read_duration_ns +=  aprof_time_now_ns() - load_file_begin_ns;

	char *pos = (char*)data;
	r_vk_material_t current_material = k_default_material;
	int current_material_index = -1;
	qboolean force_reload = false;
	qboolean create = false;
	qboolean metalness_set = false;

	string basecolor_map, normal_map, metal_map, roughness_map;

	int rendermode = 0;

	DEBUG("Loading materials from %s (exists=%d)", filename, data != 0);

	if ( !data )
		return;

	if ( !path_end )
		path_end = path_begin;
	else
		path_end++;

	for (;;) {
		char key[1024];
		char value[1024];

		const char *const line_begin = pos;
		pos = COM_ParseFile(pos, key, sizeof(key));
		ASSERT(Q_strlen(key) < sizeof(key));
		if (!pos)
			break;

		if (key[0] == '{') {
			current_material = k_default_material;
			current_material_index = -1;
			force_reload = false;
			create = false;
			metalness_set = false;
			basecolor_map[0] = normal_map[0] = metal_map[0] = roughness_map[0] = '\0';
			rendermode = 0;
			continue;
		}

		if (key[0] == '}') {
			if (current_material_index < 0)
				continue;

#define LOAD_TEXTURE_FOR(name, field) do { \
			if (name[0] != '\0') { \
				char texture_path[256]; \
				MAKE_PATH(texture_path, name); \
				const int tex_id = loadTexture(texture_path, force_reload); \
				if (tex_id < 0) { \
					ERR("Failed to load texture \"%s\" for "#name"", name); \
				} else { \
					current_material.field = tex_id; \
				} \
			}} while(0)

			LOAD_TEXTURE_FOR(basecolor_map, tex_base_color);
			LOAD_TEXTURE_FOR(normal_map, tex_normalmap);
			LOAD_TEXTURE_FOR(metal_map, tex_metalness);
			LOAD_TEXTURE_FOR(roughness_map, tex_roughness);

			// If there's no explicit basecolor_map value, use the "for" target texture
			if (current_material.tex_base_color == -1)
				current_material.tex_base_color = current_material_index;

			if (metalness_set && current_material.tex_metalness == tglob.blackTexture) {
				// Set metalness texture to white to accommodate explicitly set metalness value
				current_material.tex_metalness = tglob.whiteTexture;
			}

			if (!metalness_set && current_material.tex_metalness != tglob.blackTexture) {
				// If metalness factor wasn't set explicitly, but texture was specified, set it to match the texture value.
				current_material.metalness = 1.f;
			}

			DEBUG("Creating%s material for texture %s(%d)", create?" new":"",
				findTexture(current_material_index)->name, current_material_index);

			// Assign rendermode-specific materials
			if (rendermode > 0) {
				r_vk_material_per_mode_t* const rm = g_materials.rendermode + rendermode;
				if (rm->count == COUNTOF(rm->materials)) {
					ERR("Too many rendermode/tex_id mappings");
					continue;
				}

				DEBUG("Adding material %d for rendermode %d", current_material_index, rendermode);

				// TODO proper texid-vs-material-index
				rm->materials[rm->count].tex_id = current_material_index;
				rm->materials[rm->count].mat = current_material;
				rm->materials[rm->count].mat.set = true;
				rm->count++;
			} else {
				DEBUG("Creating%s material for texture %s(%d)", create?" new":"",
					findTexture(current_material_index)->name, current_material_index);

				g_materials.materials[current_material_index] = current_material;
				g_materials.materials[current_material_index].set = true;
			}
			continue;
		}

		pos = COM_ParseFile(pos, value, sizeof(value));
		if (!pos)
			break;

		if (Q_stricmp(key, "for") == 0) {
			const uint64_t lookup_begin_ns = aprof_time_now_ns();
			current_material_index = XVK_FindTextureNamedLike(value);
			g_stats.texture_lookup_duration_ns += aprof_time_now_ns() - lookup_begin_ns;
			g_stats.texture_lookups++;
			create = false;
		} else if (Q_stricmp(key, "new") == 0) {
			current_material_index = XVK_CreateDummyTexture(value);
			create = true;
		} else if (Q_stricmp(key, "force_reload") == 0) {
			force_reload = Q_atoi(value) != 0;
		} else if (Q_stricmp(key, "include") == 0) {
			if (depth > 0) {
				char include_path[256];
				MAKE_PATH(include_path, value);
				loadMaterialsFromFile( include_path, depth - 1);
			} else {
				ERR("material: max include depth %d reached when including '%s' from '%s'", MAX_INCLUDE_DEPTH, value, filename);
			}
		} else {
			int *tex_id_dest = NULL;
			if (Q_stricmp(key, "basecolor_map") == 0) {
				Q_strncpy(basecolor_map, value, sizeof(basecolor_map));
			} else if (Q_stricmp(key, "normal_map") == 0) {
				Q_strncpy(normal_map, value, sizeof(normal_map));
			} else if (Q_stricmp(key, "metal_map") == 0) {
				Q_strncpy(metal_map, value, sizeof(metal_map));
			} else if (Q_stricmp(key, "roughness_map") == 0) {
				Q_strncpy(roughness_map, value, sizeof(roughness_map));
			} else if (Q_stricmp(key, "roughness") == 0) {
				sscanf(value, "%f", &current_material.roughness);
			} else if (Q_stricmp(key, "metalness") == 0) {
				sscanf(value, "%f", &current_material.metalness);
				metalness_set = true;
			} else if (Q_stricmp(key, "normal_scale") == 0) {
				sscanf(value, "%f", &current_material.normal_scale);
			} else if (Q_stricmp(key, "base_color") == 0) {
				sscanf(value, "%f %f %f %f", &current_material.base_color[0], &current_material.base_color[1], &current_material.base_color[2], &current_material.base_color[3]);
			} else if (Q_stricmp(key, "for_rendermode") == 0) {
				rendermode = R_VkRenderModeFromString(value);
				if (rendermode < 0)
					ERR("Invalid rendermode \"%s\"", value);
				ASSERT(rendermode < COUNTOF(g_materials.rendermode[0].materials));
			} else {
				ERR("Unknown material key \"%s\" on line `%.*s`", key, (int)(pos - line_begin), line_begin);
				continue;
			}
		}
	}

	Mem_Free( data );
	g_stats.mat_files_read++;
}

static void loadMaterialsFromFileF( const char *fmt, ... ) {
	char buffer[256];
	va_list argptr;

	va_start( argptr, fmt );
	vsnprintf( buffer, sizeof buffer, fmt, argptr );
	va_end( argptr );

	loadMaterialsFromFile( buffer, MAX_INCLUDE_DEPTH );
}

static int findFilenameExtension(const char *s, int len) {
	if (len < 0)
		len = Q_strlen(s);

	for (int i = len - 1; i >= 0; --i) {
		if (s[i] == '.')
			return i;
	}

	return len;
}

void R_VkMaterialsReload( void ) {
	memset(&g_stats, 0, sizeof(g_stats));
	const uint64_t begin_time_ns = aprof_time_now_ns();

	for (int i = 0; i < COUNTOF(g_materials.rendermode); ++i)
		g_materials.rendermode[i].count = 0;

	k_default_material.tex_metalness = tglob.blackTexture;
	k_default_material.tex_roughness = tglob.whiteTexture;

	for (int i = 0; i < MAX_TEXTURES; ++i) {
		r_vk_material_t *const mat = g_materials.materials + i;
		const vk_texture_t *const tex = findTexture( i );
		*mat = k_default_material;

		if (tex)
			mat->tex_base_color = i;
	}

	loadMaterialsFromFile( "pbr/materials.mat", MAX_INCLUDE_DEPTH );

	{
		for(const char *wad = g_map_entities.wadlist; *wad;) {
			const char *wad_end = wad;
			const char *ext = NULL;
			while (*wad_end && *wad_end != ';') {
				if (*wad_end == '.')
					ext = wad_end;
				++wad_end;
			}

			const int full_length = wad_end - wad;

			// Length without extension
			const int short_length = ext ? ext - wad : full_length;

			loadMaterialsFromFileF("pbr/%.*s/%.*s.mat", full_length, wad, short_length, wad);
			wad = wad_end + 1;
		}
	}

	{
		const model_t *map = gEngine.pfnGetModelByIndex( 1 );
		const char *filename = COM_FileWithoutPath(map->name);
		const int no_ext_len = findFilenameExtension(filename, -1);
		loadMaterialsFromFileF("pbr/%s/%.*s.mat", map->name, no_ext_len, filename);
	}

	// Print out statistics
	{
		const int duration_ms = (aprof_time_now_ns() - begin_time_ns) / 1000000ull;
		INFO("Loading materials took %dms, .mat files parsed: %d (fread: %dms). Texture lookups: %d (%dms). Texture loads: %d (%dms).",
			duration_ms,
			g_stats.mat_files_read,
			(int)(g_stats.material_file_read_duration_ns / 1000000ull),
			g_stats.texture_lookups,
			(int)(g_stats.texture_lookup_duration_ns / 1000000ull),
			g_stats.texture_loads,
			(int)(g_stats.texture_load_duration_ns / 1000000ull)
			);
	}
}

void R_VkMaterialsLoadForModel( const struct model_s* mod ) {
	// Brush models are loaded separately
	if (mod->type == mod_brush)
		return;

	const char *filename = COM_FileWithoutPath(mod->name);
	const int no_ext_len = findFilenameExtension(filename, -1);
	loadMaterialsFromFileF("pbr/%s/%.*s.mat", mod->name, no_ext_len, filename);
}

r_vk_material_t R_VkMaterialGetForTexture( int tex_index ) {
	ASSERT(tex_index >= 0);
	ASSERT(tex_index < MAX_TEXTURES);

	return g_materials.materials[tex_index];
}

r_vk_material_ref_t R_VkMaterialGetForName( const char *name ) {
	// TODO separate material table
	// For now it depends on 1-to-1 mapping between materials and textures
	 return (r_vk_material_ref_t){.index = VK_FindTexture(name)};
}

r_vk_material_t R_VkMaterialGetForRef( r_vk_material_ref_t ref ) {
	// TODO separate material table
	// For now it depends on 1-to-1 mapping between materials and textures
	ASSERT(ref.index >= 0);
	ASSERT(ref.index < MAX_TEXTURES);

	return g_materials.materials[ref.index];
}

qboolean R_VkMaterialGetEx( int tex_id, int rendermode, r_vk_material_t *out_material ) {
	DEBUG("Getting material for tex_id=%d rendermode=%d", tex_id, rendermode);

	if (rendermode == 0) {
		WARN("rendermode==0: fallback to regular tex_id=%d", tex_id);
		*out_material = R_VkMaterialGetForTexture(tex_id);
		return true;
	}

	ASSERT(rendermode < COUNTOF(g_materials.rendermode));
	const r_vk_material_per_mode_t* const mode = &g_materials.rendermode[rendermode];
	for (int i = 0; i < mode->count; ++i) {
		if (mode->materials[i].tex_id == tex_id) {
			*out_material = mode->materials[i].mat;
			return true;
		}
	}

	return false;
}
