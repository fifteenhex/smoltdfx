/*
 * transform - model transformations and the matrix stack on smolminigl.
 *
 * One of a set of small OpenGL-1.x demos, each teaching a single concept the
 * way the classic OpenGL Programming Guide ("Red Book") examples do, but
 * written from scratch against smolminigl (its OpenGL subset on the Voodoo3).
 *
 * Concept: glTranslatef / glRotatef / glScalef build the current model
 * transform, and glPushMatrix / glPopMatrix save and restore it so each object
 * is positioned independently.  Here one unit square is drawn many times: once
 * in the centre, and once for each spoke of a ring -- each spoke rotates about
 * the centre, translates outward, then spins and scales on its own, all from
 * the same square, using push/pop to isolate the transforms.
 *
 * Freestanding (nolibc).  Prints a digest of the canonical frame, then spins.
 *
 * Usage: transform [/dev/tdfx3d] [/dev/fb0]
 */
#include "smolminigl.c"		/* single translation unit: the GL layer + smoltdfx */

#define DEG(r) ((r) * (180.0f / 3.14159265f))

/* a filled unit square centred on the origin, in the current colour */
static void square(void)
{
	glBegin(GL_QUADS);
		glVertex2f(-0.5f, -0.5f);
		glVertex2f( 0.5f, -0.5f);
		glVertex2f( 0.5f,  0.5f);
		glVertex2f(-0.5f,  0.5f);
	glEnd();
}

/* eight distinct spoke colours */
static const float spoke[8][3] = {
	{ 1.0f, 0.2f, 0.2f }, { 1.0f, 0.7f, 0.1f }, { 1.0f, 1.0f, 0.2f },
	{ 0.3f, 1.0f, 0.3f }, { 0.2f, 1.0f, 1.0f }, { 0.3f, 0.5f, 1.0f },
	{ 0.6f, 0.3f, 1.0f }, { 1.0f, 0.3f, 0.8f },
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

	/* centre square: scale the unit square up, isolated by push/pop */
	glColor3f(0.9f, 0.9f, 0.9f);
	glPushMatrix();
		glScalef(104.0f, 104.0f, 1.0f);
		square();
	glPopMatrix();

	/* ring of eight squares, each a hierarchy of transforms off the centre */
	for (i = 0; i < 8; i++) {
		float a = DEG(t) + i * 45.0f;
		float s = 26.0f + 14.0f * smoltdfx_sin(t * 2.0f + i);

		glColor3f(spoke[i][0], spoke[i][1], spoke[i][2]);
		glPushMatrix();
			glRotatef(a, 0, 0, 1);		/* orbit about the centre */
			glTranslatef(150.0f, 0, 0);	/* move out along the spoke */
			glRotatef(-3.0f * a, 0, 0, 1);	/* spin on its own axis */
			glScalef(s, s, 1.0f);
			square();
		glPopMatrix();
	}
}

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	float t = 0.0f;

	if (smolminigl_open(rdev, fdev) < 0) {
		static const char m[] = "transform: smolminigl_open failed\n";

		write(2, m, sizeof(m) - 1);
		return 1;
	}

	draw(0.4f);			/* canonical frame + digest */
	smoltdfx_digest("transform", smoltdfx_drawbuf(), 400);

	for (;;) {
		smolminigl_swap();
		t += 0.01f;
		draw(t);
		usleep(16000);
	}
	return 0;
}
