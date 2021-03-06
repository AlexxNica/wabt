/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "literal.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HEX_DIGIT_BITS 4

/* The PLUS_ONE values are used because normal IEEE floats have an implicit
 * leading one, so they have an additional bit of precision. */

#define F32_SIGN_SHIFT 31
#define F32_SIG_BITS 23
#define F32_SIG_MASK 0x7fffff
#define F32_SIG_PLUS_ONE_BITS 24
#define F32_SIG_PLUS_ONE_MASK 0xffffff
#define F32_EXP_MASK 0xff
#define F32_MIN_EXP -127
#define F32_MAX_EXP 128
#define F32_EXP_BIAS 127
#define F32_QUIET_NAN_TAG 0x400000

#define F64_SIGN_SHIFT 63
#define F64_SIG_BITS 52
#define F64_SIG_MASK 0xfffffffffffffULL
#define F64_SIG_PLUS_ONE_BITS 53
#define F64_SIG_PLUS_ONE_MASK 0x1fffffffffffffULL
#define F64_EXP_MASK 0x7ff
#define F64_MIN_EXP -1023
#define F64_MAX_EXP 1024
#define F64_EXP_BIAS 1023
#define F64_QUIET_NAN_TAG 0x8000000000000ULL

static const char s_hex_digits[] = "0123456789abcdef";

WabtResult wabt_parse_hexdigit(char c, uint32_t* out) {
  if ((unsigned int)(c - '0') <= 9) {
    *out = c - '0';
    return WABT_OK;
  } else if ((unsigned int)(c - 'a') <= 6) {
    *out = 10 + (c - 'a');
    return WABT_OK;
  } else if ((unsigned int)(c - 'A') <= 6) {
    *out = 10 + (c - 'A');
    return WABT_OK;
  }
  return WABT_ERROR;
}

/* return 1 if the non-NULL-terminated string starting with |start| and ending
 with |end| starts with the NULL-terminated string |prefix|. */
static WabtBool string_starts_with(const char* start,
                                   const char* end,
                                   const char* prefix) {
  while (start < end && *prefix) {
    if (*start != *prefix)
      return WABT_FALSE;
    start++;
    prefix++;
  }
  return *prefix == 0;
}

WabtResult wabt_parse_uint64(const char* s, const char* end, uint64_t* out) {
  if (s == end)
    return WABT_ERROR;
  uint64_t value = 0;
  if (*s == '0' && s + 1 < end && s[1] == 'x') {
    s += 2;
    if (s == end)
      return WABT_ERROR;
    for (; s < end; ++s) {
      uint32_t digit;
      if (WABT_FAILED(wabt_parse_hexdigit(*s, &digit)))
        return WABT_ERROR;
      uint64_t old_value = value;
      value = value * 16 + digit;
      /* check for overflow */
      if (old_value > value)
        return WABT_ERROR;
    }
  } else {
    for (; s < end; ++s) {
      uint32_t digit = (*s - '0');
      if (digit > 9)
        return WABT_ERROR;
      uint64_t old_value = value;
      value = value * 10 + digit;
      /* check for overflow */
      if (old_value > value)
        return WABT_ERROR;
    }
  }
  if (s != end)
    return WABT_ERROR;
  *out = value;
  return WABT_OK;
}

WabtResult wabt_parse_int64(const char* s,
                            const char* end,
                            uint64_t* out,
                            WabtParseIntType parse_type) {
  WabtBool has_sign = WABT_FALSE;
  if (*s == '-' || *s == '+') {
    if (parse_type == WABT_PARSE_UNSIGNED_ONLY)
      return WABT_ERROR;
    if (*s == '-')
      has_sign = WABT_TRUE;
    s++;
  }
  uint64_t value = 0;
  WabtResult result = wabt_parse_uint64(s, end, &value);
  if (has_sign) {
    if (value > (uint64_t)INT64_MAX + 1) /* abs(INT64_MIN) == INT64_MAX + 1 */
      return WABT_ERROR;
    value = UINT64_MAX - value + 1;
  }
  *out = value;
  return result;
}

WabtResult wabt_parse_int32(const char* s,
                            const char* end,
                            uint32_t* out,
                            WabtParseIntType parse_type) {
  uint64_t value;
  WabtBool has_sign = WABT_FALSE;
  if (*s == '-' || *s == '+') {
    if (parse_type == WABT_PARSE_UNSIGNED_ONLY)
      return WABT_ERROR;
    if (*s == '-')
      has_sign = WABT_TRUE;
    s++;
  }
  if (WABT_FAILED(wabt_parse_uint64(s, end, &value)))
    return WABT_ERROR;

  if (has_sign) {
    if (value > (uint64_t)INT32_MAX + 1) /* abs(INT32_MIN) == INT32_MAX + 1 */
      return WABT_ERROR;
    value = UINT32_MAX - value + 1;
  } else {
    if (value > (uint64_t)UINT32_MAX)
      return WABT_ERROR;
  }
  *out = (uint32_t)value;
  return WABT_OK;
}

/* floats */
static uint32_t make_float(WabtBool sign, int exp, uint32_t sig) {
  assert(exp >= F32_MIN_EXP && exp <= F32_MAX_EXP);
  assert(sig <= F32_SIG_MASK);
  return ((uint32_t)sign << F32_SIGN_SHIFT) |
         ((uint32_t)(exp + F32_EXP_BIAS) << F32_SIG_BITS) | sig;
}

static uint32_t shift_float_and_round_to_nearest(uint32_t significand,
                                                 int shift) {
  assert(shift > 0);
  /* round ties to even */
  if (significand & ((uint32_t)1 << shift))
    significand += (uint32_t)1 << (shift - 1);
  significand >>= shift;
  return significand;
}

static WabtResult parse_float_nan(const char* s,
                                  const char* end,
                                  uint32_t* out_bits) {
  WabtBool is_neg = WABT_FALSE;
  if (*s == '-') {
    is_neg = WABT_TRUE;
    s++;
  } else if (*s == '+') {
    s++;
  }
  assert(string_starts_with(s, end, "nan"));
  s += 3;

  uint32_t tag;
  if (s != end) {
    tag = 0;
    assert(string_starts_with(s, end, ":0x"));
    s += 3;

    for (; s < end; ++s) {
      uint32_t digit;
      if (WABT_FAILED(wabt_parse_hexdigit(*s, &digit)))
        return WABT_ERROR;
      tag = tag * 16 + digit;
      /* check for overflow */
      if (tag > F32_SIG_MASK)
        return WABT_ERROR;
    }

    /* NaN cannot have a zero tag, that is reserved for infinity */
    if (tag == 0)
      return WABT_ERROR;
  } else {
    tag = F32_QUIET_NAN_TAG;
  }

  *out_bits = make_float(is_neg, F32_MAX_EXP, tag);
  return WABT_OK;
}

static void parse_float_hex(const char* s,
                            const char* end,
                            uint32_t* out_bits) {
  WabtBool is_neg = WABT_FALSE;
  if (*s == '-') {
    is_neg = WABT_TRUE;
    s++;
  } else if (*s == '+') {
    s++;
  }
  assert(string_starts_with(s, end, "0x"));
  s += 2;

  /* loop over the significand; everything up to the 'p'.
   this code is a bit nasty because we want to support extra zeroes anywhere
   without having to use many significand bits.
   e.g.
   0x00000001.0p0 => significand = 1, significand_exponent = 0
   0x10000000.0p0 => significand = 1, significand_exponent = 28
   0x0.000001p0 => significand = 1, significand_exponent = -24
   */
  WabtBool seen_dot = WABT_FALSE;
  uint32_t significand = 0;
  /* how much to shift |significand| if a non-zero value is appended */
  int significand_shift = 0;
  int significand_bits = 0; /* bits of |significand| */
  int significand_exponent = 0;  /* exponent adjustment due to dot placement */
  for (; s < end; ++s) {
    uint32_t digit;
    if (*s == '.') {
      if (significand != 0)
        significand_exponent += significand_shift;
      significand_shift = 0;
      seen_dot = WABT_TRUE;
      continue;
    } else if (WABT_FAILED(wabt_parse_hexdigit(*s, &digit))) {
      break;
    }
    significand_shift += HEX_DIGIT_BITS;
    if (digit != 0 && (significand == 0 ||
                       significand_bits + significand_shift <=
                           F32_SIG_BITS + 1 + HEX_DIGIT_BITS)) {
      if (significand != 0)
        significand <<= significand_shift;
      if (seen_dot)
        significand_exponent -= significand_shift;
      significand += digit;
      significand_shift = 0;
      significand_bits += HEX_DIGIT_BITS;
    }
  }

  if (!seen_dot)
    significand_exponent += significand_shift;

  assert(s < end && *s == 'p');
  s++;

  if (significand == 0) {
    /* 0 or -0 */
    *out_bits = make_float(is_neg, F32_MIN_EXP, 0);
    return;
  }

  assert(s < end);
  int exponent = 0;
  WabtBool exponent_is_neg = WABT_FALSE;
  /* exponent is always positive, but significand_exponent is signed.
   significand_exponent_add is negated if exponent will be negative, so it  can
   be easily summed to see if the exponent is too large (see below) */
  int significand_exponent_add = 0;
  if (*s == '-') {
    exponent_is_neg = WABT_TRUE;
    significand_exponent_add = -significand_exponent;
    s++;
  } else if (*s == '+') {
    s++;
    significand_exponent_add = significand_exponent;
  }

  for (; s < end; ++s) {
    uint32_t digit = (*s - '0');
    assert(digit <= 9);
    exponent = exponent * 10 + digit;
    if (exponent + significand_exponent_add >= F32_MAX_EXP)
      break;
  }

  if (exponent_is_neg)
    exponent = -exponent;

  significand_bits = sizeof(uint32_t) * 8 - wabt_clz_u32(significand);
  /* -1 for the implicit 1 bit of the significand */
  exponent += significand_exponent + significand_bits - 1;

  if (exponent >= F32_MAX_EXP) {
    /* inf or -inf */
    *out_bits = make_float(is_neg, F32_MAX_EXP, 0);
  } else if (exponent <= F32_MIN_EXP) {
    /* maybe subnormal */
    if (significand_bits > F32_SIG_BITS) {
      significand = shift_float_and_round_to_nearest(
          significand, significand_bits - F32_SIG_BITS);
    } else if (significand_bits < F32_SIG_BITS) {
      significand <<= (F32_SIG_BITS - significand_bits);
    }

    int shift = F32_MIN_EXP - exponent;
    if (shift < F32_SIG_BITS) {
      if (shift) {
        significand =
            shift_float_and_round_to_nearest(significand, shift) & F32_SIG_MASK;
      }
      exponent = F32_MIN_EXP;

      if (significand != 0) {
        *out_bits = make_float(is_neg, exponent, significand);
        return;
      }
    }

    /* not subnormal, too small; return 0 or -0 */
    *out_bits = make_float(is_neg, F32_MIN_EXP, 0);
  } else {
    /* normal value */
    if (significand_bits > F32_SIG_PLUS_ONE_BITS) {
      significand = shift_float_and_round_to_nearest(
          significand, significand_bits - F32_SIG_PLUS_ONE_BITS);
      if (significand > F32_SIG_PLUS_ONE_MASK)
        exponent++;
    } else if (significand_bits < F32_SIG_PLUS_ONE_BITS) {
      significand <<= (F32_SIG_PLUS_ONE_BITS - significand_bits);
    }

    *out_bits = make_float(is_neg, exponent, significand & F32_SIG_MASK);
  }
}

static void parse_float_infinity(const char* s,
                                 const char* end,
                                 uint32_t* out_bits) {
  WabtBool is_neg = WABT_FALSE;
  if (*s == '-') {
    is_neg = WABT_TRUE;
    s++;
  } else if (*s == '+') {
    s++;
  }
  assert(string_starts_with(s, end, "infinity"));
  *out_bits = make_float(is_neg, F32_MAX_EXP, 0);
}

WabtResult wabt_parse_float(WabtLiteralType literal_type,
                            const char* s,
                            const char* end,
                            uint32_t* out_bits) {
  switch (literal_type) {
    case WABT_LITERAL_TYPE_INT:
    case WABT_LITERAL_TYPE_FLOAT: {
      errno = 0;
      char* endptr;
      float value;
      value = strtof(s, &endptr);
      if (endptr != end ||
          ((value == 0 || value == HUGE_VALF || value == -HUGE_VALF) &&
           errno != 0))
        return WABT_ERROR;

      memcpy(out_bits, &value, sizeof(value));
      return WABT_OK;
    }

    case WABT_LITERAL_TYPE_HEXFLOAT:
      parse_float_hex(s, end, out_bits);
      return WABT_OK;

    case WABT_LITERAL_TYPE_INFINITY:
      parse_float_infinity(s, end, out_bits);
      return WABT_OK;

    case WABT_LITERAL_TYPE_NAN:
      return parse_float_nan(s, end, out_bits);

    default:
      assert(0);
      return WABT_ERROR;
  }
}

void wabt_write_float_hex(char* out, size_t size, uint32_t bits) {
  /* 1234567890123456 */
  /* -0x#.######p-### */
  /* -nan:0x###### */
  /* -infinity */
  char buffer[WABT_MAX_FLOAT_HEX];
  char* p = buffer;
  WabtBool is_neg = (bits >> F32_SIGN_SHIFT);
  int exp = ((bits >> F32_SIG_BITS) & F32_EXP_MASK) - F32_EXP_BIAS;
  uint32_t sig = bits & F32_SIG_MASK;

  if (is_neg)
    *p++ = '-';
  if (exp == F32_MAX_EXP) {
    /* infinity or nan */
    if (sig == 0) {
      strcpy(p, "infinity");
      p += 8;
    } else {
      strcpy(p, "nan");
      p += 3;
      if (sig != F32_QUIET_NAN_TAG) {
        strcpy(p, ":0x");
        p += 3;
        /* skip leading zeroes */
        int num_nybbles = sizeof(uint32_t) * 8 / 4;
        while ((sig & 0xf0000000) == 0) {
          sig <<= 4;
          num_nybbles--;
        }
        while (num_nybbles) {
          uint32_t nybble = (sig >> (sizeof(uint32_t) * 8 - 4)) & 0xf;
          *p++ = s_hex_digits[nybble];
          sig <<= 4;
          --num_nybbles;
        }
      }
    }
  } else {
    WabtBool is_zero = sig == 0 && exp == F32_MIN_EXP;
    strcpy(p, "0x");
    p += 2;
    *p++ = is_zero ? '0' : '1';

    /* shift sig up so the top 4-bits are at the top of the uint32 */
    sig <<= sizeof(uint32_t) * 8 - F32_SIG_BITS;

    if (sig) {
      if (exp == F32_MIN_EXP) {
        /* subnormal; shift the significand up, and shift out the implicit 1 */
        uint32_t leading_zeroes = wabt_clz_u32(sig);
        if (leading_zeroes < 31)
          sig <<= leading_zeroes + 1;
        else
          sig = 0;
        exp -= leading_zeroes;
      }

      *p++ = '.';
      while (sig) {
        uint32_t nybble = (sig >> (sizeof(uint32_t) * 8 - 4)) & 0xf;
        *p++ = s_hex_digits[nybble];
        sig <<= 4;
      }
    }
    *p++ = 'p';
    if (is_zero) {
      strcpy(p, "+0");
      p += 2;
    } else {
      if (exp < 0) {
        *p++ = '-';
        exp = -exp;
      } else {
        *p++ = '+';
      }
      if (exp >= 100) *p++ = '1';
      if (exp >= 10) *p++ = '0' + (exp / 10) % 10;
      *p++ = '0' + exp % 10;
    }
  }

  size_t len = p - buffer;
  if (len >= size)
    len = size - 1;
  memcpy(out, buffer, len);
  out[len] = '\0';
}

/* doubles */
static uint64_t make_double(WabtBool sign, int exp, uint64_t sig) {
  assert(exp >= F64_MIN_EXP && exp <= F64_MAX_EXP);
  assert(sig <= F64_SIG_MASK);
  return ((uint64_t)sign << F64_SIGN_SHIFT) |
         ((uint64_t)(exp + F64_EXP_BIAS) << F64_SIG_BITS) | sig;
}

static uint64_t shift_double_and_round_to_nearest(uint64_t significand,
                                                  int shift) {
  assert(shift > 0);
  /* round ties to even */
  if (significand & ((uint64_t)1 << shift))
    significand += (uint64_t)1 << (shift - 1);
  significand >>= shift;
  return significand;
}

static WabtResult parse_double_nan(const char* s,
                                   const char* end,
                                   uint64_t* out_bits) {
  WabtBool is_neg = WABT_FALSE;
  if (*s == '-') {
    is_neg = WABT_TRUE;
    s++;
  } else if (*s == '+') {
    s++;
  }
  assert(string_starts_with(s, end, "nan"));
  s += 3;

  uint64_t tag;
  if (s != end) {
    tag = 0;
    if (!string_starts_with(s, end, ":0x"))
      return WABT_ERROR;
    s += 3;

    for (; s < end; ++s) {
      uint32_t digit;
      if (WABT_FAILED(wabt_parse_hexdigit(*s, &digit)))
        return WABT_ERROR;
      tag = tag * 16 + digit;
      /* check for overflow */
      if (tag > F64_SIG_MASK)
        return WABT_ERROR;
    }

    /* NaN cannot have a zero tag, that is reserved for infinity */
    if (tag == 0)
      return WABT_ERROR;
  } else {
    tag = F64_QUIET_NAN_TAG;
  }

  *out_bits = make_double(is_neg, F64_MAX_EXP, tag);
  return WABT_OK;
}

static void parse_double_hex(const char* s,
                             const char* end,
                             uint64_t* out_bits) {
  WabtBool is_neg = WABT_FALSE;
  if (*s == '-') {
    is_neg = WABT_TRUE;
    s++;
  } else if (*s == '+') {
    s++;
  }
  assert(string_starts_with(s, end, "0x"));
  s += 2;

  /* see the similar comment in parse_float_hex */
  WabtBool seen_dot = WABT_FALSE;
  uint64_t significand = 0;
  /* how much to shift |significand| if a non-zero value is appended */
  int significand_shift = 0;
  int significand_bits = 0; /* bits of |significand| */
  int significand_exponent = 0;  /* exponent adjustment due to dot placement */
  for (; s < end; ++s) {
    uint32_t digit;
    if (*s == '.') {
      if (significand != 0)
        significand_exponent += significand_shift;
      significand_shift = 0;
      seen_dot = WABT_TRUE;
      continue;
    } else if (WABT_FAILED(wabt_parse_hexdigit(*s, &digit))) {
      break;
    }
    significand_shift += HEX_DIGIT_BITS;
    if (digit != 0 && (significand == 0 ||
                       significand_bits + significand_shift <=
                           F64_SIG_BITS + 1 + HEX_DIGIT_BITS)) {
      if (significand != 0)
        significand <<= significand_shift;
      if (seen_dot)
        significand_exponent -= significand_shift;
      significand += digit;
      significand_shift = 0;
      significand_bits += HEX_DIGIT_BITS;
    }
  }

  if (!seen_dot)
    significand_exponent += significand_shift;

  assert(s < end && *s == 'p');
  s++;

  if (significand == 0) {
    /* 0 or -0 */
    *out_bits = make_double(is_neg, F64_MIN_EXP, 0);
    return;
  }

  assert(s < end);
  int exponent = 0;
  WabtBool exponent_is_neg = WABT_FALSE;
  /* exponent is always positive, but significand_exponent is signed.
   significand_exponent_add is negated if exponent will be negative, so it  can
   be easily summed to see if the exponent is too large (see below) */
  int significand_exponent_add = 0;
  if (*s == '-') {
    exponent_is_neg = WABT_TRUE;
    significand_exponent_add = -significand_exponent;
    s++;
  } else if (*s == '+') {
    s++;
    significand_exponent_add = significand_exponent;
  }

  for (; s < end; ++s) {
    uint32_t digit = (*s - '0');
    assert(digit <= 9);
    exponent = exponent * 10 + digit;
    if (exponent + significand_exponent_add >= F64_MAX_EXP)
      break;
  }

  if (exponent_is_neg)
    exponent = -exponent;

  significand_bits = sizeof(uint64_t) * 8 - wabt_clz_u64(significand);
  /* -1 for the implicit 1 bit of the significand */
  exponent += significand_exponent + significand_bits - 1;

  if (exponent >= F64_MAX_EXP) {
    /* inf or -inf */
    *out_bits = make_double(is_neg, F64_MAX_EXP, 0);
  } else if (exponent <= F64_MIN_EXP) {
    /* maybe subnormal */
    if (significand_bits > F64_SIG_BITS) {
      significand = shift_double_and_round_to_nearest(
          significand, significand_bits - F64_SIG_BITS);
    } else if (significand_bits < F64_SIG_BITS) {
      significand <<= (F64_SIG_BITS - significand_bits);
    }

    int shift = F64_MIN_EXP - exponent;
    if (shift < F64_SIG_BITS) {
      if (shift) {
        significand = shift_double_and_round_to_nearest(significand, shift) &
                      F64_SIG_MASK;
      }
      exponent = F64_MIN_EXP;

      if (significand != 0) {
        *out_bits = make_double(is_neg, exponent, significand);
        return;
      }
    }

    /* not subnormal, too small; return 0 or -0 */
    *out_bits = make_double(is_neg, F64_MIN_EXP, 0);
  } else {
    /* normal value */
    if (significand_bits > F64_SIG_PLUS_ONE_BITS) {
      significand = shift_double_and_round_to_nearest(
          significand, significand_bits - F64_SIG_PLUS_ONE_BITS);
      if (significand > F64_SIG_PLUS_ONE_MASK)
        exponent++;
    } else if (significand_bits < F64_SIG_PLUS_ONE_BITS) {
      significand <<= (F64_SIG_PLUS_ONE_BITS - significand_bits);
    }

    *out_bits = make_double(is_neg, exponent, significand & F64_SIG_MASK);
  }
}

static void parse_double_infinity(const char* s,
                                  const char* end,
                                  uint64_t* out_bits) {
  WabtBool is_neg = WABT_FALSE;
  if (*s == '-') {
    is_neg = WABT_TRUE;
    s++;
  } else if (*s == '+') {
    s++;
  }
  assert(string_starts_with(s, end, "infinity"));
  *out_bits = make_double(is_neg, F64_MAX_EXP, 0);
}

WabtResult wabt_parse_double(WabtLiteralType literal_type,
                             const char* s,
                             const char* end,
                             uint64_t* out_bits) {
  switch (literal_type) {
    case WABT_LITERAL_TYPE_INT:
    case WABT_LITERAL_TYPE_FLOAT: {
      errno = 0;
      char* endptr;
      double value;
      value = strtod(s, &endptr);
      if (endptr != end ||
          ((value == 0 || value == HUGE_VAL || value == -HUGE_VAL) &&
           errno != 0))
        return WABT_ERROR;

      memcpy(out_bits, &value, sizeof(value));
      return WABT_OK;
    }

    case WABT_LITERAL_TYPE_HEXFLOAT:
      parse_double_hex(s, end, out_bits);
      return WABT_OK;

    case WABT_LITERAL_TYPE_INFINITY:
      parse_double_infinity(s, end, out_bits);
      return WABT_OK;

    case WABT_LITERAL_TYPE_NAN:
      return parse_double_nan(s, end, out_bits);

    default:
      assert(0);
      return WABT_ERROR;
  }
}

void wabt_write_double_hex(char* out, size_t size, uint64_t bits) {
  /* 123456789012345678901234 */
  /* -0x#.#############p-#### */
  /* -nan:0x############# */
  /* -infinity */
  char buffer[WABT_MAX_DOUBLE_HEX];
  char* p = buffer;
  WabtBool is_neg = (bits >> F64_SIGN_SHIFT);
  int exp = ((bits >> F64_SIG_BITS) & F64_EXP_MASK) - F64_EXP_BIAS;
  uint64_t sig = bits & F64_SIG_MASK;

  if (is_neg)
    *p++ = '-';
  if (exp == F64_MAX_EXP) {
    /* infinity or nan */
    if (sig == 0) {
      strcpy(p, "infinity");
      p += 8;
    } else {
      strcpy(p, "nan");
      p += 3;
      if (sig != F64_QUIET_NAN_TAG) {
        strcpy(p, ":0x");
        p += 3;
        /* skip leading zeroes */
        int num_nybbles = sizeof(uint64_t) * 8 / 4;
        while ((sig & 0xf000000000000000ULL) == 0) {
          sig <<= 4;
          num_nybbles--;
        }
        while (num_nybbles) {
          uint32_t nybble = (sig >> (sizeof(uint64_t) * 8 - 4)) & 0xf;
          *p++ = s_hex_digits[nybble];
          sig <<= 4;
          --num_nybbles;
        }
      }
    }
  } else {
    WabtBool is_zero = sig == 0 && exp == F64_MIN_EXP;
    strcpy(p, "0x");
    p += 2;
    *p++ = is_zero ? '0' : '1';

    /* shift sig up so the top 4-bits are at the top of the uint32 */
    sig <<= sizeof(uint64_t) * 8 - F64_SIG_BITS;

    if (sig) {
      if (exp == F64_MIN_EXP) {
        /* subnormal; shift the significand up, and shift out the implicit 1 */
        uint32_t leading_zeroes = wabt_clz_u64(sig);
        if (leading_zeroes < 63)
          sig <<= leading_zeroes + 1;
        else
          sig = 0;
        exp -= leading_zeroes;
      }

      *p++ = '.';
      while (sig) {
        uint32_t nybble = (sig >> (sizeof(uint64_t) * 8 - 4)) & 0xf;
        *p++ = s_hex_digits[nybble];
        sig <<= 4;
      }
    }
    *p++ = 'p';
    if (is_zero) {
      strcpy(p, "+0");
      p += 2;
    } else {
      if (exp < 0) {
        *p++ = '-';
        exp = -exp;
      } else {
        *p++ = '+';
      }
      if (exp >= 1000) *p++ = '1';
      if (exp >= 100) *p++ = '0' + (exp / 100) % 10;
      if (exp >= 10) *p++ = '0' + (exp / 10) % 10;
      *p++ = '0' + exp % 10;
    }
  }

  size_t len = p - buffer;
  if (len >= size)
    len = size - 1;
  memcpy(out, buffer, len);
  out[len] = '\0';
}
