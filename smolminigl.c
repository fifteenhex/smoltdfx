/*
 * smolminigl - the subset of OpenGL 1.x that GLQuake uses, implemented on
 * smoltdfx (software transform + the Voodoo3 rasteriser).  The Voodoo3 has
 * no transform/lighting unit, so the matrix stack, vertex transform and
 * viewport mapping are done here on the CPU; textured/blended/depth-tested
 * triangles go to the hardware through smoltdfx.
 *
 * Milestone 1 scope: the 2D path (ortho, textured quads, fills, colour,
 * blend, alpha test) plus a working 3D transform.  Simplifications noted
 * inline (textures are rescaled to 256x256; near-plane clipping is a
 * reject).  Single translation unit: it owns smoltdfx's file-static state.
 */
#ifdef SMG_HOSTED		/* hosted (glibc) build; nolibc supplies these */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#endif
#include "smoltdfx.h"
#include "smg_cglm.h"		/* cglm matrix/vector math (nolibc-bridged) */
#include "smg_stb.h"		/* stb_image_resize2 texture resampler */
#include "GL/gl.h"

static int smg_pkt3_on, smg_pkt5_on;	/* emit strips as PKT3 / upload via PKT5 */

#define SMG_SETUP_BASE	(TDFX_SSETUP_RGB | TDFX_SSETUP_Z | TDFX_SSETUP_WFBI)
#define SMG_SETUP_TEX	(SMG_SETUP_BASE | TDFX_SSETUP_ST0)
/*
 * fbzColorPath values (the texel comes through textureMode = PASS; the
 * modulation by the iterated/primary colour is done here in the cc/cca
 * combine).  bits: rgbsel[1:0]=texture, asel[3:2]=texture; cc mselect at
 * [12:10], reverse at 13; cca mselect at [21:19], reverse at 22.
 *   REPLACE : out = texel                        (factor 1)
 *   MODULATE: out = texel * iterated colour      (mselect MCLOCAL, reverse)
 */
#define SMG_CP_REPLACE	(TDFX_CP_RGB_TEXTURE | (1u << 2) | TDFX_CP_TEXTURE_EN)
#define SMG_CP_MODULATE	(SMG_CP_REPLACE | (1u << 10) | (1u << 13) | \
			 (3u << 19) | (1u << 22))

#define SMG_MAXTEX	1024
#define SMG_MAXVERT	64
#define SMG_TEXMAX	256		/* largest square the hardware samples */
#define SMG_TEX_TRILINEAR (1u << 30)	/* textureMode[30]: LOD blend */
#define SMG_TEX_LODBIAS	4		/* tLOD bias, 4.2 fixed (+1.0 LOD) */

/*
 * Texture memory model.  The sub-256 (lod>0) sampling path had a texel
 * corruption bug that showed up as rainbow/false-colour world surfaces, so
 * colour textures are handled entirely through the proven lod=0 (256^2 +
 * full mip chain) path instead: every colour texture keeps a host-RAM copy
 * of its source and is uploaded lazily into a VRAM cache, rescaled to 256^2
 * with a box-filtered mip chain.  256^2+mips is 174 KB, so all of a level's
 * colour textures do not fit at once; the cache is a bump allocator that
 * wrap-flushes (evicts every cached colour texture, which then re-upload on
 * next use) when it runs out of room.  I8 lightmaps stay on the direct
 * native path (they are small, work correctly, and are updated in place),
 * allocated from the TOP of VRAM downward so the wrap-flush never touches
 * them.  The Voodoo3 samples a dim = 256>>lod texture whose level-`lod`
 * data sits at texBaseAddr + lodOffset(lod).
 */
static unsigned int smg_lod_off(int lod, int bpt)
{
	unsigned int texels = 0;
	int i;

	for (i = 0; i < lod; i++) {
		unsigned int d = 256u >> i;

		texels += d * d;
	}
	return (texels * (unsigned int)bpt) & ~0xfu;
}

static float smg_tex_scale = SMG_TEXMAX;	/* texcoord scale of bound tex */
static unsigned int smg_setupmode = SMG_SETUP_BASE;	/* current sSetupMode = PKT3 pmask */

/* ------------------------------- state ------------------------------- */
struct smg_tex {
	int used, uploaded;
	unsigned int vram;		/* byte offset of the texel data in VRAM */
	int fmt;			/* TDFX_TFMT_* */
	int dim;			/* square size (power of two, <= 256) */
	int lod;			/* mip level of dim: dim == 256 >> lod */
	int lodmax;			/* coarsest uploaded mip level */
	int bpt;			/* bytes per texel (1 or 2) */
	int filter;			/* MAG/MIN filter bits */
	int clamp;			/* CLAMPS|CLAMPT bits */
	unsigned char *host;		/* colour tex: host copy of the source */
	int hw, hh, hcomp;		/* host source dims + components */
	unsigned int lru;		/* frame the cache slot was last used */
};
static struct smg_tex smg_tex[SMG_MAXTEX];
static void smg_make_resident(struct smg_tex *tx);
static unsigned int smg_bound;
static unsigned int smg_vram_next;	/* colour-cache cursor (grows up) */
static unsigned int smg_vram_base;	/* first colour-cache byte offset */
static unsigned int smg_lm_top;		/* I8 lightmap cursor (grows down) */
static unsigned int smg_vram_end;	/* end of usable VRAM */
static unsigned int smg_frame;		/* incremented each swap (LRU clock) */
static unsigned int smg_wrapflushes;	/* colour-cache wrap-flushes (recycles) */

static float smg_mv[32][16];	int smg_mv_top;
static float smg_pr[4][16];	int smg_pr_top;
static float smg_mvp[16];	/* projection * modelview, refreshed per glEnd */
static GLenum smg_matmode = GL_MODELVIEW;

static float smg_cr = 1, smg_cg = 1, smg_cb = 1, smg_ca = 1;	/* colour */
static float smg_cs, smg_ct;					/* texcoord */

struct smg_vert { float x, y, z, r, g, b, a, s, t; };
static struct smg_vert smg_vbuf[SMG_MAXVERT];
static int smg_nv;
static GLenum smg_prim;

static int smg_en_tex, smg_en_depth, smg_en_blend, smg_en_alpha, smg_en_cull;
static GLenum smg_blend_s = GL_ONE, smg_blend_d = GL_ZERO;
static GLenum smg_depthfunc = GL_LEQUAL;
static int smg_depthmask = 1;
static float smg_depth_n = 0, smg_depth_f = 1;	/* glDepthRange, window Z */
static GLenum smg_alphafunc = GL_GREATER;
static float smg_alpharef;
static GLenum smg_texenv = GL_MODULATE;
static int smg_vp_x, smg_vp_y, smg_vp_w, smg_vp_h;

/* --------------------------- matrix helpers -------------------------- */
static float *smg_cur(void)
{
	return smg_matmode == GL_PROJECTION ? smg_pr[smg_pr_top]
					    : smg_mv[smg_mv_top];
}

static void smg_ident(float *m) { glm_mat4_identity(SMG_M4(m)); }

/* column-major 4x4 multiply: dst = a * b (as OpenGL applies them) */
static void smg_mul(float *dst, const float *a, const float *b)
{
	mat4 r;

	glm_mat4_mul(SMG_M4(a), SMG_M4(b), r);	/* r = a*b (temp: dst may alias a) */
	glm_mat4_ucopy(r, SMG_M4(dst));
}

static void smg_premul(const float *m)	/* cur = cur * m */
{
	smg_mul(smg_cur(), smg_cur(), m);
}

/* transform a 4-vector by a column-major matrix */
static void smg_xform(const float *m, float x, float y, float z,
		      float *ox, float *oy, float *oz, float *ow)
{
	vec4 in = { x, y, z, 1.0f }, out;

	glm_mat4_mulv(SMG_M4(m), in, out);
	*ox = out[0]; *oy = out[1]; *oz = out[2]; *ow = out[3];
}

/* ------------------------------ matrices ----------------------------- */
void glMatrixMode(GLenum mode) { smg_matmode = mode; }
void glLoadIdentity(void) { smg_ident(smg_cur()); }

void glLoadMatrixf(const GLfloat *m)
{
	int i;

	for (i = 0; i < 16; i++)
		smg_cur()[i] = m[i];
}
void glMultMatrixf(const GLfloat *m) { smg_premul(m); }

void glPushMatrix(void)
{
	if (smg_matmode == GL_PROJECTION) {
		if (smg_pr_top < 3) {
			glm_mat4_ucopy(SMG_M4(smg_pr[smg_pr_top]),
				       SMG_M4(smg_pr[smg_pr_top + 1]));
			smg_pr_top++;
		}
	} else if (smg_mv_top < 31) {
		glm_mat4_ucopy(SMG_M4(smg_mv[smg_mv_top]),
			       SMG_M4(smg_mv[smg_mv_top + 1]));
		smg_mv_top++;
	}
}
void glPopMatrix(void)
{
	if (smg_matmode == GL_PROJECTION) {
		if (smg_pr_top > 0)
			smg_pr_top--;
	} else if (smg_mv_top > 0) {
		smg_mv_top--;
	}
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
	glm_translate(SMG_M4(smg_cur()), (vec3){ x, y, z });	/* cur = cur * T */
}
void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
	glm_scale(SMG_M4(smg_cur()), (vec3){ x, y, z });	/* cur = cur * S */
}
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
	vec3 axis = { x, y, z };

	if (glm_vec3_norm(axis) < 1e-6f)	/* degenerate axis: GL no-op */
		return;
	glm_rotate(SMG_M4(smg_cur()), glm_rad(a), axis);	/* normalises axis */
}

/* GL post-multiplies the current matrix by the projection (RH, z in [-1,1]) */
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n,
	     GLdouble f)
{
	mat4 o;

	glm_ortho((float)l, (float)r, (float)b, (float)t, (float)n, (float)f, o);
	smg_premul((const float *)o);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n,
	       GLdouble f)
{
	mat4 fr;

	glm_frustum((float)l, (float)r, (float)b, (float)t, (float)n, (float)f, fr);
	smg_premul((const float *)fr);
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	smg_vp_x = x; smg_vp_y = y; smg_vp_w = w; smg_vp_h = h;
}
void glDepthRange(GLclampd n, GLclampd f)
{
	smg_depth_n = n < 0 ? 0 : n > 1 ? 1 : (float)n;
	smg_depth_f = f < 0 ? 0 : f > 1 ? 1 : (float)f;
}

/* ------------------------- immediate mode ---------------------------- */
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ smg_cr = r; smg_cg = g; smg_cb = b; smg_ca = a; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor3ubv(const GLubyte *v)
{ glColor4f(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1); }
void glTexCoord2f(GLfloat s, GLfloat t) { smg_cs = s; smg_ct = t; }
void glTexCoord2fv(const GLfloat *v) { smg_cs = v[0]; smg_ct = v[1]; }

void glBegin(GLenum mode) { smg_prim = mode; smg_nv = 0; }

static void smg_push(float x, float y, float z)
{
	struct smg_vert *v;

	if (smg_nv >= SMG_MAXVERT)
		return;
	v = &smg_vbuf[smg_nv++];
	v->x = x; v->y = y; v->z = z;
	v->r = smg_cr; v->g = smg_cg; v->b = smg_cb; v->a = smg_ca;
	v->s = smg_cs; v->t = smg_ct;
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { smg_push(x, y, z); }
void glVertex3fv(const GLfloat *v) { smg_push(v[0], v[1], v[2]); }
void glVertex2f(GLfloat x, GLfloat y) { smg_push(x, y, 0); }

/* clamp an RGBA float colour to [0,1] and pack to 0xAARRGGBB (Voodoo argb) */
static unsigned int smg_pack_color(float r, float g, float b, float a)
{
	vec4 c = { r, g, b, a };

	glm_vec4_clamp(c, 0.0f, 1.0f);
	glm_vec4_scale(c, 255.0f, c);
	return ((unsigned int)(int)c[3] << 24) | ((unsigned int)(int)c[0] << 16) |
	       ((unsigned int)(int)c[1] << 8) | (unsigned int)(int)c[2];
}

static unsigned int smg_argb(const struct smg_vert *v)
{
	return smg_pack_color(v->r, v->g, v->b, v->a);
}

/* a vertex in clip space (post modelview+projection), attributes carried */
struct smg_cvert { float x, y, z, w, r, g, b, a, s, t; };
static struct smg_cvert smg_cv[SMG_MAXVERT];	/* glEnd transforms each vertex once */

static void smg_to_clip(const struct smg_vert *v, struct smg_cvert *o)
{
	smg_xform(smg_mvp, v->x, v->y, v->z, &o->x, &o->y, &o->z, &o->w);
	o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a;
	o->s = v->s; o->t = v->t;
}

/* a clip-space vertex projected to the exact values the setup unit consumes */
struct smg_hwvert { float sx, sy, depth, oow, sc, tc; unsigned int argb; };

static void smg_project(const struct smg_cvert *c, struct smg_hwvert *h)
{
	float oow = 1.0f / c->w;
	float ndcx = c->x * oow, ndcy = c->y * oow, ndcz = c->z * oow;
	float sx = smg_vp_x + (ndcx + 1) * 0.5f * smg_vp_w;
	float sy = smg_vp_y + (ndcy + 1) * 0.5f * smg_vp_h;

	/*
	 * Window-Z = near + (ndcz*0.5+0.5)*(far-near), then scaled to the 16-bit
	 * depth buffer.  Applying glDepthRange here (rather than ignoring it) is
	 * required by GLQuake's default z-trick -- which skips the depth clear and
	 * instead flips the range/func between [0,0.5) LEQUAL and [1,0.5] GEQUAL
	 * each frame; without the remap the flipped func tests against uncleared
	 * depth and geometry pops in and out every other frame -- and by the
	 * weapon-model depth squash (glDepthRange(min,min+0.3*range)).
	 */
	h->sx = sx;
	h->sy = (float)smoltdfx_H - sy;		/* GL is y-up, fb is y-down */
	h->depth = (smg_depth_n + (ndcz * 0.5f + 0.5f) *
		    (smg_depth_f - smg_depth_n)) * 65535.0f;
	h->oow = oow;
	h->sc = c->s * smg_tex_scale * oow;
	h->tc = c->t * smg_tex_scale * oow;
	h->argb = smg_pack_color(c->r, c->g, c->b, c->a);
}

/* project a clip-space vertex to the screen and feed the setup unit (PIO/PKT1) */
static void smg_emit_cvert(const struct smg_cvert *c, int first)
{
	struct smg_hwvert h;

	smg_project(c, &h);
	smoltdfx_vtx(h.sx, h.sy, h.depth, h.argb, h.sc, h.tc, h.oow, first);
}

/* near-plane distance (inside the frustum when z + w >= 0, i.e. ndcz >= -1) */
static float smg_near_dist(const struct smg_cvert *v) { return v->z + v->w; }

static void smg_clerp(const struct smg_cvert *a, const struct smg_cvert *b,
		      float tt, struct smg_cvert *o)
{
	/* smg_cvert is 10 contiguous floats {x,y,z,w, r,g,b,a, s,t}: lerp them as
	 * two vec4s (position, colour) and a vec2 (texcoord).  Cast from the struct
	 * base so the compiler sees the full object (not a lone float). */
	float *fa = (float *)(uintptr_t)a, *fb = (float *)(uintptr_t)b, *fo = (float *)o;

	glm_vec4_lerp(fa,     fb,     tt, fo);		/* x y z w */
	glm_vec4_lerp(fa + 4, fb + 4, tt, fo + 4);	/* r g b a */
	glm_vec2_lerp(fa + 8, fb + 8, tt, fo + 8);	/* s t */
}

/* Sutherland-Hodgman clip of a convex polygon against the near plane;
 * returns the new vertex count (0 or 3..n+1). */
static int smg_clip_near(const struct smg_cvert *in, int n, struct smg_cvert *out)
{
	int nout = 0, i;

	for (i = 0; i < n; i++) {
		const struct smg_cvert *cur = &in[i];
		const struct smg_cvert *prv = &in[(i + n - 1) % n];
		float dc = smg_near_dist(cur), dp = smg_near_dist(prv);

		if ((dp >= 0) != (dc >= 0))	/* edge crosses the plane */
			smg_clerp(prv, cur, dp / (dp - dc), &out[nout++]);
		if (dc >= 0)
			out[nout++] = *cur;
	}
	return nout;
}

static void smg_setup_state(void)
{
	struct smg_tex *tx;
	unsigned int fbz;

	/* depth + 4x4 ordered dither (kills 565 banding / dark-region green
	 * shift, as real hardware does by default) */
	fbz = TDFX_FBZ_RGBWRMASK | TDFX_FBZ_ENDITHER;
	if (smg_en_depth) {
		fbz |= TDFX_FBZ_ENDEPTH;
		if (smg_depthmask)
			fbz |= TDFX_FBZ_DEPTHWRMASK;
	}
	fbz |= (smg_en_depth ? (smg_depthfunc & 7) : 7) << TDFX_FBZ_ZFUNC_SHIFT;
	/*
	 * Bound the rasteriser to the colour buffer.  The software transform
	 * clips only the near plane, so a large world polygon at a grazing angle
	 * still projects far below/right of the screen.  With clipping disabled
	 * the clip rectangle spans the whole 2048x2048 addressable range, so the
	 * hardware writes those off-screen rows PAST the colour buffer into the
	 * texture/lightmap VRAM that follows it -- corrupting textures (the
	 * "rainbow" HUD/world flashes) and eventually faulting.  Real Voodoo3
	 * hardware has the same exposure, so clipping to the render target here is
	 * both the fix and hardware-faithful.  Set every draw because this
	 * function rebuilds fbzMode from scratch (ENCLIP would otherwise be lost).
	 */
	fbz |= TDFX_FBZ_ENCLIP;
	smoltdfx_w3(TDFX_3D_CLIPLEFTRIGHT, smoltdfx_W);	/* x in [0, W) */
	smoltdfx_w3(TDFX_3D_CLIPBOTTOMTOP, smoltdfx_H);	/* y in [0, H) */
	smoltdfx_fbz = fbz;
	smoltdfx_w3(TDFX_3D_FBZMODE, fbz);

	/* blend + alpha test share ALPHAMODE, so build it in one write */
	{
		unsigned int am = 0;

		if (smg_en_blend) {
			int sf = smg_blend_s == GL_SRC_ALPHA ? TDFX_BLEND_SRCALPHA :
				 smg_blend_s == GL_ONE ? TDFX_BLEND_ONE :
				 smg_blend_s == GL_DST_COLOR ? TDFX_BLEND_DSTCOLOR :
				 TDFX_BLEND_ZERO;
			/*
			 * The Voodoo3's "colour" blend factor cross-references
			 * the other operand, so a dst-term SRC_COLOR/ONE_MINUS_
			 * SRC_COLOR scales dst by src / (1-src) -- exactly the GL
			 * semantics.  This makes GLQuake's lightmap pass
			 * (GL_ZERO, GL_ONE_MINUS_SRC_COLOR) modulate the base
			 * texture by the lightmap (real lighting), instead of the
			 * old fullbright no-op.
			 */
			int df = smg_blend_d == GL_ONE_MINUS_SRC_ALPHA ?
				 TDFX_BLEND_OMSRCALPHA :
				 smg_blend_d == GL_ONE ? TDFX_BLEND_ONE :
				 smg_blend_d == GL_DST_COLOR ? TDFX_BLEND_DSTCOLOR :
				 smg_blend_d == GL_SRC_COLOR ? TDFX_BLEND_DSTCOLOR :
				 smg_blend_d == GL_ONE_MINUS_SRC_COLOR ? TDFX_BLEND_OMDSTCOLOR :
				 smg_blend_d == GL_SRC_ALPHA ? TDFX_BLEND_SRCALPHA :
				 smg_blend_d == GL_ZERO ? TDFX_BLEND_ZERO :
				 TDFX_BLEND_ONE;

			am |= TDFX_ALPHA_ENBLEND |
			      (sf << TDFX_ALPHA_SRCFUNC_SHIFT) |
			      (df << TDFX_ALPHA_DSTFUNC_SHIFT);
		}
		if (smg_en_alpha)
			am |= TDFX_ALPHA_ENTEST |
			      (TDFX_ZF_GT << TDFX_ALPHA_FUNC_SHIFT) |
			      (((int)(smg_alpharef * 255) & 0xff)
			       << TDFX_ALPHA_REF_SHIFT);
		smoltdfx_w3(TDFX_3D_ALPHAMODE, am);
	}

	/* texture / colour path */
	tx = smg_en_tex ? &smg_tex[smg_bound] : 0;
	if (tx)
		smg_make_resident(tx);		/* no-op for I8 / already resident */
	if (tx && tx->uploaded && tx->vram) {
		unsigned int cp = smg_texenv == GL_REPLACE ? SMG_CP_REPLACE
							   : SMG_CP_MODULATE;
		/* point texBaseAddr a notional level-0 span before the data so
		 * the sampler's base + lodOffset(lod) lands on our level */
		unsigned int base = tx->vram - smg_lod_off(tx->lod, tx->bpt);

		unsigned int filt = tx->filter | tx->clamp;

		/*
		 * Colour (world/model) textures: enable trilinear LOD blend so
		 * the mip-level transitions on minified/tiled surfaces blend
		 * smoothly instead of banding into the saturated moiré the
		 * Voodoo3's isotropic nearest-mip produces on grazing floors.
		 * A small +0.5-LOD bias nudges selection one notch coarser to
		 * further cut the worst aliasing without visibly softening walls.
		 * (I8 lightmaps are single-level, so neither applies to them.)
		 */
		int bias = SMG_TEX_LODBIAS;

		if (tx->bpt == 2)
			filt |= SMG_TEX_TRILINEAR;
		smoltdfx_tex(base, tx->fmt, filt, SMOLTDFX_TC_PASS, cp);
		if (tx->bpt == 2)
			smoltdfx_w3(TDFX_3D_TLOD,
				    ((tx->lod << 2) & 0x3f) |
				    (((tx->lodmax << 2) & 0x3f) << 6) |
				    ((bias & 0x3f) << 12));
		else
			smoltdfx_lod(tx->lod, tx->lodmax);
		smg_setupmode = SMG_SETUP_TEX;
		smoltdfx_setupmode(smg_setupmode);
		smg_tex_scale = tx->dim;
	} else {
		smoltdfx_tex_off();
		smg_setupmode = SMG_SETUP_BASE;
		smoltdfx_setupmode(smg_setupmode);
	}
}

/* emit one triangle from the pre-transformed clip verts (near-clipped, sent as
 * its own sBeginTri so it is an independent triangle) */
static void smg_tri(int a, int b, int c)
{
	struct smg_cvert in[3], out[5];
	int nout, i;

	in[0] = smg_cv[a];
	in[1] = smg_cv[b];
	in[2] = smg_cv[c];
	nout = smg_clip_near(in, 3, out);	/* proper near-plane clip */
	if (nout < 3)
		return;
	for (i = 1; i + 1 < nout; i++) {	/* fan-triangulate the result */
		smg_emit_cvert(&out[0], 1);
		smg_emit_cvert(&out[i], 0);
		smg_emit_cvert(&out[i + 1], 0);
	}
}

/* true if every buffered vertex is inside the near plane (no triangle in the
 * primitive needs clipping, so it can go to the hardware as one strip) */
static int smg_all_inside(void)
{
	int i;

	for (i = 0; i < smg_nv; i++)
		if (smg_near_dist(&smg_cv[i]) < 0)
			return 0;
	return 1;
}

/*
 * Emit the buffered vertices [0, smg_nv) as one hardware triangle strip:
 * sBeginTri(v0) then sDrawTri(v1..) -- the setup unit assembles (v0,v1,v2),
 * (v1,v2,v3)...  Same triangles as the per-triangle path (vertex order within
 * a triangle does not affect rasterisation with culling off), but each shared
 * vertex is transformed and submitted once instead of ~3x.  Caller guarantees
 * all vertices are inside the near plane.
 */
static void smg_strip_seq(void)
{
	int i;

	smg_emit_cvert(&smg_cv[0], 1);
	for (i = 1; i < smg_nv; i++)
		smg_emit_cvert(&smg_cv[i], 0);
}

/*
 * Same strip, emitted as native-vertex packets (PKT3) into the command FIFO:
 * one packet per <=15-vertex chunk (numVertex is 4 bits), the first chunk
 * beginning the strip (BDDDDD) and later chunks continuing it (DDDDDD) -- the
 * setup unit's 3-vertex window persists across packets, so the triangles are
 * identical to smg_strip_seq's begin+draw stream, at ~7 words/vertex instead of
 * ~30 register writes/triangle.  The whole strip is reserved contiguously up
 * front so a ring wrap never splits it.
 */
#define SMG_PKT3_MAXVERT 15

static void smg_strip_pkt3(void)
{
	unsigned int pmask = smg_setupmode & 0xfff;
	unsigned int vsize = smoltdfx_pkt3_vsize(pmask);
	int npkt = (smg_nv + SMG_PKT3_MAXVERT - 1) / SMG_PKT3_MAXVERT;
	int off = 0;

	smoltdfx_cmd_reserve(npkt + smg_nv * vsize);	/* headers + all vertices */
	while (off < smg_nv) {
		int chunk = smg_nv - off;
		int i;

		if (chunk > SMG_PKT3_MAXVERT)
			chunk = SMG_PKT3_MAXVERT;
		smoltdfx_cmd_pkt3_hdr(pmask, off ? SMOLTDFX_PKT3_CONT
						 : SMOLTDFX_PKT3_STRIP, chunk);
		for (i = 0; i < chunk; i++) {
			struct smg_hwvert h;

			smg_project(&smg_cv[off + i], &h);
			smoltdfx_cmd_pkt3_vtx(pmask, h.sx, h.sy, h.argb, h.depth,
					      h.oow, h.sc, h.tc);
		}
		off += chunk;
	}
}

/*
 * Emit `ntri` independent triangles (vertex index triples in idx[]) as native-
 * vertex packets (PKT3) into the command FIFO, using the independent-triangle
 * command (BDDBDD): the setup unit issues sBeginTri on every third vertex, so
 * each group of three is its own triangle -- the same begin+draw+draw stream as
 * the per-triangle PIO path, at ~7 words/vertex instead of a register burst per
 * vertex.  A packet holds up to 15 vertices (numVertex is 4 bits), so it is
 * chunked on triangle (multiple-of-three) boundaries to keep the begin-every-
 * third pattern aligned.  Caller guarantees every referenced vertex is inside
 * the near plane (no per-triangle clipping needed).
 */
#define SMG_PKT3_MAXTRI 5			/* 5 tris * 3 verts = 15 (numVertex max) */

static void smg_trilist_pkt3(const int *idx, int ntri)
{
	unsigned int pmask = smg_setupmode & 0xfff;
	unsigned int vsize = smoltdfx_pkt3_vsize(pmask);
	int npkt = (ntri + SMG_PKT3_MAXTRI - 1) / SMG_PKT3_MAXTRI;
	int off = 0;

	smoltdfx_cmd_reserve(npkt + ntri * 3 * vsize);	/* headers + all vertices */
	while (off < ntri) {
		int chunk = ntri - off, t, k;

		if (chunk > SMG_PKT3_MAXTRI)
			chunk = SMG_PKT3_MAXTRI;
		smoltdfx_cmd_pkt3_hdr(pmask, SMOLTDFX_PKT3_TRIS, chunk * 3);
		for (t = 0; t < chunk; t++)
			for (k = 0; k < 3; k++) {
				struct smg_hwvert h;

				smg_project(&smg_cv[idx[(off + t) * 3 + k]], &h);
				smoltdfx_cmd_pkt3_vtx(pmask, h.sx, h.sy, h.argb,
						      h.depth, h.oow, h.sc, h.tc);
			}
		off += chunk;
	}
}

/* decompose the buffered primitive into a flat list of triangle index triples;
 * returns the triangle count.  Same triangles (and winding) as the per-triangle
 * smg_tri() loops below. */
static int smg_build_trilist(int *idx)
{
	int i, n = 0;

	switch (smg_prim) {
	case GL_TRIANGLES:
		for (i = 0; i + 2 < smg_nv; i += 3) {
			idx[n * 3] = i; idx[n * 3 + 1] = i + 1; idx[n * 3 + 2] = i + 2;
			n++;
		}
		break;
	case GL_QUADS:
		for (i = 0; i + 3 < smg_nv; i += 4) {
			idx[n * 3] = i; idx[n * 3 + 1] = i + 1; idx[n * 3 + 2] = i + 2;
			n++;
			idx[n * 3] = i; idx[n * 3 + 1] = i + 2; idx[n * 3 + 2] = i + 3;
			n++;
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for (i = 1; i + 1 < smg_nv; i++) {
			idx[n * 3] = 0; idx[n * 3 + 1] = i; idx[n * 3 + 2] = i + 1;
			n++;
		}
		break;
	}
	return n;
}

static int smg_idx[3 * SMG_MAXVERT];	/* triangle-list scratch for glEnd */

void glEnd(void)
{
	int i;

	if (smg_nv < 2)
		return;
	if (smg_prim == GL_LINES || smg_prim == GL_LINE_STRIP ||
	    smg_prim == GL_LINE_LOOP) {
		smoltdfx_setupmode(SMG_SETUP_BASE);
		smoltdfx_tex_off();
		for (i = 0; i + 1 < smg_nv; i++)
			smoltdfx_line(smg_vbuf[i].x, smg_vbuf[i].y,
				      smg_vbuf[i + 1].x, smg_vbuf[i + 1].y,
				      1.0f, smg_argb(&smg_vbuf[i]));
		return;
	}
	smg_setup_state();
	/* fold modelview+projection once per primitive: mvp = projection * mv */
	smg_mul(smg_mvp, smg_pr[smg_pr_top], smg_mv[smg_mv_top]);
	for (i = 0; i < smg_nv; i++)		/* transform each vertex once */
		smg_to_clip(&smg_vbuf[i], &smg_cv[i]);
	switch (smg_prim) {
	case GL_TRIANGLES:
	case GL_QUADS:
	case GL_TRIANGLE_FAN:
	case GL_POLYGON: {
		/*
		 * Independent triangles (also quads/fans/polygons, which decompose
		 * into them).  When the whole primitive is inside the near plane it
		 * goes to the setup unit as one PKT3 triangle list; otherwise fall
		 * back to the per-triangle path, which near-clips each triangle.
		 */
		int ntri = smg_build_trilist(smg_idx);

		if (smg_pkt3_on && smg_all_inside()) {
			smg_trilist_pkt3(smg_idx, ntri);
		} else {
			for (i = 0; i < ntri; i++)
				smg_tri(smg_idx[i * 3], smg_idx[i * 3 + 1],
					smg_idx[i * 3 + 2]);
		}
		break;
	}
	case GL_TRIANGLE_STRIP:
		/*
		 * A fully-visible strip goes to the hardware setup unit as one strip
		 * (fewer vertex submissions); if any vertex crosses the near plane the
		 * strip would need per-triangle clipping, so fall back to independent
		 * triangles (the winding swap keeps a consistent order there).
		 */
		if (smg_all_inside()) {
			if (smg_pkt3_on)
				smg_strip_pkt3();
			else
				smg_strip_seq();
		} else {
			for (i = 0; i + 2 < smg_nv; i++)
				if (i & 1)
					smg_tri(i + 1, i, i + 2);
				else
					smg_tri(i, i + 1, i + 2);
		}
		break;
	}
}

/* ------------------------------- state ------------------------------- */
void glEnable(GLenum c)
{
	switch (c) {
	case GL_TEXTURE_2D: smg_en_tex = 1; break;
	case GL_DEPTH_TEST: smg_en_depth = 1; break;
	case GL_BLEND: smg_en_blend = 1; break;
	case GL_ALPHA_TEST: smg_en_alpha = 1; break;
	case GL_CULL_FACE: smg_en_cull = 1; break;
	}
}
void glDisable(GLenum c)
{
	switch (c) {
	case GL_TEXTURE_2D: smg_en_tex = 0; break;
	case GL_DEPTH_TEST: smg_en_depth = 0; break;
	case GL_BLEND: smg_en_blend = 0; break;
	case GL_ALPHA_TEST: smg_en_alpha = 0; break;
	case GL_CULL_FACE: smg_en_cull = 0; break;
	}
}
void glBlendFunc(GLenum s, GLenum d) { smg_blend_s = s; smg_blend_d = d; }
void glDepthMask(GLboolean f) { smg_depthmask = f; }
void glDepthFunc(GLenum f) { smg_depthfunc = f; }
void glAlphaFunc(GLenum f, GLclampf ref) { smg_alphafunc = f; smg_alpharef = ref; }
void glShadeModel(GLenum m) { (void)m; }
void glCullFace(GLenum m) { (void)m; }
void glPolygonMode(GLenum f, GLenum m) { (void)f; (void)m; }
void glHint(GLenum t, GLenum m) { (void)t; (void)m; }
void glDrawBuffer(GLenum b) { (void)b; }
void glReadBuffer(GLenum b) { (void)b; }
void glFinish(void) { smoltdfx_wait_idle(); }
void glFlush(void) { smoltdfx_wait_idle(); }

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{ (void)r; (void)g; (void)b; (void)a; }

void glClear(GLbitfield mask)
{
	/*
	 * Clear ONLY the buffers named in `mask` (the fastfill honours the
	 * RGBWRMASK/DEPTHWRMASK write masks).  GLQuake runs with gl_clear=0, so
	 * it clears only GL_DEPTH_BUFFER_BIT each frame and relies on the colour
	 * buffer PERSISTING (the status bar is drawn only when it changes, per
	 * sb_updates).  Clearing colour unconditionally here wiped the persistent
	 * HUD to black on frames it was not redrawn -> "HUD sometimes disappears".
	 */
	unsigned int fbz = smoltdfx_fbz &
		~(TDFX_FBZ_RGBWRMASK | TDFX_FBZ_DEPTHWRMASK | TDFX_FBZ_ENDEPTH);

	if (mask & GL_COLOR_BUFFER_BIT)
		fbz |= TDFX_FBZ_RGBWRMASK;
	if (mask & GL_DEPTH_BUFFER_BIT)
		fbz |= TDFX_FBZ_ENDEPTH | TDFX_FBZ_DEPTHWRMASK;
	if (!(fbz & (TDFX_FBZ_RGBWRMASK | TDFX_FBZ_DEPTHWRMASK)))
		return;				/* nothing requested */
	smoltdfx_clip_full();
	smoltdfx_w3(TDFX_3D_FBZMODE, fbz);
	smoltdfx_clear(0xff000000, 0xffff);
	smoltdfx_w3(TDFX_3D_FBZMODE, smoltdfx_fbz);	/* restore drawing state */
}

/* ------------------------------ textures ----------------------------- */
void glGenTextures(GLsizei n, GLuint *tex)
{
	int i, k;

	for (k = 0; k < n; k++) {
		for (i = 1; i < SMG_MAXTEX; i++)
			if (!smg_tex[i].used) {
				smg_tex[i].used = 1;
				smg_tex[i].uploaded = 0;
				smg_tex[i].vram = 0;
				smg_tex[i].host = 0;
				smg_tex[i].filter = TDFX_TEX_MAGFILTER |
						    TDFX_TEX_MINFILTER;
				tex[k] = i;
				break;
			}
	}
}
void glDeleteTextures(GLsizei n, const GLuint *tex)
{
	int k;

	for (k = 0; k < n; k++)
		if (tex[k] && tex[k] < SMG_MAXTEX) {
			free(smg_tex[tex[k]].host);
			smg_tex[tex[k]].host = 0;
			smg_tex[tex[k]].vram = 0;
			smg_tex[tex[k]].uploaded = 0;
			smg_tex[tex[k]].used = 0;
		}
}
void glBindTexture(GLenum target, GLuint tex) { (void)target; smg_bound = tex; }

/* rescale an RGBA/RGB/LUMINANCE source to a dim-square level at VRAM `vram` */
static void smg_upload_level(struct smg_tex *tx, unsigned int vram, int dim,
			     int w, int h, GLenum format,
			     const unsigned char *px)
{
	int comp = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 1;
	int x, y;

	for (y = 0; y < dim; y++) {
		int sy = h > 0 ? y * h / dim : 0;

		for (x = 0; x < dim; x++) {
			int sx = w > 0 ? x * w / dim : 0;
			const unsigned char *p = px + (sy * w + sx) * comp;
			unsigned int idx = (unsigned int)(y * dim + x);

			if (tx->fmt == TDFX_TFMT_I8) {
				*((volatile unsigned char *)smoltdfx_vram
				  + vram + idx) = p[0];
			} else if (tx->fmt == TDFX_TFMT_ARGB1555) {
				unsigned int v = (p[3] >= 128 ? 0x8000u : 0) |
						 ((p[0] >> 3) << 10) |
						 ((p[1] >> 3) << 5) | (p[2] >> 3);
				smoltdfx_vram[vram / 2 + idx] = v;
			} else {
				smoltdfx_vram[vram / 2 + idx] =
					smoltdfx_rgb565(p[0], p[1], p[2]);
			}
		}
	}
	tx->uploaded = 1;
}

/* pack one RGBA scratch texel into the bound colour texture's 16-bit format */
static unsigned short smg_pack(const struct smg_tex *tx, const unsigned char *p)
{
	if (tx->fmt == TDFX_TFMT_ARGB1555)
		return (p[3] >= 128 ? 0x8000u : 0) | ((p[0] >> 3) << 10) |
		       ((p[1] >> 3) << 5) | (p[2] >> 3);
	return smoltdfx_rgb565(p[0], p[1], p[2]);
}

static unsigned char smg_scratch[256 * 256 * 4];	/* mip-gen workspace */

/*
 * Upload a colour texture's host source into its cache slot at 256^2 with a
 * box-filtered mip chain (levels 0..8), all through the lod=0 path.  The
 * scratch RGBA buffer is downsampled in place between levels.
 */
/*
 * Write one already-resampled mip level (dim x dim, in smg_scratch as RGBA)
 * into VRAM at byte offset `base`.  Default path: direct u16 writes (PIO --
 * kept as the fallback).  When PKT5 uploads are enabled, the level is packed
 * two texels per word into a PKT5 linear-write burst in the command FIFO ring,
 * so the upload is ordered in-stream (the card does the ring->VRAM copy) and a
 * cache recycle no longer needs to drain the engine first.  Colour textures
 * are bpt==2; the 1x1 level (odd texel count) always takes the direct path.
 */
static void smg_write_level(struct smg_tex *tx, unsigned int base, int dim)
{
	int n = dim * dim, i;

	if (smg_pkt5_on && tx->bpt == 2 && dim >= 2) {
		int words = n >> 1;		/* two u16 texels per 32-bit word */

		smoltdfx_cmd_reserve(2 + words);
		smoltdfx_cmd_pkt5_hdr(base, words);
		for (i = 0; i < n; i += 2)
			smoltdfx_cmd_word((unsigned int)smg_pack(tx,
					  smg_scratch + i * 4) |
					  ((unsigned int)smg_pack(tx,
					  smg_scratch + (i + 1) * 4) << 16));
	} else {
		for (i = 0; i < n; i++)
			smoltdfx_vram[base / 2 + i] =
				smg_pack(tx, smg_scratch + i * 4);
	}
}

static void smg_upload_cached(struct smg_tex *tx)
{
	int comp = tx->hcomp, dim, L, x, y, ch, hw = tx->hw, hh = tx->hh;
	int n = hw * hh;
	unsigned char *rgba;

	/*
	 * Wrapped-bilinear upscale of the native source to 256^2 via
	 * stb_image_resize2.  Point-scaling a small (e.g. 64^2) tile up to 256^2
	 * injects blocky step edges -- fake high frequency the base level and its
	 * mips then alias into rainbow moire under minification.  TRIANGLE is
	 * bilinear (no such edges) and EDGE_WRAP matches a repeating texture, so
	 * the result matches what sampling the native texture would give.  stb
	 * resizes channel-for-channel, so expand the source to RGBA first.
	 */
	rgba = malloc((size_t)n * 4);
	if (!rgba) {
		tx->uploaded = 0;
		return;
	}
	for (y = 0; y < n; y++) {
		const unsigned char *s = tx->host + (size_t)y * comp;
		unsigned char *d = rgba + (size_t)y * 4;

		d[0] = s[0];
		d[1] = comp >= 3 ? s[1] : s[0];
		d[2] = comp >= 3 ? s[2] : s[0];
		d[3] = comp >= 4 ? s[3] : 255;
	}
	stbir_resize(rgba, hw, hh, hw * 4, smg_scratch, 256, 256, 256 * 4,
		     STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_WRAP,
		     STBIR_FILTER_TRIANGLE);
	free(rgba);
	for (dim = 256, L = 0; L <= 8; L++, dim >>= 1) {
		unsigned int base = tx->vram + smg_lod_off(L, tx->bpt);

		smg_write_level(tx, base, dim);
		if (dim > 1) {			/* box-downsample dim -> dim/2 */
			int hd = dim >> 1;

			for (y = 0; y < hd; y++)
				for (x = 0; x < hd; x++) {
					unsigned char *s = smg_scratch +
						((2 * y) * dim + 2 * x) * 4;
					unsigned char *d = smg_scratch +
						(y * hd + x) * 4;

					for (ch = 0; ch < 4; ch++)
						d[ch] = (s[ch] + s[ch + 4] +
							 s[ch + dim * 4] +
							 s[ch + dim * 4 + 4]) >> 2;
				}
		}
	}
	tx->uploaded = 1;
}

/* ensure a colour texture occupies a VRAM cache slot (upload it if not) */
static void smg_make_resident(struct smg_tex *tx)
{
	unsigned int sz;

	if (!tx->host)			/* not a cached colour texture */
		return;
	if (tx->vram) {			/* already resident: just touch LRU */
		tx->lru = smg_frame;
		return;
	}
	sz = smg_lod_off(9, tx->bpt) + 16;		/* full 256^2 mip chain */
	if (smg_vram_next + sz > smg_lm_top) {		/* out of room: flush */
		int i;

		/*
		 * Recycling VRAM slots overwrites their texel data.  Any drawing
		 * commands still queued in the command FIFO reference the old
		 * contents, so drain the engine before reusing the memory (a no-op
		 * on the synchronous PIO path where prior draws already executed).
		 * With PKT5 uploads the re-upload is itself an ordered ring packet
		 * that lands after those draws, so no drain is needed.
		 */
		if (!smg_pkt5_on)
			smoltdfx_wait_idle();
		smg_wrapflushes++;
		for (i = 1; i < SMG_MAXTEX; i++)
			if (smg_tex[i].host)
				smg_tex[i].vram = 0;
		smg_vram_next = smg_vram_base;
	}
	tx->vram = smg_vram_next;
	smg_vram_next += sz;
	smg_upload_cached(tx);
	tx->lru = smg_frame;
}

void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w,
		  GLsizei h, GLint border, GLenum format, GLenum type,
		  const GLvoid *pixels)
{
	struct smg_tex *tx = &smg_tex[smg_bound];
	int comp = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 1;

	(void)target; (void)internal; (void)border; (void)type;
	if (!smg_bound || !pixels)
		return;

	if (format == GL_LUMINANCE) {
		/*
		 * I8 (lightmaps): direct native path, allocated from the top of
		 * VRAM downward so the colour cache's wrap-flush never clobbers
		 * it.  Only level 0 is used (lightmaps have no mip chain).
		 */
		if (level != 0)
			return;
		tx->fmt = TDFX_TFMT_I8; tx->bpt = 1;
		{
			int want = w > h ? w : h, dim = SMG_TEXMAX, lod = 0;

			while (dim > 8 && (dim >> 1) >= want) {
				dim >>= 1; lod++;
			}
			tx->dim = dim; tx->lod = lod; tx->lodmax = lod;
		}
		if (!tx->vram) {
			unsigned int sz = ((unsigned int)(tx->dim * tx->dim)
					   + 16u) & ~0xfu;

			smg_lm_top -= sz;
			tx->vram = smg_lm_top;
		}
		smg_upload_level(tx, tx->vram, tx->dim, w, h, format, pixels);
		return;
	}

	/*
	 * Colour textures: keep a host copy of the level-0 source and upload
	 * lazily (256^2 + box-filtered mips).  glTexImage2D(level>0) mip data
	 * from the caller is ignored -- we regenerate the whole chain -- so a
	 * later re-bind after an eviction can rebuild without the caller.
	 */
	if (level != 0)
		return;
	tx->fmt = format == GL_RGBA ? TDFX_TFMT_ARGB1555 : TDFX_TFMT_RGB565;
	tx->bpt = 2;
	tx->dim = 256; tx->lod = 0; tx->lodmax = 8;
	free(tx->host);
	tx->host = malloc((size_t)w * h * comp);
	if (!tx->host) {
		tx->uploaded = 0;
		return;
	}
	memcpy(tx->host, pixels, (size_t)w * h * comp);
	tx->hw = w; tx->hh = h; tx->hcomp = comp;
	tx->vram = 0;			/* force (re)residency + upload on next use */
	tx->uploaded = 0;
}
void glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo,
		     GLsizei w, GLsizei h, GLenum format, GLenum type,
		     const GLvoid *pixels)
{
	struct smg_tex *tx = &smg_tex[smg_bound];
	const unsigned char *px = pixels;
	int comp = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 1;
	int x, y;

	(void)target; (void)type;
	if (level != 0 || !pixels)
		return;
	if (tx->host) {
		/*
		 * Colour texture: patch the host copy in place and invalidate the
		 * cache slot so the next bind re-uploads (rescale makes a direct
		 * VRAM sub-rect update impossible).  Colour sub-updates are rare.
		 */
		if (xo >= 0 && yo >= 0 && xo + w <= tx->hw && yo + h <= tx->hh) {
			for (y = 0; y < h; y++)
				memcpy(tx->host + ((yo + y) * tx->hw + xo) * comp,
				       px + (size_t)y * w * comp,
				       (size_t)w * comp);
			tx->vram = 0;
		}
		return;
	}
	if (!tx->vram)
		return;
	/*
	 * I8 lightmap: proper sub-rectangle update when the source is native
	 * width (no rescale) -- e.g. GLQuake's per-frame dynamic-lightmap
	 * updates, which touch only rows [yo, yo+h).
	 */
	if (w == tx->dim && xo == 0 && yo >= 0 && yo + h <= tx->dim) {
		for (y = 0; y < h; y++) {
			unsigned int rb = (unsigned int)(yo + y) * tx->dim;
			const unsigned char *pr = px + (unsigned int)y * w * comp;

			for (x = 0; x < w; x++) {
				const unsigned char *p = pr + x * comp;
				unsigned int idx = rb + x;

				if (tx->fmt == TDFX_TFMT_I8)
					*((volatile unsigned char *)smoltdfx_vram
					  + tx->vram + idx) = p[0];
				else if (tx->fmt == TDFX_TFMT_ARGB1555)
					smoltdfx_vram[tx->vram / 2 + idx] =
						(p[3] >= 128 ? 0x8000u : 0) |
						((p[0] >> 3) << 10) |
						((p[1] >> 3) << 5) | (p[2] >> 3);
				else
					smoltdfx_vram[tx->vram / 2 + idx] =
						smoltdfx_rgb565(p[0], p[1], p[2]);
			}
		}
		return;
	}
	/* fallback: full-texture (re)upload with rescale */
	smg_upload_level(tx, tx->vram, tx->dim, w, h, format, pixels);
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat p)
{
	struct smg_tex *tx = &smg_tex[smg_bound];
	int v = (int)p;

	(void)target;
	if (pname == GL_TEXTURE_MAG_FILTER)
		tx->filter = (v == GL_LINEAR ? TDFX_TEX_MAGFILTER : 0) |
			     (tx->filter & TDFX_TEX_MINFILTER);
	else if (pname == GL_TEXTURE_MIN_FILTER)
		tx->filter = (v == GL_NEAREST ? 0 : TDFX_TEX_MINFILTER) |
			     (tx->filter & TDFX_TEX_MAGFILTER);
	else if (pname == GL_TEXTURE_WRAP_S)
		tx->clamp = (v == GL_CLAMP ? TDFX_TEX_CLAMPS : 0) |
			    (tx->clamp & TDFX_TEX_CLAMPT);
	else if (pname == GL_TEXTURE_WRAP_T)
		tx->clamp = (v == GL_CLAMP ? TDFX_TEX_CLAMPT : 0) |
			    (tx->clamp & TDFX_TEX_CLAMPS);
}
void glTexParameteri(GLenum t, GLenum p, GLint v) { glTexParameterf(t, p, v); }
void glTexEnvf(GLenum target, GLenum pname, GLfloat p)
{
	(void)target;
	if (pname == GL_TEXTURE_ENV_MODE)
		smg_texenv = (GLenum)p;
}
void glTexEnvi(GLenum t, GLenum p, GLint v) { glTexEnvf(t, p, v); }

/* ------------------------------ queries ------------------------------ */
void glGetFloatv(GLenum pname, GLfloat *params)
{
	const float *m = pname == GL_PROJECTION_MATRIX ? smg_pr[smg_pr_top]
						       : smg_mv[smg_mv_top];
	int i;

	for (i = 0; i < 16; i++)
		params[i] = m[i];
}
const GLubyte *glGetString(GLenum name)
{
	switch (name) {
	case GL_VENDOR:   return (const GLubyte *)"smoltdfx";
	case GL_RENDERER: return (const GLubyte *)"Voodoo3 (smolminigl)";
	case GL_VERSION:  return (const GLubyte *)"1.1 smolminigl";
	default:          return (const GLubyte *)"";
	}
}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
		  GLenum type, GLvoid *pixels)
{ (void)x; (void)y; (void)w; (void)h; (void)format; (void)type; (void)pixels; }
void glFogf(GLenum p, GLfloat v) { (void)p; (void)v; }
void glFogi(GLenum p, GLint v) { (void)p; (void)v; }
void glFogfv(GLenum p, const GLfloat *v) { (void)p; (void)v; }

/* --------------------------- bring-up seam --------------------------- */
int smolminigl_open(const char *regdev, const char *fbdev)
{
	int i;

	if (smoltdfx_init(regdev, fbdev) < 0)
		return -1;
	/* Route register submission through a DMA command FIFO ring: batches
	 * writes (no per-command vmexit under QEMU / PCI write on hardware),
	 * committed with one bump per flush.  The ring normally lives at the top
	 * of VRAM (which lowers smoltdfx_vram_usable, so reserve it before laying
	 * out the texture cache below).  Optionally -- for a card that can
	 * bus-master -- put it in system RAM instead (SMG_CMDFIFO_SYSRAM in the
	 * hosted build), which leaves all of VRAM free for the cache. */
	{
		int sysram = 0;

#ifdef SMG_HOSTED
		sysram = getenv("SMG_CMDFIFO_SYSRAM") != 0;
#endif
#ifdef SMG_FORCE_SYSRAM
		sysram = 1;			/* validation builds */
#endif
		if (!sysram || smoltdfx_cmdfifo_init_sysram(1024 * 1024) < 0)
			smoltdfx_cmdfifo_init(1024 * 1024);	/* 256 pages: hw baseSize max */
	}
	smg_pkt3_on = 1;			/* fully-visible strips -> PKT3 packets */
	smg_pkt5_on = 1;			/* texture uploads -> PKT5 ring bursts */
	smg_mv_top = smg_pr_top = 0;
	smg_ident(smg_mv[0]);
	smg_ident(smg_pr[0]);
	smg_vp_x = 0; smg_vp_y = 0;
	smg_vp_w = smoltdfx_W; smg_vp_h = smoltdfx_H;
	for (i = 0; i < SMG_MAXTEX; i++) {
		free(smg_tex[i].host);
		smg_tex[i].used = smg_tex[i].vram = smg_tex[i].uploaded = 0;
		smg_tex[i].host = 0;
	}
	/* textures live in VRAM after front/back/aux buffers: the colour cache
	 * grows up from the base, I8 lightmaps grow down from the top.  Bound
	 * everything to the actually-mapped VRAM (smem_len) -- writing past it
	 * lands in the adjacent register mmap and corrupts the video regs. */
	smg_vram_base = 3 * smoltdfx_back;
	smg_vram_next = smg_vram_base;
	smg_vram_end = smoltdfx_vram_usable ? smoltdfx_vram_usable
					    : 16u * 1024 * 1024;
	smg_lm_top = smg_vram_end;
	smg_frame = 0;
	smoltdfx_target();
	return 0;
}
/* render offscreen (back buffer) every frame, then composite the finished
 * frame to the scanned-out front buffer -- avoids page-flip tearing/noise */
void smolminigl_swap(void)
{
	smg_frame++;
	smoltdfx_composite();
}
int smolminigl_width(void) { return smoltdfx_W; }
int smolminigl_height(void) { return smoltdfx_H; }
