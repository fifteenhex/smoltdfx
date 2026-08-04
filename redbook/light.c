/*
 * light - a lit surface on smolminigl.
 *
 * One of a set of small OpenGL-1.x demos, each teaching a single concept the
 * way the classic OpenGL Programming Guide ("Red Book") examples do, but
 * written from scratch against smolminigl (its OpenGL subset on the Voodoo3).
 *
 * Concept: diffuse (Lambert) shading.  smolminigl is a software-transform,
 * immediate-mode subset with no fixed-function GL_LIGHTING, so the lighting is
 * done the honest way for that model -- per vertex on the CPU: a vertex's
 * brightness is ambient + its surface normal dotted with the light direction,
 * fed to the card as a plain per-vertex colour.  Gouraud interpolation across
 * each face then gives the smooth shaded look.  The surface is a UV sphere lit
 * by one directional light; depth testing keeps the far side hidden.
 *
 * Freestanding (nolibc).  Prints a digest of the canonical frame, then spins.
 *
 * Usage: light [/dev/tdfx3d] [/dev/fb0]
 */
#include "smolminigl.c"		/* single translation unit: the GL layer + smoltdfx */

#define DEG(r) ((r) * (180.0f / 3.14159265f))
#define PI     3.14159265f
#define STACKS 16		/* latitude divisions  */
#define SLICES 24		/* longitude divisions */

static const float matcol[3] = { 0.95f, 0.75f, 0.30f };	/* warm gold surface */
static float lightdir[3];	/* normalised, towards the light */

/* one sphere vertex: unit normal, position (= normal * radius), Lambert shade */
static void vertex(float theta, float phi)
{
	float st = smoltdfx_sin(theta), ct = smoltdfx_cos(theta);
	float sp = smoltdfx_sin(phi),   cp = smoltdfx_cos(phi);
	float nx = st * cp, ny = ct, nz = st * sp;
	float d = nx * lightdir[0] + ny * lightdir[1] + nz * lightdir[2];
	float i;

	if (d < 0.0f)
		d = 0.0f;
	i = 0.15f + 0.85f * d;			/* ambient + diffuse */
	glColor3f(matcol[0] * i, matcol[1] * i, matcol[2] * i);
	glVertex3f(nx, ny, nz);			/* unit-radius sphere */
}

static void draw(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H, i, j;
	float aspect = (float)W / (float)H;

	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-aspect, aspect, -1.0f, 1.0f, 2.0f, 20.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -3.2f);
	glRotatef(20.0f,          1.0f, 0.0f, 0.0f);
	glRotatef(DEG(t) * 0.5f,  0.0f, 1.0f, 0.0f);	/* spin the sphere */

	glEnable(GL_DEPTH_TEST);
	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* quad mesh over the (theta, phi) grid, shaded per vertex */
	for (i = 0; i < STACKS; i++) {
		float t0 = PI * i / STACKS, t1 = PI * (i + 1) / STACKS;

		glBegin(GL_QUADS);
		for (j = 0; j < SLICES; j++) {
			float p0 = 2.0f * PI * j / SLICES;
			float p1 = 2.0f * PI * (j + 1) / SLICES;

			vertex(t0, p0);
			vertex(t1, p0);
			vertex(t1, p1);
			vertex(t0, p1);
		}
		glEnd();
	}
}

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	float t = 0.0f, len;

	if (smolminigl_open(rdev, fdev) < 0) {
		static const char m[] = "light: smolminigl_open failed\n";

		write(2, m, sizeof(m) - 1);
		return 1;
	}

	/* one directional light from the upper-right-front, normalised */
	lightdir[0] = 0.5f; lightdir[1] = 0.7f; lightdir[2] = 0.6f;
	len = smoltdfx_sqrt(lightdir[0] * lightdir[0] + lightdir[1] * lightdir[1] +
			    lightdir[2] * lightdir[2]);
	lightdir[0] /= len; lightdir[1] /= len; lightdir[2] /= len;

	draw(0.6f);			/* canonical frame + digest */
	smoltdfx_digest("light", smoltdfx_drawbuf(), 400);

	for (;;) {
		smolminigl_swap();
		t += 0.02f;
		draw(t);
		usleep(16000);
	}
	return 0;
}
