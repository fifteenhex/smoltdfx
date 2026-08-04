/*
 * smg_cglm.h -- bring cglm in for smolminigl's matrix/vector math.
 *
 * The nolibc test/2dtest build is freestanding (-nostdlib -nostdinc): nolibc's
 * <math.h> supplies only fabsf, but cglm needs sinf/cosf/sqrtf/tanf/floorf/...
 * Bridge those to the smoltdfx freestanding approximations -- the very ones the
 * hand-coded matrix path already used -- so moving to cglm is behaviour-
 * preserving.  The hosted (glibc) GLQuake build has a real libm, so it skips
 * the bridge and cglm uses the standard functions.
 */
#ifndef SMG_CGLM_H
#define SMG_CGLM_H

#ifndef SMG_HOSTED
#include "smoltdfx.h"			/* smoltdfx_sin/cos/sqrt/fabs */

static inline float sinf(float x)  { return smoltdfx_sin(x); }
static inline float cosf(float x)  { return smoltdfx_cos(x); }
static inline float sqrtf(float x) { return smoltdfx_sqrt(x); }
static inline float tanf(float x)
{
	float c = smoltdfx_cos(x);

	return c != 0.0f ? smoltdfx_sin(x) / c : 0.0f;
}
static inline float floorf(float x)
{
	float t = (float)(long)x;

	return t > x ? t - 1.0f : t;
}
static inline float ceilf(float x)
{
	float t = (float)(long)x;

	return t < x ? t + 1.0f : t;
}
static inline float fmodf(float a, float b)
{
	return b != 0.0f ? a - (float)(long)(a / b) * b : 0.0f;
}
static inline float fminf(float a, float b) { return a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a > b ? a : b; }
static inline float truncf(float x) { return (float)(long)x; }
static inline float roundf(float x)
{
	return x >= 0.0f ? (float)(long)(x + 0.5f) : (float)(long)(x - 0.5f);
}
static inline float copysignf(float x, float s)
{
	float a = x < 0.0f ? -x : x;

	return s < 0.0f ? -a : a;
}
static inline int smg_isnan(float x) { return x != x; }
static inline int smg_isinf(float x) { return x > 3.4e38f || x < -3.4e38f; }
#define isnan(x)  smg_isnan(x)
#define isinf(x)  smg_isinf(x)
/* referenced only by cglm's euler/quat inlines, which smolminigl never calls
 * (so they are parsed but never emitted/linked) -- declarations suffice */
float powf(float, float);
float atan2f(float, float);
float atanf(float);
float acosf(float);
float asinf(float);
float modff(float, float *);
#endif /* !SMG_HOSTED */

/*
 * smolminigl stores 4x4 matrices as plain (unaligned) OpenGL column-major
 * float[16], so cglm must not assume 16-byte alignment for its SIMD loads.
 */
#define CGLM_ALL_UNALIGNED
#include "cglm/cglm.h"

/*
 * A column-major float[16] is bit-identical to cglm's mat4 (float[4][4],
 * column-major), so reinterpret in place instead of copying.  The uintptr_t
 * round-trip also drops const for cglm's non-const mat4 parameters.
 */
#define SMG_M4(p) (*(mat4 *)(uintptr_t)(p))

#endif /* SMG_CGLM_H */
