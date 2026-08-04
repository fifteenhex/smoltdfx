/*
 * smooth - Gouraud (smooth) shading on smolminigl.
 *
 * One of a set of small OpenGL-1.x demos, each teaching a single concept the
 * way the classic OpenGL Programming Guide ("Red Book") examples do, but
 * written from scratch against smolminigl (its OpenGL subset on the Voodoo3).
 *
 * Concept: a triangle given a different primary colour at each corner.  With
 * smooth shading (the default) the hardware interpolates the colour linearly
 * across the face, so the interior is a continuous blend; with flat shading it
 * would be one solid colour.
 *
 * Freestanding (nolibc).  Prints a digest of the canonical frame to the serial
 * console (for QEMU-vs-hardware comparison), then animates a gentle spin.
 *
 * Usage: smooth [/dev/tdfx3d] [/dev/fb0]
 */
#include "smolminigl.c"		/* single translation unit: the GL layer + smoltdfx */

/* draw the shaded triangle, rotated by `ang` radians about the screen centre */
static void draw(float ang)
{
	int W = smoltdfx_W, H = smoltdfx_H;

	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);		/* window-space 2D coordinates */

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(W * 0.5f, H * 0.5f, 0);
	glRotatef(ang * (180.0f / 3.14159265f), 0, 0, 1);

	glClearColor(0.06f, 0.06f, 0.10f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glBegin(GL_TRIANGLES);
		glColor3f(1.0f, 0.0f, 0.0f);
		glVertex2f(0.0f,          H * 0.36f);	/* top    -> red   */
		glColor3f(0.0f, 1.0f, 0.0f);
		glVertex2f(-W * 0.34f, -H * 0.30f);	/* bottom-left  -> green */
		glColor3f(0.0f, 0.0f, 1.0f);
		glVertex2f( W * 0.34f, -H * 0.30f);	/* bottom-right -> blue  */
	glEnd();
}

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	float ang = 0.0f;

	if (smolminigl_open(rdev, fdev) < 0) {
		static const char m[] = "smooth: smolminigl_open failed\n";

		write(2, m, sizeof(m) - 1);
		return 1;
	}

	/* canonical frame (fixed phase) + digest, for QEMU-vs-hardware compare */
	draw(0.4f);
	smoltdfx_digest("smooth", smoltdfx_drawbuf(), 400);

	for (;;) {			/* present, then animate the spin */
		smolminigl_swap();
		ang += 0.02f;
		draw(ang);
		usleep(16000);
	}
	return 0;
}
