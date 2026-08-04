/*
 * texquad - texture mapping on smolminigl.
 *
 * One of a set of small OpenGL-1.x demos, each teaching a single concept the
 * way the classic OpenGL Programming Guide ("Red Book") examples do, but
 * written from scratch against smolminigl (its OpenGL subset on the Voodoo3).
 *
 * Concept: a 2D image is wrapped onto geometry.  glTexImage2D hands the image
 * to the card, glTexParameteri picks how it is sampled and wrapped, and a
 * glTexCoord2f before each glVertex says which texel maps to which corner.  A
 * single quad tilted in perspective shows the mapping stays correct across the
 * foreshortened surface.  The image is generated procedurally: an 8x8-cell
 * checkerboard whose lit cells carry an RGB gradient, so the texture is
 * obviously an image and not just the geometry's own colour.
 *
 * The quad is drawn large (a 64x64 texture magnified over a few hundred
 * pixels), i.e. the well-behaved magnification case, GL_NEAREST sampled.
 *
 * Freestanding (nolibc).  Prints a digest of the canonical frame, then spins.
 *
 * Usage: texquad [/dev/tdfx3d] [/dev/fb0]
 */
#include "smolminigl.c"		/* single translation unit: the GL layer + smoltdfx */

#define DEG(r) ((r) * (180.0f / 3.14159265f))
#define TEX 64			/* texture is TEX x TEX texels */

static unsigned char image[TEX][TEX][4];

/* an 8x8-cell checkerboard; lit cells carry an RGB gradient across the image */
static void make_image(void)
{
	int i, j;

	for (i = 0; i < TEX; i++)
		for (j = 0; j < TEX; j++) {
			int lit = (((i >> 3) ^ (j >> 3)) & 1);

			image[i][j][0] = lit ? (unsigned char)(j * 255 / (TEX - 1)) : 20;
			image[i][j][1] = lit ? (unsigned char)(i * 255 / (TEX - 1)) : 20;
			image[i][j][2] = lit ? 140 : 40;
			image[i][j][3] = 255;
		}
}

static GLuint tex;

static void draw(float t)
{
	int W = smoltdfx_W, H = smoltdfx_H;
	float aspect = (float)W / (float)H;

	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-aspect, aspect, -1.0f, 1.0f, 2.0f, 20.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -4.0f);
	glRotatef(DEG(t) * 0.4f, 0.0f, 1.0f, 0.0f);	/* tilt for perspective */

	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex);
	glColor3f(1.0f, 1.0f, 1.0f);
	glBegin(GL_QUADS);
		glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.6f, -1.6f, 0.0f);
		glTexCoord2f(1.0f, 0.0f); glVertex3f( 1.6f, -1.6f, 0.0f);
		glTexCoord2f(1.0f, 1.0f); glVertex3f( 1.6f,  1.6f, 0.0f);
		glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.6f,  1.6f, 0.0f);
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

int main(int argc, char **argv)
{
	const char *rdev = argc > 1 ? argv[1] : "/dev/tdfx3d";
	const char *fdev = argc > 2 ? argv[2] : "/dev/fb0";
	float t = 0.0f;

	if (smolminigl_open(rdev, fdev) < 0) {
		static const char m[] = "texquad: smolminigl_open failed\n";

		write(2, m, sizeof(m) - 1);
		return 1;
	}

	make_image();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX, TEX, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, image);

	draw(0.6f);			/* canonical frame + digest */
	smoltdfx_digest("texquad", smoltdfx_drawbuf(), 400);

	for (;;) {
		smolminigl_swap();
		t += 0.02f;
		draw(t);
		usleep(16000);
	}
	return 0;
}
