/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup modifiers
 */

#include "DNA_customdata_types.h"
#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "BLI_math.h"
#include "BLI_math_inline.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "BKE_library.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_ocean.h"

#include "DEG_depsgraph_query.h"

#include "MOD_modifiertypes.h"

#ifdef WITH_OCEANSIM
static void init_cache_data(Object *ob, struct OceanModifierData *omd)
{
	const char *relbase = modifier_path_relbase_from_global(ob);

	omd->oceancache = BKE_ocean_init_cache(omd->cachepath, relbase,
	                                       omd->bakestart, omd->bakeend, omd->wave_scale,
	                                       omd->chop_amount, omd->foam_coverage, omd->foam_fade, omd->resolution);
}

static void simulate_ocean_modifier(struct OceanModifierData *omd)
{
	BKE_ocean_simulate(omd->ocean, omd->time, omd->wave_scale, omd->chop_amount);
}
#endif /* WITH_OCEANSIM */



/* Modifier Code */

static void initData(ModifierData *md)
{
#ifdef WITH_OCEANSIM
	OceanModifierData *omd = (OceanModifierData *) md;

	omd->resolution = 7;
	omd->spatial_size = 50;

	omd->wave_alignment = 0.0;
	omd->wind_velocity = 30.0;

	omd->damp = 0.5;
	omd->smallest_wave = 0.01;
	omd->wave_direction = 0.0;
	omd->depth = 200.0;

	omd->wave_scale = 1.0;

	omd->chop_amount = 1.0;

	omd->foam_coverage = 0.0;

	omd->seed = 0;
	omd->time = 1.0;

	omd->size = 1.0;
	omd->repeat_x = 1;
	omd->repeat_y = 1;

	modifier_path_init(omd->cachepath, sizeof(omd->cachepath), "cache_ocean");

	omd->cached = 0;
	omd->bakestart = 1;
	omd->bakeend = 250;
	omd->oceancache = NULL;
	omd->foam_fade = 0.98;
	omd->foamlayername[0] = '\0';   /* layer name empty by default */

	omd->ocean = BKE_ocean_add();
	BKE_ocean_init_from_modifier(omd->ocean, omd);
	simulate_ocean_modifier(omd);
#else  /* WITH_OCEANSIM */
	   /* unused */
	(void)md;
#endif /* WITH_OCEANSIM */
}

static void freeData(ModifierData *md)
{
#ifdef WITH_OCEANSIM
	OceanModifierData *omd = (OceanModifierData *) md;

	BKE_ocean_free(omd->ocean);
	if (omd->oceancache)
		BKE_ocean_free_cache(omd->oceancache);
#else /* WITH_OCEANSIM */
	/* unused */
	(void)md;
#endif /* WITH_OCEANSIM */
}

static void copyData(const ModifierData *md, ModifierData *target, const int flag)
{
#ifdef WITH_OCEANSIM
#if 0
	const OceanModifierData *omd = (const OceanModifierData *) md;
#endif
	OceanModifierData *tomd = (OceanModifierData *) target;

	modifier_copyData_generic(md, target, flag);

	/* The oceancache object will be recreated for this copy
	 * automatically when cached=true */
	tomd->oceancache = NULL;

	tomd->ocean = BKE_ocean_add();
	BKE_ocean_init_from_modifier(tomd->ocean, tomd);
	simulate_ocean_modifier(tomd);
#else /* WITH_OCEANSIM */
	/* unused */
	(void)md;
	(void)target;
	(void)flag;
#endif /* WITH_OCEANSIM */
}

#ifdef WITH_OCEANSIM
static CustomDataMask requiredDataMask(Object *UNUSED(ob), ModifierData *md)
{
	OceanModifierData *omd = (OceanModifierData *)md;
	CustomDataMask dataMask = 0;

	if (omd->flag & MOD_OCEAN_GENERATE_FOAM)
		dataMask |= CD_MASK_MCOL;

	return dataMask;
}
#else /* WITH_OCEANSIM */
static CustomDataMask requiredDataMask(Object *UNUSED(ob), ModifierData *md)
{
	/* unused */
	(void)md;
	return 0;
}
#endif /* WITH_OCEANSIM */

static bool dependsOnNormals(ModifierData *md)
{
	OceanModifierData *omd = (OceanModifierData *)md;
	return (omd->geometry_mode != MOD_OCEAN_GEOM_GENERATE);
}

#ifdef WITH_OCEANSIM

typedef struct GenerateOceanGeometryData {
	MVert *mverts;
	MPoly *mpolys;
	MLoop *mloops;
	int *origindex;
	MLoopUV *mloopuvs;

	int res_x, res_y;
	int rx, ry;
	float ox, oy;
	float sx, sy;
	float ix, iy;
} GenerateOceanGeometryData;

static void generate_ocean_geometry_vertices(
        void *__restrict userdata,
        const int y,
        const ParallelRangeTLS *__restrict UNUSED(tls))
{
	GenerateOceanGeometryData *gogd = userdata;
	int x;

	for (x = 0; x <= gogd->res_x; x++) {
		const int i = y * (gogd->res_x + 1) + x;
		float *co = gogd->mverts[i].co;
		co[0] = gogd->ox + (x * gogd->sx);
		co[1] = gogd->oy + (y * gogd->sy);
		co[2] = 0.0f;
	}
}

static void generate_ocean_geometry_polygons(
        void *__restrict userdata,
        const int y,
        const ParallelRangeTLS *__restrict UNUSED(tls))
{
	GenerateOceanGeometryData *gogd = userdata;
	int x;

	for (x = 0; x < gogd->res_x; x++) {
		const int fi = y * gogd->res_x + x;
		const int vi = y * (gogd->res_x + 1) + x;
		MPoly *mp = &gogd->mpolys[fi];
		MLoop *ml = &gogd->mloops[fi * 4];

		ml->v = vi;
		ml++;
		ml->v = vi + 1;
		ml++;
		ml->v = vi + 1 + gogd->res_x + 1;
		ml++;
		ml->v = vi + gogd->res_x + 1;
		ml++;

		mp->loopstart = fi * 4;
		mp->totloop = 4;

		mp->flag |= ME_SMOOTH;

		/* generated geometry does not map to original faces */
		gogd->origindex[fi] = ORIGINDEX_NONE;
	}
}

static void generate_ocean_geometry_uvs(
        void *__restrict userdata,
        const int y,
        const ParallelRangeTLS *__restrict UNUSED(tls))
{
	GenerateOceanGeometryData *gogd = userdata;
	int x;

	for (x = 0; x < gogd->res_x; x++) {
		const int i = y * gogd->res_x + x;
		MLoopUV *luv = &gogd->mloopuvs[i * 4];

		luv->uv[0] = x * gogd->ix;
		luv->uv[1] = y * gogd->iy;
		luv++;

		luv->uv[0] = (x + 1) * gogd->ix;
		luv->uv[1] = y * gogd->iy;
		luv++;

		luv->uv[0] = (x + 1) * gogd->ix;
		luv->uv[1] = (y + 1) * gogd->iy;
		luv++;

		luv->uv[0] = x * gogd->ix;
		luv->uv[1] = (y + 1) * gogd->iy;
		luv++;
	}
}

static Mesh *generate_ocean_geometry(OceanModifierData *omd)
{
	Mesh *result;

	GenerateOceanGeometryData gogd;

	int num_verts;
	int num_polys;

	const bool use_threading = omd->resolution > 4;

	gogd.rx = omd->resolution * omd->resolution;
	gogd.ry = omd->resolution * omd->resolution;
	gogd.res_x = gogd.rx * omd->repeat_x;
	gogd.res_y = gogd.ry * omd->repeat_y;

	num_verts = (gogd.res_x + 1) * (gogd.res_y + 1);
	num_polys = gogd.res_x * gogd.res_y;

	gogd.sx = omd->size * omd->spatial_size;
	gogd.sy = omd->size * omd->spatial_size;
	gogd.ox = -gogd.sx / 2.0f;
	gogd.oy = -gogd.sy / 2.0f;

	gogd.sx /= gogd.rx;
	gogd.sy /= gogd.ry;

	result = BKE_mesh_new_nomain(num_verts, 0, 0, num_polys * 4, num_polys);

	gogd.mverts = result->mvert;
	gogd.mpolys = result->mpoly;
	gogd.mloops = result->mloop;

	gogd.origindex = CustomData_get_layer(&result->pdata, CD_ORIGINDEX);

	ParallelRangeSettings settings;
	BLI_parallel_range_settings_defaults(&settings);
	settings.use_threading = use_threading;

	/* create vertices */
	BLI_task_parallel_range(0, gogd.res_y + 1, &gogd, generate_ocean_geometry_vertices, &settings);

	/* create faces */
	BLI_task_parallel_range(0, gogd.res_y, &gogd, generate_ocean_geometry_polygons, &settings);

	BKE_mesh_calc_edges(result, false, false);

	/* add uvs */
	if (CustomData_number_of_layers(&result->ldata, CD_MLOOPUV) < MAX_MTFACE) {
		gogd.mloopuvs = CustomData_add_layer(&result->ldata, CD_MLOOPUV, CD_CALLOC, NULL, num_polys * 4);

		if (gogd.mloopuvs) { /* unlikely to fail */
			gogd.ix = 1.0 / gogd.rx;
			gogd.iy = 1.0 / gogd.ry;

			BLI_task_parallel_range(0, gogd.res_y, &gogd, generate_ocean_geometry_uvs, &settings);
		}
	}

	result->runtime.cd_dirty_vert |= CD_MASK_NORMAL;

	return result;
}

static Mesh *doOcean(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
	OceanModifierData *omd = (OceanModifierData *) md;
	int cfra_scene = (int)DEG_get_ctime(ctx->depsgraph);
	Object *ob = ctx->object;
	bool allocated_ocean = false;

	Mesh *result = NULL;
	OceanResult ocr;

	MVert *mverts;

	int cfra_for_cache;
	int i, j;

	/* use cached & inverted value for speed
	 * expanded this would read...
	 *
	 * (axis / (omd->size * omd->spatial_size)) + 0.5f) */
#define OCEAN_CO(_size_co_inv, _v) ((_v * _size_co_inv) + 0.5f)

	const float size_co_inv = 1.0f / (omd->size * omd->spatial_size);

	/* can happen in when size is small, avoid bad array lookups later and quit now */
	if (!isfinite(size_co_inv)) {
		return mesh;
	}

	/* do ocean simulation */
	if (omd->cached == true) {
		if (!omd->oceancache) {
			init_cache_data(ob, omd);
		}
		BKE_ocean_simulate_cache(omd->oceancache, cfra_scene);
	}
	else {
		/* omd->ocean is NULL on an original object (in contrast to an evaluated one).
		 * We can create a new one, but we have to free it as well once we're done.
		 * This function is only called on an original object when applying the modifier
		 * using the 'Apply Modifier' button, and thus it is not called frequently for
		 * simulation. */
		allocated_ocean |= BKE_ocean_ensure(omd);
		simulate_ocean_modifier(omd);
	}

	if (omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE) {
		result = generate_ocean_geometry(omd);
		BKE_mesh_ensure_normals(result);
	}
	else if (omd->geometry_mode == MOD_OCEAN_GEOM_DISPLACE) {
		BKE_id_copy_ex(NULL, &mesh->id, (ID **)&result, LIB_ID_COPY_LOCALIZE);
	}

	cfra_for_cache = cfra_scene;
	CLAMP(cfra_for_cache, omd->bakestart, omd->bakeend);
	cfra_for_cache -= omd->bakestart; /* shift to 0 based */

	mverts = result->mvert;

	/* add vcols before displacement - allows lookup based on position */

	if (omd->flag & MOD_OCEAN_GENERATE_FOAM) {
		if (CustomData_number_of_layers(&result->ldata, CD_MLOOPCOL) < MAX_MCOL) {
			const int num_polys = result->totpoly;
			const int num_loops = result->totloop;
			MLoop *mloops = result->mloop;
			MLoopCol *mloopcols = CustomData_add_layer_named(
			                          &result->ldata, CD_MLOOPCOL, CD_CALLOC, NULL, num_loops, omd->foamlayername);

			if (mloopcols) { /* unlikely to fail */
				MPoly *mpolys = result->mpoly;
				MPoly *mp;

				for (i = 0, mp = mpolys; i < num_polys; i++, mp++) {
					MLoop *ml = &mloops[mp->loopstart];
					MLoopCol *mlcol = &mloopcols[mp->loopstart];

					for (j = mp->totloop; j--; ml++, mlcol++) {
						const float *vco = mverts[ml->v].co;
						const float u = OCEAN_CO(size_co_inv, vco[0]);
						const float v = OCEAN_CO(size_co_inv, vco[1]);
						float foam;

						if (omd->oceancache && omd->cached == true) {
							BKE_ocean_cache_eval_uv(omd->oceancache, &ocr, cfra_for_cache, u, v);
							foam = ocr.foam;
							CLAMP(foam, 0.0f, 1.0f);
						}
						else {
							BKE_ocean_eval_uv(omd->ocean, &ocr, u, v);
							foam = BKE_ocean_jminus_to_foam(ocr.Jminus, omd->foam_coverage);
						}

						mlcol->r = mlcol->g = mlcol->b = (char)(foam * 255);
						/* This needs to be set (render engine uses) */
						mlcol->a = 255;
					}
				}
			}
		}
	}


	/* displace the geometry */

	/* Note: tried to parallelized that one and previous foam loop, but gives 20% slower results... odd. */
	{
		const int num_verts = result->totvert;

		for (i = 0; i < num_verts; i++) {
			float *vco = mverts[i].co;
			const float u = OCEAN_CO(size_co_inv, vco[0]);
			const float v = OCEAN_CO(size_co_inv, vco[1]);

			if (omd->oceancache && omd->cached == true) {
				BKE_ocean_cache_eval_uv(omd->oceancache, &ocr, cfra_for_cache, u, v);
			}
			else {
				BKE_ocean_eval_uv(omd->ocean, &ocr, u, v);
			}

			vco[2] += ocr.disp[1];

			if (omd->chop_amount > 0.0f) {
				vco[0] += ocr.disp[0];
				vco[1] += ocr.disp[2];
			}
		}
	}

	if (allocated_ocean) {
		BKE_ocean_free(omd->ocean);
		omd->ocean = NULL;
	}

#undef OCEAN_CO

	return result;
}
#else  /* WITH_OCEANSIM */
static Mesh *doOcean(ModifierData *UNUSED(md), const ModifierEvalContext *UNUSED(ctx), Mesh *mesh)
{
	return mesh;
}
#endif /* WITH_OCEANSIM */

static Mesh *applyModifier(
        ModifierData *md, const ModifierEvalContext *ctx,
        Mesh *mesh)
{
	Mesh *result;

	result = doOcean(md, ctx, mesh);

	if (result != mesh)
		result->runtime.cd_dirty_vert |= CD_MASK_NORMAL;

	return result;
}


ModifierTypeInfo modifierType_Ocean = {
	/* name */              "Ocean",
	/* structName */        "OceanModifierData",
	/* structSize */        sizeof(OceanModifierData),
	/* type */              eModifierTypeType_Constructive,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_SupportsEditmode |
	                        eModifierTypeFlag_EnableInEditmode,

	/* copyData */          copyData,
	/* deformMatrices_DM */ NULL,

	/* deformVerts_DM */    NULL,
	/* deformVertsEM_DM */  NULL,
	/* deformMatricesEM_DM*/NULL,
	/* applyModifier_DM */  NULL,

	/* deformVerts */       NULL,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     NULL,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     applyModifier,

	/* initData */          initData,
	/* requiredDataMask */  requiredDataMask,
	/* freeData */          freeData,
	/* isDisabled */        NULL,
	/* updateDepsgraph */   NULL,
	/* dependsOnTime */     NULL,
	/* dependsOnNormals */	dependsOnNormals,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
};
