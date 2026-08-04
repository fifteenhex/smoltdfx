/*
 * smoltdfx.h - a tiny freestanding (nolibc) library for driving the 3dfx
 * Voodoo3 ("Avenger"/SST) 3D + 2D engine through a /dev/tdfx3d misc
 * device (added by out-of-tree patches) and the /dev/fbN VRAM mapping.
 *
 * It wraps the raw register pokes into a small state-machine API:
 *   - smoltdfx_init()     open the device, learn the mode, set up buffers
 *   - smoltdfx_clear()    fast-fill the framebuffer
 *   - smoltdfx_present()  swap-buffer at vblank
 *   - smoltdfx_vtx()/quad() feed the setup unit (Gouraud triangles)
 *   - smoltdfx_digest()   dump a comparable summary of the rendered frame
 *
 * Register offsets and bitfields all come from <linux/tdfx3d.h>.
 *
 * Single translation unit only (uses file-static state).
 */
#ifndef SMOLTDFX_H
#define SMOLTDFX_H

#include <linux/tdfx3d.h>
#include <linux/fb.h>
#include <stdarg.h>

/* =============================== state =============================== */
static volatile unsigned char *smoltdfx_regs;	/* BAR0 register aperture */
static volatile unsigned short *smoltdfx_vram;	/* VRAM (via /dev/fbN) */
static int smoltdfx_fd;				/* /dev/tdfx3d */
static unsigned int smoltdfx_W, smoltdfx_H, smoltdfx_stride;/* mode from fbdev */
static unsigned int smoltdfx_front, smoltdfx_back, smoltdfx_aux;/* buffer offsets */
static unsigned int smoltdfx_cur;			/* which buffer is the back one */
static unsigned int smoltdfx_fbz;			/* shadow of fbzMode */
static unsigned int smoltdfx_alpha;			/* shadow of alphaMode */
static unsigned int smoltdfx_vram_size;			/* mapped VRAM length (smem_len) */
static unsigned int smoltdfx_vram_usable;		/* VRAM below the cmdFifo ring */

/*
 * DMA command FIFO (cmdFifo0): batch register writes into a VRAM ring the card
 * pulls from, committed with one `bump` per flush, instead of a MMIO write (a
 * PCI transaction) per command.  Opt-in:
 * smoltdfx uses direct PIO until smoltdfx_cmdfifo_init() is called.  The ring
 * is a contiguous buffer: smoltdfx_cmd_mem is the CPU pointer we append to,
 * smoltdfx_cmd_hw the byte address the card reads from, wp/committed offsets
 * from the start.  Register offsets / packet format are HW-validated.
 */
static int smoltdfx_cmd_on;				/* route writes via cmdFifo */
static volatile unsigned char *smoltdfx_cmd_mem;	/* CPU ptr to ring base */
static unsigned int smoltdfx_cmd_hw;			/* card base addr (VRAM offset) */
static unsigned int smoltdfx_cmd_size;			/* ring bytes */
static unsigned int smoltdfx_cmd_wp, smoltdfx_cmd_committed;	/* 0..size */
static inline void smoltdfx_wait_idle(void);		/* defined below */

#define SMOLTDFX_CMD_BASEADDRL	0x80020
#define SMOLTDFX_CMD_BASESIZE	0x80024
#define SMOLTDFX_CMD_BUMP	0x80028
#define SMOLTDFX_CMD_RDPTRL	0x8002c
#define SMOLTDFX_CMD_EN		(1u << 8)
#define SMOLTDFX_CMD_AGP	(1u << 9)		/* ring in system RAM */

static inline void smoltdfx_cmd_creg(unsigned int off, unsigned int v)
{
	*(volatile unsigned int *)(smoltdfx_regs + off) = v;	/* absolute aperture */
}
/* commit the packets appended since the last flush (advance the card) */
static inline void smoltdfx_cmd_flush(void)
{
	if (smoltdfx_cmd_wp != smoltdfx_cmd_committed) {
		smoltdfx_cmd_creg(SMOLTDFX_CMD_BUMP,
				  smoltdfx_cmd_wp - smoltdfx_cmd_committed);
		smoltdfx_cmd_committed = smoltdfx_cmd_wp;
	}
}
/* ring full: commit, drain, and rewind the read/write pointers to the start */
static inline void smoltdfx_cmd_wrap(void)
{
	smoltdfx_cmd_flush();
	smoltdfx_wait_idle();
	smoltdfx_cmd_wp = smoltdfx_cmd_committed = 0;
	smoltdfx_cmd_creg(SMOLTDFX_CMD_RDPTRL, smoltdfx_cmd_hw);
}
/* append a one-word register-write packet (PKT1, NWORDS=1) to the ring */
static inline void smoltdfx_cmd_pkt1(unsigned int off, unsigned int val, int is2d)
{
	volatile unsigned int *r;

	if (smoltdfx_cmd_wp + 8 > smoltdfx_cmd_size)
		smoltdfx_cmd_wrap();
	r = (volatile unsigned int *)(smoltdfx_cmd_mem + smoltdfx_cmd_wp);
	r[0] = 1u | (((off >> 2) & 0x3ff) << 3) | (is2d ? (1u << 14) : 0) |
	       (1u << 16);
	r[1] = val;
	smoltdfx_cmd_wp += 8;
}

/* reserve `nwords` contiguously before the ring end (drain + wrap if not) */
static inline void smoltdfx_cmd_reserve(unsigned int nwords)
{
	if (smoltdfx_cmd_wp + nwords * 4 > smoltdfx_cmd_size)
		smoltdfx_cmd_wrap();
}
static inline void smoltdfx_cmd_word(unsigned int v)	/* append (space reserved) */
{
	*(volatile unsigned int *)(smoltdfx_cmd_mem + smoltdfx_cmd_wp) = v;
	smoltdfx_cmd_wp += 4;
}
static inline void smoltdfx_cmd_wordf(float f)
{
	union { float f; unsigned int u; } c;

	c.f = f;
	smoltdfx_cmd_word(c.u);
}

/*
 * PKT3 native-vertex packets: stream a whole triangle strip into the ring in
 * the setup unit's packed layout (one packet per <=15-vertex chunk) instead of
 * ~30 register writes per triangle.  HW-validated by the tdfx_pkt3 probe.  Each
 * vertex is X, Y, then the params their PMASK bit selects, in canonical order.
 */
#define SMOLTDFX_PKT3		3u
#define SMOLTDFX_PKT3_TRIS	(0u << 3)	/* BDDBDD: independent tris, begin/3 */
#define SMOLTDFX_PKT3_STRIP	(1u << 3)	/* BDDDDD: begin, then draws */
#define SMOLTDFX_PKT3_CONT	(2u << 3)	/* DDDDDD: strip continuation */
#define SMOLTDFX_PKT3_PACKED	(1u << 28)	/* colour is one packed ARGB word */
#define SMOLTDFX_P_RGB		(1u << 0)
#define SMOLTDFX_P_Z		(1u << 2)
#define SMOLTDFX_P_WFBI		(1u << 3)
#define SMOLTDFX_P_ST0		(1u << 5)

/* words a PKT3 vertex occupies for `pmask` (packed colour) */
static inline unsigned int smoltdfx_pkt3_vsize(unsigned int pmask)
{
	unsigned int n = 2;				/* X, Y always */

	if (pmask & SMOLTDFX_P_RGB)  n += 1;		/* packed ARGB */
	if (pmask & SMOLTDFX_P_Z)    n += 1;
	if (pmask & SMOLTDFX_P_WFBI) n += 1;
	if (pmask & SMOLTDFX_P_ST0)  n += 2;		/* S0, T0 */
	return n;
}
/* PKT3 header for a chunk of `nvert` (<=15) vertices; cmd = STRIP or CONT */
static inline void smoltdfx_cmd_pkt3_hdr(unsigned int pmask, unsigned int cmd,
					 unsigned int nvert)
{
	smoltdfx_cmd_word(SMOLTDFX_PKT3 | cmd | ((nvert & 0xf) << 6) |
			  ((pmask & 0xfff) << 10) | SMOLTDFX_PKT3_PACKED);
}
/* one packed vertex, params in canonical order matching `pmask` */
static inline void smoltdfx_cmd_pkt3_vtx(unsigned int pmask, float x, float y,
					 unsigned int argb, float z, float w,
					 float s0, float t0)
{
	smoltdfx_cmd_wordf(x);
	smoltdfx_cmd_wordf(y);
	if (pmask & SMOLTDFX_P_RGB)  smoltdfx_cmd_word(argb);
	if (pmask & SMOLTDFX_P_Z)    smoltdfx_cmd_wordf(z);
	if (pmask & SMOLTDFX_P_WFBI) smoltdfx_cmd_wordf(w);
	if (pmask & SMOLTDFX_P_ST0)  { smoltdfx_cmd_wordf(s0); smoltdfx_cmd_wordf(t0); }
}

/*
 * PKT5 linear data burst: write `nwords` inline data words into VRAM starting
 * at byte address `base` (LFB space) -- e.g. a texture upload.  The payload
 * travels through the ring, so there is no system-RAM DMA.  Caller reserves
 * 2 + nwords and appends the payload with smoltdfx_cmd_word().  HW-validated by
 * the tdfx_pkt5 probe.
 */
#define SMOLTDFX_PKT5		5u
static inline void smoltdfx_cmd_pkt5_hdr(unsigned int base, unsigned int nwords)
{
	smoltdfx_cmd_word(SMOLTDFX_PKT5 | (nwords << 3));	/* space LFB, byten 0 */
	smoltdfx_cmd_word(base & 0x1ffffff);
}

/* ---------------------- low-level register access -------------------- */
static inline void smoltdfx_w3(unsigned int off, unsigned int v)
{
	if (smoltdfx_cmd_on) {
		smoltdfx_cmd_pkt1(off, v, 0);
		return;
	}
	*(volatile unsigned int *)(smoltdfx_regs + TDFX_3D_BASE + off) = v;
}
static inline unsigned int smoltdfx_rio(unsigned int off)
{
	return *(volatile unsigned int *)(smoltdfx_regs + TDFX_IO_BASE + off);
}
static inline void smoltdfx_wf(unsigned int off, float f)
{
	union { float f; unsigned int u; } c;

	c.f = f;
	smoltdfx_w3(off, c.u);
}

/* --------------------------- freestanding math ---------------------- */
static inline float smoltdfx_fabs(float x)
{
	return x < 0 ? -x : x;
}
static inline float smoltdfx_sin(float x)
{
	const float PI = 3.14159265f, TWO_PI = 6.2831853f;
	float y;

	while (x < -PI)
		x += TWO_PI;
	while (x >  PI)
		x -= TWO_PI;
	y = 1.27323954f * x - 0.405284735f * x * smoltdfx_fabs(x);
	return 0.225f * (y * smoltdfx_fabs(y) - y) + y;
}
static inline float smoltdfx_cos(float x)
{
	return smoltdfx_sin(x + 1.5707963f);
}
static inline float smoltdfx_sqrt(float x)
{
	union { float f; int i; } u;
	float y;

	if (x <= 0)
		return 0;
	u.f = x; u.i = 0x5f3759df - (u.i >> 1);
	y = u.f; y = y * (1.5f - 0.5f * x * y * y);
	y = y * (1.5f - 0.5f * x * y * y);
	return x * y;			/* x * rsqrt(x) = sqrt(x) */
}
static inline unsigned int smoltdfx_rgb565(unsigned int r, unsigned int g, unsigned int b)
{
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* ---------------------------- lifecycle ------------------------------ */
/*
 * Open the register device + the framebuffer VRAM, learn the mode the
 * fbdev is in, lay out front/back/depth buffers, and program the common
 * 3D state.  Returns 0 on success, -1 on failure.
 */
static int smoltdfx_init(const char *regdev, const char *fbdev)
{
	struct fb_fix_screeninfo fix;
	int ffd;

	smoltdfx_fd = open(regdev, O_RDWR);
	if (smoltdfx_fd < 0)
		return -1;
	smoltdfx_regs = mmap(0, 0x400000, PROT_READ | PROT_WRITE, MAP_SHARED,
			     smoltdfx_fd, 0);
	if (smoltdfx_regs == (void *)-1)
		return -1;

	ffd = open(fbdev, O_RDWR);
	if (ffd < 0)
		return -1;
	if (ioctl(ffd, FBIOGET_FSCREENINFO, &fix))
		return -1;
	smoltdfx_vram = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE,
			     MAP_SHARED, ffd, 0);
	if (smoltdfx_vram == (void *)-1)
		return -1;
	smoltdfx_vram_size = fix.smem_len;
	smoltdfx_vram_usable = fix.smem_len;	/* whole of VRAM until a ring is carved */

	smoltdfx_W = smoltdfx_rio(TDFX_IO_VIDSCREENSIZE) & 0xfff;
	smoltdfx_H = (smoltdfx_rio(TDFX_IO_VIDSCREENSIZE) >> 12) & 0xfff;
	smoltdfx_stride = smoltdfx_rio(TDFX_IO_VIDDESKSTRIDE) & 0x7fff;
	if (!smoltdfx_W || !smoltdfx_H || !smoltdfx_stride)
		return -1;

	smoltdfx_front = 0;
	smoltdfx_back  = smoltdfx_H * smoltdfx_stride;
	smoltdfx_aux   = 2 * smoltdfx_back;
	smoltdfx_cur   = 1;		/* render to the back buffer first */

	smoltdfx_w3(TDFX_3D_COLBUFFERSTRIDE, smoltdfx_stride);
	smoltdfx_w3(TDFX_3D_AUXBUFFERADDR, smoltdfx_aux);
	smoltdfx_w3(TDFX_3D_AUXBUFFERSTRIDE, smoltdfx_stride);
	smoltdfx_w3(TDFX_3D_CLIPLEFTRIGHT, smoltdfx_W);
	smoltdfx_w3(TDFX_3D_CLIPBOTTOMTOP, smoltdfx_H);
	smoltdfx_w3(TDFX_3D_FBZCOLORPATH, TDFX_CP_RGB_ITERATED);
	smoltdfx_w3(TDFX_3D_ALPHAMODE, 0);
	smoltdfx_w3(TDFX_3D_FOGMODE, 0);
	smoltdfx_fbz = TDFX_FBZ_RGBWRMASK |
		       (TDFX_ZF_GT << TDFX_FBZ_ZFUNC_SHIFT);
	smoltdfx_w3(TDFX_3D_FBZMODE, smoltdfx_fbz);
	return 0;
}

/*
 * Carve a command-FIFO ring out of the top of VRAM and route subsequent
 * register writes through it (opt-in; smoltdfx uses PIO until this is called).
 * ring_bytes is rounded up to whole 4KB pages.  Returns 0 on success, -1 if the
 * VRAM size is unknown or too small.
 */
static inline int smoltdfx_cmdfifo_init(unsigned int ring_bytes)
{
	unsigned int pages, base;

	if (!smoltdfx_vram_size)
		return -1;
	pages = (ring_bytes + 0xfffu) >> 12;
	if (!pages)
		pages = 1;
	if ((pages << 12) >= smoltdfx_vram_size)
		return -1;
	base = (smoltdfx_vram_size - (pages << 12)) & ~0xfffu;
	smoltdfx_cmd_mem = (volatile unsigned char *)smoltdfx_vram + base;
	smoltdfx_cmd_hw = base;			/* VRAM byte offset */
	smoltdfx_cmd_size = pages << 12;
	smoltdfx_cmd_wp = smoltdfx_cmd_committed = 0;
	smoltdfx_vram_usable = base;		/* buffers/textures stay below the ring */
	smoltdfx_cmd_creg(SMOLTDFX_CMD_BASEADDRL, base >> 12);
	smoltdfx_cmd_creg(SMOLTDFX_CMD_RDPTRL, base);
	smoltdfx_cmd_creg(SMOLTDFX_CMD_BASESIZE, (pages - 1) | SMOLTDFX_CMD_EN);
	smoltdfx_cmd_on = 1;
	return 0;
}

/*
 * Alternative to smoltdfx_cmdfifo_init(): put the ring in SYSTEM RAM instead of
 * VRAM, for a card that can bus-master.  The kernel allocates a DMA-coherent
 * ring (TDFX3D_ALLOC_CMDFIFO) and returns its PCI bus address; we mmap it and
 * program the card with that address + the AGP bit.  VRAM is left entirely free
 * for textures.  Returns 0 on success, -1 if the driver lacks the ioctl or the
 * mapping fails.
 */
static inline int smoltdfx_cmdfifo_init_sysram(unsigned int ring_bytes)
{
	struct tdfx3d_cmdfifo cf;
	unsigned int pages;
	void *p;

	cf.size = ring_bytes ? ring_bytes : 4096;
	cf.bus_addr = 0;
	if (ioctl(smoltdfx_fd, TDFX3D_ALLOC_CMDFIFO, &cf) < 0)
		return -1;
	p = mmap(0, cf.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		 smoltdfx_fd, TDFX3D_CMDFIFO_MMAP_OFFSET);
	if (p == (void *)-1)
		return -1;
	pages = cf.size >> 12;
	smoltdfx_cmd_mem = (volatile unsigned char *)p;
	smoltdfx_cmd_hw   = cf.bus_addr;	/* PCI bus address */
	smoltdfx_cmd_size = cf.size;
	smoltdfx_cmd_wp = smoltdfx_cmd_committed = 0;
	smoltdfx_cmd_creg(SMOLTDFX_CMD_BASEADDRL, cf.bus_addr >> 12);
	smoltdfx_cmd_creg(SMOLTDFX_CMD_RDPTRL, cf.bus_addr);
	smoltdfx_cmd_creg(SMOLTDFX_CMD_BASESIZE,
			  (pages - 1) | SMOLTDFX_CMD_EN | SMOLTDFX_CMD_AGP);
	smoltdfx_cmd_on = 1;
	return 0;
}

/* VRAM byte offset of the current render (back) buffer */
static inline unsigned int smoltdfx_drawbuf(void)
{
	return smoltdfx_cur ? smoltdfx_back : smoltdfx_front;
}

/* target the back (render) colour buffer */
static inline void smoltdfx_target(void)
{ smoltdfx_w3(TDFX_3D_COLBUFFERADDR, smoltdfx_drawbuf()); }

/* fast-fill the clip rectangle with an ARGB colour and a depth value */
static inline void smoltdfx_clear(unsigned int argb, unsigned int depth)
{
	smoltdfx_w3(TDFX_3D_C1, argb);
	smoltdfx_w3(TDFX_3D_ZACOLOR, depth & 0xffff);
	smoltdfx_w3(TDFX_3D_FASTFILLCMD, 1);
}

/* present the back buffer at the next vblank and flip */
static inline void smoltdfx_present(void)
{
	smoltdfx_w3(TDFX_3D_SWAPBUFFERCMD, TDFX_SWAP_WAIT_VSYNC);
	ioctl(smoltdfx_fd, TDFX3D_WAIT_VBLANK, 0);
	smoltdfx_cur ^= 1;
	smoltdfx_target();
}

/* ------------------------- fbzMode state ----------------------------- */
static inline void smoltdfx_fbz_bit(unsigned int mask, int on)
{
	if (on)
		smoltdfx_fbz |= mask;
	else
		smoltdfx_fbz &= ~mask;
	smoltdfx_w3(TDFX_3D_FBZMODE, smoltdfx_fbz);
}
static inline void smoltdfx_zfunc(unsigned int func)
{
	smoltdfx_fbz = (smoltdfx_fbz & ~TDFX_FBZ_ZFUNC_MASK) |
		       ((func & 7) << TDFX_FBZ_ZFUNC_SHIFT);
	smoltdfx_w3(TDFX_3D_FBZMODE, smoltdfx_fbz);
}

/* enable depth testing (and depth writes) with a compare func */
static inline void smoltdfx_depth(unsigned int func)
{
	smoltdfx_fbz |= TDFX_FBZ_ENDEPTH | TDFX_FBZ_DEPTHWRMASK;
	smoltdfx_zfunc(func);
}
static inline void smoltdfx_depth_off(void)
{
	smoltdfx_fbz &= ~(TDFX_FBZ_ENDEPTH | TDFX_FBZ_DEPTHWRMASK);
	smoltdfx_w3(TDFX_3D_FBZMODE, smoltdfx_fbz);
}

static inline void smoltdfx_clip(int x0, int y0, int x1, int y1)
{
	smoltdfx_w3(TDFX_3D_CLIPLEFTRIGHT, ((x0 & 0x3ff) << 16) | (x1 & 0x3ff));
	smoltdfx_w3(TDFX_3D_CLIPBOTTOMTOP, ((y0 & 0x3ff) << 16) | (y1 & 0x3ff));
	smoltdfx_fbz_bit(TDFX_FBZ_ENCLIP, 1);
}
static inline void smoltdfx_clip_off(void)
{
	smoltdfx_fbz_bit(TDFX_FBZ_ENCLIP, 0);
}
/*
 * Reset the clip rectangle to the whole screen and disable clipping.  The
 * fast-fill command fills the CLIP RECTANGLE (not the whole buffer), so a
 * full-screen clear must set this first.
 */
static inline void smoltdfx_clip_full(void)
{
	smoltdfx_w3(TDFX_3D_CLIPLEFTRIGHT, smoltdfx_W);
	smoltdfx_w3(TDFX_3D_CLIPBOTTOMTOP, smoltdfx_H);
	smoltdfx_fbz_bit(TDFX_FBZ_ENCLIP, 0);
}

/* --------------------------- texturing ------------------------------- */
/* textureMode combine preset: Ctmu = the fetched texel (replace) */
#define SMOLTDFX_TC_PASS	((1u << 12) | (1u << 18) | (1u << 21) | (1u << 27))

static inline void smoltdfx_colorpath(unsigned int v)
{
	smoltdfx_w3(TDFX_3D_FBZCOLORPATH, v);
}

/*
 * Enable TMU0 texturing.  base = VRAM byte offset of a 256x256 texture,
 * fmt = TDFX_TFMT_*, filter = 0 (point) or TDFX_TEX_MAGFILTER/MINFILTER,
 * combine = a textureMode combine preset (e.g. SMOLTDFX_TC_PASS), cpath
 * the fbzColorPath value (usually TDFX_CP_RGB_TEXTURE | TDFX_CP_TEXTURE_EN).
 */
static inline void smoltdfx_tex(unsigned int base, int fmt, unsigned int filter,
				unsigned int combine, unsigned int cpath)
{
	smoltdfx_w3(TDFX_3D_TEXBASEADDR, base);
	smoltdfx_w3(TDFX_3D_TLOD, 0);			/* 256x256, LOD 0 */
	smoltdfx_w3(TDFX_3D_TEXTUREMODE,
		    (fmt << TDFX_TEX_TFORMAT_SHIFT) | filter | combine);
	smoltdfx_w3(TDFX_3D_FBZCOLORPATH, cpath);
}

static inline void smoltdfx_tex_off(void)
{
	smoltdfx_w3(TDFX_3D_TEXTUREMODE, 0);
	smoltdfx_w3(TDFX_3D_FBZCOLORPATH, TDFX_CP_RGB_ITERATED);
}

/*
 * Select the active mip range for the current texture.  lodmin/lodmax are
 * integer levels (level L is a 256>>L square); call after smoltdfx_tex(),
 * which resets the range to level 0 only.  The tLOD fields are 4.2 fixed.
 */
static inline void smoltdfx_lod(unsigned int lodmin, unsigned int lodmax)
{
	smoltdfx_w3(TDFX_3D_TLOD,
		    ((lodmin << 2) & 0x3f) | (((lodmax << 2) & 0x3f) << 6));
}

/* download one 0xRRGGBB CLUT entry (P8/AP88), broadcast to both TMUs */
static inline void smoltdfx_palette(int idx, unsigned int rgb)
{
	smoltdfx_w3(TDFX_3D_NCCTABLE0 + 16 + (idx & 7) * 4,
		    0x80000000u | ((idx & 0xfe) << 23) | (rgb & 0xffffff));
}

/* textureMode combine preset: Ctmu = this TMU's texel * the "other" colour */
#define SMOLTDFX_TC_MODULATE	((1u << 14) | (1u << 17) | (1u << 23) | (1u << 26))

/* enable TMU1 - its colour becomes the "other" input to TMU0's combine */
static inline void smoltdfx_tex1(unsigned int base, int fmt, unsigned int filter,
				 unsigned int combine)
{
	smoltdfx_w3(TDFX_3D_TMU1 + TDFX_3D_TEXBASEADDR, base);
	smoltdfx_w3(TDFX_3D_TMU1 + TDFX_3D_TLOD, 0);
	smoltdfx_w3(TDFX_3D_TMU1 + TDFX_3D_TEXTUREMODE,
		    (fmt << TDFX_TEX_TFORMAT_SHIFT) | filter | combine);
}

static inline void smoltdfx_tex1_off(void)
{
	smoltdfx_w3(TDFX_3D_TMU1 + TDFX_3D_TEXTUREMODE, 0);
}

/*
 * Single-pass multitexturing: sample base0 on TMU0 (passed through) and base1
 * on TMU1, which modulates (multiplies) it -- the classic base x light-map
 * combine done in one pass.  Both TMUs use the shared iterated texcoords.
 * Disable afterwards with smoltdfx_tex1_off() then smoltdfx_tex_off().
 */
static inline void smoltdfx_multitex(unsigned int base0, int fmt0,
				     unsigned int filt0, unsigned int base1,
				     int fmt1, unsigned int filt1)
{
	smoltdfx_tex(base0, fmt0, filt0, SMOLTDFX_TC_PASS,
		     TDFX_CP_RGB_TEXTURE | TDFX_CP_TEXTURE_EN);
	smoltdfx_tex1(base1, fmt1, filt1, SMOLTDFX_TC_MODULATE);
}

/* ------------------------- alpha / fog ------------------------------- */
/*
 * Alpha blend and alpha test share the one alphaMode register.  Like fbzMode,
 * it is kept in a shadow (smoltdfx_alpha) and each helper updates only its own
 * fields, so blend and test COMPOSE: call both to test the incoming pixels and
 * blend the survivors in one pass.  smoltdfx_alpha_off() clears everything.
 */
#define SMOLTDFX_ALPHA_BLENDBITS (TDFX_ALPHA_ENBLEND | \
		(0xfu << TDFX_ALPHA_SRCFUNC_SHIFT) | (0xfu << TDFX_ALPHA_DSTFUNC_SHIFT))
#define SMOLTDFX_ALPHA_TESTBITS (TDFX_ALPHA_ENTEST | \
		(7u << TDFX_ALPHA_FUNC_SHIFT) | (0xffu << TDFX_ALPHA_REF_SHIFT))

static inline void smoltdfx_blend(int srcf, int dstf)
{
	smoltdfx_alpha = (smoltdfx_alpha & ~SMOLTDFX_ALPHA_BLENDBITS) |
			 TDFX_ALPHA_ENBLEND |
			 (srcf << TDFX_ALPHA_SRCFUNC_SHIFT) |
			 (dstf << TDFX_ALPHA_DSTFUNC_SHIFT);
	smoltdfx_w3(TDFX_3D_ALPHAMODE, smoltdfx_alpha);
}

static inline void smoltdfx_alpha_test(int func, int ref)
{
	smoltdfx_alpha = (smoltdfx_alpha & ~SMOLTDFX_ALPHA_TESTBITS) |
			 TDFX_ALPHA_ENTEST |
			 ((func & 7) << TDFX_ALPHA_FUNC_SHIFT) |
			 ((ref & 0xff) << TDFX_ALPHA_REF_SHIFT);
	smoltdfx_w3(TDFX_3D_ALPHAMODE, smoltdfx_alpha);
}

static inline void smoltdfx_alpha_off(void)
{
	smoltdfx_alpha = 0;
	smoltdfx_w3(TDFX_3D_ALPHAMODE, 0);
}

/* table fog indexed by eye-W: fill all 64 entries with a linear 0->255 ramp */
static inline void smoltdfx_fog_table(unsigned int color)
{
	int i;

	smoltdfx_w3(TDFX_3D_FOGCOLOR, color & 0xffffff);
	for (i = 0; i < 32; i++) {
		unsigned int lo = (i * 2) * 255 / 63;
		unsigned int hi = (i * 2 + 1) * 255 / 63;

		smoltdfx_w3(TDFX_3D_FOGTABLE + i * 4, (lo << 8) | (hi << 24));
	}
	smoltdfx_w3(TDFX_3D_FOGMODE, TDFX_FOG_ENABLE);
}

/*
 * Enable fog with an explicit fogMode: the factor comes from iterated alpha
 * (TDFX_FOG_ALPHA) or Z (TDFX_FOG_Z), or fogColor is added as a constant
 * (TDFX_FOG_CONSTANT).  No fog table is needed for these modes.
 */
static inline void smoltdfx_fog(unsigned int color, unsigned int mode)
{
	smoltdfx_w3(TDFX_3D_FOGCOLOR, color & 0xffffff);
	smoltdfx_w3(TDFX_3D_FOGMODE, TDFX_FOG_ENABLE | mode);
}
static inline void smoltdfx_fog_off(void)
{
	smoltdfx_w3(TDFX_3D_FOGMODE, 0);
}

/* ------------------------- raster ops -------------------------------- */
/* ordered dither: twobytwo selects the 2x2 matrix (else the default 4x4) */
static inline void smoltdfx_dither(int twobytwo)
{
	if (twobytwo)
		smoltdfx_fbz |= TDFX_FBZ_DITHER2X2;
	else
		smoltdfx_fbz &= ~TDFX_FBZ_DITHER2X2;
	smoltdfx_fbz_bit(TDFX_FBZ_ENDITHER, 1);
}
static inline void smoltdfx_dither_off(void)
{
	smoltdfx_fbz &= ~TDFX_FBZ_DITHER2X2;
	smoltdfx_fbz_bit(TDFX_FBZ_ENDITHER, 0);
}

/* chroma-key: discard pixels whose colour equals `key` (0xRRGGBB) */
static inline void smoltdfx_chroma(unsigned int key)
{
	smoltdfx_w3(TDFX_3D_CHROMAKEY, key & 0xffffff);
	smoltdfx_fbz_bit(TDFX_FBZ_ENCHROMAKEY, 1);
}
static inline void smoltdfx_chroma_off(void)
{
	smoltdfx_fbz_bit(TDFX_FBZ_ENCHROMAKEY, 0);
}

/*
 * 4x4-pattern stipple: the low 16 bits of `pattern` mask a repeating 4x4 grid
 * of pixels (bit set = drawn).  smoltdfx_stipple_off() disables stippling and
 * clears the mode bit so the fbzMode shadow returns to a clean state.
 */
static inline void smoltdfx_stipple(unsigned int pattern)
{
	smoltdfx_w3(TDFX_3D_STIPPLE, pattern);
	smoltdfx_fbz |= TDFX_FBZ_STIPPLEPATTERN;		/* 4x4 mode */
	smoltdfx_fbz_bit(TDFX_FBZ_ENSTIPPLE, 1);
}
static inline void smoltdfx_stipple_off(void)
{
	smoltdfx_fbz &= ~TDFX_FBZ_STIPPLEPATTERN;
	smoltdfx_fbz_bit(TDFX_FBZ_ENSTIPPLE, 0);
}

/*
 * Y-origin swap: flip the vertical axis about miscInit0[29:18].  Enabling it
 * loads the pivot with screenHeight-1 (read-modify-write to keep the
 * memory-config bits).
 */
static inline void smoltdfx_yorigin(void)
{
	volatile unsigned int *m = (volatile unsigned int *)
		(smoltdfx_regs + TDFX_IO_BASE + TDFX_IO_MISCINIT0);

	*m = (*m & ~(0xfffu << 18)) | ((smoltdfx_H - 1) << 18);
	smoltdfx_fbz_bit(TDFX_FBZ_YORIGIN, 1);
}
static inline void smoltdfx_yorigin_off(void)
{
	smoltdfx_fbz_bit(TDFX_FBZ_YORIGIN, 0);
}

/* --------------------------- geometry -------------------------------- */
static inline void smoltdfx_setupmode(unsigned int mode)
{
	smoltdfx_w3(TDFX_3D_SSETUPMODE, mode);
}

/* one setup-unit vertex; the texcoords are shared by TMU0 and TMU1 */
static inline void smoltdfx_vtx(float x, float y, float z, unsigned int argb,
				float s, float t, float w, int first)
{
	smoltdfx_wf(TDFX_3D_SVX, x);
	smoltdfx_wf(TDFX_3D_SVY, y);
	smoltdfx_wf(TDFX_3D_SVZ, z);
	smoltdfx_wf(TDFX_3D_SWOOWFBI, w);
	smoltdfx_w3(TDFX_3D_SARGB, argb);
	smoltdfx_wf(TDFX_3D_SSOW0, s);
	smoltdfx_wf(TDFX_3D_STOW0, t);
	smoltdfx_wf(TDFX_3D_SSOW1, s);
	smoltdfx_wf(TDFX_3D_STOW1, t);
	smoltdfx_w3(first ? TDFX_3D_SBEGINTRICMD : TDFX_3D_SDRAWTRICMD, 1);
}

/*
 * Draw an axis-aligned quad as two independent triangles (each begun with
 * its own SBEGIN), so it does not depend on the setup unit's strip/fan
 * vertex assembly.  Corner colours (Gouraud) are given in reading order --
 * top-left, top-right, bottom-left, bottom-right; pass texel span (s1,t1) = 0
 * for untextured.
 */
static inline void smoltdfx_quad(float x0, float y0, float x1, float y1,
				 unsigned int cTL, unsigned int cTR,
				 unsigned int cBL, unsigned int cBR,
				 float s1, float t1)
{
	smoltdfx_vtx(x0, y0, 1.0f, cTL, 0,  0,  1.0f, 1);	/* TL */
	smoltdfx_vtx(x1, y0, 1.0f, cTR, s1, 0,  1.0f, 0);	/* TR */
	smoltdfx_vtx(x1, y1, 1.0f, cBR, s1, t1, 1.0f, 0);	/* BR */

	smoltdfx_vtx(x0, y0, 1.0f, cTL, 0,  0,  1.0f, 1);	/* TL */
	smoltdfx_vtx(x1, y1, 1.0f, cBR, s1, t1, 1.0f, 0);	/* BR */
	smoltdfx_vtx(x0, y1, 1.0f, cBL, 0,  t1, 1.0f, 0);	/* BL */
}

/*
 * A point as a size x size quad centred on (x,y).  The Voodoo3 has no
 * native point primitive, so it is drawn as a quad.  The caller sets the
 * setup mode (SM_BASE for a flat point).
 */
static inline void smoltdfx_point(float x, float y, float size,
				  unsigned int argb)
{
	float h = size * 0.5f;

	smoltdfx_quad(x - h, y - h, x + h, y + h, argb, argb, argb, argb, 0, 0);
}

/*
 * A line of the given width as a rectangle (two triangles) built from the
 * segment thickened along its perpendicular, since the setup unit only
 * draws triangles.  The caller sets the setup mode.
 */
static inline void smoltdfx_line(float x0, float y0, float x1, float y1,
				 float width, unsigned int argb)
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = smoltdfx_sqrt(dx * dx + dy * dy);
	float nx, ny;

	if (len < 0.001f) {
		smoltdfx_point(x0, y0, width, argb);
		return;
	}
	nx = -dy / len * width * 0.5f;
	ny =  dx / len * width * 0.5f;

	smoltdfx_vtx(x0 + nx, y0 + ny, 1.0f, argb, 0, 0, 1.0f, 1);
	smoltdfx_vtx(x1 + nx, y1 + ny, 1.0f, argb, 0, 0, 1.0f, 0);
	smoltdfx_vtx(x1 - nx, y1 - ny, 1.0f, argb, 0, 0, 1.0f, 0);

	smoltdfx_vtx(x0 + nx, y0 + ny, 1.0f, argb, 0, 0, 1.0f, 1);
	smoltdfx_vtx(x1 - nx, y1 - ny, 1.0f, argb, 0, 0, 1.0f, 0);
	smoltdfx_vtx(x0 - nx, y0 - ny, 1.0f, argb, 0, 0, 1.0f, 0);
}

/* =============================== 2D engine =========================== */
static inline void smoltdfx_wait_idle(void);		/* defined below */

static inline void smoltdfx_w2(unsigned int off, unsigned int v)
{
	if (smoltdfx_cmd_on) {
		smoltdfx_cmd_pkt1(off, v, 1);
		return;
	}
	*(volatile unsigned int *)(smoltdfx_regs + TDFX_2D_BASE + off) = v;
}

/*
 * Host-to-screen colour blit: stream a w x h block of RGB565 pixels from
 * host memory into the current render target at (dx,dy).  Source pixels
 * are packed two per 32-bit launch word with each row padded to a 32-bit
 * boundary, which is how the blitter consumes a 16bpp colour source.
 */
static inline void smoltdfx_blt_rgb565(int dx, int dy, int w, int h,
				       const unsigned short *src)
{
	unsigned int dst = smoltdfx_drawbuf();
	int x, y;

	smoltdfx_wait_idle();
	smoltdfx_w2(TDFX_2D_CLIP0MIN, 0);
	smoltdfx_w2(TDFX_2D_CLIP0MAX, (smoltdfx_H << 16) | smoltdfx_W);
	smoltdfx_w2(TDFX_2D_DSTBASE, dst);
	smoltdfx_w2(TDFX_2D_DSTFORMAT, (3u << 16) | smoltdfx_stride);
	smoltdfx_w2(TDFX_2D_SRCFORMAT, 3u << 16);
	smoltdfx_w2(TDFX_2D_DSTSIZE, (h << 16) | w);
	smoltdfx_w2(TDFX_2D_DSTXY, (dy << 16) | dx);
	smoltdfx_w2(TDFX_2D_COMMAND, TDFX_2D_OP_H2S_BLT | TDFX_2D_CMD_INITIATE |
		    (TDFX_2D_ROP_SRCCOPY << 24));

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x += 2) {
			unsigned int word = src[y * w + x];

			if (x + 1 < w)
				word |= (unsigned int)src[y * w + x + 1] << 16;
			smoltdfx_w2(TDFX_2D_LAUNCH, word);
		}
	}
	smoltdfx_wait_idle();
}

/* fill a w x h rectangle at (dx,dy) in the render target with an RGB565 colour */
static inline void smoltdfx_rectfill(int dx, int dy, int w, int h,
				     unsigned int color)
{
	unsigned int dst = smoltdfx_drawbuf();

	smoltdfx_wait_idle();
	smoltdfx_w2(TDFX_2D_CLIP0MIN, 0);
	smoltdfx_w2(TDFX_2D_CLIP0MAX, (smoltdfx_H << 16) | smoltdfx_W);
	smoltdfx_w2(TDFX_2D_DSTBASE, dst);
	smoltdfx_w2(TDFX_2D_DSTFORMAT, (3u << 16) | smoltdfx_stride);
	smoltdfx_w2(TDFX_2D_COLORFORE, color);
	smoltdfx_w2(TDFX_2D_DSTSIZE, (h << 16) | w);
	smoltdfx_w2(TDFX_2D_DSTXY, (dy << 16) | dx);
	smoltdfx_w2(TDFX_2D_COMMAND, TDFX_2D_OP_RECTFILL | TDFX_2D_CMD_INITIATE |
		    (TDFX_2D_ROP_SRCCOPY << 24));
	smoltdfx_wait_idle();
}

/*
 * Screen-to-screen blit: copy a w x h block from (sx,sy) to (dx,dy) within
 * the render target.  Copies top-left to bottom-right, so it is safe for
 * non-overlapping regions (as used here).
 */
static inline void smoltdfx_blt_s2s(int sx, int sy, int dx, int dy,
				    int w, int h)
{
	unsigned int buf = smoltdfx_drawbuf();

	smoltdfx_wait_idle();
	smoltdfx_w2(TDFX_2D_CLIP0MIN, 0);
	smoltdfx_w2(TDFX_2D_CLIP0MAX, (smoltdfx_H << 16) | smoltdfx_W);
	smoltdfx_w2(TDFX_2D_SRCBASE, buf);
	smoltdfx_w2(TDFX_2D_SRCFORMAT, (3u << 16) | smoltdfx_stride);
	smoltdfx_w2(TDFX_2D_DSTBASE, buf);
	smoltdfx_w2(TDFX_2D_DSTFORMAT, (3u << 16) | smoltdfx_stride);
	smoltdfx_w2(TDFX_2D_DSTSIZE, (h << 16) | w);
	smoltdfx_w2(TDFX_2D_SRCXY, (sy << 16) | sx);
	smoltdfx_w2(TDFX_2D_DSTXY, (dy << 16) | dx);
	smoltdfx_w2(TDFX_2D_COMMAND, TDFX_2D_OP_S2S_BLT | TDFX_2D_CMD_INITIATE |
		    (TDFX_2D_ROP_SRCCOPY << 24));
	smoltdfx_wait_idle();
}

/*
 * Composite present: copy the finished offscreen (back) buffer to the
 * scanned-out front buffer with a 2D blit, synced to vblank.  Unlike a page
 * flip (SWAPBUFFERCMD), this does not rely on the CRTC start address following
 * the swap, so the display always shows a complete frame -- no tearing /
 * partial-render noise.  The caller renders to the back buffer every frame
 * (smoltdfx_cur == 1) and never flips.
 */
static inline void smoltdfx_composite(void)
{
	smoltdfx_wait_idle();			/* finish the 3D frame offscreen */
	ioctl(smoltdfx_fd, TDFX3D_WAIT_VBLANK, 0);
	smoltdfx_w2(TDFX_2D_CLIP0MIN, 0);
	smoltdfx_w2(TDFX_2D_CLIP0MAX, (smoltdfx_H << 16) | smoltdfx_W);
	smoltdfx_w2(TDFX_2D_SRCBASE, smoltdfx_back);
	smoltdfx_w2(TDFX_2D_SRCFORMAT, (3u << 16) | smoltdfx_stride);
	smoltdfx_w2(TDFX_2D_DSTBASE, smoltdfx_front);
	smoltdfx_w2(TDFX_2D_DSTFORMAT, (3u << 16) | smoltdfx_stride);
	smoltdfx_w2(TDFX_2D_DSTSIZE, (smoltdfx_H << 16) | smoltdfx_W);
	smoltdfx_w2(TDFX_2D_SRCXY, 0);
	smoltdfx_w2(TDFX_2D_DSTXY, 0);
	smoltdfx_w2(TDFX_2D_COMMAND, TDFX_2D_OP_S2S_BLT | TDFX_2D_CMD_INITIATE |
		    (TDFX_2D_ROP_SRCCOPY << 24));
	smoltdfx_wait_idle();
	/*
	 * The 2D blit only marks the front buffer dirty; the scanout address is
	 * moved by a buffer swap.  Present the (now-whole) front buffer, then
	 * restore the back buffer as the 3D render target for the next frame.
	 */
	smoltdfx_w3(TDFX_3D_COLBUFFERADDR, smoltdfx_front);
	smoltdfx_w3(TDFX_3D_SWAPBUFFERCMD, 0);
	smoltdfx_w3(TDFX_3D_COLBUFFERADDR, smoltdfx_back);
	smoltdfx_cmd_flush();			/* push the present out immediately */
}

/* ============================ diagnostics ============================ *
 * A self-describing dump of the rendered frame so a QEMU run and a real
 * hardware run can be compared without transferring images: a whole-frame
 * checksum plus a fixed 8x6 grid of sampled RGB565 pixels, printed to
 * stdout (the serial console under the test initramfs).  The sample grid
 * is content-independent.  smoltdfx_dump_ppm() also writes the frame out.
 */
static inline void smoltdfx_wait_idle(void)
{
	int i;

	smoltdfx_cmd_flush();		/* commit any queued cmdFifo packets first */
	for (i = 0; i < 2000000; i++)
		if (!(*(volatile unsigned int *)(smoltdfx_regs + TDFX_3D_BASE +
					     TDFX_3D_STATUS) & TDFX_STATUS_BUSY))
			break;
}

/* read one pixel back from the buffer at byte offset `off` */
static inline unsigned int smoltdfx_peek(unsigned int off, int x, int y)
{
	return smoltdfx_vram[off / 2 + (unsigned int)y * smoltdfx_W + (unsigned int)x];
}

/* FNV-1a hash over the whole visible buffer */
static inline unsigned int smoltdfx_checksum(unsigned int off)
{
	unsigned int h = 2166136261u, n = smoltdfx_W * smoltdfx_H, i;
	volatile unsigned short *p = smoltdfx_vram + off / 2;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static inline void smoltdfx_emit(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		write(1, b, n > (int)sizeof(b) ? (int)sizeof(b) : n);
}

/* print a comparable digest of the buffer at byte offset `off` */
static inline void smoltdfx_digest(const char *tag, unsigned int off, int milli)
{
	int r, c;

	smoltdfx_wait_idle();
	smoltdfx_emit("== smoltdfx digest: %s ==\n", tag);
	smoltdfx_emit("mode %ux%u t=%d.%03d sum 0x%08x\n", smoltdfx_W,
		       smoltdfx_H, milli / 1000, milli % 1000,
		       smoltdfx_checksum(off));
	for (r = 0; r < 6; r++) {
		int y = (2 * r + 1) * (int)smoltdfx_H / 12;

		smoltdfx_emit("r%d:", r);
		for (c = 0; c < 8; c++) {
			int x = (2 * c + 1) * (int)smoltdfx_W / 16;

			smoltdfx_emit(" %04x", smoltdfx_peek(off, x, y));
		}
		smoltdfx_emit("\n");
	}
	smoltdfx_emit("== end ==\n");
}

/* write the buffer at byte offset `off` as a binary PPM (P6) */
static inline void smoltdfx_dump_ppm(const char *path, unsigned int off)
{
	static unsigned char row[2048 * 3];
	char hdr[64];
	int fd, x, y, n;

	smoltdfx_wait_idle();
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	n = snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", smoltdfx_W, smoltdfx_H);
	write(fd, hdr, n);
	for (y = 0; y < (int)smoltdfx_H; y++) {
		for (x = 0; x < (int)smoltdfx_W; x++) {
			unsigned int p = smoltdfx_peek(off, x, y);

			row[x * 3 + 0] = ((p >> 11) & 0x1f) * 255 / 31;
			row[x * 3 + 1] = ((p >> 5) & 0x3f) * 255 / 63;
			row[x * 3 + 2] = (p & 0x1f) * 255 / 31;
		}
		write(fd, row, smoltdfx_W * 3);
	}
	close(fd);
}

#endif /* SMOLTDFX_H */
