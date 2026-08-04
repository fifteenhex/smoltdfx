/*
 * GL/gl.h - the subset of OpenGL 1.x that GLQuake uses, declared so the
 * GLQuake renderer compiles against smolminigl (implemented on smoltdfx).
 * Only the ~60 entry points and enums GLQuake references are provided.
 */
#ifndef __smolminigl_gl_h
#define __smolminigl_gl_h

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef void		GLvoid;
typedef signed char	GLbyte;
typedef short		GLshort;
typedef int		GLint;
typedef int		GLsizei;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef float		GLfloat;
typedef float		GLclampf;
typedef double		GLdouble;
typedef double		GLclampd;

#define GL_FALSE			0
#define GL_TRUE				1

/* primitives */
#define GL_POINTS			0x0000
#define GL_LINES			0x0001
#define GL_LINE_LOOP			0x0002
#define GL_LINE_STRIP			0x0003
#define GL_TRIANGLES			0x0004
#define GL_TRIANGLE_STRIP		0x0005
#define GL_TRIANGLE_FAN			0x0006
#define GL_QUADS			0x0007
#define GL_QUAD_STRIP			0x0008
#define GL_POLYGON			0x0009

/* clear bits */
#define GL_DEPTH_BUFFER_BIT		0x00000100
#define GL_COLOR_BUFFER_BIT		0x00004000

/* enable caps */
#define GL_CULL_FACE			0x0B44
#define GL_DEPTH_TEST			0x0B71
#define GL_BLEND			0x0BE2
#define GL_FOG				0x0B60
#define GL_ALPHA_TEST			0x0BC0
#define GL_TEXTURE_2D			0x0DE1
#define GL_DITHER			0x0BD0

/* faces / winding */
#define GL_FRONT			0x0404
#define GL_BACK				0x0405
#define GL_FRONT_AND_BACK		0x0408
#define GL_CW				0x0900
#define GL_CCW				0x0901

/* blend factors */
#define GL_ZERO				0
#define GL_ONE				1
#define GL_SRC_COLOR			0x0300
#define GL_ONE_MINUS_SRC_COLOR		0x0301
#define GL_SRC_ALPHA			0x0302
#define GL_ONE_MINUS_SRC_ALPHA		0x0303
#define GL_DST_ALPHA			0x0304
#define GL_ONE_MINUS_DST_ALPHA		0x0305
#define GL_DST_COLOR			0x0306
#define GL_ONE_MINUS_DST_COLOR		0x0307
#define GL_SRC_ALPHA_SATURATE		0x0308

/* compare funcs */
#define GL_NEVER			0x0200
#define GL_LESS				0x0201
#define GL_EQUAL			0x0202
#define GL_LEQUAL			0x0203
#define GL_GREATER			0x0204
#define GL_NOTEQUAL			0x0205
#define GL_GEQUAL			0x0206
#define GL_ALWAYS			0x0207

/* matrix modes */
#define GL_MODELVIEW			0x1700
#define GL_PROJECTION			0x1701
#define GL_TEXTURE			0x1702
#define GL_MODELVIEW_MATRIX		0x0BA6
#define GL_PROJECTION_MATRIX		0x0BA7

/* data types */
#define GL_BYTE				0x1400
#define GL_UNSIGNED_BYTE		0x1401
#define GL_SHORT			0x1402
#define GL_UNSIGNED_SHORT		0x1403
#define GL_INT				0x1404
#define GL_UNSIGNED_INT			0x1405
#define GL_FLOAT			0x1406

/* pixel/texture formats */
#define GL_COLOR_INDEX			0x1900
#define GL_RED				0x1903
#define GL_GREEN			0x1904
#define GL_BLUE				0x1905
#define GL_ALPHA			0x1906
#define GL_RGB				0x1907
#define GL_RGBA				0x1908
#define GL_LUMINANCE			0x1909
#define GL_LUMINANCE_ALPHA		0x190A
#define GL_INTENSITY			0x8049
#define GL_RGBA8			0x8058
#define GL_RGB8				0x8051

/* texenv / texture params */
#define GL_TEXTURE_ENV			0x2300
#define GL_TEXTURE_ENV_MODE		0x2200
#define GL_TEXTURE_ENV_COLOR		0x2201
#define GL_MODULATE			0x2100
#define GL_DECAL			0x2101
#define GL_ADD				0x0104
#define GL_REPLACE			0x1E01
#define GL_TEXTURE_MAG_FILTER		0x2800
#define GL_TEXTURE_MIN_FILTER		0x2801
#define GL_TEXTURE_WRAP_S		0x2802
#define GL_TEXTURE_WRAP_T		0x2803
#define GL_NEAREST			0x2600
#define GL_LINEAR			0x2601
#define GL_NEAREST_MIPMAP_NEAREST	0x2700
#define GL_LINEAR_MIPMAP_NEAREST	0x2701
#define GL_NEAREST_MIPMAP_LINEAR	0x2702
#define GL_LINEAR_MIPMAP_LINEAR		0x2703
#define GL_CLAMP			0x2900
#define GL_REPEAT			0x2901

/* shade / hint */
#define GL_FLAT				0x1D00
#define GL_SMOOTH			0x1D01
#define GL_PERSPECTIVE_CORRECTION_HINT	0x0C50
#define GL_DONT_CARE			0x1100
#define GL_FASTEST			0x1101
#define GL_NICEST			0x1102

/* fog */
#define GL_FOG_MODE			0x0B65
#define GL_FOG_DENSITY			0x0B62
#define GL_FOG_COLOR			0x0B66
#define GL_FOG_START			0x0B63
#define GL_FOG_END			0x0B64
#define GL_EXP				0x0800
#define GL_EXP2				0x0801

/* glGetString names */
#define GL_VENDOR			0x1F00
#define GL_RENDERER			0x1F01
#define GL_VERSION			0x1F02
#define GL_EXTENSIONS			0x1F03

/* buffers */
#define GL_FRONT_LEFT			0x0400
#define GL_BACK_LEFT			0x0402

/* polygon mode */
#define GL_POINT			0x1B00
#define GL_LINE				0x1B01
#define GL_FILL				0x1B02

void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ubv(const GLubyte *v);
void glColor4fv(const GLfloat *v);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat *v);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n,
	     GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n,
	       GLdouble f);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glDepthRange(GLclampd n, GLclampd f);

void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glBlendFunc(GLenum s, GLenum d);
void glDepthMask(GLboolean flag);
void glDepthFunc(GLenum f);
void glAlphaFunc(GLenum f, GLclampf ref);
void glShadeModel(GLenum m);
void glCullFace(GLenum m);
void glHint(GLenum t, GLenum m);
void glFinish(void);
void glFlush(void);
void glDrawBuffer(GLenum b);
void glReadBuffer(GLenum b);

void glGenTextures(GLsizei n, GLuint *tex);
void glDeleteTextures(GLsizei n, const GLuint *tex);
void glBindTexture(GLenum target, GLuint tex);
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w,
		  GLsizei h, GLint border, GLenum format, GLenum type,
		  const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo,
		     GLsizei w, GLsizei h, GLenum format, GLenum type,
		     const GLvoid *pixels);
void glTexParameterf(GLenum target, GLenum pname, GLfloat p);
void glTexParameteri(GLenum target, GLenum pname, GLint p);
void glTexEnvf(GLenum target, GLenum pname, GLfloat p);
void glTexEnvi(GLenum target, GLenum pname, GLint p);

void glGetFloatv(GLenum pname, GLfloat *params);
const GLubyte *glGetString(GLenum name);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
		  GLenum type, GLvoid *pixels);
void glFogf(GLenum pname, GLfloat p);
void glFogi(GLenum pname, GLint p);
void glFogfv(GLenum pname, const GLfloat *p);
void glPolygonMode(GLenum face, GLenum mode);

/* smolminigl extras: bring up / tear down the smoltdfx backend + present */
int  smolminigl_open(const char *regdev, const char *fbdev);
void smolminigl_swap(void);
int  smolminigl_width(void);
int  smolminigl_height(void);

#ifdef __cplusplus
}
#endif

#endif
