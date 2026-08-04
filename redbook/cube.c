/*
 * cube - perspective projection and the depth buffer on smolminigl.
 *
 * One of a set of small OpenGL-1.x demos, each teaching a single concept the
 * way the classic OpenGL Programming Guide ("Red Book") examples do, but
 * written from scratch against smolminigl (its OpenGL subset on the Voodoo3).
 *
 * Concept: this is the first true 3D scene.  glFrustum sets a perspective
 * projection, glTranslatef/glRotatef place the model in front of the eye, and
 * GL_DEPTH_TEST + a per-frame GL_DEPTH_BUFFER_BIT clear let the six faces of a
 * solid cube occlude one another correctly regardless of draw order.  Each face
 * is a flat-shaded quad in its own colour.
 *
 * Freestanding (nolibc).  Prints a digest of the canonical frame, then spins.
 *
 * Usage: cube [/dev/tdfx3d] [/dev/fb0]
 */
#include "smolminigl.c"		/* single translation unit: the GL layer + smoltdfx */

#define DEG(r) ((r) * (180.0f / 3.14159265f))

/* the eight corners of a cube centred on the origin, edge length 2 */
static const float corner[8][3] = {
	{ -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
	{ -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};

/* six faces, each four corner indices wound counter-clockwise when seen from
 * outside, and each in its own colour */
static const int face[6][4] = {
	{ 0, 3, 2, 1 },		/* -Z  back   */
	{ 4, 5, 6, 7 },		/* +Z  front  */
	{ 0, 4, 7, 3 },		/* -X  left   */
	{ 1, 2, 6, 5 },		/* +X  right  */
	{ 0, 1, 5, 4 },		/* -Y  bottom */
	{ 3, 7, 6, 2 },		/* +Y  top    */
};
static const float facecol[6][3] = {
	{ 1.0f, 0.3f, 0.3f }, { 0.3f, 1.0f, 0.3f }, { 0.3f, 0.4f, 1.0f },
	{ 1.0f, 1.0f, 0.3f }, { 1.0f, 0.4f, 1.0f }, { 0.3f, 1.0f, 1.0f },
};

static void draw(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H, f, k;
	float aspect = (float)W / (float)H;

	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-aspect, aspect, -1.0f, 1.0f, 2.0f, 20.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -5.0f);	/* push the cube in front of the eye */
	glRotatef(DEG(t) * 0.7f, 1.0f, 0.0f, 0.0f);
	glRotatef(DEG(t),        0.0f, 1.0f, 0.0f);

	glEnable(GL_DEPTH_TEST);		/* let near faces hide far ones */
	glDepthFunc(GL_LESS);
	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	for (f = 0; f < 6; f++) {
		glColor3f(facecol[f][0], facecol[f][1], facecol[f][2]);
		glBegin(GL_QUADS);
			for (k = 0; k < 4; k++)
				glVertex3fv(corner[face[f][k]]);
		glEnd();
	}
}

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	float t = 0.0f;

	if (smolminigl_open(rdev, fdev) < 0) {
		static const char m[] = "cube: smolminigl_open failed\n";

		write(2, m, sizeof(m) - 1);
		return 1;
	}

	draw(0.6f);			/* canonical frame + digest */
	smoltdfx_digest("cube", smoltdfx_drawbuf(), 400);

	for (;;) {
		smolminigl_swap();
		t += 0.02f;
		draw(t);
		usleep(16000);
	}
	return 0;
}
