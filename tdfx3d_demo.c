/*
 * tdfx3d_demo - a visual test for the 3dfx Voodoo3 3D engine, built on
 * smoltdfx.h.
 *
 * It offers selectable scenes (pick one on the command line):
 *   basic  - a full-screen Gouraud background with a bouncing quad
 *   cubes  - several depth-buffered Gouraud cubes orbiting the screen
 *   grid   - a grid of tiles, each exercising one feature: flat, Gouraud,
 *            depth cube, point/bilinear textures, the ARGB4444/1555/RGB332
 *            formats, an 8bpp palette and an NCC/YIQ table
 *   twod   - the 2D engine: rectangle fills and screen-to-screen blits
 *   clamp  - texture coordinate wrap versus clamp, side by side
 *   texfmt - one tile per remaining texture format (I8/A8/AI44/AI88/
 *            ARGB8332/AYIQ/AP88/P8_RGBA)
 *   fog    - the fog factor sources: eye-W table, alpha, Z, constant
 *   minif  - mip LOD selection: the texture drawn at shrinking sizes
 *
 * At start-up it renders one canonical frame (a fixed animation phase) of
 * the selected scene and prints a digest of it to the serial console,
 * then animates.  The digest (a whole-frame checksum plus a fixed sample
 * grid) lets a QEMU run and a real-hardware run be compared, and the
 * selectable scenes make per-feature regression testing possible.
 *
 * Usage: tdfx3d_demo [/dev/tdfx3d] [/dev/fb0] [basic|cubes|grid|twod|clamp|texfmt|fog|minif] [dump]
 *
 * Freestanding (nolibc); build with the Makefile here.  Boot the card
 * with e.g. tdfxfb.mode_option=640x480-16@60 (RGB565, fb at VRAM 0).
 */
#include "smoltdfx.h"

#define SM_BASE		(TDFX_SSETUP_RGB | TDFX_SSETUP_Z | TDFX_SSETUP_WFBI)
#define SM_TEX		(SM_BASE | TDFX_SSETUP_ST0)
#define SM_A		(SM_BASE | TDFX_SSETUP_ALPHA)
#define TEXCP		(TDFX_CP_RGB_TEXTURE | TDFX_CP_TEXTURE_EN)
#define CANON_T		1.234f		/* canonical animation phase */

enum {
	SC_BASIC, SC_CUBES, SC_GRID, SC_TWOD, SC_CLAMP, SC_TEXFMT, SC_FOG,
	SC_MINIF
};

/* ============================ scene: basic ========================== */
static void scene_basic(float t)
{
	float W = smoltdfx_W, H = smoltdfx_H, s = 60.0f;
	float bx = (0.5f + 0.45f * smoltdfx_sin(t)) * (W - 2 * s);
	float by = (0.5f + 0.45f * smoltdfx_sin(t * 0.73f)) * (H - 2 * s);

	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_clear(0xff101018, 0xffff);

	/* full-screen Gouraud quad: red/green/blue/yellow corners */
	smoltdfx_setupmode(SM_BASE);
	smoltdfx_quad(0, 0, W, H, 0xffff0000, 0xff0000ff, 0xff00ff00,
		      0xffffff00, 0, 0);

	/* a solid white quad bouncing across the background */
	smoltdfx_quad(bx, by, bx + s, by + s, 0xffffffff, 0xffffffff,
		      0xffffffff, 0xffffffff, 0, 0);
}

/* ============================== cube ================================ */
/* unit-cube corners and the 12 triangles of its 6 faces */
static const signed char cvx[8] = { -1, 1, -1, 1, -1, 1, -1, 1 };
static const signed char cvy[8] = { -1, -1, 1, 1, -1, -1, 1, 1 };
static const signed char cvz[8] = { -1, -1, -1, -1, 1, 1, 1, 1 };
static const int cidx[12][3] = {
	{ 0, 1, 3 }, { 0, 3, 2 }, { 4, 5, 7 }, { 4, 7, 6 },
	{ 0, 1, 5 }, { 0, 5, 4 }, { 2, 3, 7 }, { 2, 7, 6 },
	{ 0, 2, 6 }, { 0, 6, 4 }, { 1, 3, 7 }, { 1, 7, 5 },
};
static const unsigned int cface[6] = {
	0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff, 0x00ffff,
};

/* draw one perspective-projected, depth-buffered cube */
static void draw_cube(float cx, float cy, float rad, float ang)
{
	float c1 = smoltdfx_cos(ang), s1 = smoltdfx_sin(ang);
	float c2 = smoltdfx_cos(ang * 0.6f), s2 = smoltdfx_sin(ang * 0.6f);
	float px[8], py[8], pd[8];
	int i;

	for (i = 0; i < 8; i++) {
		float x = cvx[i], y = cvy[i], z = cvz[i];
		float x1 =  x * c1 + z * s1;
		float z1 = -x * s1 + z * c1;
		float y2 =  y * c2 - z1 * s2;
		float z2 =  y * s2 + z1 * c2;
		float zc = z2 + 4.0f;

		px[i] = cx + rad * x1 / zc;
		py[i] = cy - rad * y2 / zc;
		pd[i] = (zc - 2.2f) * (60000.0f / 3.6f) + 1000.0f;
	}
	smoltdfx_setupmode(SM_BASE);
	for (i = 0; i < 12; i++) {
		unsigned int col = 0xff000000u | cface[i >> 1];
		int a = cidx[i][0], b = cidx[i][1], d = cidx[i][2];

		smoltdfx_vtx(px[a], py[a], pd[a], col, 0, 0, 1.0f, 1);
		smoltdfx_vtx(px[b], py[b], pd[b], col, 0, 0, 1.0f, 0);
		smoltdfx_vtx(px[d], py[d], pd[d], col, 0, 0, 1.0f, 0);
	}
}

/* ============================ scene: cubes ========================== */
static void scene_cubes(float t)
{
	float W = smoltdfx_W, H = smoltdfx_H, rad = H * 0.22f;
	int i;

	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_depth(1, TDFX_ZF_LT);		/* also enables depth writes */
	smoltdfx_clear(0xff101820, 0xffff);	/* clears colour and depth */

	for (i = 0; i < 5; i++) {
		float ph = t * (0.4f + 0.1f * i) + i * 1.2566f;
		float cx = W * (0.5f + 0.34f * smoltdfx_cos(ph));
		float cy = H * (0.5f + 0.34f * smoltdfx_sin(ph * 1.3f));

		draw_cube(cx, cy, rad, t * (1.0f + 0.2f * i) + i);
	}
}

/* ============================= textures ============================= */
#define TEXW		256
#define TEX(k)		(0x300000u + (k) * 0x20000u)
#define TX_CHECK	TEX(0)		/* red/yellow checker (565) */
#define TX_4444		TEX(1)
#define TX_1555		TEX(2)
#define TX_332		TEX(3)		/* 8bpp */
#define TX_PAL		TEX(4)		/* 8bpp palette indices */
#define TX_NCC		TEX(5)		/* 8bpp NCC indices */
#define TX_BILIN	TEX(6)
#define TX_LIGHT	TEX(7)		/* radial light map (multitexture) */
#define TX_SPRITE	TEX(8)		/* cyan diamond on magenta (chroma) */
#define TX_I8		TEX(9)		/* 8bpp intensity */
#define TX_A8		TEX(10)		/* 8bpp alpha (rgb = alpha) */
#define TX_AI44		TEX(11)		/* 8bpp alpha(4) intensity(4) */
#define TX_AI88		TEX(12)		/* 16bpp alpha(8) intensity(8) */
#define TX_8332		TEX(13)		/* 16bpp argb 8-3-3-2 */
#define TX_AYIQ		TEX(14)		/* 16bpp alpha(8) NCC */
#define TX_AP88		TEX(15)		/* 16bpp alpha(8) palette(8) */
#define TX_PRGBA	TEX(16)		/* 8bpp palette -> rgba */
#define TX_MIP		0x600000u	/* mip chain: one solid colour per level */

static volatile unsigned short *tx16(unsigned int off)
{
	return smoltdfx_vram + off / 2;
}

static volatile unsigned char *tx8(unsigned int off)
{
	return (volatile unsigned char *)smoltdfx_vram + off;
}

static unsigned int pack4444(unsigned int a, unsigned int r, unsigned int g,
			     unsigned int b)
{
	return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
}

static unsigned int pack1555(unsigned int a, unsigned int r, unsigned int g,
			     unsigned int b)
{
	return (a ? 0x8000u : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

static unsigned int pack332(unsigned int r, unsigned int g, unsigned int b)
{
	return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);
}

static void gen_textures(void)
{
	int u, v, i;
	volatile unsigned short *p;

	for (v = 0; v < TEXW; v++) {
		for (u = 0; u < TEXW; u++) {
			int cell = ((u >> 5) ^ (v >> 5)) & 1;
			int j = v * TEXW + u;

			tx16(TX_CHECK)[j] = cell ? smoltdfx_rgb565(255, 40, 40)
						 : smoltdfx_rgb565(255, 240, 40);
			tx16(TX_4444)[j] = cell ? pack4444(255, 40, 255, 255)
						: pack4444(255, 255, 40, 255);
			tx16(TX_1555)[j] = cell ? pack1555(1, 40, 255, 40)
						: pack1555(1, 40, 40, 255);
			tx8(TX_332)[j] = cell ? pack332(255, 128, 0)
					      : pack332(0, 128, 255);
			tx8(TX_PAL)[j] = (u ^ v) & 0xff;
			tx8(TX_NCC)[j] = ((v >> 4) << 4) |
					 (((u >> 6) & 3) << 2) | ((u >> 4) & 3);
		}
	}

	/* palette: a smooth HSV-ish rainbow */
	for (i = 0; i < 256; i++) {
		int r, g, bl, seg = i * 6 / 256, f = (i * 6) % 256;

		switch (seg) {
		case 0:
			r = 255; g = f; bl = 0;
			break;
		case 1:
			r = 255 - f; g = 255; bl = 0;
			break;
		case 2:
			r = 0; g = 255; bl = f;
			break;
		case 3:
			r = 0; g = 255 - f; bl = 255;
			break;
		case 4:
			r = f; g = 0; bl = 255;
			break;
		default:
			r = 255; g = 0; bl = 255 - f;
			break;
		}
		smoltdfx_palette(i, (r << 16) | (g << 8) | bl);
	}

	/* NCC table: Y ramp + red (I) and blue (Q) chroma steps */
	for (i = 0; i < 4; i++) {
		unsigned int y0 = (i * 4 + 0) * 17, y1 = (i * 4 + 1) * 17;
		unsigned int y2 = (i * 4 + 2) * 17, y3 = (i * 4 + 3) * 17;

		smoltdfx_w3(TDFX_3D_NCCTABLE0 + i * 4,
			    y0 | (y1 << 8) | (y2 << 16) | (y3 << 24));
	}
	for (i = 0; i < 4; i++) {
		smoltdfx_w3(TDFX_3D_NCCTABLE0 + 16 + i * 4, (i * 60) << 18);
		smoltdfx_w3(TDFX_3D_NCCTABLE0 + 32 + i * 4, i * 60);
	}

	/* bilinear probe: 4 distinct corner texels, rest flat grey */
	p = tx16(TX_BILIN);
	for (i = 0; i < TEXW * TEXW; i++)
		p[i] = smoltdfx_rgb565(60, 60, 60);
	p[0] = smoltdfx_rgb565(255, 0, 0);
	p[1] = smoltdfx_rgb565(0, 255, 0);
	p[TEXW] = smoltdfx_rgb565(0, 0, 255);
	p[TEXW + 1] = smoltdfx_rgb565(255, 255, 255);

	/* radial light map (grayscale) for the multitexture tile */
	for (v = 0; v < TEXW; v++) {
		for (u = 0; u < TEXW; u++) {
			float dx = (u - 128) / 128.0f, dy = (v - 128) / 128.0f;
			int l = (int)((1.0f - smoltdfx_sqrt(dx * dx + dy * dy)) * 255.0f);

			if (l < 0)
				l = 0;
			tx16(TX_LIGHT)[v * TEXW + u] = smoltdfx_rgb565(l, l, l);
		}
	}

	/* sprite: magenta field (the chroma key) with a cyan diamond */
	for (v = 0; v < TEXW; v++) {
		for (u = 0; u < TEXW; u++) {
			int dx = u - 128, dy = v - 128;

			if (dx < 0)
				dx = -dx;
			if (dy < 0)
				dy = -dy;
			tx16(TX_SPRITE)[v * TEXW + u] =
				(dx + dy < 96) ? smoltdfx_rgb565(0, 220, 255)
					       : smoltdfx_rgb565(255, 0, 255);
		}
	}
}

/*
 * Textures for the extra pixel formats.  Each is a horizontal ramp of the
 * format's value (intensity, 3-3-2 colour, or NCC/palette index, whose
 * tables gen_textures() sets up), so a tile sweeps every value the format
 * can carry left to right.  Alpha is left at full.
 */
static void gen_texfmt(void)
{
	int u, v, j;

	for (v = 0; v < TEXW; v++) {
		for (u = 0; u < TEXW; u++) {
			j = v * TEXW + u;
			int band = (u >> 5) << 5;	/* 8 wide value bands */

			tx8(TX_I8)[j] = u;
			tx8(TX_A8)[j] = u;
			tx8(TX_AI44)[j] = (0xf << 4) | (u >> 4);
			tx16(TX_AI88)[j] = (0xff << 8) | u;
			tx16(TX_8332)[j] = (0xffu << 8) | band;
			tx16(TX_AYIQ)[j] = (0xffu << 8) | band;
			tx16(TX_AP88)[j] = (0xffu << 8) | band;
			tx8(TX_PRGBA)[j] = band;
		}
	}
}

/*
 * A mip chain with a distinct solid colour per level, packed level after
 * level (the linear layout the hardware expects): level L is a 256>>L
 * square, so the level the sampler picks under minification shows as that
 * level's colour.
 */
static void gen_mip(void)
{
	static const unsigned int col[5] = {
		0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff,
	};
	unsigned int off = TX_MIP;
	int lvl, n, dim;

	for (lvl = 0; lvl < 5; lvl++) {
		unsigned short c = smoltdfx_rgb565((col[lvl] >> 16) & 0xff,
						   (col[lvl] >> 8) & 0xff,
						   col[lvl] & 0xff);

		dim = 256 >> lvl;
		for (n = 0; n < dim * dim; n++)
			tx16(off)[n] = c;
		off += dim * dim * 2;
	}
}

/* =========================== scene: texfmt ========================= */
/* one tile per otherwise-untested texture format, showing its decode */
static const struct {
	unsigned int base;
	int fmt;
} texfmt_tiles[8] = {
	{ TX_I8,	TDFX_TFMT_I8 },
	{ TX_A8,	TDFX_TFMT_A8 },
	{ TX_AI44,	TDFX_TFMT_AI44 },
	{ TX_AI88,	TDFX_TFMT_AI88 },
	{ TX_8332,	TDFX_TFMT_ARGB8332 },
	{ TX_AYIQ,	TDFX_TFMT_AYIQ },
	{ TX_AP88,	TDFX_TFMT_AP88 },
	{ TX_PRGBA,	TDFX_TFMT_P8_RGBA },
};

static void scene_texfmt(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H, cw = W / 4, ch = H / 2, i;

	(void)t;
	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_clear(0xff101018, 0xffff);

	for (i = 0; i < 8; i++) {
		int col = i & 3, row = i >> 2;
		int x0 = col * cw + 3, y0 = row * ch + 3;
		int x1 = (col + 1) * cw - 3, y1 = (row + 1) * ch - 3;

		smoltdfx_clip(x0, y0, x1, y1);
		smoltdfx_tex(texfmt_tiles[i].base, texfmt_tiles[i].fmt, 0,
			     SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(x0, y0, x1, y1, -1, -1, -1, -1, 256, 256);
	}
}

/* ============================ scene: grid =========================== */
#define GX		5
#define GY		4
#define INSET		3
#define NTILE		20		/* every tile drawn */

static void tile_rect(int idx, int *x0, int *y0, int *x1, int *y1)
{
	int cw = smoltdfx_W / GX, ch = smoltdfx_H / GY;
	int col = idx % GX, row = idx / GX;

	*x0 = col * cw + INSET;
	*y0 = row * ch + INSET;
	*x1 = (col + 1) * cw - INSET;
	*y1 = (row + 1) * ch - INSET;
}

/* reset to a clean per-tile state, clipped to the tile */
static void tile_begin(int x0, int y0, int x1, int y1)
{
	smoltdfx_alpha_off();
	smoltdfx_fog_off();
	smoltdfx_tex1_off();
	smoltdfx_tex_off();
	smoltdfx_fbz = TDFX_FBZ_RGBWRMASK | (TDFX_ZF_GT << TDFX_FBZ_ZFUNC_SHIFT);
	smoltdfx_zfunc(7);			/* ALWAYS */
	smoltdfx_clip(x0, y0, x1, y1);		/* also enables ENCLIP */
}

static void draw_tile(int idx, float t)
{
	int x0, y0, x1, y1;
	float fx0, fy0, fx1, fy1, cx, cy;

	tile_rect(idx, &x0, &y0, &x1, &y1);
	tile_begin(x0, y0, x1, y1);
	fx0 = x0; fy0 = y0; fx1 = x1; fy1 = y1;
	cx = (fx0 + fx1) * 0.5f;
	cy = (fy0 + fy1) * 0.5f;

	switch (idx) {
	case 0:	/* flat fill through the clip rect */
		smoltdfx_setupmode(SM_BASE);
		smoltdfx_quad(fx0, fy0, fx1, fy1, 0xff2060c0, 0xff2060c0,
			      0xff2060c0, 0xff2060c0, 0, 0);
		break;
	case 1:	/* Gouraud quad */
		smoltdfx_setupmode(SM_BASE);
		smoltdfx_quad(fx0, fy0, fx1, fy1, 0xffff0000, 0xff00ff00,
			      0xff0000ff, 0xffffff00, 0, 0);
		break;
	case 2:	/* depth-buffered spinning cube */
		smoltdfx_depth(1, TDFX_ZF_LT);
		draw_cube(cx, cy, (fx1 - fx0) * 0.55f, t);
		break;
	case 3:	/* point-sampled RGB565 checker */
		smoltdfx_tex(TX_CHECK, TDFX_TFMT_RGB565, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 4:	/* bilinear magnification (2x2 texels blended) */
		smoltdfx_tex(TX_BILIN, TDFX_TFMT_RGB565, TDFX_TEX_MAGFILTER,
			     SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 1, 1);
		break;
	case 5:	/* ARGB4444 */
		smoltdfx_tex(TX_4444, TDFX_TFMT_ARGB4444, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 6:	/* ARGB1555 */
		smoltdfx_tex(TX_1555, TDFX_TFMT_ARGB1555, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 7:	/* RGB332 (8bpp) */
		smoltdfx_tex(TX_332, TDFX_TFMT_RGB332, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 8:	/* P8 palette (rainbow CLUT) */
		smoltdfx_tex(TX_PAL, TDFX_TFMT_P8, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 9:	/* NCC (YIQ) */
		smoltdfx_tex(TX_NCC, TDFX_TFMT_YIQ, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 10: {	/* alpha blend: translucent bar sliding over a checker */
		float w = fx1 - fx0;
		float bx = fx0 + (0.5f + 0.4f * smoltdfx_sin(t)) * w * 0.5f;

		smoltdfx_tex(TX_CHECK, TDFX_TFMT_RGB565, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		smoltdfx_tex_off();
		smoltdfx_blend(TDFX_BLEND_SRCALPHA, TDFX_BLEND_OMSRCALPHA);
		smoltdfx_setupmode(SM_A);
		smoltdfx_quad(bx, fy0, bx + w * 0.35f, fy1, 0x80ffffff,
			      0x80ffffff, 0x80ffffff, 0x80ffffff, 0, 0);
		break;
	}
	case 11: {	/* alpha test: columns of pass (a>128) / fail alpha */
		int k;
		float w = (fx1 - fx0) / 6.0f;

		smoltdfx_alpha_test(TDFX_ZF_GT, 128);	/* GREATER, ref = 128 */
		smoltdfx_setupmode(SM_A);
		for (k = 0; k < 6; k++) {
			unsigned int a = (k & 1) ? 0xffu : 0x40u;
			unsigned int c = (a << 24) | 0x0000ff00;	/* green */

			smoltdfx_quad(fx0 + k * w, fy0, fx0 + (k + 1) * w, fy1,
				      c, c, c, c, 0, 0);
		}
		break;
	}
	case 12:	/* table fog: near->far gradient (eye-W varies across x) */
		smoltdfx_fog_table(0x2040ff);
		smoltdfx_setupmode(SM_BASE);
		/*
		 * SWOOWFBI is 1/w; sweep 1.0 (near) -> 0.0002 (far) across x.
		 * Two explicit triangles (per-vertex W rules out smoltdfx_quad)
		 * so it does not depend on strip/fan assembly.
		 */
		smoltdfx_vtx(fx0, fy0, 1.0f, 0xffffffff, 0, 0, 1.0f, 1);
		smoltdfx_vtx(fx1, fy0, 1.0f, 0xffffffff, 0, 0, 0.0002f, 0);
		smoltdfx_vtx(fx1, fy1, 1.0f, 0xffffffff, 0, 0, 0.0002f, 0);

		smoltdfx_vtx(fx0, fy0, 1.0f, 0xffffffff, 0, 0, 1.0f, 1);
		smoltdfx_vtx(fx1, fy1, 1.0f, 0xffffffff, 0, 0, 0.0002f, 0);
		smoltdfx_vtx(fx0, fy1, 1.0f, 0xffffffff, 0, 0, 1.0f, 0);
		break;
	case 13:	/* ordered dither: smooth dark gradient */
		smoltdfx_dither(1, 0);
		smoltdfx_setupmode(SM_BASE);
		smoltdfx_quad(fx0, fy0, fx1, fy1, 0xff000000, 0xff000000,
			      0xff283040, 0xff283040, 0, 0);
		break;
	case 14:	/* chroma-key: magenta punched out of a sprite */
		smoltdfx_chroma(1, 0xff00ff);
		smoltdfx_tex(TX_SPRITE, TDFX_TFMT_RGB565, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 15:	/* dual-TMU: checker (TMU0) x light map on downstream TMU1 */
		smoltdfx_tex(TX_CHECK, TDFX_TFMT_RGB565, 0, SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_tex1(TX_LIGHT, TDFX_TFMT_RGB565, 0, SMOLTDFX_TC_MODULATE);
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(fx0, fy0, fx1, fy1, -1, -1, -1, -1, 256, 256);
		break;
	case 16:	/* stipple: 8x4 pattern (checker) punches holes in a fill */
		smoltdfx_stipple(1, 0xaa55aa55u);
		smoltdfx_setupmode(SM_BASE);
		smoltdfx_quad(fx0, fy0, fx1, fy1, 0xff40ff40, 0xff40ff40,
			      0xff40ff40, 0xff40ff40, 0, 0);
		break;
	case 17: {	/* Y-origin: draw in flipped screen space (appears inverted) */
		int fh = smoltdfx_H;
		float dy0 = (fh - 1) - fy1, dy1 = (fh - 1) - fy0;

		smoltdfx_clip(x0, (fh - 1) - y1, x1, (fh - 1) - y0);
		smoltdfx_yorigin(1);
		smoltdfx_setupmode(SM_BASE);
		smoltdfx_quad(fx0, dy0, fx1, dy1, 0xffffffff, 0xff000000,
			      0xffffffff, 0xff000000, 0, 0);
		break;
	}
	case 18: {	/* 2D host-to-screen blit: RGB565 pixels from host memory */
		static unsigned short img[128 * 120];	/* one 640x480 grid tile */
		int w = x1 - x0, h = y1 - y0;
		int u, v;

		for (v = 0; v < h; v++)
			for (u = 0; u < w; u++) {
				int r = u * 255 / w, g = v * 255 / h;

				img[v * w + u] = smoltdfx_rgb565(r, g,
								 (u ^ v) & 0xff);
			}
		smoltdfx_blt_rgb565(x0, y0, w, h, img);
		break;
	}
	case 19: {	/* spinning textured quad (rotated 3D triangles) */
		static const signed char qx[4] = { -1, 1, 1, -1 };
		static const signed char qy[4] = { -1, -1, 1, 1 };
		static const short qs[4] = { 0, 256, 256, 0 };
		static const short qt[4] = { 0, 0, 256, 256 };
		static const int qi[6] = { 0, 1, 2, 0, 2, 3 };
		float c = smoltdfx_cos(t), s = smoltdfx_sin(t);
		float r = (fx1 - fx0) * 0.35f;
		float px[4], py[4];
		int k;

		for (k = 0; k < 4; k++) {
			px[k] = cx + (qx[k] * c - qy[k] * s) * r;
			py[k] = cy + (qx[k] * s + qy[k] * c) * r;
		}
		smoltdfx_tex(TX_CHECK, TDFX_TFMT_RGB565, TDFX_TEX_MAGFILTER,
			     SMOLTDFX_TC_PASS, TEXCP);
		smoltdfx_setupmode(SM_TEX);
		for (k = 0; k < 6; k++) {
			int j = qi[k];

			smoltdfx_vtx(px[j], py[j], 1.0f, -1, qs[j], qt[j],
				     1.0f, k % 3 == 0);
		}
		break;
	}
	}
}

static void scene_grid(float t)
{
	int i;

	smoltdfx_target();
	smoltdfx_clip_full();
	/* clear colour AND depth (enable depth writes for the fast-fill) */
	smoltdfx_fbz = TDFX_FBZ_RGBWRMASK | TDFX_FBZ_ENDEPTH |
		       TDFX_FBZ_DEPTHWRMASK | (TDFX_ZF_GT << TDFX_FBZ_ZFUNC_SHIFT);
	smoltdfx_w3(TDFX_3D_FBZMODE, smoltdfx_fbz);
	smoltdfx_clear(0xff000000, 0xffff);

	for (i = 0; i < NTILE; i++)
		draw_tile(i, t);
}

/* =========================== scene: twod =========================== */
/* the 2D engine on its own: rectangle fills and screen-to-screen blits */
static void scene_twod(float t)
{
	static const unsigned short bar[6] = {
		0xf800, 0xfd20, 0xffe0, 0x07e0, 0x001f, 0xf81f,
	};
	int W = smoltdfx_W, H = smoltdfx_H, bw = W / 6, s = H / 4, i;
	int sx = 40, sy = H / 2;

	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_clear(0xff101018, 0xffff);

	/* a row of solid colour bars, each a rectangle fill */
	for (i = 0; i < 6; i++)
		smoltdfx_rectfill(i * bw, 0, bw - 2, s, bar[i]);

	/* a Gouraud source block, then screen-to-screen copies of it */
	smoltdfx_setupmode(SM_BASE);
	smoltdfx_quad(sx, sy, sx + s, sy + s, 0xffff0000, 0xff0000ff,
		      0xff00ff00, 0xffffff00, 0, 0);
	for (i = 1; i <= 3; i++) {
		int dx = sx + i * (s + 20);
		int dy = sy + (int)(20.0f * smoltdfx_sin(t + i));

		smoltdfx_blt_s2s(sx, sy, dx, dy, s, s);
	}
}

/* =========================== scene: clamp ========================== */
/* texture coordinates run 0..2: wrap repeats the texture, clamp holds edge */
static void scene_clamp(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H;
	float mid = W * 0.5f;

	(void)t;
	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_clear(0xff101018, 0xffff);

	/* left half: wrap (default) - the sprite tiles 2x2 */
	smoltdfx_tex(TX_SPRITE, TDFX_TFMT_RGB565, 0, SMOLTDFX_TC_PASS, TEXCP);
	smoltdfx_setupmode(SM_TEX);
	smoltdfx_quad(0, 0, mid - 2, H, -1, -1, -1, -1, 512, 512);

	/* right half: clamp - one sprite, its edge texels stretched outward */
	smoltdfx_tex(TX_SPRITE, TDFX_TFMT_RGB565,
		     TDFX_TEX_CLAMPS | TDFX_TEX_CLAMPT, SMOLTDFX_TC_PASS, TEXCP);
	smoltdfx_setupmode(SM_TEX);
	smoltdfx_quad(mid, 0, W, H, -1, -1, -1, -1, 512, 512);
}

/* ============================ scene: fog =========================== */
/*
 * the fog unit's factor sources: eye-W table, iterated alpha, iterated Z,
 * and the constant fogColor add.  one quadrant each.
 */
static void scene_fog(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H, mx = W / 2, my = H / 2;

	(void)t;
	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_clear(0xff101018, 0xffff);
	smoltdfx_fbz = TDFX_FBZ_RGBWRMASK | (TDFX_ZF_GT << TDFX_FBZ_ZFUNC_SHIFT);
	smoltdfx_zfunc(7);				/* ALWAYS: no depth test */

	/* TL: table fog, factor from eye-W (near left -> far right) */
	smoltdfx_clip(0, 0, mx - 1, my - 1);
	smoltdfx_fog_table(0x2040ff);
	smoltdfx_setupmode(SM_BASE);
	smoltdfx_vtx(0,  0,  1, 0xffff8000, 0, 0, 1.0f,    1);
	smoltdfx_vtx(mx, 0,  1, 0xffff8000, 0, 0, 0.0002f, 0);
	smoltdfx_vtx(mx, my, 1, 0xffff8000, 0, 0, 0.0002f, 0);
	smoltdfx_vtx(0,  0,  1, 0xffff8000, 0, 0, 1.0f,    1);
	smoltdfx_vtx(mx, my, 1, 0xffff8000, 0, 0, 0.0002f, 0);
	smoltdfx_vtx(0,  my, 1, 0xffff8000, 0, 0, 1.0f,    0);
	smoltdfx_fog_off();

	/* TR: factor from iterated alpha (left a=0 -> pixel, right a=255 -> fog) */
	smoltdfx_clip(mx, 0, W - 1, my - 1);
	smoltdfx_fog(0x20ff40, TDFX_FOG_ALPHA);
	smoltdfx_setupmode(SM_A);
	smoltdfx_quad(mx, 0, W, my, 0x00ff8000, 0x00ff8000,
		      0xffff8000, 0xffff8000, 0, 0);
	smoltdfx_fog_off();

	/* BL: factor from iterated Z (left z=0 -> pixel, right z=max -> fog) */
	smoltdfx_clip(0, my, mx - 1, H - 1);
	smoltdfx_fog(0xff4020, TDFX_FOG_Z);
	smoltdfx_setupmode(SM_BASE);
	smoltdfx_vtx(0,  my, 0,     0xff20a0ff, 0, 0, 1.0f, 1);
	smoltdfx_vtx(mx, my, 65000, 0xff20a0ff, 0, 0, 1.0f, 0);
	smoltdfx_vtx(mx, H,  65000, 0xff20a0ff, 0, 0, 1.0f, 0);
	smoltdfx_vtx(0,  my, 0,     0xff20a0ff, 0, 0, 1.0f, 1);
	smoltdfx_vtx(mx, H,  65000, 0xff20a0ff, 0, 0, 1.0f, 0);
	smoltdfx_vtx(0,  H,  0,     0xff20a0ff, 0, 0, 1.0f, 0);
	smoltdfx_fog_off();

	/* BR: constant fog, fogColor added to a grey gradient */
	smoltdfx_clip(mx, my, W - 1, H - 1);
	smoltdfx_fog(0x000060, TDFX_FOG_CONSTANT);
	smoltdfx_setupmode(SM_BASE);
	smoltdfx_quad(mx, my, W, H, 0xff000000, 0xff808080,
		      0xff404040, 0xffc0c0c0, 0, 0);
	smoltdfx_fog_off();
}

/* =========================== scene: minif ========================= */
/*
 * Minification / mip LOD selection: draw the same mipmapped texture at
 * shrinking sizes.  Mapping the whole texture across an s-pixel square
 * minifies it by 256/s, so the sampler steps up one mip level each time s
 * halves, and each square shows that level's solid colour.
 */
static void scene_minif(float t)
{
	static const int size[5] = { 256, 128, 64, 32, 16 };
	int H = smoltdfx_H, x = 20, y = H / 3, i;

	(void)t;
	smoltdfx_target();
	smoltdfx_clip_full();
	smoltdfx_clear(0xff101018, 0xffff);

	smoltdfx_tex(TX_MIP, TDFX_TFMT_RGB565,
		     TDFX_TEX_MINFILTER | TDFX_TEX_MAGFILTER, SMOLTDFX_TC_PASS,
		     TEXCP);
	smoltdfx_lod(0, 4);
	for (i = 0; i < 5; i++) {
		smoltdfx_setupmode(SM_TEX);
		smoltdfx_quad(x, y, x + size[i], y + size[i],
			      -1, -1, -1, -1, 256, 256);
		x += size[i] + 10;
	}
}

/* ------------------------------ driver ------------------------------- */
static void draw_scene(int scene, float t)
{
	if (scene == SC_BASIC)
		scene_basic(t);
	else if (scene == SC_CUBES)
		scene_cubes(t);
	else if (scene == SC_TWOD)
		scene_twod(t);
	else if (scene == SC_CLAMP)
		scene_clamp(t);
	else if (scene == SC_TEXFMT)
		scene_texfmt(t);
	else if (scene == SC_FOG)
		scene_fog(t);
	else if (scene == SC_MINIF)
		scene_minif(t);
	else
		scene_grid(t);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int main(int argc, char **argv)
{
	const char *rdev = "/dev/tdfx3d", *fdev = "/dev/fb0";
	const char *tag = "grid";
	int scene = SC_GRID, do_ppm = 0, npos = 0, i;
	float t;

	for (i = 1; i < argc; i++) {
		if (streq(argv[i], "basic")) {
			scene = SC_BASIC;
			tag = "basic";
		} else if (streq(argv[i], "cubes")) {
			scene = SC_CUBES;
			tag = "cubes";
		} else if (streq(argv[i], "grid")) {
			scene = SC_GRID;
			tag = "grid";
		} else if (streq(argv[i], "twod")) {
			scene = SC_TWOD;
			tag = "twod";
		} else if (streq(argv[i], "clamp")) {
			scene = SC_CLAMP;
			tag = "clamp";
		} else if (streq(argv[i], "texfmt")) {
			scene = SC_TEXFMT;
			tag = "texfmt";
		} else if (streq(argv[i], "fog")) {
			scene = SC_FOG;
			tag = "fog";
		} else if (streq(argv[i], "minif")) {
			scene = SC_MINIF;
			tag = "minif";
		} else if (streq(argv[i], "dump")) {
			do_ppm = 1;
		} else if (npos++ == 0) {
			rdev = argv[i];
		} else {
			fdev = argv[i];
		}
	}

	mount("none", "/dev", "devtmpfs", 0, 0);

	if (smoltdfx_init(rdev, fdev) < 0) {
		static const char m[] = "tdfx3d_demo: init failed\n";

		write(2, m, sizeof(m) - 1);
		for (;;)
			usleep(1000000);
	}
	if (scene == SC_GRID || scene == SC_CLAMP || scene == SC_TEXFMT)
		gen_textures();
	if (scene == SC_TEXFMT)
		gen_texfmt();
	if (scene == SC_MINIF)
		gen_mip();

	/* canonical frame + digest for QEMU-vs-hardware comparison */
	draw_scene(scene, CANON_T);
	{
		unsigned int off = smoltdfx_cur ? smoltdfx_back : smoltdfx_front;

		smoltdfx_digest(tag, off, (int)(CANON_T * 1000));
		if (do_ppm)
			smoltdfx_dump_ppm("/tmp/smoltdfx.ppm", off);
	}
	smoltdfx_present();

	for (t = 0.0f;; t += 0.02f) {
		draw_scene(scene, t);
		smoltdfx_present();
	}
	return 0;
}
