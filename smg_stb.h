/*
 * smg_stb.h -- stb_image_resize2 for smolminigl's texture resampling.
 *
 * Freestanding config for the nolibc build: no SIMD, assert compiled out, and
 * the double floor/ceil that nolibc's <math.h> lacks (it ships only fabsf).
 * floorf/ceilf come from smg_cglm.h, which is included before this.  The hosted
 * (glibc) build has a real libm/assert and uses them.
 */
#ifndef SMG_STB_H
#define SMG_STB_H

#ifndef SMG_HOSTED
#include "smoltdfx.h"			/* pulls nolibc (malloc/memcpy/...) */

static inline double floor(double x)
{
	double t = (double)(long long)x;

	return t > x ? t - 1.0 : t;
}
static inline double ceil(double x)
{
	double t = (double)(long long)x;

	return t < x ? t + 1.0 : t;
}
#define STBIR_NO_SIMD			/* scalar path: no <*mmintrin.h> */
#define STBIR_ASSERT(x) ((void)0)	/* no assert.h in the freestanding build */
#endif /* !SMG_HOSTED */

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#endif /* SMG_STB_H */
