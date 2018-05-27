#include "glcache.h"
#include "rend/rend.h"

#include <algorithm>
/*

Drawing and related state management
Takes vertex, textures and renders to the currently set up target




*/

const static u32 CullMode[]= 
{

	GL_NONE, //0    No culling          No culling
	GL_NONE, //1    Cull if Small       Cull if ( |det| < fpu_cull_val )

	GL_FRONT, //2   Cull if Negative    Cull if ( |det| < 0 ) or ( |det| < fpu_cull_val )
	GL_BACK,  //3   Cull if Positive    Cull if ( |det| > 0 ) or ( |det| < fpu_cull_val )
};
#define INVERT_DEPTH_FUNC
const static u32 Zfunction[]=
{
	GL_NEVER,      //GL_NEVER,              //0 Never
#ifndef INVERT_DEPTH_FUNC
	GL_LESS,        //GL_LESS/*EQUAL*/,     //1 Less
	GL_EQUAL,       //GL_EQUAL,             //2 Equal
	GL_LEQUAL,      //GL_LEQUAL,            //3 Less Or Equal
	GL_GREATER,     //GL_GREATER/*EQUAL*/,  //4 Greater
	GL_NOTEQUAL,    //GL_NOTEQUAL,          //5 Not Equal
	GL_GEQUAL,      //GL_GEQUAL,            //6 Greater Or Equal
#else
	GL_GREATER,        //GL_LESS/*EQUAL*/,     //1 Less
	GL_EQUAL,       //GL_EQUAL,             //2 Equal
	GL_GEQUAL,      //GL_LEQUAL,            //3 Less Or Equal
	GL_LESS,     //GL_GREATER/*EQUAL*/,  //4 Greater
	GL_NOTEQUAL,    //GL_NOTEQUAL,          //5 Not Equal
	GL_LEQUAL,      //GL_GEQUAL,            //6 Greater Or Equal
#endif
	GL_ALWAYS,      //GL_ALWAYS,            //7 Always
};

/*
0   Zero                  (0, 0, 0, 0)
1   One                   (1, 1, 1, 1)
2   Dither Color          (OR, OG, OB, OA) 
3   Inverse Dither Color  (1-OR, 1-OG, 1-OB, 1-OA)
4   SRC Alpha             (SA, SA, SA, SA)
5   Inverse SRC Alpha     (1-SA, 1-SA, 1-SA, 1-SA)
6   DST Alpha             (DA, DA, DA, DA)
7   Inverse DST Alpha     (1-DA, 1-DA, 1-DA, 1-DA)
*/

const static u32 DstBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

const static u32 SrcBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

PipelineShader* CurrentShader;
u32 gcflip;
GLuint geom_fbo;
GLuint stencilTexId;
//GLuint opaqueTexId;
GLuint depthTexId;

s32 SetTileClip(u32 val, bool set)
{
	u32 clipmode=val>>28;
	s32 clip_mode;
	if (clipmode<2)
	{
		clip_mode=0;    //always passes
	}
	else if (clipmode&1)
		clip_mode=-1;   //render stuff outside the region
	else
		clip_mode=1;    //render stuff inside the region

	float csx=0,csy=0,cex=0,cey=0;


	csx=(float)(val&63);
	cex=(float)((val>>6)&63);
	csy=(float)((val>>12)&31);
	cey=(float)((val>>17)&31);
	csx=csx*32;
	cex=cex*32 +32;
	csy=csy*32;
	cey=cey*32 +32;

	if (csx <= 0 && csy <= 0 && cex >= 640 && cey >= 480)
		return 0;
	
	if (set && clip_mode) {
		csx *= scale_x;
		csy *= scale_y;
		cex *= scale_x;
		cey *= scale_y;
		if (!pvrrc.isRTT) {
			float t = cey;
			cey = 480 - csy;
			csy = 480 - t;
			float dc2s_scale_h = screen_height / 480.0f;
			float ds2s_offs_x = (screen_width - dc2s_scale_h * 640) / 2;
			csx = csx * dc2s_scale_h + ds2s_offs_x;
			cex = cex * dc2s_scale_h + ds2s_offs_x;
			csy = csy * dc2s_scale_h;
			cey = cey * dc2s_scale_h;
		}
		glUniform4f(CurrentShader->pp_ClipTest, csx, csy, cex, cey);
	}

	return clip_mode;
}

void SetCull(u32 CulliMode)
{
	if (CullMode[CulliMode]==GL_NONE)
	{ 
		glcache.Disable(GL_CULL_FACE);
	}
	else
	{
		glcache.Enable(GL_CULL_FACE);
		glcache.CullFace(CullMode[CulliMode]); //GL_FRONT/GL_BACK, ...
	}
}

static void SetTextureRepeatMode(GLuint dir, u32 clamp, u32 mirror)
{
	if (clamp)
		glcache.TexParameteri(GL_TEXTURE_2D, dir, GL_CLAMP_TO_EDGE);
	else
		glcache.TexParameteri(GL_TEXTURE_2D, dir, mirror ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

template <u32 Type, bool SortingEnabled>
	void SetGPState(const PolyParam* gp, bool weighted_average = false, u32 front_peeling = 0, bool geometry_only = false, u32 cflip=0)
{
	s32 clipping = SetTileClip(gp->tileclip, false);
	int shaderId;
	if (geometry_only)
	{
		shaderId = GetProgramID(Type == ListType_Punch_Through ? 1 : 0,
				clipping + 1,
				Type == ListType_Punch_Through ? gp->pcw.Texture : 0,
				0,
				gp->tsp.IgnoreTexA,
				0,
				0,
				2,
				false,
				1);		// FIXME Hack: using front peeling to avoid writing to 3D array
		CurrentShader = gl.getShader(shaderId);
		if (CurrentShader->program == -1) {
			CurrentShader->cp_AlphaTest = Type == ListType_Punch_Through ? 1 : 0;
			CurrentShader->pp_ClipTestMode = clipping;
			CurrentShader->pp_Texture = Type == ListType_Punch_Through ? gp->pcw.Texture : 0;
			CurrentShader->pp_UseAlpha = 0;
			CurrentShader->pp_IgnoreTexA = gp->tsp.IgnoreTexA;
			CurrentShader->pp_ShadInstr = 0;
			CurrentShader->pp_Offset = 0;
			CurrentShader->pp_FogCtrl = 2;
			CurrentShader->pp_WeightedAverage = false;
			CurrentShader->pp_FrontPeeling = 1;
			CompilePipelineShader(CurrentShader);
		}
	}
	else
	{
		shaderId = GetProgramID(Type == ListType_Punch_Through ? 1 : 0,
											 	  clipping + 1,
												  gp->pcw.Texture,
												  gp->tsp.UseAlpha,
												  gp->tsp.IgnoreTexA,
												  gp->tsp.ShadInstr,
												  gp->pcw.Offset,
												  gp->tsp.FogCtrl,
												  weighted_average,
												  front_peeling);
		CurrentShader = gl.getShader(shaderId);
		if (CurrentShader->program == -1) {
			CurrentShader->cp_AlphaTest = Type == ListType_Punch_Through ? 1 : 0;
			CurrentShader->pp_ClipTestMode = clipping;
			CurrentShader->pp_Texture = gp->pcw.Texture;
			CurrentShader->pp_UseAlpha = gp->tsp.UseAlpha;
			CurrentShader->pp_IgnoreTexA = gp->tsp.IgnoreTexA;
			CurrentShader->pp_ShadInstr = gp->tsp.ShadInstr;
			CurrentShader->pp_Offset = gp->pcw.Offset;
			CurrentShader->pp_FogCtrl = gp->tsp.FogCtrl;
			CurrentShader->pp_WeightedAverage = weighted_average;
			CurrentShader->pp_FrontPeeling = front_peeling;
			CompilePipelineShader(CurrentShader);
		}
	}
	
	glcache.UseProgram(CurrentShader->program);
	if (Type == ListType_Opaque || Type == ListType_Punch_Through)	// TODO Can PT have a non-zero and non-one alpha?
	{
		ShaderUniforms.blend_mode[0] = 1;
		ShaderUniforms.blend_mode[1] = 0;
	}
	else
	{
		ShaderUniforms.blend_mode[0] = gp->tsp.SrcInstr;
		ShaderUniforms.blend_mode[1] = gp->tsp.DstInstr;
	}
	ShaderUniforms.Set(CurrentShader);

	SetTileClip(gp->tileclip,true);

	//This bit control which pixels are affected
	//by modvols
	const u32 stencil=(gp->pcw.Shadow!=0)?0x80:0x0;

	glcache.StencilFunc(GL_ALWAYS,stencil,stencil);

	glcache.BindTexture(GL_TEXTURE_2D, gp->texid == -1 ? 0 : gp->texid);

	if (gp->texid > 0)
	{
		SetTextureRepeatMode(GL_TEXTURE_WRAP_S, gp->tsp.ClampU, gp->tsp.FlipU);
		SetTextureRepeatMode(GL_TEXTURE_WRAP_T, gp->tsp.ClampV, gp->tsp.FlipV);

		//set texture filter mode
		if (gp->tsp.FilterMode == 0)
		{
			//disable filtering, mipmaps
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
		else
		{
			//bilinear filtering
			//PowerVR supports also trilinear via two passes, but we ignore that for now
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (gp->tcw.MipMapped && settings.rend.UseMipmaps) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
	}

	if (Type==ListType_Translucent)
	{
//		glcache.Enable(GL_BLEND);
//		glcache.BlendFunc(SrcBlendGL[gp->tsp.SrcInstr],DstBlendGL[gp->tsp.DstInstr]);
	}
	else
		glcache.Disable(GL_BLEND);

	//set cull mode !
	//cflip is required when exploding triangles for triangle sorting
	//gcflip is global clip flip, needed for when rendering to texture due to mirrored Y direction
	SetCull(gp->isp.CullMode^cflip^gcflip);

	//set Z mode, only if required
	if (Type == ListType_Punch_Through || (Type == ListType_Translucent && SortingEnabled))
	{
		if (gp->isp.DepthMode == 7) {		// Fixes VR2 menu but not sure about this one
			glcache.DepthFunc(GL_ALWAYS);
		}
		else
		{
			glcache.DepthFunc(Zfunction[6]);	// Greater or equal
//			glcache.DepthFunc(GL_LESS);
		}
	}
	else
	{
		glcache.DepthFunc(Zfunction[gp->isp.DepthMode]);
	}

//#if TRIG_SORT
	if (Type == ListType_Translucent)
		glcache.DepthMask(GL_FALSE);
	else
//#endif
		if (!weighted_average)
			glcache.DepthMask(!gp->isp.ZWriteDis);
}

template <u32 Type, bool SortingEnabled>
void DrawList(const List<PolyParam>& gply, int first, int count, bool weighted_average = false, u32 front_peeling = 0,
		int srcBlendModeFilter = -1, int dstBlendModeFilter = -1, bool geometry_only = false)
{
	PolyParam* params = &gply.head()[first];


	if (count==0)
		return;
	//we want at least 1 PParam


	//set some 'global' modes for all primitives

	glcache.Enable(GL_STENCIL_TEST);
	glcache.StencilFunc(GL_ALWAYS,0,0);
	glcache.StencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);

	while(count-->0)
	{
		if (params->count>2) //this actually happens for some games. No idea why ..
		{
			if (Type == ListType_Translucent) {
				if ((params->tsp.SrcInstr == 0 && params->tsp.DstInstr == 1)						// Nothing to do
					|| (srcBlendModeFilter != -1 && params->tsp.SrcInstr != srcBlendModeFilter)		// src filter doesn't match
					|| (dstBlendModeFilter != -1 && params->tsp.DstInstr != dstBlendModeFilter)) {	// dst filter doesn't match
					params++;
					continue;
				}
			}

			SetGPState<Type,SortingEnabled>(params, weighted_average, front_peeling, geometry_only);
			glDrawElements(GL_TRIANGLE_STRIP, params->count, GL_UNSIGNED_SHORT, (GLvoid*)(2*params->first)); glCheck();
		}

		params++;
	}
}

void DrawListTranslucentAutoSorted(const List<PolyParam>& gply, int first, int count, bool weighted_average = false, u32 front_peeling = 0,
		int srcBlendModeFilter = -1, int dstBlendModeFilter = -1)
{
	DrawList<ListType_Translucent, true>(gply, first, count, weighted_average, front_peeling, srcBlendModeFilter, dstBlendModeFilter);
}

void DrawListOpaque(const List<PolyParam>& gply, int first, int count, bool weighted_average = false, u32 front_peeling = 0)
{
	DrawList<ListType_Opaque, false>(gply, first, count, weighted_average, front_peeling);
}

void DrawListPunchThrough(const List<PolyParam>& gply, int first, int count, bool weighted_average = false, u32 front_peeling = 0)
{
	DrawList<ListType_Punch_Through, false>(gply, first, count, weighted_average, front_peeling);
}

template <u32 Type>
void DrawListGeometry(const List<PolyParam>& gply, int first, int count)
{
	DrawList<Type, false>(gply, first, count, false, 0, -1, -1, true);
}

bool operator<(const PolyParam &left, const PolyParam &right)
{
/* put any condition you want to sort on here */
	return left.zvZ<right.zvZ;
	//return left.zMin<right.zMax;
}

//Sort based on min-z of each strip
void SortPParams(int first, int count)
{
	if (pvrrc.verts.used() == 0 || count <= 1)
		return;

	Vertex* vtx_base=pvrrc.verts.head();
	u16* idx_base=pvrrc.idx.head();

	PolyParam* pp = &pvrrc.global_param_tr.head()[first];
	PolyParam* pp_end = pp + count;

	while(pp!=pp_end)
	{
		if (pp->count<2)
		{
			pp->zvZ=0;
		}
		else
		{
			u16* idx=idx_base+pp->first;

			Vertex* vtx=vtx_base+idx[0];
			Vertex* vtx_end=vtx_base + idx[pp->count-1]+1;

			u32 zv=0xFFFFFFFF;
			while(vtx!=vtx_end)
			{
				zv=min(zv,(u32&)vtx->z);
				vtx++;
			}

			pp->zvZ=(f32&)zv;
		}
		pp++;
	}

	std::stable_sort(pvrrc.global_param_tr.head() + first, pvrrc.global_param_tr.head() + first + count);
}

Vertex* vtx_sort_base;


struct IndexTrig
{
	u16 id[3];
	u16 pid;
	f32 z;
};


struct SortTrigDrawParam
{
	PolyParam* ppid;
	u16 first;
	u16 count;
};

float min3(float v0,float v1,float v2)
{
	return min(min(v0,v1),v2);
}

float max3(float v0,float v1,float v2)
{
	return max(max(v0,v1),v2);
}


float minZ(Vertex* v,u16* mod)
{
	return min(min(v[mod[0]].z,v[mod[1]].z),v[mod[2]].z);
}

bool operator<(const IndexTrig &left, const IndexTrig &right)
{
	return left.z<right.z;
}


#if 0
/*

	Per triangle sorting experiments

*/

//approximate the triangle area
float area_x2(Vertex* v)
{
	return 2/3*fabs( (v[0].x-v[2].x)*(v[1].y-v[0].y) - (v[0].x-v[1].x)*(v[2].y-v[0].y)) ;
}

//approximate the distance ^2
float distance_apprx(Vertex* a, Vertex* b)
{
	float xd=a->x-b->x;
	float yd=a->y-b->y;

	return xd*xd+yd*yd;
}

//was good idea, but not really working ..
bool Intersect(Vertex* a, Vertex* b)
{
	float a1=area_x2(a);
	float a2=area_x2(b);

	float d = distance_apprx(a,b);

	return (a1+a1)>d;
}

//root for quick-union
u16 rid(vector<u16>& v, u16 id)
{
	while(id!=v[id]) id=v[id];
	return id;
}

struct TrigBounds
{
	float xs,xe;
	float ys,ye;
	float zs,ze;
};

//find 3d bounding box for triangle
TrigBounds bound(Vertex* v)
{
	TrigBounds rv = {	min(min(v[0].x,v[1].x),v[2].x), max(max(v[0].x,v[1].x),v[2].x),
						min(min(v[0].y,v[1].y),v[2].y), max(max(v[0].y,v[1].y),v[2].y),
						min(min(v[0].z,v[1].z),v[2].z), max(max(v[0].z,v[1].z),v[2].z),
					};

	return rv;
}

//bounding box 2d intersection
bool Intersect(TrigBounds& a, TrigBounds& b)
{
	return  ( !(a.xe<b.xs || a.xs>b.xe) && !(a.ye<b.ys || a.ys>b.ye) /*&& !(a.ze<b.zs || a.zs>b.ze)*/ );
}


bool operator<(const IndexTrig &left, const IndexTrig &right)
{
	/*
	TrigBounds l=bound(vtx_sort_base+left.id);
	TrigBounds r=bound(vtx_sort_base+right.id);

	if (!Intersect(l,r))
	{
		return true;
	}
	else
	{
		return (l.zs + l.ze) < (r.zs + r.ze);
	}*/

	return minZ(&vtx_sort_base[left.id])<minZ(&vtx_sort_base[right.id]);
}

//Not really working cuz of broken intersect
bool Intersect(const IndexTrig &left, const IndexTrig &right)
{
	TrigBounds l=bound(vtx_sort_base+left.id);
	TrigBounds r=bound(vtx_sort_base+right.id);

	return Intersect(l,r);
}

#endif

//are two poly params the same?
bool PP_EQ(PolyParam* pp0, PolyParam* pp1)
{
	return (pp0->pcw.full&PCW_DRAW_MASK)==(pp1->pcw.full&PCW_DRAW_MASK) && pp0->isp.full==pp1->isp.full && pp0->tcw.full==pp1->tcw.full && pp0->tsp.full==pp1->tsp.full && pp0->tileclip==pp1->tileclip;
}

static vector<SortTrigDrawParam>	pidx_sort;

void fill_id(u16* d, Vertex* v0, Vertex* v1, Vertex* v2,  Vertex* vb)
{
	d[0]=v0-vb;
	d[1]=v1-vb;
	d[2]=v2-vb;
}

void GenSorted(int first, int count)
{
	u32 tess_gen=0;

	pidx_sort.clear();

	if (pvrrc.verts.used() == 0 || count <= 1)
		return;

	Vertex* vtx_base=pvrrc.verts.head();
	u16* idx_base=pvrrc.idx.head();

	PolyParam* pp_base = &pvrrc.global_param_tr.head()[first];
	PolyParam* pp = pp_base;
	PolyParam* pp_end = pp + count;
	
	Vertex* vtx_arr=vtx_base+idx_base[pp->first];
	vtx_sort_base=vtx_base;

	static u32 vtx_cnt;

	int vtx_count=idx_base[pp_end[-1].first+pp_end[-1].count-1]-idx_base[pp->first];
	if (vtx_count>vtx_cnt)
		vtx_cnt=vtx_count;

#if PRINT_SORT_STATS
	printf("TVTX: %d || %d\n",vtx_cnt,vtx_count);
#endif
	
	if (vtx_count<=0)
		return;

	//make lists of all triangles, with their pid and vid
	static vector<IndexTrig> lst;
	
	lst.resize(vtx_count*4);
	

	int pfsti=0;

	while(pp!=pp_end)
	{
		u32 ppid=(pp-pp_base);

		if (pp->count>2)
		{
			u16* idx=idx_base+pp->first;

			Vertex* vtx=vtx_base+idx[0];
			Vertex* vtx_end=vtx_base + idx[pp->count-1]-1;
			u32 flip=0;
			while(vtx!=vtx_end)
			{
				Vertex* v0, * v1, * v2, * v3, * v4, * v5;

				if (flip)
				{
					v0=&vtx[2];
					v1=&vtx[1];
					v2=&vtx[0];
				}
				else
				{
					v0=&vtx[0];
					v1=&vtx[1];
					v2=&vtx[2];
				}
#if 0
				if (settings.pvr.subdivide_transp)
				{
					u32 tess_x=(max3(v0->x,v1->x,v2->x)-min3(v0->x,v1->x,v2->x))/32;
					u32 tess_y=(max3(v0->y,v1->y,v2->y)-min3(v0->y,v1->y,v2->y))/32;

					if (tess_x==1) tess_x=0;
					if (tess_y==1) tess_y=0;

					//bool tess=(maxZ(v0,v1,v2)/minZ(v0,v1,v2))>=1.2;

					if (tess_x + tess_y)
					{
						v3=pvrrc.verts.Append(3);
						v4=v3+1;
						v5=v4+1;

						//xyz
						for (int i=0;i<3;i++)
						{
							((float*)&v3->x)[i]=((float*)&v0->x)[i]*0.5f+((float*)&v2->x)[i]*0.5f;
							((float*)&v4->x)[i]=((float*)&v0->x)[i]*0.5f+((float*)&v1->x)[i]*0.5f;
							((float*)&v5->x)[i]=((float*)&v1->x)[i]*0.5f+((float*)&v2->x)[i]*0.5f;
						}

						//*TODO* Make it perspective correct

						//uv
						for (int i=0;i<2;i++)
						{
							((float*)&v3->u)[i]=((float*)&v0->u)[i]*0.5f+((float*)&v2->u)[i]*0.5f;
							((float*)&v4->u)[i]=((float*)&v0->u)[i]*0.5f+((float*)&v1->u)[i]*0.5f;
							((float*)&v5->u)[i]=((float*)&v1->u)[i]*0.5f+((float*)&v2->u)[i]*0.5f;
						}

						//color
						for (int i=0;i<4;i++)
						{
							v3->col[i]=v0->col[i]/2+v2->col[i]/2;
							v4->col[i]=v0->col[i]/2+v1->col[i]/2;
							v5->col[i]=v1->col[i]/2+v2->col[i]/2;
						}

						fill_id(lst[pfsti].id,v0,v3,v4,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						fill_id(lst[pfsti].id,v2,v3,v5,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						fill_id(lst[pfsti].id,v3,v4,v5,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						fill_id(lst[pfsti].id,v5,v4,v1,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						tess_gen+=3;
					}
					else
					{
						fill_id(lst[pfsti].id,v0,v1,v2,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;
					}
				}
				else
#endif
				{
					fill_id(lst[pfsti].id,v0,v1,v2,vtx_base);
					lst[pfsti].pid= ppid ;
					lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
					pfsti++;
				}

				flip ^= 1;
				
				vtx++;
			}
		}
		pp++;
	}

	u32 aused=pfsti;

	lst.resize(aused);

	//sort them
#if 1
	std::stable_sort(lst.begin(),lst.end());

	//Merge pids/draw cmds if two different pids are actually equal
	if (true)
	{
		for (u32 k=1;k<aused;k++)
		{
			if (lst[k].pid!=lst[k-1].pid)
			{
				if (PP_EQ(&pp_base[lst[k].pid],&pp_base[lst[k-1].pid]))
				{
					lst[k].pid=lst[k-1].pid;
				}
			}
		}
	}
#endif

	
#if 0
	//tries to optimise draw calls by reordering non-intersecting polygons
	//uber slow and not very effective
	{
		int opid=lst[0].pid;

		for (int k=1;k<aused;k++)
		{
			if (lst[k].pid!=opid)
			{
				if (opid>lst[k].pid)
				{
					//MOVE UP
					for (int j=k;j>0 && lst[j].pid!=lst[j-1].pid && !Intersect(lst[j],lst[j-1]);j--)
					{
						swap(lst[j],lst[j-1]);
					}
				}
				else
				{
					//move down
					for (int j=k+1;j<aused && lst[j].pid!=lst[j-1].pid && !Intersect(lst[j],lst[j-1]);j++)
					{
						swap(lst[j],lst[j-1]);
					}
				}
			}

			opid=lst[k].pid;
		}
	}
#endif

	//re-assemble them into drawing commands
	static vector<u16> vidx_sort;

	vidx_sort.resize(aused*3);

	int idx=-1;

	for (u32 i=0; i<aused; i++)
	{
		int pid=lst[i].pid;
		u16* midx=lst[i].id;

		vidx_sort[i*3 + 0]=midx[0];
		vidx_sort[i*3 + 1]=midx[1];
		vidx_sort[i*3 + 2]=midx[2];

		if (idx!=pid /* && !PP_EQ(&pp_base[pid],&pp_base[idx]) */ )
		{
			SortTrigDrawParam stdp={pp_base + pid, (u16)(i*3), 0};
			
			if (idx!=-1)
			{
				SortTrigDrawParam* last=&pidx_sort[pidx_sort.size()-1];
				last->count=stdp.first-last->first;
			}

			pidx_sort.push_back(stdp);
			idx=pid;
		}
	}

	SortTrigDrawParam* stdp=&pidx_sort[pidx_sort.size()-1];
	stdp->count=aused*3-stdp->first;

#if PRINT_SORT_STATS
	printf("Reassembled into %d from %d\n",pidx_sort.size(),pp_end-pp_base);
#endif

	//Upload to GPU if needed
	if (pidx_sort.size())
	{
		//Bind and upload sorted index buffer
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs2); glCheck();
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,vidx_sort.size()*2,&vidx_sort[0],GL_STREAM_DRAW);

		if (tess_gen) printf("Generated %.2fK Triangles !\n",tess_gen/1000.0);
	}
}

void DrawSorted(bool multipass)
{
	//if any drawing commands, draw them
	if (pidx_sort.size())
	{
		u32 count=pidx_sort.size();
		
		{
			//set some 'global' modes for all primitives

			glcache.Enable(GL_STENCIL_TEST);
			glcache.StencilFunc(GL_ALWAYS,0,0);
			glcache.StencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);

			for (u32 p=0; p<count; p++)
			{
				PolyParam* params = pidx_sort[p].ppid;
				if (pidx_sort[p].count>2) //this actually happens for some games. No idea why ..
				{
					SetGPState<ListType_Translucent,true>(params);
					glDrawElements(GL_TRIANGLES, pidx_sort[p].count, GL_UNSIGNED_SHORT, (GLvoid*)(2*pidx_sort[p].first)); glCheck();
				
#if 0
					//Verify restriping -- only valid if no sort
					int fs=pidx_sort[p].first;

					for (u32 j=0; j<(params->count-2); j++)
					{
						for (u32 k=0; k<3; k++)
						{
							verify(idx_base[params->first+j+k]==vidx_sort[fs++]);
						}
					}

					verify(fs==(pidx_sort[p].first+pidx_sort[p].count));
#endif
				}
				params++;
			}

			if (multipass && settings.rend.TranslucentPolygonDepthMask)
			{
				// Write to the depth buffer now. The next render pass might need it. (Cosmic Smash)
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				glcache.Disable(GL_BLEND);

				glcache.StencilMask(0);

				// We use the modifier volumes shader because it's fast. We don't need textures, etc.
				glcache.UseProgram(gl.modvol_shader.program);
				glUniform1f(gl.modvol_shader.sp_ShaderColor, 1.f);

				glcache.DepthFunc(Zfunction[6]);	// Greater or equal
				glcache.DepthMask(GL_TRUE);

				for (u32 p = 0; p < count; p++)
				{
					PolyParam* params = pidx_sort[p].ppid;
					if (pidx_sort[p].count > 2 && !params->isp.ZWriteDis) {
						// FIXME no clipping in modvol shader
						//SetTileClip(gp->tileclip,true);

						SetCull(params->isp.CullMode ^ gcflip);

						glDrawElements(GL_TRIANGLES, pidx_sort[p].count, GL_UNSIGNED_SHORT, (GLvoid*)(2 * pidx_sort[p].first));
					}
				}
				glcache.StencilMask(0xFF);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			}
		}
		// Re-bind the previous index buffer for subsequent render passes
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs);
	}
}

//All pixels are in area 0 by default.
//If inside an 'in' volume, they are in area 1
//if inside an 'out' volume, they are in area 0
/*
	Stencil bits:
		bit 7: mv affected (must be preserved)
		bit 1: current volume state
		but 0: summary result (starts off as 0)

	Lower 2 bits:

	IN volume (logical OR):
	00 -> 00
	01 -> 01
	10 -> 01
	11 -> 01

	Out volume (logical AND):
	00 -> 00
	01 -> 00
	10 -> 00
	11 -> 01
*/
void SetMVS_Mode(u32 mv_mode,ISP_Modvol ispc)
{
	if (mv_mode==0)	//normal trigs
	{
		//set states
		glcache.Enable(GL_DEPTH_TEST);
		//write only bit 1
		glcache.StencilMask(2);
		//no stencil testing
		glcache.StencilFunc(GL_ALWAYS,0,2);
		//count the number of pixels in front of the Z buffer (xor zpass)
		glcache.StencilOp(GL_KEEP,GL_KEEP,GL_INVERT);

		//Cull mode needs to be set
		SetCull(ispc.CullMode);
	}
	else
	{
		//1 (last in) or 2 (last out)
		//each triangle forms the last of a volume

		//common states

		//no depth test
		glcache.Disable(GL_DEPTH_TEST);
		//write bits 1:0
		glcache.StencilMask(3);

		if (mv_mode==1)
		{
			// Inclusion volume
			//res : old : final 
			//0   : 0      : 00
			//0   : 1      : 01
			//1   : 0      : 01
			//1   : 1      : 01
			
			//if (1<=st) st=1; else st=0;
			glcache.StencilFunc(GL_LEQUAL,1,3);
			glcache.StencilOp(GL_ZERO,GL_ZERO,GL_REPLACE);
		}
		else
		{
			// Exclusion volume
			/*
				I've only seen a single game use it, so i guess it doesn't matter ? (Zombie revenge)
				(actually, i think there was also another, racing game)
			*/
			// The initial value for exclusion volumes is 1 so we need to invert the result before and'ing.
			//res : old : final 
			//0   : 0   : 00
			//0   : 1   : 01
			//1   : 0   : 00
			//1   : 1   : 00

			// if (1 == st) st = 1; else st = 0;
			glcache.StencilFunc(GL_EQUAL, 1, 3);
			glcache.StencilOp(GL_ZERO, GL_ZERO, GL_REPLACE);
		}
	}
}


void SetupMainVBO()
{
#ifndef GLES
	glBindVertexArray(gl.vbo.vao);
#endif

	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.geometry); glCheck();
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs); glCheck();

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x)); glCheck();

	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col)); glCheck();

	glEnableVertexAttribArray(VERTEX_COL_OFFS_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,spc)); glCheck();

	glEnableVertexAttribArray(VERTEX_UV_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u)); glCheck();
}

void SetupModvolVBO()
{
#ifndef GLES
	glBindVertexArray(gl.vbo.vao);
#endif

	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.modvols); glCheck();

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float)*3, (void*)0); glCheck();

	glDisableVertexAttribArray(VERTEX_UV_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
}
void DrawModVols(int first, int count)
{
	if (count == 0 || pvrrc.modtrig.used() == 0)
		return;

	SetupModvolVBO();

//	glcache.Enable(GL_BLEND);
//	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glcache.UseProgram(gl.modvol_shader.program);
	glUniform1f(gl.modvol_shader.sp_ShaderColor, 1 - FPU_SHAD_SCALE.scale_factor / 256.f);

	glcache.DepthMask(GL_FALSE);
	glcache.DepthFunc(Zfunction[4]);

	if(0)
	{
		//simply draw the volumes -- for debugging
		SetCull(0);
		glDrawArrays(GL_TRIANGLES, first, count * 3);
		SetupMainVBO();
	}
	else
	{
		/*
		mode :
		normal trig : flip
		last *in*   : flip, merge*in* &clear from last merge
		last *out*  : flip, merge*out* &clear from last merge
		*/

		/*

			Do not write to color
			Do not write to depth

			read from stencil bits 1:0
			write to stencil bits 1:0
		*/

		glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);

		if (0)
		{
			//simple single level stencil
			glcache.Enable(GL_STENCIL_TEST);
			glcache.StencilFunc(GL_ALWAYS,0x1,0x1);
			glcache.StencilOp(GL_KEEP,GL_KEEP,GL_INVERT);
			glcache.StencilMask(0x1);
			SetCull(0);
			glDrawArrays(GL_TRIANGLES, first, count * 3);
		}
		else if (true)
		{
			//Full emulation
			//the *out* mode is buggy

			u32 mod_base=0; //cur start triangle
			u32 mod_last=0; //last merge

			u32 cmv_count = count - 1;
			ISP_Modvol* params = &pvrrc.global_param_mvo.head()[first];

			//ISP_Modvol
			for (u32 cmv=0;cmv<cmv_count;cmv++)
			{

				ISP_Modvol ispc=params[cmv];
				mod_base=ispc.id;
				u32 sz=params[cmv+1].id-mod_base;
				if (sz == 0)
					continue;

				u32 mv_mode = ispc.DepthMode;


				if (mv_mode==0)	//normal trigs
				{
					SetMVS_Mode(0,ispc);
					//Render em (counts intersections)
					//verifyc(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,sz,pvrrc.modtrig.data+mod_base,3*4));
					glDrawArrays(GL_TRIANGLES,mod_base*3,sz*3);
				}
				else if (mv_mode<3)
				{
					while(sz)
					{
						//merge and clear all the prev. stencil bits

						//Count Intersections (last poly)
						SetMVS_Mode(0,ispc);
						glDrawArrays(GL_TRIANGLES,mod_base*3,3);

						//Sum the area
						SetMVS_Mode(mv_mode,ispc);
						glDrawArrays(GL_TRIANGLES,mod_last*3,(mod_base-mod_last+1)*3);

						//update pointers
						mod_last=mod_base+1;
						sz--;
						mod_base++;
					}
				}
			}
		}
		//disable culling
//		SetCull(0);
//		//enable color writes
//		glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
//
//		//black out any stencil with '1'
//		glcache.Enable(GL_BLEND);
//		glcache.BlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
//
//		glcache.Enable(GL_STENCIL_TEST);
//		glcache.StencilFunc(GL_EQUAL,0x81,0x81); //only pixels that are Modvol enabled, and in area 1
//
//		//clear the stencil result bit
//		glcache.StencilMask(0x3);    //write to lsb
//		glcache.StencilOp(GL_ZERO,GL_ZERO,GL_ZERO);
//
//		//don't do depth testing
//		glcache.Disable(GL_DEPTH_TEST);

		SetupMainVBO();
//		glDrawArrays(GL_TRIANGLE_STRIP,0,4);

	}

	//restore states
	glcache.Enable(GL_DEPTH_TEST);
	glcache.DepthMask(GL_TRUE);
}

void InitDualPeeling();
void RenderAverageColors();
void RenderWeightedBlended();
void RenderFrontToBackPeeling(int first, int count);
void DualPeelingReshape(int w, int h);
void renderABuffer();

void CreateGeometryTexture()
{
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);

	stencilTexId = glcache.GenTexture();
	glcache.BindTexture(GL_TEXTURE_2D, stencilTexId); glCheck();
	glcache.TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);		// OpenGL >= 4.3
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	// Using glTexStorage2D instead of glTexImage2D to satisfy requirement GL_TEXTURE_IMMUTABLE_FORMAT=true, needed for glTextureView below
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, screen_width, screen_height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencilTexId, 0); glCheck();
	glCheck();

//	opaqueTexId = glcache.GenTexture();
//	glcache.BindTexture(GL_TEXTURE_2D, opaqueTexId);
//	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
//	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
//	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_width, screen_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
//	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, opaqueTexId, 0);
//	glCheck();
	depthTexId = glcache.GenTexture();
	glTextureView(depthTexId, GL_TEXTURE_2D, stencilTexId, GL_DEPTH32F_STENCIL8, 0, 1, 0, 1);
	glCheck();
	glcache.BindTexture(GL_TEXTURE_2D, depthTexId);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glCheck();
}

void DrawStrips()
{
	if (geom_fbo == 0)
	{
		glGenFramebuffers(1, &geom_fbo);
		CreateGeometryTexture();

		// Color buffer. Not normally needed
		//GLuint colortexid = glcache.GenTexture();
		//glcache.BindTexture(GL_TEXTURE_2D, colortexid);
		//
		//glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_width, screen_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0); glCheck();
		//glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colortexid, 0); glCheck();

		GLuint uStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

		verify(uStatus == GL_FRAMEBUFFER_COMPLETE);
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
		if (stencilTexId == 0)
		{
			CreateGeometryTexture();
		}
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.DepthMask(GL_TRUE);
		glStencilMask(0xFF); glCheck();
		glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();
	}
	InitDualPeeling();
	DualPeelingReshape(screen_width, screen_height);

	SetupMainVBO();
	//Draw the strips !

	//We use sampler 0
	glActiveTexture(GL_TEXTURE0);

	RenderPass previous_pass = {0};
    for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++) {
        const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

		//initial state
		glcache.Enable(GL_DEPTH_TEST);
		glcache.DepthMask(GL_TRUE);

        // Do a first pass on the depth+stencil buffer
		glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        DrawListGeometry<ListType_Opaque>(pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count - previous_pass.op_count);
        DrawListGeometry<ListType_Punch_Through>(pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

		// Modifier volumes
		DrawModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count);

		glBindFramebuffer(GL_FRAMEBUFFER, 0); glCheck();

//		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// Bind stencil buffer for the fragment shader (shadowing)
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, stencilTexId);
		glCheck();

		// Bind depth texture for manual depth testing in fragment shader
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, depthTexId);
		glActiveTexture(GL_TEXTURE0);

		//Opaque
		DrawList<ListType_Opaque,false>(pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count - previous_pass.op_count);

		//Alpha tested
		DrawList<ListType_Punch_Through,false>(pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);

		//Alpha blended
		{
			DrawList<ListType_Translucent,true>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
			glCheck();

glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			renderABuffer();
//			if (hack_on)
//				RenderAverageColors();
//			else
	//			RenderFrontToBackPeeling(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
			//RenderWeightedBlended();
//			if (pvrrc.isAutoSort)
//				GenSorted(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
//
//#if TRIG_SORT
//			if (pvrrc.isAutoSort)
//				DrawSorted(render_pass < pvrrc.render_passes.used() - 1);
//			else
//				DrawList<ListType_Translucent,false>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
//#else
//			if (pvrrc.isAutoSort)
//				SortPParams(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
//			DrawList<ListType_Translucent,true>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
//#endif
			SetupMainVBO();
		}
		previous_pass = current_pass;
	}
}
