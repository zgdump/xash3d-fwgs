# Frame structure wrt calls from the engine
- (eng) SCR_UpdateScreen()
	- (eng) V_PreRender()
		- **(ref) R_BeginFrame()**
	- (eng) V_RenderView()
		- **(ref) GL_BackendStartFrame()** -- ref_gl only sets speeds string to empty here
			- (eng) loop over ref_params_t views
				- **(ref) GL_RenderFrame()**
			- (eng) ??? SV_DrawDebugTriangles()
		- **(ref) GL_BackendEndFrame()** -- ref_gl only produces speeds string here
	- (eng) V_PostRender()
		- **(ref) R_AllowFog(), R_Set2DMode(true)**
		- **(ref) R_DrawTileClear()** x N
		- (vgui) Paint() -> ???
		- (eng) SCR_RSpeeds()
			- **(ref) R_SpeedsMessage()**
			- (eng) CL_DrawString() ...
			  - **(ref) GL_SetRenderMode()**
				- **(ref) RefGetParm()** for texture resolution
				- **(ref) Color4ub()**
				- **(ref) R_DrawStretchPic()**
		- (eng) SRC_DrawNetGraph()
			- **(ref) many TriApi calls** -- 2D usage of triapi. we were not ready for this (maybe track R_Set2DMode()?)
		- **(ref) R_ShowTextures()** kekw
		- **(ref) VID_ScreenShot()**
		- **(ref) R_AllowFog(true)**
		- **(ref) R_EndFrame()**

# Staging and multiple command buffers
We want to get rid of extra command buffers used for staging (and building blases). That would mean tying any command-buffer related things in there to framectl.
However, there are several staging cmdbuf usages which are technically out-of-band wrt framectl:
0. Staging data can get full, which requires sync flush: filling cmdbuf outside of frame (or while still building a frame), submitting it and waiting on it.
1. Texture uploading. There's an explicit usage of staging cmdbuf in vk_texture to do layout transfer. This layout transfer can be moved to staging itself.
2. BLAS building. Creating a ray model uploads its geometry via staging and then immediately builds its BLAS on the same staging cmdbuf. Ideally(?), we'd like to split BLAS building to some later stage to do it in bulk.

# OpenGL-like immediate mode rendering, ~TriApi
## Functions:
	R_Set2DMode(bool) -- switches between 3D scene and 2D overlay modes; used in engine
	R_DrawStretchRaw,
	R_DrawStretchPic,
	R_DrawTileClear,
	CL_FillRGBA,
	CL_FillRGBABlend,

	R_AllowFog,
	GL_SetRenderMode,

	void		(*GL_Bind)( int tmu, unsigned int texnum );
	void		(*GL_SelectTexture)( int tmu );

	void		(*GL_LoadTextureMatrix)( const float *glmatrix ); -- exported to the game, not used in engine
	void		(*GL_TexMatrixIdentity)( void ); -- exported to the game, not used in engine

	void		(*GL_CleanUpTextureUnits)( int last );	// pass 0 for clear all the texture units
	void		(*GL_TexGen)( unsigned int coord, unsigned int mode );
	void		(*GL_TextureTarget)( unsigned int target ); // change texture unit mode without bind texture
	void		(*GL_TexCoordArrayMode)( unsigned int texmode );
	void		(*GL_UpdateTexSize)( int texnum, int width, int height, int depth ); // recalc statistics

	TriRenderMode,
	TriBegin,
	TriEnd,
	TriColor4f,
	TriColor4ub,
	TriTexCoord2f,
	TriVertex3fv,
	TriVertex3f,
	TriFog,
	TriGetMatrix,
	TriFogParams,
	TriCullFace,


# Better BLAS management API

~~
BLAS:
- geom_count => kusok.geom/material.size() == geom_count

Model types:
1. Fully static (brush model w/o animated textures; studio model w/o animations): singleton, fixed geoms and materials, uploaded only once
2. Semi-static (brush model w/ animated textures): singleton, fixed geoms, may update materials, inplace (e.g. animated textures)
3. Dynamic (beams, triapi, etc): singleton, may update both geoms and materials, inplace
4. Template (sprites): used by multiple instances, fixed geom, multiple materials (colors, textures etc) instances/copies
5. Update-from template (studo models): used by multiple dynamic models, deriving from it wvia BLAS UPDATE, dynamic geom+locations, fixed-ish materials.

API ~
1. RT_ModelCreate(geometries_count dynamic?static?) -> rt_model + preallocated mem
2. RT_ModelBuild/Update(geometries[]) -> (blas + kusok.geom[])
3. RT_ModelUpdateMaterials(model, geometries/textures/materials[]); -> (kusok.material[])
4. RT_FrameAddModel(model + kusok.geom[] + kusok.material[] + render_type + xform + color)
~~


rt_instance_t/rt_blas_t:
- VkAS blas
	- VkASGeometry geom[] -> (vertex+index buffer address)
	- VkASBuildRangeInfo ranges[] -> (vtxidx buffer offsets)
	- ~~TODO: updateable: blas[2]? Ping-pong update, cannot do inplace?~~ Nope, can do inplace.
- kusochki
	- kusok[]
		- geometry -> (vtxidx buffer offsets)
			- TODO roughly the same data as VkASBuildRangeInfo, can reuse?
		- material (currently embedded in kusok)
			- static: tex[], scalar[]
			- semi-dynamic:
				- (a few) animated tex_base_color
				- emissive
					- animated with tex_base_color
					- individual per-surface patches
			- TODO: extract as a different modality not congruent with kusok data

Usage cases for the above:
1. (Fully+semi) static.
  - Accept geom[] from above with vtx+idx refernces. Consider them static.
	- Allocate static/fixed blas + kusok data once at map load.
	- Allocate geom+ranges[] temporarily. Fill them with vtx+idx refs.
	- Build BLAS (?: how does this work with lazy/deferred BLAS building wrt geom+ranges allocation)
		- Similar to staging: collect everything + temp data, then commit.
		- Needs BLAS manager, similar to vk_staging
	- Generate Kusok data with current geoms and materials
	- Free geom+ranges
	- Each frame:
		- (semi-static only) Update kusochki materials for animated textures
		- Add blas+kusochki_offset (+dynamic color/xform/mmode) to TLAS
2. Preallocated dynamic (triapi)
  - Preallocate for fixed N geoms:
		- geom+ranges[N].
		- BLAS for N geometries
		- kusochki[N]
	- Each frame:
		- Fill geom+ranges with geom data fed from outside
		- Fill kusochki --//--
		- Fast-Build BLAS as new
		- Add to TLAS
3. Dynamic with update (animated studio models, beams)
	- When a new studio model entity is encountered:
		- Allocate:
			- AT FIXED OFFSET: vtx+idx block
			- geom+ranges[N], BLAS for N, kusochki[N]
	- Each frame:
		- Fill geom+ranges with geom data
		- Fill kusochki --//--
		- First frame: BLAS as new
		- Next frames: UPDATE BLAS in-place (depends on fixed offsets for vtx+idx)
		- Add to TLAS
4. Instanced (sprites, studio models w/o animations).
	- Same as static, BUT potentially dynamic and different materials. I.e. have to have per-instance kusochki copies with slightly different material contents.
	- I.e. each frame
		- If modifying materials (e.g. different texture for sprites):
			- allocate temporary (for this frame only) kusochki block
			- fill geom+material kusochki data
		- Add to TLAS w/ correct kusochki offset.

Exposed ops:
- Create BLAS for N geoms
- Allocate kusochki[N]
	- static (fixed pos)
	- temporary (any location, single frame lifetime)
- Fill kusochki
	- All geoms[]
	- Subset of geoms[] (animated textures for static)
- Build BLAS
	- Allocate geom+ranges[N]
		- Single frame staging-like?
		- Needed only for BLAS BUILD/UPDATE
	- from geoms+ranges[N]
	- build vs update
- Add to TLAS w/ color/xform/mmode/...

- geometry_buffer -- vtx+idx static + multi-frame dynamic + single-frame dynamic
- kusochki_buffer -- kusok[] static + dynamic + clone_dynamic
- accel_buffer -- static + multiframe dynamic + single-frame dynamic
- scratch_buffer - single-frame dynamic
- model_buffer - single-frame dynamic
