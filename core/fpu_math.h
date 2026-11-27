// Quad-precision (SoftFloat float128) building blocks for the x87 transcendentals.
// Results carry ~110 correct bits, so rounding them to the x87's 64-bit significand
// is correct except in vanishingly rare cases.
#ifndef FPU_MATH_H
#define FPU_MATH_H

#include <stdbool.h>
#include "softfloat.h"

float128_t q_pi(void);                    // pi
float128_t q_ln2(void);                   // ln 2
// sin and cos of x (finite, |x| < 2^63) after reduction by Intel's 66-bit pi/2.
void       q_sincos(float128_t x, float128_t *s, float128_t *c);
float128_t q_atan2(float128_t y, float128_t x);   // finite, not both zero
float128_t q_exp2m1(float128_t x);                // 2^x - 1, |x| <= 1
float128_t q_log2(float128_t x);                  // x > 0 finite
float128_t q_log2p1(float128_t x);                // log2(1 + x), |x| small

#endif
