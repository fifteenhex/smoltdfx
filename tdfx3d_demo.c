/*
 * tdfx3d_demo - a visual test for the 3dfx Voodoo3 3D engine, built on
 * smoltdfx.h.
 *
 * It offers selectable scenes (pick one on the command line):
 *   basic  - a full-screen Gouraud background with a bouncing quad
 *   cubes  - several depth-buffered Gouraud cubes orbiting the screen
 *
 * At start-up it renders one canonical frame (a fixed animation phase) of
 * the selected scene and prints a digest of it to the serial console,
 * then animates.  The digest (a whole-frame checksum plus a fixed sample
 * grid) lets a QEMU run and a real-hardware run be compared, and the
 * selectable scenes make per-feature regression testing possible.
 *
 * Usage: tdfx3d_demo [/dev/tdfx3d] [/dev/fb0] [basic|cubes] [dump]
 *
 * Freestanding (nolibc); build with the Makefile here.  Boot the card
 * with e.g. tdfxfb.mode_option=640x480-16@60 (RGB565, fb at VRAM 0).
 */
#include "smoltdfx.h"

#define SM_BASE		(TDFX_SSETUP_RGB | TDFX_SSETUP_Z | TDFX_SSETUP_WFBI)
#define CANON_T		1.234f		/* canonical animation phase */

enum { SC_BASIC, SC_CUBES };

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

/* ============================ scene: cubes ========================== */
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

/* ------------------------------ driver ------------------------------- */
static void draw_scene(int scene, float t)
{
	if (scene == SC_CUBES)
		scene_cubes(t);
	else
		scene_basic(t);
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
	const char *tag = "cubes";
	int scene = SC_CUBES, do_ppm = 0, npos = 0, i;
	float t;

	for (i = 1; i < argc; i++) {
		if (streq(argv[i], "basic")) {
			scene = SC_BASIC;
			tag = "basic";
		} else if (streq(argv[i], "cubes")) {
			scene = SC_CUBES;
			tag = "cubes";
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
