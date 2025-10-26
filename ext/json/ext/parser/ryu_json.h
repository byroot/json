// Copyright 2018 Ulf Adams
//
// The contents of this file may be used under the terms of the Apache License,
// Version 2.0.
//
//    (See accompanying file LICENSE-Apache2 or copy at
//     http://www.apache.org/licenses/LICENSE-2.0)
//
// Alternatively, the contents of this file may be used under the terms of
// the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE-Boost or copy at
//     https://www.boost.org/LICENSE_1_0.txt)
//
// Unless required by applicable law or agreed to in writing, this software
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.
//
// ---
// Minimal Ryu implementation adapted for Ruby JSON gem by Josef Šimánek
// Optimized for pre-extracted mantissa/exponent from JSON parsing
// This is a stripped-down version containing only what's needed for
// converting decimal mantissa+exponent to IEEE 754 double precision.

#ifndef RYU_JSON_H
#define RYU_JSON_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Detect __builtin_clzll availability (for floor_log2)
// Note: MSVC doesn't have __builtin_clzll, so we provide a fallback
#ifdef __clang__
  #if __has_builtin(__builtin_clzll)
    #define RYU_HAVE_BUILTIN_CLZLL 1
  #else
    #define RYU_HAVE_BUILTIN_CLZLL 0
  #endif
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3))
  #define RYU_HAVE_BUILTIN_CLZLL 1
#else
  #define RYU_HAVE_BUILTIN_CLZLL 0
#endif

// Count leading zeros (for floor_log2)
static inline uint32_t ryu_leading_zeros64(uint64_t input)
{
#if RYU_HAVE_BUILTIN_CLZLL
  return __builtin_clzll(input);
#else
  // Fallback: binary search for the highest set bit
  // This works on MSVC and other compilers without __builtin_clzll
  if (input == 0) return 64;
  uint32_t n = 0;
  if (input <= 0x00000000FFFFFFFFULL) { n += 32; input <<= 32; }
  if (input <= 0x0000FFFFFFFFFFFFULL) { n += 16; input <<= 16; }
  if (input <= 0x00FFFFFFFFFFFFFFULL) { n +=  8; input <<=  8; }
  if (input <= 0x0FFFFFFFFFFFFFFFULL) { n +=  4; input <<=  4; }
  if (input <= 0x3FFFFFFFFFFFFFFFULL) { n +=  2; input <<=  2; }
  if (input <= 0x7FFFFFFFFFFFFFFFULL) { n +=  1; }
  return n;
#endif
}

// Include the power-of-5 tables
#include "ryu_table.h"

// IEEE 754 double precision constants
#define DOUBLE_MANTISSA_BITS 52
#define DOUBLE_EXPONENT_BITS 11
#define DOUBLE_EXPONENT_BIAS 1023

// Helper: floor(log2(value)) using ryu_leading_zeros64
static inline uint32_t floor_log2(const uint64_t value) {
  return 63 - ryu_leading_zeros64(value);
}

// Helper: log2(5^e) approximation
static inline int32_t log2pow5(const int32_t e) {
  return (int32_t) ((((uint32_t) e) * 1217359) >> 19);
}

// Helper: ceil(log2(5^e))
static inline int32_t ceil_log2pow5(const int32_t e) {
  return log2pow5(e) + 1;
}

// Helper: max of two int32
static inline int32_t max32(int32_t a, int32_t b) {
  return a < b ? b : a;
}

// Helper: convert uint64 bits to double
static inline double int64Bits2Double(uint64_t bits) {
  double f;
  memcpy(&f, &bits, sizeof(double));
  return f;
}

// Check if value is multiple of 2^p
static inline bool multipleOfPowerOf2(const uint64_t value, const uint32_t p) {
  return (value & ((1ull << p) - 1)) == 0;
}

// Count how many times value is divisible by 5
// Uses modular inverse to avoid expensive division
static inline uint32_t pow5Factor(uint64_t value) {
  const uint64_t m_inv_5 = 14757395258967641293u; // 5 * m_inv_5 = 1 (mod 2^64)
  const uint64_t n_div_5 = 3689348814741910323u;  // 2^64 / 5
  uint32_t count = 0;
  for (;;) {
    value *= m_inv_5;
    if (value > n_div_5)
      break;
    ++count;
  }
  return count;
}

// Check if value is multiple of 5^p
// Optimized: uses modular inverse instead of division
static inline bool multipleOfPowerOf5(const uint64_t value, const uint32_t p) {
  return pow5Factor(value) >= p;
}

// 128-bit multiplication with shift
// This is the core operation for converting decimal to binary
#if defined(__SIZEOF_INT128__)
// Use native 128-bit integers if available (GCC/Clang)
static inline uint64_t mulShift64(const uint64_t m, const uint64_t* const mul, const int32_t j) {
  const unsigned __int128 b0 = ((unsigned __int128) m) * mul[0];
  const unsigned __int128 b2 = ((unsigned __int128) m) * mul[1];
  return (uint64_t) (((b0 >> 64) + b2) >> (j - 64));
}
#else
// Fallback for systems without 128-bit integers
static inline uint64_t umul128(const uint64_t a, const uint64_t b, uint64_t* const productHi) {
  const uint32_t aLo = (uint32_t)a;
  const uint32_t aHi = (uint32_t)(a >> 32);
  const uint32_t bLo = (uint32_t)b;
  const uint32_t bHi = (uint32_t)(b >> 32);

  const uint64_t b00 = (uint64_t)aLo * bLo;
  const uint64_t b01 = (uint64_t)aLo * bHi;
  const uint64_t b10 = (uint64_t)aHi * bLo;
  const uint64_t b11 = (uint64_t)aHi * bHi;

  const uint32_t b00Lo = (uint32_t)b00;
  const uint32_t b00Hi = (uint32_t)(b00 >> 32);

  const uint64_t mid1 = b10 + b00Hi;
  const uint32_t mid1Lo = (uint32_t)(mid1);
  const uint32_t mid1Hi = (uint32_t)(mid1 >> 32);

  const uint64_t mid2 = b01 + mid1Lo;
  const uint32_t mid2Lo = (uint32_t)(mid2);
  const uint32_t mid2Hi = (uint32_t)(mid2 >> 32);

  const uint64_t pHi = b11 + mid1Hi + mid2Hi;
  const uint64_t pLo = ((uint64_t)mid2Lo << 32) | b00Lo;

  *productHi = pHi;
  return pLo;
}

static inline uint64_t shiftright128(const uint64_t lo, const uint64_t hi, const uint32_t dist) {
  return (hi << (64 - dist)) | (lo >> dist);
}

static inline uint64_t mulShift64(const uint64_t m, const uint64_t* const mul, const int32_t j) {
  uint64_t high1;
  const uint64_t low1 = umul128(m, mul[1], &high1);
  uint64_t high0;
  umul128(m, mul[0], &high0);
  const uint64_t sum = high0 + low1;
  if (sum < high0) {
    ++high1;
  }
  return shiftright128(sum, high1, j - 64);
}
#endif

// Main conversion function: decimal mantissa+exponent to IEEE 754 double
// Optimized for JSON parsing with fast paths for edge cases
static inline double ryu_s2d_from_parts(uint64_t m10, int m10digits, int32_t e10, bool signedM) {
  // Fast path: handle zero explicitly (e.g., "0.0", "0e0")
  if (m10 == 0) {
    return int64Bits2Double(((uint64_t) signedM) << 63);
  }

  // Fast path: handle overflow/underflow early
  if (m10digits + e10 <= -324) {
    // Underflow to zero
    return int64Bits2Double(((uint64_t) signedM) << 63);
  }

  if (m10digits + e10 >= 310) {
    // Overflow to infinity
    return int64Bits2Double((((uint64_t) signedM) << 63) | 0x7ff0000000000000ULL);
  }

  // Convert decimal to binary: m10 * 10^e10 = m2 * 2^e2
  int32_t e2;
  uint64_t m2;
  bool trailingZeros;

  if (e10 >= 0) {
    // Positive exponent: multiply by 5^e10 and adjust binary exponent
    e2 = floor_log2(m10) + e10 + log2pow5(e10) - (DOUBLE_MANTISSA_BITS + 1);
    int j = e2 - e10 - ceil_log2pow5(e10) + DOUBLE_POW5_BITCOUNT;
    m2 = mulShift64(m10, DOUBLE_POW5_SPLIT[e10], j);
    trailingZeros = e2 < e10 || (e2 - e10 < 64 && multipleOfPowerOf2(m10, e2 - e10));
  } else {
    // Negative exponent: divide by 5^(-e10)
    e2 = floor_log2(m10) + e10 - ceil_log2pow5(-e10) - (DOUBLE_MANTISSA_BITS + 1);
    int j = e2 - e10 + ceil_log2pow5(-e10) - 1 + DOUBLE_POW5_INV_BITCOUNT;
    m2 = mulShift64(m10, DOUBLE_POW5_INV_SPLIT[-e10], j);
    trailingZeros = multipleOfPowerOf5(m10, -e10);
  }

  // Compute IEEE 754 exponent
  uint32_t ieee_e2 = (uint32_t) max32(0, e2 + DOUBLE_EXPONENT_BIAS + floor_log2(m2));

  if (ieee_e2 > 0x7fe) {
    // Overflow to infinity
    return int64Bits2Double((((uint64_t) signedM) << 63) | 0x7ff0000000000000ULL);
  }

  // Compute shift amount for rounding
  int32_t shift = (ieee_e2 == 0 ? 1 : ieee_e2) - e2 - DOUBLE_EXPONENT_BIAS - DOUBLE_MANTISSA_BITS;

  // IEEE 754 round-to-even (banker's rounding)
  trailingZeros &= (m2 & ((1ull << (shift - 1)) - 1)) == 0;
  uint64_t lastRemovedBit = (m2 >> (shift - 1)) & 1;
  bool roundUp = (lastRemovedBit != 0) && (!trailingZeros || (((m2 >> shift) & 1) != 0));

  uint64_t ieee_m2 = (m2 >> shift) + roundUp;
  ieee_m2 &= (1ull << DOUBLE_MANTISSA_BITS) - 1;

  if (ieee_m2 == 0 && roundUp) {
    ieee_e2++;
  }

  // Pack sign, exponent, and mantissa into IEEE 754 format
  // Match original Ryu: group sign+exponent, then shift and add mantissa
  uint64_t ieee = (((((uint64_t) signedM) << DOUBLE_EXPONENT_BITS) | (uint64_t)ieee_e2) << DOUBLE_MANTISSA_BITS) | ieee_m2;
  return int64Bits2Double(ieee);
}

#endif // RYU_JSON_H
