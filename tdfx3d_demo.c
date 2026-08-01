/*
 * tdfx3d_demo - a visual test for the 3dfx Voodoo3 3D engine, built on
 * smoltdfx.h.
 *
 * It renders a full-screen Gouraud-shaded background with a solid quad
 * bouncing across it, double-buffered and synced to the vblank.
 *
 * At start-up it renders one canonical frame (a fixed animation phase)
 * and prints a digest of it to the serial console, then animates.  The
 * digest (a whole-frame checksum plus a fixed sample grid) lets a QEMU
 * run and a real-hardware run be compared directly.  Pass "dump" as an
 * argument to also write the canonical frame to /tmp/smoltdfx.ppm.
 *
 * Freestanding (nolibc); build with the Makefile here.  Boot the card
 * with e.g. tdfxfb.mode_option=640x480-16@60 (RGB565, fb at VRAM 0).
 */
#include "smoltdfx.h"

#define SM_BASE		(TDFX_SSETUP_RGB | TDFX_SSETUP_Z | TDFX_SSETUP_WFBI)
#define CANON_T		1.234f		/* canonical animation phase */

/* render one full frame at animation phase t into the back buffer */
static void draw_frame(float t)
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

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	int do_ppm = 0;
	float t;
	int i;

	for (i = 1; i < argc; i++)
		if (argv[i][0] == 'd' && argv[i][1] == 'u')
			do_ppm = 1;

	mount("none", "/dev", "devtmpfs", 0, 0);

	if (smoltdfx_init(rdev, fdev) < 0) {
		static const char m[] = "tdfx3d_demo: init failed\n";

		write(2, m, sizeof(m) - 1);
		for (;;)
			usleep(1000000);
	}

	/* canonical frame + digest for QEMU-vs-hardware comparison */
	draw_frame(CANON_T);
	{
		unsigned int off = smoltdfx_cur ? smoltdfx_back : smoltdfx_front;

		smoltdfx_digest("basic-rendering", off, (int)(CANON_T * 1000));
		if (do_ppm)
			smoltdfx_dump_ppm("/tmp/smoltdfx.ppm", off);
	}
	smoltdfx_present();

	for (t = 0.0f;; t += 0.03f) {
		draw_frame(t);
		smoltdfx_present();
	}
	return 0;
}
