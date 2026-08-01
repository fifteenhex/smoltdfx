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

/* ---------------------- low-level register access -------------------- */
static inline void smoltdfx_w3(unsigned int off, unsigned int v)
{
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

/* target the back (render) colour buffer */
static inline void smoltdfx_target(void)
{ smoltdfx_w3(TDFX_3D_COLBUFFERADDR,
	      smoltdfx_cur ? smoltdfx_back : smoltdfx_front); }

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

/* enable/disable depth testing (and depth writes) with a compare func */
static inline void smoltdfx_depth(int enable, unsigned int func)
{
	if (enable)
		smoltdfx_fbz |= TDFX_FBZ_ENDEPTH | TDFX_FBZ_DEPTHWRMASK;
	else
		smoltdfx_fbz &= ~(TDFX_FBZ_ENDEPTH | TDFX_FBZ_DEPTHWRMASK);
	smoltdfx_zfunc(func);
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

/* --------------------------- geometry -------------------------------- */
static inline void smoltdfx_setupmode(unsigned int mode)
{
	smoltdfx_w3(TDFX_3D_SSETUPMODE, mode);
}

/* one setup-unit vertex (texcoords are ignored until texturing is on) */
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
	smoltdfx_w3(first ? TDFX_3D_SBEGINTRICMD : TDFX_3D_SDRAWTRICMD, 1);
}

/*
 * Draw an axis-aligned quad as two independent triangles (each begun with
 * its own SBEGIN), so it does not depend on the setup unit's strip/fan
 * vertex assembly.  Corner colours (Gouraud) via the named args; pass
 * texel span (s1,t1) = 0 for untextured.
 */
static inline void smoltdfx_quad(float x0, float y0, float x1, float y1,
				 unsigned int cTL, unsigned int cBL,
				 unsigned int cTR, unsigned int cBR,
				 float s1, float t1)
{
	smoltdfx_vtx(x0, y0, 1.0f, cTL, 0,  0,  1.0f, 1);	/* TL */
	smoltdfx_vtx(x1, y0, 1.0f, cTR, s1, 0,  1.0f, 0);	/* TR */
	smoltdfx_vtx(x1, y1, 1.0f, cBR, s1, t1, 1.0f, 0);	/* BR */

	smoltdfx_vtx(x0, y0, 1.0f, cTL, 0,  0,  1.0f, 1);	/* TL */
	smoltdfx_vtx(x1, y1, 1.0f, cBR, s1, t1, 1.0f, 0);	/* BR */
	smoltdfx_vtx(x0, y1, 1.0f, cBL, 0,  t1, 1.0f, 0);	/* BL */
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

static inline void smoltdfx__emit(const char *fmt, ...)
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
	smoltdfx__emit("== smoltdfx digest: %s ==\n", tag);
	smoltdfx__emit("mode %ux%u t=%d.%03d sum 0x%08x\n", smoltdfx_W,
		       smoltdfx_H, milli / 1000, milli % 1000,
		       smoltdfx_checksum(off));
	for (r = 0; r < 6; r++) {
		int y = (2 * r + 1) * (int)smoltdfx_H / 12;

		smoltdfx__emit("r%d:", r);
		for (c = 0; c < 8; c++) {
			int x = (2 * c + 1) * (int)smoltdfx_W / 16;

			smoltdfx__emit(" %04x", smoltdfx_peek(off, x, y));
		}
		smoltdfx__emit("\n");
	}
	smoltdfx__emit("== end ==\n");
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
