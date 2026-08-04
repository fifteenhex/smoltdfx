/*
 * blend - alpha blending / transparency on smolminigl.
 *
 * One of a set of small OpenGL-1.x demos, each teaching a single concept the
 * way the classic OpenGL Programming Guide ("Red Book") examples do, but
 * written from scratch against smolminigl (its OpenGL subset on the Voodoo3).
 *
 * Concept: with GL_BLEND enabled and glBlendFunc(GL_SRC_ALPHA,
 * GL_ONE_MINUS_SRC_ALPHA), each fragment is mixed into the framebuffer by its
 * own alpha instead of overwriting it, so a translucent surface lets what is
 * behind it show through.  Three half-transparent quads -- red, green, blue --
 * are laid over one another so the pairwise and three-way overlaps blend into
 * new colours.  Because the blend is order-dependent, they are drawn back to
 * front.
 *
 * Freestanding (nolibc).  Prints a digest of the canonical frame, then spins.
 *
 * Usage: blend [/dev/tdfx3d] [/dev/fb0]
 */
#include "smolminigl.c"		/* single translation unit: the GL layer + smoltdfx */

#define DEG(r) ((r) * (180.0f / 3.14159265f))

/* a filled square of half-unit radius, centred on the origin */
static void square(void)
{
	glBegin(GL_QUADS);
		glVertex2f(-0.5f, -0.5f);
		glVertex2f( 0.5f, -0.5f);
		glVertex2f( 0.5f,  0.5f);
		glVertex2f(-0.5f,  0.5f);
	glEnd();
}

/* three translucent quads, each offset from the centre by 120 degrees */
static const float quadcol[3][3] = {
	{ 1.0f, 0.2f, 0.2f }, { 0.2f, 1.0f, 0.2f }, { 0.3f, 0.4f, 1.0f },
};

static void draw(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H, i;

	/* world space = pixels, origin at the screen centre */
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-W * 0.5f, W * 0.5f, -H * 0.5f, H * 0.5f, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glDisable(GL_DEPTH_TEST);		/* transparency: no depth reject */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	/* drawn in order, back to front -- the blend is order-dependent */
	for (i = 0; i < 3; i++) {
		float a = DEG(t) + i * 120.0f;

		glColor4f(quadcol[i][0], quadcol[i][1], quadcol[i][2], 0.5f);
		glPushMatrix();
			glRotatef(a, 0, 0, 1);		/* place around the centre */
			glTranslatef(70.0f, 0, 0);
			glScalef(240.0f, 240.0f, 1.0f);
			square();
		glPopMatrix();
	}

	glDisable(GL_BLEND);
}

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	float t = 0.0f;

	if (smolminigl_open(rdev, fdev) < 0) {
		static const char m[] = "blend: smolminigl_open failed\n";

		write(2, m, sizeof(m) - 1);
		return 1;
	}

	draw(0.6f);			/* canonical frame + digest */
	smoltdfx_digest("blend", smoltdfx_drawbuf(), 400);

	for (;;) {
		smolminigl_swap();
		t += 0.01f;
		draw(t);
		usleep(16000);
	}
	return 0;
}
