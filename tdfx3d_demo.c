/*
 * tdfx3d_demo - a visual test for the 3dfx Voodoo3 3D engine, built on
 * smoltdfx.h.
 *
 * It renders a full-screen Gouraud-shaded background with a solid quad
 * bouncing across it, double-buffered and synced to the vblank.
 *
 *
 * Freestanding (nolibc); build with the Makefile here.  Boot the card
 * with e.g. tdfxfb.mode_option=640x480-16@60 (RGB565, fb at VRAM 0).
 */
#include "smoltdfx.h"

#define SM_BASE		(TDFX_SSETUP_RGB | TDFX_SSETUP_Z | TDFX_SSETUP_WFBI)

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
	float t;

	mount("none", "/dev", "devtmpfs", 0, 0);

	if (smoltdfx_init(rdev, fdev) < 0) {
		static const char m[] = "tdfx3d_demo: init failed\n";

		write(2, m, sizeof(m) - 1);
		for (;;)
			usleep(1000000);
	}

	for (t = 0.0f;; t += 0.03f) {
		draw_frame(t);
		smoltdfx_present();
	}
	return 0;
}
