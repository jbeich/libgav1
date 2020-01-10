// Copyright 2019 The libgav1 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/dsp/convolve.h"
#include "src/utils/cpu.h"

#if LIBGAV1_ENABLE_NEON

#include <arm_neon.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "src/dsp/arm/common_neon.h"
#include "src/dsp/constants.h"
#include "src/dsp/dsp.h"
#include "src/utils/common.h"
#include "src/utils/compiler_attributes.h"

namespace libgav1 {
namespace dsp {
namespace low_bitdepth {
namespace {

constexpr int kBitdepth8 = 8;
constexpr int kIntermediateStride = kMaxSuperBlockSizeInPixels;
constexpr int kSubPixelMask = (1 << kSubPixelBits) - 1;
constexpr int kHorizontalOffset = 3;
constexpr int kFilterIndexShift = 6;

// Multiply every entry in |src[]| by the corresponding entry in |taps[]| and
// sum. The filters in |taps[]| are pre-shifted by 1. This prevents the final
// sum from outranging int16_t.
template <int filter_index, bool negative_outside_taps = false>
int16x8_t SumOnePassTaps(const uint8x8_t* const src,
                         const uint8x8_t* const taps) {
  uint16x8_t sum;
  if (filter_index == 0) {
    // 6 taps. + - + + - +
    sum = vmull_u8(src[0], taps[0]);
    // Unsigned overflow will result in a valid int16_t value.
    sum = vmlsl_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlsl_u8(sum, src[4], taps[4]);
    return vreinterpretq_s16_u16(vmlal_u8(sum, src[5], taps[5]));
  } else if (filter_index == 1 && negative_outside_taps) {
    // 6 taps. - + + + + -
    // Set a base we can subtract from.
    sum = vmull_u8(src[1], taps[1]);
    sum = vmlsl_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlal_u8(sum, src[4], taps[4]);
    return vreinterpretq_s16_u16(vmlsl_u8(sum, src[5], taps[5]));
  } else if (filter_index == 1) {
    // 6 taps. All are positive.
    sum = vmull_u8(src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlal_u8(sum, src[4], taps[4]);
    sum = vmlal_u8(sum, src[5], taps[5]);
  } else if (filter_index == 2) {
    // 8 taps. - + - + + - + -
    sum = vmull_u8(src[1], taps[1]);
    sum = vmlsl_u8(sum, src[0], taps[0]);
    sum = vmlsl_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlal_u8(sum, src[4], taps[4]);
    sum = vmlsl_u8(sum, src[5], taps[5]);
    sum = vmlal_u8(sum, src[6], taps[6]);
    return vreinterpretq_s16_u16(vmlsl_u8(sum, src[7], taps[7]));
  } else if (filter_index == 3) {
    // 2 taps. All are positive.
    sum = vmull_u8(src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
  } else if (filter_index == 4) {
    // 4 taps. - + + -
    sum = vmull_u8(src[1], taps[1]);
    sum = vmlsl_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    return vreinterpretq_s16_u16(vmlsl_u8(sum, src[3], taps[3]));
  } else if (filter_index == 5) {
    // 4 taps. All are positive.
    sum = vmull_u8(src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
  } else {
    sum = vdupq_n_u16(0);
  }
  return vreinterpretq_s16_u16(sum);
}

// Add an offset to ensure the sum is positive and it fits within uint16_t.
template <int filter_index, bool negative_outside_taps = false>
uint16x8_t SumVerticalTaps8To16(const uint8x8_t* const src,
                                const uint8x8_t taps[8]) {
  // The worst case sum of negative taps is -56. The worst case sum of positive
  // taps is 184. With the single pass versions of the Convolve we could safely
  // saturate to int16_t because it outranged the final shift and narrow to
  // uint8_t. For the Compound Convolve the output values are 16 bits so we
  // don't have that option.
  // 184 * 255 = 46920 which is greater than int16_t can hold, but not uint16_t.
  // The minimum value we need to handle is -56 * 255 = -14280.
  // By offsetting the sum with 1 << 14 = 16384 we ensure that the sum is never
  // negative and that 46920 + 16384 = 63304 fits comfortably in uint16_t. This
  // allows us to use 16 bit registers instead of 32 bit registers.
  // When considering the bit operations it is safe to ignore signedness. Due to
  // the magic of 2's complement and well defined rollover rules the bit
  // representations are equivalent.
  uint16x8_t sum = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 1));
  if (filter_index == 0) {
    sum = vmlal_u8(sum, src[0], taps[0]);
    sum = vmlsl_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlsl_u8(sum, src[4], taps[4]);
    sum = vmlal_u8(sum, src[5], taps[5]);
  } else if (filter_index == 1 && negative_outside_taps) {
    sum = vmlsl_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlal_u8(sum, src[4], taps[4]);
    sum = vmlsl_u8(sum, src[5], taps[5]);
  } else if (filter_index == 1) {
    sum = vmlal_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlal_u8(sum, src[4], taps[4]);
    sum = vmlal_u8(sum, src[5], taps[5]);
  } else if (filter_index == 2) {
    sum = vmlsl_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlsl_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
    sum = vmlal_u8(sum, src[4], taps[4]);
    sum = vmlsl_u8(sum, src[5], taps[5]);
    sum = vmlal_u8(sum, src[6], taps[6]);
    sum = vmlsl_u8(sum, src[7], taps[7]);
  } else if (filter_index == 3) {
    sum = vmlal_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
  } else if (filter_index == 4) {
    sum = vmlsl_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlsl_u8(sum, src[3], taps[3]);
  } else if (filter_index == 5) {
    sum = vmlal_u8(sum, src[0], taps[0]);
    sum = vmlal_u8(sum, src[1], taps[1]);
    sum = vmlal_u8(sum, src[2], taps[2]);
    sum = vmlal_u8(sum, src[3], taps[3]);
  }

  return sum;
}

// Special case for single pass non-compound.
template <int filter_index, bool negative_outside_taps>
int16x8_t SumHorizontalTaps(const uint8_t* const src,
                            const uint8x8_t* const v_tap) {
  uint8x8_t v_src[8];
  const uint8x16_t src_long = vld1q_u8(src);
  int16x8_t sum;

  if (filter_index < 2) {
    v_src[0] = vget_low_u8(vextq_u8(src_long, src_long, 1));
    v_src[1] = vget_low_u8(vextq_u8(src_long, src_long, 2));
    v_src[2] = vget_low_u8(vextq_u8(src_long, src_long, 3));
    v_src[3] = vget_low_u8(vextq_u8(src_long, src_long, 4));
    v_src[4] = vget_low_u8(vextq_u8(src_long, src_long, 5));
    v_src[5] = vget_low_u8(vextq_u8(src_long, src_long, 6));
    sum = SumOnePassTaps<filter_index, negative_outside_taps>(v_src, v_tap + 1);
  } else if (filter_index == 2) {
    v_src[0] = vget_low_u8(src_long);
    v_src[1] = vget_low_u8(vextq_u8(src_long, src_long, 1));
    v_src[2] = vget_low_u8(vextq_u8(src_long, src_long, 2));
    v_src[3] = vget_low_u8(vextq_u8(src_long, src_long, 3));
    v_src[4] = vget_low_u8(vextq_u8(src_long, src_long, 4));
    v_src[5] = vget_low_u8(vextq_u8(src_long, src_long, 5));
    v_src[6] = vget_low_u8(vextq_u8(src_long, src_long, 6));
    v_src[7] = vget_low_u8(vextq_u8(src_long, src_long, 7));
    sum = SumOnePassTaps<filter_index, negative_outside_taps>(v_src, v_tap);
  } else if (filter_index == 3) {
    v_src[0] = vget_low_u8(vextq_u8(src_long, src_long, 3));
    v_src[1] = vget_low_u8(vextq_u8(src_long, src_long, 4));
    sum = SumOnePassTaps<filter_index, negative_outside_taps>(v_src, v_tap + 3);
  } else if (filter_index > 3) {
    v_src[0] = vget_low_u8(vextq_u8(src_long, src_long, 2));
    v_src[1] = vget_low_u8(vextq_u8(src_long, src_long, 3));
    v_src[2] = vget_low_u8(vextq_u8(src_long, src_long, 4));
    v_src[3] = vget_low_u8(vextq_u8(src_long, src_long, 5));
    sum = SumOnePassTaps<filter_index, negative_outside_taps>(v_src, v_tap + 2);
  }
  return sum;
}

template <int filter_index, bool negative_outside_taps>
uint8x8_t SimpleHorizontalTaps(const uint8_t* const src,
                               const uint8x8_t* const v_tap) {
  int16x8_t sum =
      SumHorizontalTaps<filter_index, negative_outside_taps>(src, v_tap);

  // Normally the Horizontal pass does the downshift in two passes:
  // kInterRoundBitsHorizontal - 1 and then (kFilterBits -
  // kInterRoundBitsHorizontal). Each one uses a rounding shift. Combining them
  // requires adding the rounding offset from the skipped shift.
  constexpr int first_shift_rounding_bit = 1 << (kInterRoundBitsHorizontal - 2);

  sum = vaddq_s16(sum, vdupq_n_s16(first_shift_rounding_bit));
  return vqrshrun_n_s16(sum, kFilterBits - 1);
}

template <int filter_index, bool negative_outside_taps>
uint16x8_t CompoundHorizontalTaps(const uint8_t* const src,
                                  const uint8x8_t* const v_tap,
                                  const int inter_round_bits_vertical) {
  int16x8_t sum =
      SumHorizontalTaps<filter_index, negative_outside_taps>(src, v_tap);

  const int inter_round_vertical_shift =
      inter_round_bits_vertical - kFilterBits;
  const int16x8_t v_compound_round_offset =
      vdupq_n_s16(((1 << (kBitdepth8 + 4)) + (1 << (kBitdepth8 + 3))) >>
                  inter_round_vertical_shift);
  const int16x8_t v_inter_round_vertical_shift =
      vdupq_n_s16(-inter_round_vertical_shift);

  sum = vrshrq_n_s16(sum, kInterRoundBitsHorizontal - 1);
  sum = vrshlq_s16(sum, v_inter_round_vertical_shift);
  sum = vaddq_s16(sum, v_compound_round_offset);
  return vreinterpretq_u16_s16(sum);
}

template <int num_taps, int filter_index, bool negative_outside_taps>
uint16x8_t SumHorizontalTaps(const uint8_t* const src,
                             const uint8x8_t* const v_tap) {
  // Start with an offset to guarantee the sum is non negative.
  // This will end up being replaced by SumHorizontalTaps() function above.
  uint16x8_t v_sum = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 2));
  uint8x16_t v_src[8];
  v_src[0] = vld1q_u8(&src[0]);
  if (num_taps == 8) {
    v_src[1] = vextq_u8(v_src[0], v_src[0], 1);
    v_src[2] = vextq_u8(v_src[0], v_src[0], 2);
    v_src[3] = vextq_u8(v_src[0], v_src[0], 3);
    v_src[4] = vextq_u8(v_src[0], v_src[0], 4);
    v_src[5] = vextq_u8(v_src[0], v_src[0], 5);
    v_src[6] = vextq_u8(v_src[0], v_src[0], 6);
    v_src[7] = vextq_u8(v_src[0], v_src[0], 7);

    // tap signs : - + - + + - + -
    v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[0]), v_tap[0]);
    v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[1]), v_tap[1]);
    v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[2]), v_tap[2]);
    v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
    v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
    v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[5]), v_tap[5]);
    v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[6]), v_tap[6]);
    v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[7]), v_tap[7]);
  } else if (num_taps == 6) {
    v_src[1] = vextq_u8(v_src[0], v_src[0], 1);
    v_src[2] = vextq_u8(v_src[0], v_src[0], 2);
    v_src[3] = vextq_u8(v_src[0], v_src[0], 3);
    v_src[4] = vextq_u8(v_src[0], v_src[0], 4);
    v_src[5] = vextq_u8(v_src[0], v_src[0], 5);
    v_src[6] = vextq_u8(v_src[0], v_src[0], 6);
    if (filter_index == 0) {
      // tap signs : + - + + - +
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[1]), v_tap[1]);
      v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[2]), v_tap[2]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
      v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[5]), v_tap[5]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[6]), v_tap[6]);
    } else {
      if (negative_outside_taps) {
        // tap signs : - + + + + -
        v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[1]), v_tap[1]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[2]), v_tap[2]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[5]), v_tap[5]);
        v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[6]), v_tap[6]);
      } else {
        // tap signs : + + + + + +
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[1]), v_tap[1]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[2]), v_tap[2]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[5]), v_tap[5]);
        v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[6]), v_tap[6]);
      }
    }
  } else if (num_taps == 4) {
    v_src[2] = vextq_u8(v_src[0], v_src[0], 2);
    v_src[3] = vextq_u8(v_src[0], v_src[0], 3);
    v_src[4] = vextq_u8(v_src[0], v_src[0], 4);
    v_src[5] = vextq_u8(v_src[0], v_src[0], 5);
    if (filter_index == 4) {
      // tap signs : - + + -
      v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[2]), v_tap[2]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
      v_sum = vmlsl_u8(v_sum, vget_low_u8(v_src[5]), v_tap[5]);
    } else {
      // tap signs : + + + +
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[2]), v_tap[2]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
      v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[5]), v_tap[5]);
    }
  } else {
    assert(num_taps == 2);
    v_src[3] = vextq_u8(v_src[0], v_src[0], 3);
    v_src[4] = vextq_u8(v_src[0], v_src[0], 4);
    // tap signs : + +
    v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[3]), v_tap[3]);
    v_sum = vmlal_u8(v_sum, vget_low_u8(v_src[4]), v_tap[4]);
  }

  return v_sum;
}

template <int num_taps, int filter_index>
uint16x8_t SumHorizontalTaps2x2(const uint8_t* src, const ptrdiff_t src_stride,
                                const uint8x8_t* const v_tap) {
  constexpr int positive_offset_bits = kBitdepth8 + kFilterBits - 2;
  uint16x8_t sum = vdupq_n_u16(1 << positive_offset_bits);
  const uint8x8_t input0 = vld1_u8(src);
  src += src_stride;
  const uint8x8_t input1 = vld1_u8(src);
  uint8x8x2_t input = vzip_u8(input0, input1);

  if (num_taps == 2) {
    // tap signs : + +
    sum = vmlal_u8(sum, vext_u8(input.val[0], input.val[1], 6), v_tap[3]);
    sum = vmlal_u8(sum, input.val[1], v_tap[4]);
  } else if (filter_index == 4) {
    // tap signs : - + + -
    sum = vmlsl_u8(sum, RightShift<4 * 8>(input.val[0]), v_tap[2]);
    sum = vmlal_u8(sum, vext_u8(input.val[0], input.val[1], 6), v_tap[3]);
    sum = vmlal_u8(sum, input.val[1], v_tap[4]);
    sum = vmlsl_u8(sum, RightShift<2 * 8>(input.val[1]), v_tap[5]);
  } else {
    // tap signs : + + + +
    sum = vmlal_u8(sum, RightShift<4 * 8>(input.val[0]), v_tap[2]);
    sum = vmlal_u8(sum, vext_u8(input.val[0], input.val[1], 6), v_tap[3]);
    sum = vmlal_u8(sum, input.val[1], v_tap[4]);
    sum = vmlal_u8(sum, RightShift<2 * 8>(input.val[1]), v_tap[5]);
  }

  return vrshrq_n_u16(sum, kInterRoundBitsHorizontal - 1);
}

// TODO(johannkoenig): Combine with the other SumHorizontalTaps2x2().
template <int filter_index>
int16x8_t SumHorizontalTaps2x2(const uint8_t* src, const ptrdiff_t src_stride,
                               const uint8x8_t* const v_tap) {
  uint16x8_t sum;
  const uint8x8_t input0 = vld1_u8(src);
  src += src_stride;
  const uint8x8_t input1 = vld1_u8(src);
  uint8x8x2_t input = vzip_u8(input0, input1);

  if (filter_index == 3) {
    // tap signs : + +
    sum = vmull_u8(vext_u8(input.val[0], input.val[1], 6), v_tap[3]);
    sum = vmlal_u8(sum, input.val[1], v_tap[4]);
  } else if (filter_index == 4) {
    // tap signs : - + + -
    sum = vmull_u8(vext_u8(input.val[0], input.val[1], 6), v_tap[3]);
    sum = vmlsl_u8(sum, RightShift<4 * 8>(input.val[0]), v_tap[2]);
    sum = vmlal_u8(sum, input.val[1], v_tap[4]);
    sum = vmlsl_u8(sum, RightShift<2 * 8>(input.val[1]), v_tap[5]);
  } else {
    // tap signs : + + + +
    sum = vmull_u8(RightShift<4 * 8>(input.val[0]), v_tap[2]);
    sum = vmlal_u8(sum, vext_u8(input.val[0], input.val[1], 6), v_tap[3]);
    sum = vmlal_u8(sum, input.val[1], v_tap[4]);
    sum = vmlal_u8(sum, RightShift<2 * 8>(input.val[1]), v_tap[5]);
  }

  return vreinterpretq_s16_u16(sum);
}

template <int filter_index>
uint8x8_t SimpleHorizontalTaps2x2(const uint8_t* src,
                                  const ptrdiff_t src_stride,
                                  const uint8x8_t* const v_tap) {
  int16x8_t sum = SumHorizontalTaps2x2<filter_index>(src, src_stride, v_tap);

  // Normally the Horizontal pass does the downshift in two passes:
  // kInterRoundBitsHorizontal - 1 and then (kFilterBits -
  // kInterRoundBitsHorizontal). Each one uses a rounding shift. Combining them
  // requires adding the rounding offset from the skipped shift.
  constexpr int first_shift_rounding_bit = 1 << (kInterRoundBitsHorizontal - 2);

  sum = vaddq_u16(sum, vdupq_n_s16(first_shift_rounding_bit));
  return vqrshrun_n_s16(sum, kFilterBits - 1);
}

template <int filter_index>
uint16x8_t CompoundHorizontalTaps2x2(const uint8_t* src,
                                     const ptrdiff_t src_stride,
                                     const uint8x8_t* const v_tap,
                                     const int inter_round_bits_vertical) {
  int16x8_t sum = SumHorizontalTaps2x2<filter_index>(src, src_stride, v_tap);
  const int inter_round_vertical_shift =
      inter_round_bits_vertical - kFilterBits;
  const int16x8_t v_compound_round_offset =
      vdupq_n_s16(((1 << (kBitdepth8 + 4)) + (1 << (kBitdepth8 + 3))) >>
                  inter_round_vertical_shift);
  const int16x8_t v_inter_round_vertical_shift =
      vdupq_n_s16(-inter_round_vertical_shift);

  sum = vrshrq_n_s16(sum, kInterRoundBitsHorizontal - 1);
  sum = vrshlq_s16(sum, v_inter_round_vertical_shift);
  sum = vaddq_s16(sum, v_compound_round_offset);
  return vreinterpretq_u16_s16(sum);
}

template <int num_taps, int step, int filter_index,
          bool negative_outside_taps = true, bool is_2d = false,
          bool is_compound = false>
void FilterHorizontal(const uint8_t* src, const ptrdiff_t src_stride,
                      void* const dest, const ptrdiff_t pred_stride,
                      const int width, const int height,
                      const uint8x8_t* const v_tap,
                      const int inter_round_bits_vertical) {
  const int16x8_t v_inter_round_bits_0 =
      vdupq_n_s16(-(kInterRoundBitsHorizontal - 1));

  auto* dest8 = static_cast<uint8_t*>(dest);
  auto* dest16 = static_cast<uint16_t*>(dest);

  // 4 tap filters are never used when width > 4.
  if (num_taps != 4 && width > 4) {
    int y = 0;
    do {
      int x = 0;
      do {
        if (is_2d) {
          uint16x8_t v_sum =
              SumHorizontalTaps<num_taps, filter_index, negative_outside_taps>(
                  &src[x], v_tap);
          v_sum = vrshlq_u16(v_sum, v_inter_round_bits_0);
          vst1q_u16(&dest16[x], v_sum);
        } else if (is_compound) {
          const uint16x8_t v_sum =
              CompoundHorizontalTaps<filter_index, negative_outside_taps>(
                  &src[x], v_tap, inter_round_bits_vertical);
          vst1q_u16(&dest16[x], v_sum);
        } else {
          const uint8x8_t result =
              SimpleHorizontalTaps<filter_index, negative_outside_taps>(&src[x],
                                                                        v_tap);
          vst1_u8(&dest8[x], result);
        }
        x += step;
      } while (x < width);
      src += src_stride;
      dest8 += pred_stride;
      dest16 += pred_stride;
    } while (++y < height);
    return;
  }

  // Horizontal passes only needs to account for |num_taps| 2 and 4 when
  // |width| <= 4.
  assert(width <= 4);
  assert(num_taps <= 4);
  if (num_taps <= 4) {
    if (width == 4) {
      int y = 0;
      do {
        if (is_2d) {
          uint16x8_t v_sum =
              SumHorizontalTaps<num_taps, filter_index, negative_outside_taps>(
                  &src[0], v_tap);
          v_sum = vrshlq_u16(v_sum, v_inter_round_bits_0);
          vst1_u16(&dest16[0], vget_low_u16(v_sum));
        } else if (is_compound) {
          const uint16x8_t v_sum =
              CompoundHorizontalTaps<filter_index, negative_outside_taps>(
                  src, v_tap, inter_round_bits_vertical);
          vst1_u16(dest16, vget_low_u16(v_sum));
        } else {
          const uint8x8_t result =
              SimpleHorizontalTaps<filter_index, negative_outside_taps>(src,
                                                                        v_tap);
          StoreLo4(&dest8[0], result);
        }
        src += src_stride;
        dest8 += pred_stride;
        dest16 += pred_stride;
      } while (++y < height);
      return;
    }

    constexpr int positive_offset_bits = kBitdepth8 + kFilterBits - 2;

    int y = 0;
    do {
      if (is_2d) {
        const uint16x8_t sum = SumHorizontalTaps2x2<num_taps, filter_index>(
            src, src_stride, v_tap);

        dest16[0] = vgetq_lane_u16(sum, 0);
        dest16[1] = vgetq_lane_u16(sum, 2);
        dest16 += pred_stride;
        dest16[0] = vgetq_lane_u16(sum, 1);
        dest16[1] = vgetq_lane_u16(sum, 3);
        dest16 += pred_stride;
      } else if (is_compound) {
        const uint16x8_t sum = CompoundHorizontalTaps2x2<filter_index>(
            src, src_stride, v_tap, inter_round_bits_vertical);
        dest16[0] = vgetq_lane_u16(sum, 0);
        dest16[1] = vgetq_lane_u16(sum, 2);
        dest16 += pred_stride;
        dest16[0] = vgetq_lane_u16(sum, 1);
        dest16[1] = vgetq_lane_u16(sum, 3);
        dest16 += pred_stride;
      } else {
        const uint8x8_t sum =
            SimpleHorizontalTaps2x2<filter_index>(src, src_stride, v_tap);

        dest8[0] = vget_lane_u8(sum, 0);
        dest8[1] = vget_lane_u8(sum, 2);
        dest8 += pred_stride;

        dest8[0] = vget_lane_u8(sum, 1);
        dest8[1] = vget_lane_u8(sum, 3);
        dest8 += pred_stride;
      }

      src += src_stride << 1;
      y += 2;
    } while (y < height - 1);

    // The 2d filters have an odd |height| because the horizontal pass generates
    // context for the vertical pass.
    if (is_2d) {
      assert(height % 2 == 1);
      uint16x8_t sum = vdupq_n_u16(1 << positive_offset_bits);
      const uint8x8_t input = vld1_u8(src);
      if (filter_index == 3) {  // |num_taps| == 2
        sum = vmlal_u8(sum, RightShift<3 * 8>(input), v_tap[3]);
        sum = vmlal_u8(sum, RightShift<4 * 8>(input), v_tap[4]);
      } else if (filter_index == 4) {
        sum = vmlsl_u8(sum, RightShift<2 * 8>(input), v_tap[2]);
        sum = vmlal_u8(sum, RightShift<3 * 8>(input), v_tap[3]);
        sum = vmlal_u8(sum, RightShift<4 * 8>(input), v_tap[4]);
        sum = vmlsl_u8(sum, RightShift<5 * 8>(input), v_tap[5]);
      } else {
        assert(filter_index == 5);
        sum = vmlal_u8(sum, RightShift<2 * 8>(input), v_tap[2]);
        sum = vmlal_u8(sum, RightShift<3 * 8>(input), v_tap[3]);
        sum = vmlal_u8(sum, RightShift<4 * 8>(input), v_tap[4]);
        sum = vmlal_u8(sum, RightShift<5 * 8>(input), v_tap[5]);
      }
      sum = vrshrq_n_u16(sum, kInterRoundBitsHorizontal - 1);
      Store2<0>(dest16, sum);
    }
  }
}

// Process 16 bit inputs and output 32 bits.
template <int num_taps>
inline uint32x4_t Sum2DVerticalTaps4(const int16x4_t* const src,
                                     const int16x8_t taps) {
  // In order to get the rollover correct with the lengthening instruction we
  // need to treat these as signed so that they sign extend properly.
  const int16x4_t taps_lo = vget_low_s16(taps);
  const int16x4_t taps_hi = vget_high_s16(taps);
  // An offset to guarantee the sum is non negative. Captures 28 * -4590 =
  // 128520 (worst case negative value from horizontal pass). It should be
  // possible to use 1 << 17 (131072) instead of 1 << 18 but there probably
  // isn't any benefit.
  // |offset_bits| = bitdepth + 2 * kFilterBits - kInterRoundBitsHorizontal - 1
  // == 18
  int32x4_t sum = vdupq_n_s32(1 << 18);
  if (num_taps == 8) {
    sum = vmlal_lane_s16(sum, src[0], taps_lo, 0);
    sum = vmlal_lane_s16(sum, src[1], taps_lo, 1);
    sum = vmlal_lane_s16(sum, src[2], taps_lo, 2);
    sum = vmlal_lane_s16(sum, src[3], taps_lo, 3);
    sum = vmlal_lane_s16(sum, src[4], taps_hi, 0);
    sum = vmlal_lane_s16(sum, src[5], taps_hi, 1);
    sum = vmlal_lane_s16(sum, src[6], taps_hi, 2);
    sum = vmlal_lane_s16(sum, src[7], taps_hi, 3);
  } else if (num_taps == 6) {
    sum = vmlal_lane_s16(sum, src[0], taps_lo, 1);
    sum = vmlal_lane_s16(sum, src[1], taps_lo, 2);
    sum = vmlal_lane_s16(sum, src[2], taps_lo, 3);
    sum = vmlal_lane_s16(sum, src[3], taps_hi, 0);
    sum = vmlal_lane_s16(sum, src[4], taps_hi, 1);
    sum = vmlal_lane_s16(sum, src[5], taps_hi, 2);
  } else if (num_taps == 4) {
    sum = vmlal_lane_s16(sum, src[0], taps_lo, 2);
    sum = vmlal_lane_s16(sum, src[1], taps_lo, 3);
    sum = vmlal_lane_s16(sum, src[2], taps_hi, 0);
    sum = vmlal_lane_s16(sum, src[3], taps_hi, 1);
  } else if (num_taps == 2) {
    sum = vmlal_lane_s16(sum, src[0], taps_lo, 3);
    sum = vmlal_lane_s16(sum, src[1], taps_hi, 0);
  }

  // This is guaranteed to be positive. Convert it for the final shift.
  const uint32x4_t return_val = vreinterpretq_u32_s32(sum);
  return return_val;
}

// Process 16 bit inputs and output 32 bits.
template <int num_taps>
uint32x4x2_t Sum2DVerticalTaps(const int16x8_t* const src,
                               const int16x8_t taps) {
  // In order to get the rollover correct with the lengthening instruction we
  // need to treat these as signed so that they sign extend properly.
  const int16x4_t taps_lo = vget_low_s16(taps);
  const int16x4_t taps_hi = vget_high_s16(taps);
  // An offset to guarantee the sum is non negative. Captures 28 * -4590 =
  // 128520 (worst case negative value from horizontal pass). It should be
  // possible to use 1 << 17 (131072) instead of 1 << 18 but there probably
  // isn't any benefit.
  // |offset_bits| = bitdepth + 2 * kFilterBits - kInterRoundBitsHorizontal - 1
  // == 18
  int32x4_t sum_lo = vdupq_n_s32(1 << 18);
  int32x4_t sum_hi = sum_lo;
  if (num_taps == 8) {
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[0]), taps_lo, 0);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[0]), taps_lo, 0);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[1]), taps_lo, 1);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[1]), taps_lo, 1);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[2]), taps_lo, 2);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[2]), taps_lo, 2);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[3]), taps_lo, 3);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[3]), taps_lo, 3);

    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[4]), taps_hi, 0);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[4]), taps_hi, 0);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[5]), taps_hi, 1);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[5]), taps_hi, 1);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[6]), taps_hi, 2);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[6]), taps_hi, 2);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[7]), taps_hi, 3);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[7]), taps_hi, 3);
  } else if (num_taps == 6) {
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[0]), taps_lo, 1);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[0]), taps_lo, 1);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[1]), taps_lo, 2);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[1]), taps_lo, 2);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[2]), taps_lo, 3);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[2]), taps_lo, 3);

    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[3]), taps_hi, 0);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[3]), taps_hi, 0);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[4]), taps_hi, 1);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[4]), taps_hi, 1);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[5]), taps_hi, 2);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[5]), taps_hi, 2);
  } else if (num_taps == 4) {
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[0]), taps_lo, 2);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[0]), taps_lo, 2);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[1]), taps_lo, 3);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[1]), taps_lo, 3);

    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[2]), taps_hi, 0);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[2]), taps_hi, 0);
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[3]), taps_hi, 1);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[3]), taps_hi, 1);
  } else if (num_taps == 2) {
    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[0]), taps_lo, 3);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[0]), taps_lo, 3);

    sum_lo = vmlal_lane_s16(sum_lo, vget_low_s16(src[1]), taps_hi, 0);
    sum_hi = vmlal_lane_s16(sum_hi, vget_high_s16(src[1]), taps_hi, 0);
  }

  // This is guaranteed to be positive. Convert it for the final shift.
  const uint32x4x2_t return_val = {vreinterpretq_u32_s32(sum_lo),
                                   vreinterpretq_u32_s32(sum_hi)};
  return return_val;
}

template <int num_taps, bool is_compound = false>
void Filter2DVertical(const uint16_t* src, void* const dst,
                      const ptrdiff_t dst_stride, const int width,
                      const int height, const int16x8_t taps,
                      const int inter_round_bits_vertical) {
  assert(width >= 8);
  constexpr int next_row = num_taps - 1;
  const int32x4_t v_inter_round_bits_vertical =
      vdupq_n_s32(-(inter_round_bits_vertical - 1));
  // The Horizontal pass uses |width| as |stride| for the intermediate buffer.
  const ptrdiff_t src_stride = width;

  auto* dst8 = static_cast<uint8_t*>(dst);
  auto* dst16 = static_cast<uint16_t*>(dst);

  int x = 0;
  do {
    int16x8_t srcs[8];
    const uint16_t* src_x = src + x;
    srcs[0] = vreinterpretq_s16_u16(vld1q_u16(src_x));
    src_x += src_stride;
    if (num_taps >= 4) {
      srcs[1] = vreinterpretq_s16_u16(vld1q_u16(src_x));
      src_x += src_stride;
      srcs[2] = vreinterpretq_s16_u16(vld1q_u16(src_x));
      src_x += src_stride;
      if (num_taps >= 6) {
        srcs[3] = vreinterpretq_s16_u16(vld1q_u16(src_x));
        src_x += src_stride;
        srcs[4] = vreinterpretq_s16_u16(vld1q_u16(src_x));
        src_x += src_stride;
        if (num_taps == 8) {
          srcs[5] = vreinterpretq_s16_u16(vld1q_u16(src_x));
          src_x += src_stride;
          srcs[6] = vreinterpretq_s16_u16(vld1q_u16(src_x));
          src_x += src_stride;
        }
      }
    }

    int y = 0;
    do {
      srcs[next_row] = vreinterpretq_s16_u16(vld1q_u16(src_x));
      src_x += src_stride;

      const uint32x4x2_t sums = Sum2DVerticalTaps<num_taps>(srcs, taps);
      if (is_compound) {
        const uint16x8_t results = vcombine_u16(
            vmovn_u32(vqrshlq_u32(sums.val[0], v_inter_round_bits_vertical)),
            vmovn_u32(vqrshlq_u32(sums.val[1], v_inter_round_bits_vertical)));
        vst1q_u16(dst16 + x + y * dst_stride, results);
      } else {
        const uint16x8_t first_shift = vcombine_u16(
            vqrshrn_n_u32(sums.val[0], kInterRoundBitsVertical - 1),
            vqrshrn_n_u32(sums.val[1], kInterRoundBitsVertical - 1));
        // |single_round_offset| == (1 << bitdepth) + (1 << (bitdepth - 1)) ==
        // 384
        const uint8x8_t results =
            vqmovn_u16(vqsubq_u16(first_shift, vdupq_n_u16(384)));

        vst1_u8(dst8 + x + y * dst_stride, results);
      }

      srcs[0] = srcs[1];
      if (num_taps >= 4) {
        srcs[1] = srcs[2];
        srcs[2] = srcs[3];
        if (num_taps >= 6) {
          srcs[3] = srcs[4];
          srcs[4] = srcs[5];
          if (num_taps == 8) {
            srcs[5] = srcs[6];
            srcs[6] = srcs[7];
          }
        }
      }
    } while (++y < height);
    x += 8;
  } while (x < width);
}

// Take advantage of |src_stride| == |width| to process two rows at a time.
template <int num_taps, bool is_compound = false>
void Filter2DVertical4xH(const uint16_t* src, void* const dst,
                         const ptrdiff_t dst_stride, const int height,
                         const int16x8_t taps,
                         const int inter_round_bits_vertical) {
  const int32x4_t v_inter_round_bits_vertical =
      vdupq_n_s32(-(inter_round_bits_vertical - 1));

  auto* dst8 = static_cast<uint8_t*>(dst);
  auto* dst16 = static_cast<uint16_t*>(dst);

  int16x8_t srcs[9];
  srcs[0] = vreinterpretq_s16_u16(vld1q_u16(src));
  src += 8;
  if (num_taps >= 4) {
    srcs[2] = vreinterpretq_s16_u16(vld1q_u16(src));
    src += 8;
    srcs[1] = vcombine_s16(vget_high_s16(srcs[0]), vget_low_s16(srcs[2]));
    if (num_taps >= 6) {
      srcs[4] = vreinterpretq_s16_u16(vld1q_u16(src));
      src += 8;
      srcs[3] = vcombine_s16(vget_high_s16(srcs[2]), vget_low_s16(srcs[4]));
      if (num_taps == 8) {
        srcs[6] = vreinterpretq_s16_u16(vld1q_u16(src));
        src += 8;
        srcs[5] = vcombine_s16(vget_high_s16(srcs[4]), vget_low_s16(srcs[6]));
      }
    }
  }

  int y = 0;
  do {
    srcs[num_taps] = vreinterpretq_s16_u16(vld1q_u16(src));
    src += 8;
    srcs[num_taps - 1] = vcombine_s16(vget_high_s16(srcs[num_taps - 2]),
                                      vget_low_s16(srcs[num_taps]));

    const uint32x4x2_t sums = Sum2DVerticalTaps<num_taps>(srcs, taps);
    if (is_compound) {
      const uint16x8_t results = vcombine_u16(
          vmovn_u32(vqrshlq_u32(sums.val[0], v_inter_round_bits_vertical)),
          vmovn_u32(vqrshlq_u32(sums.val[1], v_inter_round_bits_vertical)));
      vst1_u16(dst16, vget_low_u16(results));
      dst16 += dst_stride;
      vst1_u16(dst16, vget_high_u16(results));
      dst16 += dst_stride;
    } else {
      const uint16x8_t first_shift =
          vcombine_u16(vqrshrn_n_u32(sums.val[0], kInterRoundBitsVertical - 1),
                       vqrshrn_n_u32(sums.val[1], kInterRoundBitsVertical - 1));
      // |single_round_offset| == (1 << bitdepth) + (1 << (bitdepth - 1)) ==
      // 384
      const uint8x8_t results =
          vqmovn_u16(vqsubq_u16(first_shift, vdupq_n_u16(384)));

      StoreLo4(dst8, results);
      dst8 += dst_stride;
      StoreHi4(dst8, results);
      dst8 += dst_stride;
    }

    srcs[0] = srcs[2];
    if (num_taps >= 4) {
      srcs[1] = srcs[3];
      srcs[2] = srcs[4];
      if (num_taps >= 6) {
        srcs[3] = srcs[5];
        srcs[4] = srcs[6];
        if (num_taps == 8) {
          srcs[5] = srcs[7];
          srcs[6] = srcs[8];
        }
      }
    }
    y += 2;
  } while (y < height);
}

// Take advantage of |src_stride| == |width| to process four rows at a time.
template <int num_taps, bool is_compound = false>
void Filter2DVertical2xH(const uint16_t* src, void* const dst,
                         const ptrdiff_t dst_stride, const int height,
                         const int16x8_t taps,
                         const int inter_round_bits_vertical) {
  constexpr int next_row = (num_taps < 6) ? 4 : 8;
  const int32x4_t v_inter_round_bits_vertical =
      vdupq_n_s32(-(inter_round_bits_vertical - 1));

  auto* dst8 = static_cast<uint8_t*>(dst);
  auto* dst16 = static_cast<uint16_t*>(dst);

  int16x8_t srcs[9];
  srcs[0] = vreinterpretq_s16_u16(vld1q_u16(src));
  src += 8;
  if (num_taps >= 6) {
    srcs[4] = vreinterpretq_s16_u16(vld1q_u16(src));
    src += 8;
    srcs[1] = vextq_s16(srcs[0], srcs[4], 2);
    if (num_taps == 8) {
      srcs[2] = vcombine_s16(vget_high_s16(srcs[0]), vget_low_s16(srcs[4]));
      srcs[3] = vextq_s16(srcs[0], srcs[4], 6);
    }
  }

  int y = 0;
  do {
    srcs[next_row] = vreinterpretq_s16_u16(vld1q_u16(src));
    src += 8;
    if (num_taps == 2) {
      srcs[1] = vextq_s16(srcs[0], srcs[4], 2);
    } else if (num_taps == 4) {
      srcs[1] = vextq_s16(srcs[0], srcs[4], 2);
      srcs[2] = vcombine_s16(vget_high_s16(srcs[0]), vget_low_s16(srcs[4]));
      srcs[3] = vextq_s16(srcs[0], srcs[4], 6);
    } else if (num_taps == 6) {
      srcs[2] = vcombine_s16(vget_high_s16(srcs[0]), vget_low_s16(srcs[4]));
      srcs[3] = vextq_s16(srcs[0], srcs[4], 6);
      srcs[5] = vextq_s16(srcs[4], srcs[8], 2);
    } else if (num_taps == 8) {
      srcs[5] = vextq_s16(srcs[4], srcs[8], 2);
      srcs[6] = vcombine_s16(vget_high_s16(srcs[4]), vget_low_s16(srcs[8]));
      srcs[7] = vextq_s16(srcs[4], srcs[8], 6);
    }

    const uint32x4x2_t sums = Sum2DVerticalTaps<num_taps>(srcs, taps);
    if (is_compound) {
      const uint16x8_t results = vcombine_u16(
          vmovn_u32(vqrshlq_u32(sums.val[0], v_inter_round_bits_vertical)),
          vmovn_u32(vqrshlq_u32(sums.val[1], v_inter_round_bits_vertical)));
      Store2<0>(dst16, results);
      dst16 += dst_stride;
      Store2<1>(dst16, results);
      // When |height| <= 4 the taps are restricted to 2 and 4 tap variants.
      // Therefore we don't need to check this condition when |height| > 4.
      if (num_taps <= 4 && height == 2) return;
      dst16 += dst_stride;
      Store2<2>(dst16, results);
      dst16 += dst_stride;
      Store2<3>(dst16, results);
      dst16 += dst_stride;
    } else {
      const uint16x8_t first_shift =
          vcombine_u16(vqrshrn_n_u32(sums.val[0], kInterRoundBitsVertical - 1),
                       vqrshrn_n_u32(sums.val[1], kInterRoundBitsVertical - 1));
      // |single_round_offset| == (1 << bitdepth) + (1 << (bitdepth - 1)) ==
      // 384
      const uint8x8_t results =
          vqmovn_u16(vqsubq_u16(first_shift, vdupq_n_u16(384)));

      Store2<0>(dst8, results);
      dst8 += dst_stride;
      Store2<1>(dst8, results);
      // When |height| <= 4 the taps are restricted to 2 and 4 tap variants.
      // Therefore we don't need to check this condition when |height| > 4.
      if (num_taps <= 4 && height == 2) return;
      dst8 += dst_stride;
      Store2<2>(dst8, results);
      dst8 += dst_stride;
      Store2<3>(dst8, results);
      dst8 += dst_stride;
    }

    srcs[0] = srcs[4];
    if (num_taps == 6) {
      srcs[1] = srcs[5];
      srcs[4] = srcs[8];
    } else if (num_taps == 8) {
      srcs[1] = srcs[5];
      srcs[2] = srcs[6];
      srcs[3] = srcs[7];
      srcs[4] = srcs[8];
    }

    y += 4;
  } while (y < height);
}

template <bool is_2d = false, bool is_compound = false>
LIBGAV1_ALWAYS_INLINE void DoHorizontalPass(
    const uint8_t* const src, const ptrdiff_t src_stride, void* const dst,
    const ptrdiff_t dst_stride, const int width, const int height,
    const int subpixel, const int filter_index,
    const int inter_round_bits_vertical) {
  // Duplicate the absolute value for each tap.  Negative taps are corrected
  // by using the vmlsl_u8 instruction.  Positive taps use vmlal_u8.
  uint8x8_t v_tap[kSubPixelTaps];
  const int filter_id = (subpixel >> 6) & kSubPixelMask;
  for (int k = 0; k < kSubPixelTaps; ++k) {
    v_tap[k] = vreinterpret_u8_s8(
        vabs_s8(vdup_n_s8(kHalfSubPixelFilters[filter_index][filter_id][k])));
  }

  if (filter_index == 2) {  // 8 tap.
    FilterHorizontal<8, 8, 2, true, is_2d, is_compound>(
        src, src_stride, dst, dst_stride, width, height, v_tap,
        inter_round_bits_vertical);
  } else if (filter_index == 1) {  // 6 tap.
    // Check if outside taps are positive.
    if ((filter_id == 1) | (filter_id == 15)) {
      FilterHorizontal<6, 8, 1, false, is_2d, is_compound>(
          src, src_stride, dst, dst_stride, width, height, v_tap,
          inter_round_bits_vertical);
    } else {
      FilterHorizontal<6, 8, 1, true, is_2d, is_compound>(
          src, src_stride, dst, dst_stride, width, height, v_tap,
          inter_round_bits_vertical);
    }
  } else if (filter_index == 0) {  // 6 tap.
    FilterHorizontal<6, 8, 0, true, is_2d, is_compound>(
        src, src_stride, dst, dst_stride, width, height, v_tap,
        inter_round_bits_vertical);
  } else if (filter_index == 4) {  // 4 tap.
    FilterHorizontal<4, 8, 4, true, is_2d, is_compound>(
        src, src_stride, dst, dst_stride, width, height, v_tap,
        inter_round_bits_vertical);
  } else if (filter_index == 5) {  // 4 tap.
    FilterHorizontal<4, 8, 5, true, is_2d, is_compound>(
        src, src_stride, dst, dst_stride, width, height, v_tap,
        inter_round_bits_vertical);
  } else {  // 2 tap.
    FilterHorizontal<2, 8, 3, true, is_2d, is_compound>(
        src, src_stride, dst, dst_stride, width, height, v_tap,
        inter_round_bits_vertical);
  }
}

int GetNumTapsInFilter(const int filter_index) {
  if (filter_index < 2) {
    // Despite the names these only use 6 taps.
    // kInterpolationFilterEightTap
    // kInterpolationFilterEightTapSmooth
    return 6;
  }

  if (filter_index == 2) {
    // kInterpolationFilterEightTapSharp
    return 8;
  }

  if (filter_index == 3) {
    // kInterpolationFilterBilinear
    return 2;
  }

  assert(filter_index > 3);
  // For small sizes (width/height <= 4) the large filters are replaced with 4
  // tap options.
  // If the original filters were |kInterpolationFilterEightTap| or
  // |kInterpolationFilterEightTapSharp| then it becomes
  // |kInterpolationFilterSwitchable|.
  // If it was |kInterpolationFilterEightTapSmooth| then it becomes an unnamed 4
  // tap filter.
  return 4;
}

void Convolve2D_NEON(const void* const reference,
                     const ptrdiff_t reference_stride,
                     const int horizontal_filter_index,
                     const int vertical_filter_index,
                     const int /*inter_round_bits_vertical*/,
                     const int subpixel_x, const int subpixel_y,
                     const int width, const int height, void* prediction,
                     const ptrdiff_t pred_stride) {
  const int horiz_filter_index = GetFilterIndex(horizontal_filter_index, width);
  const int vert_filter_index = GetFilterIndex(vertical_filter_index, height);
  const int vertical_taps = GetNumTapsInFilter(vert_filter_index);

  // The output of the horizontal filter is guaranteed to fit in 16 bits.
  uint16_t
      intermediate_result[kMaxSuperBlockSizeInPixels *
                          (kMaxSuperBlockSizeInPixels + kSubPixelTaps - 1)];
  const int intermediate_height = height + vertical_taps - 1;

  const ptrdiff_t src_stride = reference_stride;
  const auto* src = static_cast<const uint8_t*>(reference) -
                    (vertical_taps / 2 - 1) * src_stride - kHorizontalOffset;

  DoHorizontalPass</*is_2d=*/true>(src, src_stride, intermediate_result, width,
                                   width, intermediate_height, subpixel_x,
                                   horiz_filter_index, kFilterBits);

  // Vertical filter.
  auto* dest = static_cast<uint8_t*>(prediction);
  const ptrdiff_t dest_stride = pred_stride;
  const int filter_id = ((subpixel_y & 1023) >> 6) & kSubPixelMask;
  const int16x8_t taps =
      vmovl_s8(vld1_s8(kHalfSubPixelFilters[vert_filter_index][filter_id]));

  if (vertical_taps == 8) {
    if (width == 2) {
      Filter2DVertical2xH<8>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else if (width == 4) {
      Filter2DVertical4xH<8>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else {
      Filter2DVertical<8>(intermediate_result, dest, dest_stride, width, height,
                          taps, 0);
    }
  } else if (vertical_taps == 6) {
    if (width == 2) {
      Filter2DVertical2xH<6>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else if (width == 4) {
      Filter2DVertical4xH<6>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else {
      Filter2DVertical<6>(intermediate_result, dest, dest_stride, width, height,
                          taps, 0);
    }
  } else if (vertical_taps == 4) {
    if (width == 2) {
      Filter2DVertical2xH<4>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else if (width == 4) {
      Filter2DVertical4xH<4>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else {
      Filter2DVertical<4>(intermediate_result, dest, dest_stride, width, height,
                          taps, 0);
    }
  } else {  // |vertical_taps| == 2
    if (width == 2) {
      Filter2DVertical2xH<2>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else if (width == 4) {
      Filter2DVertical4xH<2>(intermediate_result, dest, dest_stride, height,
                             taps, 0);
    } else {
      Filter2DVertical<2>(intermediate_result, dest, dest_stride, width, height,
                          taps, 0);
    }
  }
}

// There are many opportunities for overreading in scaled convolve, because the
// range of starting points for filter windows is anywhere from 0 to 16 for 8
// destination pixels, and the window sizes range from 2 to 8. To accommodate
// this range concisely, we use |grade_x| to mean the most steps in src that can
// be traversed in a single |step_x| increment, i.e. 1 or 2. When grade_x is 2,
// we are guaranteed to exceed 8 whole steps in src for every 8 |step_x|
// increments. The first load covers the initial elements of src_x, while the
// final load covers the taps.
template <int grade_x>
inline uint8x8x3_t LoadSrcVals(const uint8_t* src_x) {
  uint8x8x3_t ret;
  const uint8x16_t src_val = vld1q_u8(src_x);
  ret.val[0] = vget_low_u8(src_val);
  ret.val[1] = vget_high_u8(src_val);
  if (grade_x > 1) {
    ret.val[2] = vld1_u8(src_x + 16);
  }
  return ret;
}

// Positive indicates that the outputs can be used with vmlal_u8.
inline uint8x16_t GetPositive2TapFilter(const int tap_index) {
  assert(tap_index < 2);
  static constexpr uint8_t kSubPixel2TapFilterColumns[2][16] = {
      {128, 120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120}};

  return vld1q_u8(kSubPixel2TapFilterColumns[tap_index]);
}

template <int grade_x>
inline void ConvolveKernelHorizontal2Tap(const uint8_t* src,
                                         const ptrdiff_t src_stride,
                                         const int width, const int subpixel_x,
                                         const int step_x,
                                         const int intermediate_height,
                                         int16_t* intermediate) {
  // Account for the 0-taps that precede the 2 nonzero taps.
  const int kernel_offset = 3;
  const int ref_x = subpixel_x >> kScaleSubPixelBits;
  const int step_x8 = step_x << 3;
  const uint8x16_t filter_taps0 = GetPositive2TapFilter(0);
  const uint8x16_t filter_taps1 = GetPositive2TapFilter(1);
  const uint16x8_t sum = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 1));
  const uint16x8_t index_steps = vmulq_n_u16(
      vmovl_u8(vcreate_u8(0x0706050403020100)), static_cast<uint16_t>(step_x));
  const uint8x8_t filter_index_mask = vdup_n_u8(kSubPixelMask);

  int p = subpixel_x;
  if (width <= 4) {
    const uint8_t* src_x =
        &src[(p >> kScaleSubPixelBits) - ref_x + kernel_offset];
    // Only add steps to the 10-bit truncated p to avoid overflow.
    const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
    const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
    const uint8x8_t filter_indices =
        vand_u8(vshrn_n_u16(subpel_index_offsets, 6), filter_index_mask);
    // This is a special case. The 2-tap filter has no negative taps, so we
    // can use unsigned values.
    // For each x, a lane of tapsK has
    // kSubPixelFilters[filter_index][filter_id][k], where filter_id depends
    // on x.
    const uint8x8_t taps0 = VQTbl1U8(filter_taps0, filter_indices);
    const uint8x8_t taps1 = VQTbl1U8(filter_taps1, filter_indices);
    int y = 0;
    do {
      // Load a pool of samples to select from using stepped indices.
      const uint8x16_t src_vals = vld1q_u8(src_x);
      const uint8x8_t src_indices =
          vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));

      // For each x, a lane of srcK contains src_x[k].
      const uint8x8_t src0 = VQTbl1U8(src_vals, src_indices);
      const uint8x8_t src1 =
          VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(1)));

      const uint16x8_t product0 = vmlal_u8(sum, taps0, src0);
      // product0 + product1
      const uint16x8_t result = vmlal_u8(product0, taps1, src1);

      vst1q_s16(intermediate, vreinterpretq_s16_u16(vrshrq_n_u16(
                                  result, kInterRoundBitsHorizontal)));
      src_x += src_stride;
      intermediate += kIntermediateStride;
    } while (++y < intermediate_height);
    return;
  }

  // |width| >= 8
  int x = 0;
  do {
    const uint8_t* src_x =
        &src[(p >> kScaleSubPixelBits) - ref_x + kernel_offset];
    int16_t* intermediate_x = intermediate + x;
    // Only add steps to the 10-bit truncated p to avoid overflow.
    const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
    const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
    const uint8x8_t filter_indices =
        vand_u8(vshrn_n_u16(subpel_index_offsets, kFilterIndexShift),
                filter_index_mask);
    // This is a special case. The 2-tap filter has no negative taps, so we
    // can use unsigned values.
    // For each x, a lane of tapsK has
    // kSubPixelFilters[filter_index][filter_id][k], where filter_id depends
    // on x.
    const uint8x8_t taps0 = VQTbl1U8(filter_taps0, filter_indices);
    const uint8x8_t taps1 = VQTbl1U8(filter_taps1, filter_indices);
    int y = 0;
    do {
      // Load a pool of samples to select from using stepped indices.
      const uint8x8x3_t src_vals = LoadSrcVals<grade_x>(src_x);
      const uint8x8_t src_indices =
          vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));

      // For each x, a lane of srcK contains src_x[k].
      const uint8x8_t src0 = vtbl3_u8(src_vals, src_indices);
      const uint8x8_t src1 =
          vtbl3_u8(src_vals, vadd_u8(src_indices, vdup_n_u8(1)));

      const uint16x8_t product0 = vmlal_u8(sum, taps0, src0);
      // product0 + product1
      const uint16x8_t result = vmlal_u8(product0, taps1, src1);

      vst1q_s16(intermediate_x, vreinterpretq_s16_u16(vrshrq_n_u16(
                                    result, kInterRoundBitsHorizontal)));
      src_x += src_stride;
      intermediate_x += kIntermediateStride;
    } while (++y < intermediate_height);
    x += 8;
    p += step_x8;
  } while (x < width);
}

// Positive indicates that the outputs can be used with vmlal_u8.
inline uint8x16_t GetPositive4TapFilter(const int tap_index) {
  assert(tap_index < 4);
  static constexpr uint8_t kSubPixel4TapPositiveFilterColumns[4][16] = {
      {0, 30, 26, 22, 20, 18, 16, 14, 12, 12, 10, 8, 6, 4, 4, 2},
      {128, 62, 62, 62, 60, 58, 56, 54, 52, 48, 46, 44, 42, 40, 36, 34},
      {0, 34, 36, 40, 42, 44, 46, 48, 52, 54, 56, 58, 60, 62, 62, 62},
      {0, 2, 4, 4, 6, 8, 10, 12, 12, 14, 16, 18, 20, 22, 26, 30}};

  return vld1q_u8(kSubPixel4TapPositiveFilterColumns[tap_index]);
}

// This filter is only possible when width <= 4.
void ConvolveKernelHorizontalPositive4Tap(
    const uint8_t* src, const ptrdiff_t src_stride, const int subpixel_x,
    const int step_x, const int intermediate_height, int16_t* intermediate) {
  const int kernel_offset = 2;
  const int ref_x = subpixel_x >> kScaleSubPixelBits;
  const uint8x8_t filter_index_mask = vdup_n_u8(kSubPixelMask);
  const uint8x16_t filter_taps0 = GetPositive4TapFilter(0);
  const uint8x16_t filter_taps1 = GetPositive4TapFilter(1);
  const uint8x16_t filter_taps2 = GetPositive4TapFilter(2);
  const uint8x16_t filter_taps3 = GetPositive4TapFilter(3);
  const uint16x8_t index_steps = vmulq_n_u16(
      vmovl_u8(vcreate_u8(0x0706050403020100)), static_cast<uint16_t>(step_x));
  const int p = subpixel_x;
  const uint16x8_t base = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 1));
  // First filter is special, just a 128 tap on the center.
  const uint8_t* src_x =
      &src[(p >> kScaleSubPixelBits) - ref_x + kernel_offset];
  // Only add steps to the 10-bit truncated p to avoid overflow.
  const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
  const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
  const uint8x8_t filter_indices = vand_u8(
      vshrn_n_u16(subpel_index_offsets, kFilterIndexShift), filter_index_mask);
  // Note that filter_id depends on x.
  // For each x, tapsK has kSubPixelFilters[filter_index][filter_id][k].
  const uint8x8_t taps0 = VQTbl1U8(filter_taps0, filter_indices);
  const uint8x8_t taps1 = VQTbl1U8(filter_taps1, filter_indices);
  const uint8x8_t taps2 = VQTbl1U8(filter_taps2, filter_indices);
  const uint8x8_t taps3 = VQTbl1U8(filter_taps3, filter_indices);

  const uint8x8_t src_indices =
      vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));
  int y = 0;
  do {
    // Load a pool of samples to select from using stepped index vectors.
    const uint8x16_t src_vals = vld1q_u8(src_x);

    // For each x, srcK contains src_x[k] where k=1.
    // Whereas taps come from different arrays, src pixels are drawn from the
    // same contiguous line.
    const uint8x8_t src0 = VQTbl1U8(src_vals, src_indices);
    const uint8x8_t src1 =
        VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(1)));
    const uint8x8_t src2 =
        VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(2)));
    const uint8x8_t src3 =
        VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(3)));

    uint16x8_t sum = vmlal_u8(base, taps0, src0);
    sum = vmlal_u8(sum, taps1, src1);
    sum = vmlal_u8(sum, taps2, src2);
    sum = vmlal_u8(sum, taps3, src3);

    vst1_s16(intermediate,
             vreinterpret_s16_u16(vrshr_n_u16(vget_low_u16(sum), 3)));

    src_x += src_stride;
    intermediate += kIntermediateStride;
  } while (++y < intermediate_height);
}

// Signed indicates that some of the outputs should be used with vmlsl_u8
// instead of vmlal_u8.
inline uint8x16_t GetSigned4TapFilter(const int tap_index) {
  assert(tap_index < 4);
  // The first and fourth taps of each filter are negative. However
  // 128 does not fit in an 8-bit signed integer. Thus we use subtraction to
  // keep everything unsigned.
  static constexpr uint8_t kSubPixel4TapSignedFilterColumns[4][16] = {
      {0, 4, 8, 10, 12, 12, 14, 12, 12, 10, 10, 10, 8, 6, 4, 2},
      {128, 126, 122, 116, 110, 102, 94, 84, 76, 66, 58, 48, 38, 28, 18, 8},
      {0, 8, 18, 28, 38, 48, 58, 66, 76, 84, 94, 102, 110, 116, 122, 126},
      {0, 2, 4, 6, 8, 10, 10, 10, 12, 12, 14, 12, 12, 10, 8, 4}};

  return vld1q_u8(kSubPixel4TapSignedFilterColumns[tap_index]);
}

// This filter is only possible when width <= 4.
inline void ConvolveKernelHorizontalSigned4Tap(
    const uint8_t* src, const ptrdiff_t src_stride, const int subpixel_x,
    const int step_x, const int intermediate_height, int16_t* intermediate) {
  const int kernel_offset = 2;
  const int ref_x = subpixel_x >> kScaleSubPixelBits;
  const uint8x8_t filter_index_mask = vdup_n_u8(kSubPixelMask);
  const uint8x16_t filter_taps0 = GetSigned4TapFilter(0);
  const uint8x16_t filter_taps1 = GetSigned4TapFilter(1);
  const uint8x16_t filter_taps2 = GetSigned4TapFilter(2);
  const uint8x16_t filter_taps3 = GetSigned4TapFilter(3);
  const uint16x8_t index_steps = vmulq_n_u16(vmovl_u8(vcreate_u8(0x03020100)),
                                             static_cast<uint16_t>(step_x));

  const uint16x8_t base = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 1));
  const int p = subpixel_x;
  const uint8_t* src_x =
      &src[(p >> kScaleSubPixelBits) - ref_x + kernel_offset];
  // Only add steps to the 10-bit truncated p to avoid overflow.
  const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
  const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
  const uint8x8_t filter_indices = vand_u8(
      vshrn_n_u16(subpel_index_offsets, kFilterIndexShift), filter_index_mask);
  // Note that filter_id depends on x.
  // For each x, tapsK has kSubPixelFilters[filter_index][filter_id][k].
  const uint8x8_t taps0 = VQTbl1U8(filter_taps0, filter_indices);
  const uint8x8_t taps1 = VQTbl1U8(filter_taps1, filter_indices);
  const uint8x8_t taps2 = VQTbl1U8(filter_taps2, filter_indices);
  const uint8x8_t taps3 = VQTbl1U8(filter_taps3, filter_indices);
  int y = 0;
  do {
    // Load a pool of samples to select from using stepped indices.
    const uint8x16_t src_vals = vld1q_u8(src_x);
    const uint8x8_t src_indices =
        vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));

    // For each x, srcK contains src_x[k] where k=1.
    // Whereas taps come from different arrays, src pixels are drawn from the
    // same contiguous line.
    const uint8x8_t src0 = VQTbl1U8(src_vals, src_indices);
    const uint8x8_t src1 =
        VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(1)));
    const uint8x8_t src2 =
        VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(2)));
    const uint8x8_t src3 =
        VQTbl1U8(src_vals, vadd_u8(src_indices, vdup_n_u8(3)));

    // Offsetting by base permits a guaranteed positive.
    uint16x8_t sum = vmlsl_u8(base, taps0, src0);
    sum = vmlal_u8(sum, taps1, src1);
    sum = vmlal_u8(sum, taps2, src2);
    sum = vmlsl_u8(sum, taps3, src3);

    vst1_s16(intermediate,
             vreinterpret_s16_u16(vrshr_n_u16(vget_low_u16(sum), 3)));
    src_x += src_stride;
    intermediate += kIntermediateStride;
  } while (++y < intermediate_height);
}

// Signed indicates that some of the outputs should be used with vmlsl_u8
// instead of vmlal_u8.
inline uint8x16_t GetSigned6TapFilter(const int tap_index) {
  assert(tap_index < 6);
  // The second and fourth taps of each filter are negative. However
  // 128 does not fit in an 8-bit signed integer. Thus we use subtraction to
  // keep everything unsigned.
  static constexpr uint8_t kSubPixel6TapSignedFilterColumns[6][16] = {
      {0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0},
      {0, 6, 10, 12, 14, 14, 16, 14, 14, 12, 12, 12, 10, 8, 4, 2},
      {128, 126, 122, 116, 110, 102, 94, 84, 76, 66, 58, 48, 38, 28, 18, 8},
      {0, 8, 18, 28, 38, 48, 58, 66, 76, 84, 94, 102, 110, 116, 122, 126},
      {0, 2, 4, 8, 10, 12, 12, 12, 14, 14, 16, 14, 14, 12, 10, 6},
      {0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}};

  return vld1q_u8(kSubPixel6TapSignedFilterColumns[tap_index]);
}

// This filter is only possible when width >= 8.
template <int grade_x>
inline void ConvolveKernelHorizontalSigned6Tap(
    const uint8_t* src, const ptrdiff_t src_stride, const int width,
    const int subpixel_x, const int step_x, const int intermediate_height,
    int16_t* intermediate) {
  const int kernel_offset = 1;
  const uint8x8_t one = vdup_n_u8(1);
  const uint8x8_t filter_index_mask = vdup_n_u8(kSubPixelMask);
  const int ref_x = subpixel_x >> kScaleSubPixelBits;
  const int step_x8 = step_x << 3;
  uint8x16_t filter_taps[6];
  for (int i = 0; i < 6; ++i) {
    filter_taps[i] = GetSigned6TapFilter(i);
  }
  const uint16x8_t index_steps = vmulq_n_u16(
      vmovl_u8(vcreate_u8(0x0706050403020100)), static_cast<uint16_t>(step_x));

  int x = 0;
  int p = subpixel_x;
  do {
    // Avoid overloading outside the reference boundaries. This means
    // |trailing_width| can be up to 24.
    const uint8_t* src_x =
        &src[(p >> kScaleSubPixelBits) - ref_x + kernel_offset];
    int16_t* intermediate_x = intermediate + x;
    // Only add steps to the 10-bit truncated p to avoid overflow.
    const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
    const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
    const uint8x8_t src_indices =
        vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));
    uint8x8_t src_lookup[6];
    src_lookup[0] = src_indices;
    for (int i = 1; i < 6; ++i) {
      src_lookup[i] = vadd_u8(src_lookup[i - 1], one);
    }

    const uint8x8_t filter_indices =
        vand_u8(vshrn_n_u16(subpel_index_offsets, kFilterIndexShift),
                filter_index_mask);
    // For each x, a lane of taps[k] has
    // kSubPixelFilters[filter_index][filter_id][k], where filter_id depends
    // on x.
    uint8x8_t taps[6];
    for (int i = 0; i < 6; ++i) {
      taps[i] = VQTbl1U8(filter_taps[i], filter_indices);
    }
    int y = 0;
    do {
      uint16x8_t sum = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 1));
      // Load a pool of samples to select from using stepped indices.
      const uint8x8x3_t src_vals = LoadSrcVals<grade_x>(src_x);

      // tap signs : + - + + - +
      sum = vmlal_u8(sum, taps[0], vtbl3_u8(src_vals, src_lookup[0]));
      sum = vmlsl_u8(sum, taps[1], vtbl3_u8(src_vals, src_lookup[1]));
      sum = vmlal_u8(sum, taps[2], vtbl3_u8(src_vals, src_lookup[2]));
      sum = vmlal_u8(sum, taps[3], vtbl3_u8(src_vals, src_lookup[3]));
      sum = vmlsl_u8(sum, taps[4], vtbl3_u8(src_vals, src_lookup[4]));
      sum = vmlal_u8(sum, taps[5], vtbl3_u8(src_vals, src_lookup[5]));

      vst1q_s16(intermediate_x, vreinterpretq_s16_u16(vrshrq_n_u16(
                                    sum, kInterRoundBitsHorizontal)));
      src_x += src_stride;
      intermediate_x += kIntermediateStride;
    } while (++y < intermediate_height);
    x += 8;
    p += step_x8;
  } while (x < width);
}

// Positive indicates that the outputs can be used with vmlal_u8. Filter 1 has
// its tap columns divided between positive and mixed treatment.
inline uint8x16_t GetPositive6TapFilter(const int tap_index) {
  assert(tap_index < 6);
  static constexpr uint8_t kSubPixel6TapPositiveFilterColumns[4][16] = {
      {0, 28, 26, 22, 20, 18, 16, 16, 14, 12, 10, 8, 6, 4, 4, 2},
      {128, 62, 62, 62, 60, 58, 56, 54, 52, 48, 46, 44, 42, 40, 36, 34},
      {0, 34, 36, 40, 42, 44, 46, 48, 52, 54, 56, 58, 60, 62, 62, 62},
      {0, 2, 4, 4, 6, 8, 10, 12, 14, 16, 16, 18, 20, 22, 26, 28}};

  return vld1q_u8(kSubPixel6TapPositiveFilterColumns[tap_index]);
}

// Mixed indicates that each column has both positive and negative values, so
// outputs should be used with vmlal_s8.
inline int8x16_t GetMixed6TapFilter(const int tap_index) {
  assert(tap_index < 2);
  static constexpr int8_t kSubPixel6TapMixedFilterColumns[2][16] = {
      {0, 2, 0, 0, 0, 0, 0, -2, -2, 0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, -2, -2, 0, 0, 0, 0, 0, 2}};

  return vld1q_s8(kSubPixel6TapMixedFilterColumns[tap_index]);
}

// This filter is only possible when width >= 8.
template <int grade_x>
inline void ConvolveKernelHorizontalMixed6Tap(
    const uint8_t* src, const ptrdiff_t src_stride, const int width,
    const int subpixel_x, const int step_x, const int intermediate_height,
    int16_t* intermediate) {
  const int kernel_offset = 1;
  const uint8x8_t one = vdup_n_u8(1);
  const uint8x8_t filter_index_mask = vdup_n_u8(kSubPixelMask);
  const int ref_x = subpixel_x >> kScaleSubPixelBits;
  const int step_x8 = step_x << 3;
  uint8x8_t taps[4];
  int16x8_t mixed_taps[2];
  uint8x16_t positive_filter_taps[4];
  for (int i = 0; i < 4; ++i) {
    positive_filter_taps[i] = GetPositive6TapFilter(i);
  }
  int8x16_t mixed_filter_taps[2];
  mixed_filter_taps[0] = GetMixed6TapFilter(0);
  mixed_filter_taps[1] = GetMixed6TapFilter(1);
  const uint16x8_t index_steps = vmulq_n_u16(
      vmovl_u8(vcreate_u8(0x0706050403020100)), static_cast<uint16_t>(step_x));

  int x = 0;
  int p = subpixel_x;
  do {
    const uint8_t* src_x =
        &src[(p >> kScaleSubPixelBits) - ref_x + kernel_offset];
    int16_t* intermediate_x = intermediate + x;
    // Only add steps to the 10-bit truncated p to avoid overflow.
    const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
    const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
    const uint8x8_t src_indices =
        vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));
    uint8x8_t src_lookup[6];
    src_lookup[0] = src_indices;
    for (int i = 1; i < 6; ++i) {
      src_lookup[i] = vadd_u8(src_lookup[i - 1], one);
    }

    const uint8x8_t filter_indices =
        vand_u8(vshrn_n_u16(subpel_index_offsets, kFilterIndexShift),
                filter_index_mask);
    // For each x, a lane of taps[k] has
    // kSubPixelFilters[filter_index][filter_id][k], where filter_id depends
    // on x.
    for (int i = 0; i < 4; ++i) {
      taps[i] = VQTbl1U8(positive_filter_taps[i], filter_indices);
    }
    mixed_taps[0] = vmovl_s8(VQTbl1S8(mixed_filter_taps[0], filter_indices));
    mixed_taps[1] = vmovl_s8(VQTbl1S8(mixed_filter_taps[1], filter_indices));

    int y = 0;
    do {
      int16x8_t base = vdupq_n_s16(1 << (kBitdepth8 + kFilterBits - 1));
      // Load a pool of samples to select from using stepped indices.
      const uint8x8x3_t src_vals = LoadSrcVals<grade_x>(src_x);

      base = vmlaq_s16(base, mixed_taps[0],
                       ZeroExtend(vtbl3_u8(src_vals, src_lookup[0])));
      base = vmlaq_s16(base, mixed_taps[1],
                       ZeroExtend(vtbl3_u8(src_vals, src_lookup[5])));
      uint16x8_t sum = vreinterpretq_u16_s16(base);
      sum = vmlal_u8(sum, taps[0], vtbl3_u8(src_vals, src_lookup[1]));
      sum = vmlal_u8(sum, taps[1], vtbl3_u8(src_vals, src_lookup[2]));
      sum = vmlal_u8(sum, taps[2], vtbl3_u8(src_vals, src_lookup[3]));
      sum = vmlal_u8(sum, taps[3], vtbl3_u8(src_vals, src_lookup[4]));

      vst1q_s16(intermediate_x, vreinterpretq_s16_u16(vrshrq_n_u16(
                                    sum, kInterRoundBitsHorizontal)));
      src_x += src_stride;
      intermediate_x += kIntermediateStride;
    } while (++y < intermediate_height);
    x += 8;
    p += step_x8;
  } while (x < width);
}

inline uint8x16_t GetSigned8TapFilter(const int tap_index) {
  assert(tap_index < 8);
  // The first and fourth taps of each filter are negative. However
  // 128 does not fit in an 8-bit signed integer. Thus we use subtraction to
  // keep everything unsigned.
  static constexpr uint8_t kSubPixel8TapSignedFilterColumns[8][16] = {
      {0, 2, 2, 2, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 0},
      {0, 2, 6, 8, 10, 10, 10, 10, 12, 10, 8, 8, 6, 6, 4, 2},
      {0, 6, 12, 18, 22, 22, 24, 24, 24, 22, 20, 18, 14, 10, 6, 2},
      {128, 126, 124, 120, 116, 108, 100, 90, 80, 70, 60, 48, 38, 26, 16, 8},
      {0, 8, 16, 26, 38, 48, 60, 70, 80, 90, 100, 108, 116, 120, 124, 126},
      {0, 2, 6, 10, 14, 18, 20, 22, 24, 24, 24, 22, 22, 18, 12, 6},
      {0, 2, 4, 6, 6, 8, 8, 10, 12, 10, 10, 10, 10, 8, 6, 2},
      {0, 0, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 2, 2, 2}};

  return vld1q_u8(kSubPixel8TapSignedFilterColumns[tap_index]);
}

// This filter is only possible when width >= 8.
template <int grade_x>
inline void ConvolveKernelHorizontalSigned8Tap(
    const uint8_t* src, const ptrdiff_t src_stride, const int width,
    const int subpixel_x, const int step_x, const int intermediate_height,
    int16_t* intermediate) {
  const uint8x8_t one = vdup_n_u8(1);
  const uint8x8_t filter_index_mask = vdup_n_u8(kSubPixelMask);
  const int ref_x = subpixel_x >> kScaleSubPixelBits;
  const int step_x8 = step_x << 3;
  uint8x8_t taps[8];
  uint8x16_t filter_taps[8];
  for (int i = 0; i < 8; ++i) {
    filter_taps[i] = GetSigned8TapFilter(i);
  }
  const uint16x8_t index_steps = vmulq_n_u16(
      vmovl_u8(vcreate_u8(0x0706050403020100)), static_cast<uint16_t>(step_x));
  int x = 0;
  int p = subpixel_x;
  do {
    const uint8_t* src_x = &src[(p >> kScaleSubPixelBits) - ref_x];
    int16_t* intermediate_x = intermediate + x;
    // Only add steps to the 10-bit truncated p to avoid overflow.
    const uint16x8_t p_fraction = vdupq_n_u16(p & 1023);
    const uint16x8_t subpel_index_offsets = vaddq_u16(index_steps, p_fraction);
    const uint8x8_t src_indices =
        vmovn_u16(vshrq_n_u16(subpel_index_offsets, kScaleSubPixelBits));
    uint8x8_t src_lookup[8];
    src_lookup[0] = src_indices;
    for (int i = 1; i < 8; ++i) {
      src_lookup[i] = vadd_u8(src_lookup[i - 1], one);
    }

    const uint8x8_t filter_indices =
        vand_u8(vshrn_n_u16(subpel_index_offsets, kFilterIndexShift),
                filter_index_mask);
    // For each x, a lane of taps[k] has
    // kSubPixelFilters[filter_index][filter_id][k], where filter_id depends
    // on x.
    for (int i = 0; i < 8; ++i) {
      taps[i] = VQTbl1U8(filter_taps[i], filter_indices);
    }

    int y = 0;
    do {
      uint16x8_t sum = vdupq_n_u16(1 << (kBitdepth8 + kFilterBits - 1));
      // Load a pool of samples to select from using stepped indices.
      const uint8x8x3_t src_vals = LoadSrcVals<grade_x>(src_x);

      // tap signs : - + - + + - + -
      sum = vmlsl_u8(sum, taps[0], vtbl3_u8(src_vals, src_lookup[0]));
      sum = vmlal_u8(sum, taps[1], vtbl3_u8(src_vals, src_lookup[1]));
      sum = vmlsl_u8(sum, taps[2], vtbl3_u8(src_vals, src_lookup[2]));
      sum = vmlal_u8(sum, taps[3], vtbl3_u8(src_vals, src_lookup[3]));
      sum = vmlal_u8(sum, taps[4], vtbl3_u8(src_vals, src_lookup[4]));
      sum = vmlsl_u8(sum, taps[5], vtbl3_u8(src_vals, src_lookup[5]));
      sum = vmlal_u8(sum, taps[6], vtbl3_u8(src_vals, src_lookup[6]));
      sum = vmlsl_u8(sum, taps[7], vtbl3_u8(src_vals, src_lookup[7]));

      vst1q_s16(intermediate_x, vreinterpretq_s16_u16(vrshrq_n_u16(
                                    sum, kInterRoundBitsHorizontal)));
      src_x += src_stride;
      intermediate_x += kIntermediateStride;
    } while (++y < intermediate_height);
    x += 8;
    p += step_x8;
  } while (x < width);
}

// This function handles blocks of width 2 or 4.
template <int num_taps, int grade_y, int width, bool is_compound>
void ConvolveVerticalScale4xH(const int16_t* src, const int subpixel_y,
                              const int inter_round_bits,
                              const int filter_index, const int step_y,
                              const int height, void* dest,
                              const ptrdiff_t dest_stride) {
  const int32x4_t v_inter_round_bits = vdupq_n_s32(-(inter_round_bits - 1));
  const uint16x4_t single_round_offset =
      vdup_n_u16((1 << kBitdepth8) + (1 << (kBitdepth8 - 1)));
  constexpr ptrdiff_t src_stride = kIntermediateStride;
  constexpr int kernel_offset = (8 - num_taps) / 2;
  src += src_stride * kernel_offset;
  const int16_t* src_y = src;
  // |dest| is 16-bit in compound mode, Pixel otherwise.
  uint16_t* dest16_y = static_cast<uint16_t*>(dest);
  uint8_t* dest_y = static_cast<uint8_t*>(dest);
  int16x4_t s[num_taps + grade_y];

  int p = subpixel_y & 1023;
  int prev_p = p;
  int y = 0;
  do {  // y < height
    for (int i = 0; i < num_taps; ++i) {
      s[i] = vld1_s16(src_y + i * src_stride);
    }
    int filter_id = (p >> 6) & kSubPixelMask;
    int16x8_t filter =
        vmovl_s8(vld1_s8(kHalfSubPixelFilters[filter_index][filter_id]));
    uint32x4_t sums = Sum2DVerticalTaps4<num_taps>(s, filter);
    uint16x4_t result = vmovn_u32(vrshlq_u32(sums, v_inter_round_bits));
    if (is_compound) {
      if (width == 2) {
        Store2<0>(dest16_y, result);
      } else {
        vst1_u16(dest16_y, result);
      }
    } else {
      result = vqsub_u16(result, single_round_offset);
      if (width == 2) {
        Store2<0>(dest_y, vqmovn_u16(vcombine_u16(result, result)));
      } else {
        StoreLo4(dest_y, vqmovn_u16(vcombine_u16(result, result)));
      }
    }
    p += step_y;
    const int p_diff =
        (p >> kScaleSubPixelBits) - (prev_p >> kScaleSubPixelBits);
    prev_p = p;
    // Here we load extra source in case it is needed. If |p_diff| == 0, these
    // values will be unused, but it's faster to load than to branch.
    s[num_taps] = vld1_s16(src_y + num_taps * src_stride);
    if (grade_y > 1) {
      s[num_taps + 1] = vld1_s16(src_y + (num_taps + 1) * src_stride);
    }
    dest16_y += dest_stride;
    dest_y += dest_stride;

    filter_id = (p >> 6) & kSubPixelMask;
    filter = vmovl_s8(vld1_s8(kHalfSubPixelFilters[filter_index][filter_id]));
    sums = Sum2DVerticalTaps4<num_taps>(&s[p_diff], filter);
    result = vmovn_u32(vrshlq_u32(sums, v_inter_round_bits));
    if (is_compound) {
      if (width == 2) {
        Store2<0>(dest16_y, result);
      } else {
        vst1_u16(dest16_y, result);
      }
    } else {
      result = vqsub_u16(result, single_round_offset);
      if (width == 2) {
        Store2<0>(dest_y, vqmovn_u16(vcombine_u16(result, result)));
      } else {
        StoreLo4(dest_y, vqmovn_u16(vcombine_u16(result, result)));
      }
    }
    p += step_y;
    src_y = src + (p >> kScaleSubPixelBits) * src_stride;
    prev_p = p;
    dest16_y += dest_stride;
    dest_y += dest_stride;

    y += 2;
  } while (y < height);
}

template <int num_taps, int grade_y, bool is_compound>
inline void ConvolveVerticalScale(const int16_t* src, const int width,
                                  const int subpixel_y,
                                  const int inter_round_bits,
                                  const int filter_index, const int step_y,
                                  const int height, void* dest,
                                  const ptrdiff_t dest_stride) {
  const int32x4_t v_inter_round_bits = vdupq_n_s32(-(inter_round_bits - 1));
  constexpr ptrdiff_t src_stride = kIntermediateStride;
  const uint16x8_t single_round_offset =
      vdupq_n_u16((1 << kBitdepth8) + (1 << (kBitdepth8 - 1)));
  constexpr int kernel_offset = (8 - num_taps) / 2;
  src += src_stride * kernel_offset;
  // A possible improvement is to use arithmetic to decide how many times to
  // apply filters to same source before checking whether to load new srcs.
  // However, this will only improve performance with very small step sizes.
  int16x8_t s[num_taps + grade_y];
  // |dest| is 16-bit in compound mode, Pixel otherwise.
  uint16_t* dest16_y;
  uint8_t* dest_y;

  int x = 0;
  do {  // x < width
    const int16_t* src_x = src + x;
    const int16_t* src_y = src_x;
    dest16_y = static_cast<uint16_t*>(dest) + x;
    dest_y = static_cast<uint8_t*>(dest) + x;
    int p = subpixel_y & 1023;
    int prev_p = p;
    int y = 0;
    do {  // y < height
      for (int i = 0; i < num_taps; ++i) {
        s[i] = vld1q_s16(src_y + i * src_stride);
      }
      int filter_id = (p >> 6) & kSubPixelMask;
      int16x8_t filter =
          vmovl_s8(vld1_s8(kHalfSubPixelFilters[filter_index][filter_id]));
      uint32x4x2_t sums = Sum2DVerticalTaps<num_taps>(s, filter);
      uint16x8_t result =
          vcombine_u16(vmovn_u32(vrshlq_u32(sums.val[0], v_inter_round_bits)),
                       vmovn_u32(vrshlq_u32(sums.val[1], v_inter_round_bits)));
      if (is_compound) {
        vst1q_u16(dest16_y, result);
      } else {
        result = vqsubq_u16(result, single_round_offset);
        vst1_u8(dest_y, vqmovn_u16(result));
      }
      p += step_y;
      const int p_diff =
          (p >> kScaleSubPixelBits) - (prev_p >> kScaleSubPixelBits);
      // |grade_y| > 1 always means p_diff > 0, so load vectors that may be
      // needed. Otherwise, we only need to load one vector because |p_diff|
      // can't exceed 1.
      s[num_taps] = vld1q_s16(src_y + num_taps * src_stride);
      if (grade_y > 1) {
        s[num_taps + 1] = vld1q_s16(src_y + (num_taps + 1) * src_stride);
      }
      dest16_y += dest_stride;
      dest_y += dest_stride;

      filter_id = (p >> 6) & kSubPixelMask;
      filter = vmovl_s8(vld1_s8(kHalfSubPixelFilters[filter_index][filter_id]));
      sums = Sum2DVerticalTaps<num_taps>(&s[p_diff], filter);
      result =
          vcombine_u16(vmovn_u32(vrshlq_u32(sums.val[0], v_inter_round_bits)),
                       vmovn_u32(vrshlq_u32(sums.val[1], v_inter_round_bits)));
      if (is_compound) {
        vst1q_u16(dest16_y, result);
      } else {
        result = vqsubq_u16(result, single_round_offset);
        vst1_u8(dest_y, vqmovn_u16(result));
      }
      p += step_y;
      src_y = src_x + (p >> kScaleSubPixelBits) * src_stride;
      prev_p = p;
      dest16_y += dest_stride;
      dest_y += dest_stride;

      y += 2;
    } while (y < height);
    x += 8;
  } while (x < width);
}

template <bool is_compound>
void ConvolveScale2D_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int horizontal_filter_index, const int vertical_filter_index,
    const int inter_round_bits_vertical, const int subpixel_x,
    const int subpixel_y, const int step_x, const int step_y, const int width,
    const int height, void* prediction, const ptrdiff_t pred_stride) {
  // TODO(petersonab): Reduce the height here by using the vertical filter
  // size and offset horizontal filter. Reduce intermediate block stride to
  // width to make smaller blocks faster.
  const int intermediate_height =
      (((height - 1) * step_y + (1 << kScaleSubPixelBits) - 1) >>
       kScaleSubPixelBits) +
      kSubPixelTaps;
  // TODO(b/133525024): Decide whether it's worth branching to a special case
  // when step_x or step_y is 1024.
  assert(step_x <= 2048);
  // The output of the horizontal filter, i.e. the intermediate_result, is
  // guaranteed to fit in int16_t.
  int16_t intermediate_result[kMaxSuperBlockSizeInPixels *
                              (2 * kMaxSuperBlockSizeInPixels + 8)];

  // Horizontal filter.
  // Filter types used for width <= 4 are different from those for width > 4.
  // When width > 4, the valid filter index range is always [0, 3].
  // When width <= 4, the valid filter index range is always [3, 5].
  // Similarly for height.
  int filter_index = GetFilterIndex(horizontal_filter_index, width);
  int16_t* intermediate = intermediate_result;
  const auto* src = static_cast<const uint8_t*>(reference);
  const ptrdiff_t src_stride = reference_stride;
  switch (filter_index) {
    case 0:
      if (step_x > 1024) {
        ConvolveKernelHorizontalSigned6Tap<2>(
            src, src_stride, width, subpixel_x, step_x, intermediate_height,
            intermediate);
      } else {
        ConvolveKernelHorizontalSigned6Tap<1>(
            src, src_stride, width, subpixel_x, step_x, intermediate_height,
            intermediate);
      }
      break;
    case 1:
      if (step_x > 1024) {
        ConvolveKernelHorizontalMixed6Tap<2>(src, src_stride, width, subpixel_x,
                                             step_x, intermediate_height,
                                             intermediate);

      } else {
        ConvolveKernelHorizontalMixed6Tap<1>(src, src_stride, width, subpixel_x,
                                             step_x, intermediate_height,
                                             intermediate);
      }
      break;
    case 2:
      if (step_x > 1024) {
        ConvolveKernelHorizontalSigned8Tap<2>(
            src, src_stride, width, subpixel_x, step_x, intermediate_height,
            intermediate);
      } else {
        ConvolveKernelHorizontalSigned8Tap<1>(
            src, src_stride, width, subpixel_x, step_x, intermediate_height,
            intermediate);
      }
      break;
    case 3:
      if (step_x > 1024) {
        ConvolveKernelHorizontal2Tap<2>(src, src_stride, width, subpixel_x,
                                        step_x, intermediate_height,
                                        intermediate);
      } else {
        ConvolveKernelHorizontal2Tap<1>(src, src_stride, width, subpixel_x,
                                        step_x, intermediate_height,
                                        intermediate);
      }
      break;
    case 4:
      assert(width <= 4);
      ConvolveKernelHorizontalSigned4Tap(src, src_stride, subpixel_x, step_x,
                                         intermediate_height, intermediate);
      break;
    default:
      assert(filter_index == 5);
      ConvolveKernelHorizontalPositive4Tap(src, src_stride, subpixel_x, step_x,
                                           intermediate_height, intermediate);
  }
  // Vertical filter.
  filter_index = GetFilterIndex(vertical_filter_index, height);
  intermediate = intermediate_result;

  switch (filter_index) {
    case 0:
    case 1:
      if (step_y <= 1024) {
        if (width == 2) {
          ConvolveVerticalScale4xH<6, 1, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<6, 1, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<6, 1, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      } else {
        if (width == 2) {
          ConvolveVerticalScale4xH<6, 2, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<6, 2, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<6, 2, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      }
      break;
    case 2:
      if (step_y <= 1024) {
        if (width == 2) {
          ConvolveVerticalScale4xH<8, 1, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<8, 1, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<8, 1, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      } else {
        if (width == 2) {
          ConvolveVerticalScale4xH<8, 2, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<8, 2, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<8, 2, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      }
      break;
    case 3:
      if (step_y <= 1024) {
        if (width == 2) {
          ConvolveVerticalScale4xH<2, 1, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<2, 1, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<2, 1, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      } else {
        if (width == 2) {
          ConvolveVerticalScale4xH<2, 2, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<2, 2, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<2, 2, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      }
      break;
    case 4:
    default:
      assert(filter_index == 4 || filter_index == 5);
      assert(height <= 4);
      if (step_y <= 1024) {
        if (width == 2) {
          ConvolveVerticalScale4xH<4, 1, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<4, 1, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<4, 1, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      } else {
        if (width == 2) {
          ConvolveVerticalScale4xH<4, 2, 2, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else if (width == 4) {
          ConvolveVerticalScale4xH<4, 2, 4, is_compound>(
              intermediate, subpixel_y, inter_round_bits_vertical, filter_index,
              step_y, height, prediction, pred_stride);
        } else {
          ConvolveVerticalScale<4, 2, is_compound>(
              intermediate, width, subpixel_y, inter_round_bits_vertical,
              filter_index, step_y, height, prediction, pred_stride);
        }
      }
  }
}

void ConvolveHorizontal_NEON(const void* const reference,
                             const ptrdiff_t reference_stride,
                             const int horizontal_filter_index,
                             const int /*vertical_filter_index*/,
                             const int /*inter_round_bits_vertical*/,
                             const int subpixel_x, const int /*subpixel_y*/,
                             const int width, const int height,
                             void* prediction, const ptrdiff_t pred_stride) {
  const int filter_index = GetFilterIndex(horizontal_filter_index, width);
  // Set |src| to the outermost tap.
  const auto* src = static_cast<const uint8_t*>(reference) - kHorizontalOffset;
  auto* dest = static_cast<uint8_t*>(prediction);

  DoHorizontalPass(src, reference_stride, dest, pred_stride, width, height,
                   subpixel_x, filter_index, kFilterBits);
}

template <int filter_index, bool negative_outside_taps = false>
void FilterVertical(const uint8_t* src, const ptrdiff_t src_stride,
                    uint8_t* dst, const ptrdiff_t dst_stride, const int width,
                    const int height, const uint8x8_t* const taps) {
  const int num_taps = GetNumTapsInFilter(filter_index);
  const int next_row = num_taps - 1;
  assert(width >= 8);

  int x = 0;
  do {
    const uint8_t* src_x = src + x;
    uint8x8_t srcs[8];
    srcs[0] = vld1_u8(src_x);
    src_x += src_stride;
    if (num_taps >= 4) {
      srcs[1] = vld1_u8(src_x);
      src_x += src_stride;
      srcs[2] = vld1_u8(src_x);
      src_x += src_stride;
      if (num_taps >= 6) {
        srcs[3] = vld1_u8(src_x);
        src_x += src_stride;
        srcs[4] = vld1_u8(src_x);
        src_x += src_stride;
        if (num_taps == 8) {
          srcs[5] = vld1_u8(src_x);
          src_x += src_stride;
          srcs[6] = vld1_u8(src_x);
          src_x += src_stride;
        }
      }
    }

    int y = 0;
    do {
      srcs[next_row] = vld1_u8(src_x);
      src_x += src_stride;

      const int16x8_t sums =
          SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
      const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

      vst1_u8(dst + x + y * dst_stride, results);

      srcs[0] = srcs[1];
      if (num_taps >= 4) {
        srcs[1] = srcs[2];
        srcs[2] = srcs[3];
        if (num_taps >= 6) {
          srcs[3] = srcs[4];
          srcs[4] = srcs[5];
          if (num_taps == 8) {
            srcs[5] = srcs[6];
            srcs[6] = srcs[7];
          }
        }
      }
    } while (++y < height);
    x += 8;
  } while (x < width);
}

template <int filter_index, bool negative_outside_taps = false>
void FilterVertical4xH(const uint8_t* src, const ptrdiff_t src_stride,
                       uint8_t* dst, const ptrdiff_t dst_stride,
                       const int height, const uint8x8_t* const taps) {
  const int num_taps = GetNumTapsInFilter(filter_index);

  uint8x8_t srcs[9];

  if (num_taps == 2) {
    srcs[0] = vdup_n_u8(0);
    srcs[2] = vdup_n_u8(0);

    srcs[0] = LoadLo4(src, srcs[0]);
    src += src_stride;

    int y = 0;
    do {
      srcs[0] = LoadHi4(src, srcs[0]);
      src += src_stride;
      srcs[2] = LoadLo4(src, srcs[2]);
      src += src_stride;
      srcs[1] = vext_u8(srcs[0], srcs[2], 4);

      const int16x8_t sums =
          SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
      const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

      StoreLo4(dst, results);
      dst += dst_stride;
      StoreHi4(dst, results);
      dst += dst_stride;

      srcs[0] = srcs[2];
      y += 2;
    } while (y < height);
  } else if (num_taps == 4) {
    srcs[0] = vdup_n_u8(0);
    srcs[2] = vdup_n_u8(0);
    srcs[4] = vdup_n_u8(0);

    srcs[0] = LoadLo4(src, srcs[0]);
    src += src_stride;
    srcs[0] = LoadHi4(src, srcs[0]);
    src += src_stride;
    srcs[2] = LoadLo4(src, srcs[2]);
    src += src_stride;
    srcs[1] = vext_u8(srcs[0], srcs[2], 4);

    int y = 0;
    do {
      srcs[2] = LoadHi4(src, srcs[2]);
      src += src_stride;
      srcs[4] = LoadLo4(src, srcs[2]);
      src += src_stride;
      srcs[3] = vext_u8(srcs[2], srcs[4], 4);

      const int16x8_t sums =
          SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
      const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

      StoreLo4(dst, results);
      dst += dst_stride;
      StoreHi4(dst, results);
      dst += dst_stride;

      srcs[0] = srcs[2];
      srcs[1] = srcs[3];
      srcs[2] = srcs[4];
      y += 2;
    } while (y < height);
  } else if (num_taps == 6) {
    srcs[0] = vdup_n_u8(0);
    srcs[2] = vdup_n_u8(0);
    srcs[4] = vdup_n_u8(0);
    srcs[6] = vdup_n_u8(0);

    srcs[0] = LoadLo4(src, srcs[0]);
    src += src_stride;
    srcs[0] = LoadHi4(src, srcs[0]);
    src += src_stride;
    srcs[2] = LoadLo4(src, srcs[2]);
    src += src_stride;
    srcs[1] = vext_u8(srcs[0], srcs[2], 4);
    srcs[2] = LoadHi4(src, srcs[2]);
    src += src_stride;
    srcs[4] = LoadLo4(src, srcs[4]);
    src += src_stride;
    srcs[3] = vext_u8(srcs[2], srcs[4], 4);

    int y = 0;
    do {
      srcs[4] = LoadHi4(src, srcs[4]);
      src += src_stride;
      srcs[6] = LoadLo4(src, srcs[6]);
      src += src_stride;
      srcs[5] = vext_u8(srcs[4], srcs[6], 4);

      const int16x8_t sums =
          SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
      const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

      StoreLo4(dst, results);
      dst += dst_stride;
      StoreHi4(dst, results);
      dst += dst_stride;

      srcs[0] = srcs[2];
      srcs[1] = srcs[3];
      srcs[2] = srcs[4];
      srcs[3] = srcs[5];
      srcs[4] = srcs[6];
      y += 2;
    } while (y < height);
  } else if (num_taps == 8) {
    srcs[0] = vdup_n_u8(0);
    srcs[2] = vdup_n_u8(0);
    srcs[4] = vdup_n_u8(0);
    srcs[6] = vdup_n_u8(0);
    srcs[8] = vdup_n_u8(0);

    srcs[0] = LoadLo4(src, srcs[0]);
    src += src_stride;
    srcs[0] = LoadHi4(src, srcs[0]);
    src += src_stride;
    srcs[2] = LoadLo4(src, srcs[2]);
    src += src_stride;
    srcs[1] = vext_u8(srcs[0], srcs[2], 4);
    srcs[2] = LoadHi4(src, srcs[2]);
    src += src_stride;
    srcs[4] = LoadLo4(src, srcs[4]);
    src += src_stride;
    srcs[3] = vext_u8(srcs[2], srcs[4], 4);
    srcs[4] = LoadHi4(src, srcs[4]);
    src += src_stride;
    srcs[6] = LoadLo4(src, srcs[6]);
    src += src_stride;
    srcs[5] = vext_u8(srcs[4], srcs[6], 4);

    int y = 0;
    do {
      srcs[6] = LoadHi4(src, srcs[6]);
      src += src_stride;
      srcs[8] = LoadLo4(src, srcs[8]);
      src += src_stride;
      srcs[7] = vext_u8(srcs[6], srcs[8], 4);

      const int16x8_t sums =
          SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
      const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

      StoreLo4(dst, results);
      dst += dst_stride;
      StoreHi4(dst, results);
      dst += dst_stride;

      srcs[0] = srcs[2];
      srcs[1] = srcs[3];
      srcs[2] = srcs[4];
      srcs[3] = srcs[5];
      srcs[4] = srcs[6];
      srcs[5] = srcs[7];
      srcs[6] = srcs[8];
      y += 2;
    } while (y < height);
  }
}

template <int filter_index, bool is_compound = false,
          bool negative_outside_taps = false>
void FilterVertical2xH(const uint8_t* src, const ptrdiff_t src_stride,
                       void* const dst, const ptrdiff_t dst_stride,
                       const int height, const uint8x8_t* const taps) {
  const int num_taps = GetNumTapsInFilter(filter_index);
  // In order to keep the sum in 16 bits we added an offset to the sum:
  // (1 << (bitdepth + kFilterBits - 2) == 1 << 13).
  // This ensures that the results will never be negative.
  // Normally ConvolveCompoundVertical would add |compound_round_offset| at
  // the end. Instead we use that to compensate for the initial offset.
  // (1 << (bitdepth + 3)) + (1 << (bitdepth + 2)) == (1 << 11) + (1 << 10)
  // After taking into account the shift after the sum:
  // RightShiftWithRounding(LeftShift(sum, bits_shift),
  //                        inter_round_bits_vertical)
  // where bits_shift == kFilterBits - kInterRoundBitsHorizontal == 4
  // and inter_round_bits_vertical == 7
  // and simplifying it to RightShiftWithRounding(sum, 2)
  // we see that the initial offset of 1 << 13 >> 3 == 1 << 10 and
  // |compound_round_offset| can be simplified to 1 << 11.
  const uint16x8_t compound_round_offset = vdupq_n_u16(1 << 11);
  auto* dst8 = static_cast<uint8_t*>(dst);
  auto* dst16 = static_cast<uint16_t*>(dst);

  uint8x8_t srcs[9];

  if (num_taps == 2) {
    srcs[0] = vdup_n_u8(0);
    srcs[2] = vdup_n_u8(0);
    srcs[0] = Load2<0>(src, srcs[0]);
    src += src_stride;

    int y = 0;
    do {
      srcs[0] = Load2<1>(src, srcs[0]);
      src += src_stride;
      srcs[0] = Load2<2>(src, srcs[0]);
      src += src_stride;
      srcs[0] = Load2<3>(src, srcs[0]);
      src += src_stride;
      srcs[2] = Load2<0>(src, srcs[2]);
      src += src_stride;
      srcs[1] = vext_u8(srcs[0], srcs[2], 2);

      if (is_compound) {
        const uint16x8_t sums =
            SumVerticalTaps8To16<filter_index, negative_outside_taps>(srcs,
                                                                      taps);
        const uint16x8_t shifted = vrshrq_n_u16(sums, 2);
        const uint16x8_t offset = vaddq_u16(shifted, compound_round_offset);

        Store2<0>(dst16, offset);
        dst16 += dst_stride;
        Store2<1>(dst16, offset);
        if (height == 2) return;
        dst16 += dst_stride;
        Store2<2>(dst16, offset);
        dst16 += dst_stride;
        Store2<3>(dst16, offset);
        dst16 += dst_stride;
      } else {
        const int16x8_t sums =
            SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
        const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

        Store2<0>(dst8, results);
        dst8 += dst_stride;
        Store2<1>(dst8, results);
        if (height == 2) return;
        dst8 += dst_stride;
        Store2<2>(dst8, results);
        dst8 += dst_stride;
        Store2<3>(dst8, results);
        dst8 += dst_stride;
      }

      srcs[0] = srcs[2];
      y += 4;
    } while (y < height);
  } else if (num_taps == 4) {
    srcs[0] = vdup_n_u8(0);
    srcs[4] = vdup_n_u8(0);
    srcs[0] = Load2<0>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<1>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<2>(src, srcs[0]);
    src += src_stride;

    int y = 0;
    do {
      srcs[0] = Load2<3>(src, srcs[0]);
      src += src_stride;
      srcs[4] = Load2<0>(src, srcs[4]);
      src += src_stride;
      srcs[1] = vext_u8(srcs[0], srcs[4], 2);
      srcs[4] = Load2<1>(src, srcs[4]);
      src += src_stride;
      srcs[2] = vext_u8(srcs[0], srcs[4], 4);
      srcs[4] = Load2<2>(src, srcs[4]);
      src += src_stride;
      srcs[3] = vext_u8(srcs[0], srcs[4], 6);

      if (is_compound) {
        const uint16x8_t sums =
            SumVerticalTaps8To16<filter_index, negative_outside_taps>(srcs,
                                                                      taps);
        const uint16x8_t shifted = vrshrq_n_u16(sums, 2);
        const uint16x8_t offset = vaddq_u16(shifted, compound_round_offset);

        Store2<0>(dst16, offset);
        dst16 += dst_stride;
        Store2<1>(dst16, offset);
        if (height == 2) return;
        dst16 += dst_stride;
        Store2<2>(dst16, offset);
        dst16 += dst_stride;
        Store2<3>(dst16, offset);
        dst16 += dst_stride;
      } else {
        const int16x8_t sums =
            SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
        const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

        Store2<0>(dst8, results);
        dst8 += dst_stride;
        Store2<1>(dst8, results);
        if (height == 2) return;
        dst8 += dst_stride;
        Store2<2>(dst8, results);
        dst8 += dst_stride;
        Store2<3>(dst8, results);
        dst8 += dst_stride;
      }

      srcs[0] = srcs[4];
      y += 4;
    } while (y < height);
  } else if (num_taps == 6) {
    // During the vertical pass the number of taps is restricted when
    // |height| <= 4.
    assert(height > 4);
    srcs[0] = vdup_n_u8(0);
    srcs[4] = vdup_n_u8(0);
    srcs[8] = vdup_n_u8(0);
    srcs[0] = Load2<0>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<1>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<2>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<3>(src, srcs[0]);
    src += src_stride;
    srcs[4] = Load2<0>(src, srcs[1]);
    src += src_stride;
    srcs[1] = vext_u8(srcs[0], srcs[4], 2);
    srcs[4] = Load2<1>(src, srcs[4]);
    src += src_stride;
    srcs[2] = vext_u8(srcs[0], srcs[4], 4);

    int y = 0;
    do {
      srcs[4] = Load2<2>(src, srcs[4]);
      src += src_stride;
      srcs[3] = vext_u8(srcs[0], srcs[4], 6);
      srcs[4] = Load2<3>(src, srcs[4]);
      src += src_stride;
      srcs[8] = Load2<0>(src, srcs[8]);
      src += src_stride;
      srcs[5] = vext_u8(srcs[4], srcs[8], 2);
      srcs[8] = Load2<1>(src, srcs[8]);
      src += src_stride;
      srcs[6] = vext_u8(srcs[4], srcs[8], 4);

      if (is_compound) {
        const uint16x8_t sums =
            SumVerticalTaps8To16<filter_index, negative_outside_taps>(srcs,
                                                                      taps);
        const uint16x8_t shifted = vrshrq_n_u16(sums, 2);
        const uint16x8_t offset = vaddq_u16(shifted, compound_round_offset);

        Store2<0>(dst16, offset);
        dst16 += dst_stride;
        Store2<1>(dst16, offset);
        dst16 += dst_stride;
        Store2<2>(dst16, offset);
        dst16 += dst_stride;
        Store2<3>(dst16, offset);
        dst16 += dst_stride;
      } else {
        const int16x8_t sums =
            SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
        const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

        Store2<0>(dst8, results);
        dst8 += dst_stride;
        Store2<1>(dst8, results);
        dst8 += dst_stride;
        Store2<2>(dst8, results);
        dst8 += dst_stride;
        Store2<3>(dst8, results);
        dst8 += dst_stride;
      }

      srcs[0] = srcs[4];
      srcs[1] = srcs[5];
      srcs[2] = srcs[6];
      srcs[4] = srcs[8];
      y += 4;
    } while (y < height);
  } else if (num_taps == 8) {
    // During the vertical pass the number of taps is restricted when
    // |height| <= 4.
    assert(height > 4);
    srcs[0] = vdup_n_u8(0);
    srcs[4] = vdup_n_u8(0);
    srcs[8] = vdup_n_u8(0);
    srcs[0] = Load2<0>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<1>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<2>(src, srcs[0]);
    src += src_stride;
    srcs[0] = Load2<3>(src, srcs[0]);
    src += src_stride;
    srcs[4] = Load2<0>(src, srcs[4]);
    src += src_stride;
    srcs[1] = vext_u8(srcs[0], srcs[4], 2);
    srcs[4] = Load2<1>(src, srcs[4]);
    src += src_stride;
    srcs[2] = vext_u8(srcs[0], srcs[4], 4);
    srcs[4] = Load2<2>(src, srcs[4]);
    src += src_stride;
    srcs[3] = vext_u8(srcs[0], srcs[4], 6);
    srcs[4] = Load2<3>(src, srcs[4]);
    src += src_stride;

    int y = 0;
    do {
      srcs[8] = Load2<0>(src, srcs[8]);
      src += src_stride;
      srcs[5] = vext_u8(srcs[4], srcs[8], 2);
      srcs[8] = Load2<1>(src, srcs[8]);
      src += src_stride;
      srcs[6] = vext_u8(srcs[4], srcs[8], 4);
      srcs[8] = Load2<2>(src, srcs[8]);
      src += src_stride;
      srcs[7] = vext_u8(srcs[4], srcs[8], 6);
      srcs[8] = Load2<3>(src, srcs[8]);
      src += src_stride;

      if (is_compound) {
        const uint16x8_t sums =
            SumVerticalTaps8To16<filter_index, negative_outside_taps>(srcs,
                                                                      taps);
        const uint16x8_t shifted = vrshrq_n_u16(sums, 2);
        const uint16x8_t offset = vaddq_u16(shifted, compound_round_offset);

        Store2<0>(dst16, offset);
        dst16 += dst_stride;
        Store2<1>(dst16, offset);
        dst16 += dst_stride;
        Store2<2>(dst16, offset);
        dst16 += dst_stride;
        Store2<3>(dst16, offset);
        dst16 += dst_stride;
      } else {
        const int16x8_t sums =
            SumOnePassTaps<filter_index, negative_outside_taps>(srcs, taps);
        const uint8x8_t results = vqrshrun_n_s16(sums, kFilterBits - 1);

        Store2<0>(dst8, results);
        dst8 += dst_stride;
        Store2<1>(dst8, results);
        dst8 += dst_stride;
        Store2<2>(dst8, results);
        dst8 += dst_stride;
        Store2<3>(dst8, results);
        dst8 += dst_stride;
      }

      srcs[0] = srcs[4];
      srcs[1] = srcs[5];
      srcs[2] = srcs[6];
      srcs[3] = srcs[7];
      srcs[4] = srcs[8];
      y += 4;
    } while (y < height);
  }
}

// This function is a simplified version of Convolve2D_C.
// It is called when it is single prediction mode, where only vertical
// filtering is required.
// The output is the single prediction of the block, clipped to valid pixel
// range.
void ConvolveVertical_NEON(const void* const reference,
                           const ptrdiff_t reference_stride,
                           const int /*horizontal_filter_index*/,
                           const int vertical_filter_index,
                           const int /*inter_round_bits_vertical*/,
                           const int /*subpixel_x*/, const int subpixel_y,
                           const int width, const int height, void* prediction,
                           const ptrdiff_t pred_stride) {
  const int filter_index = GetFilterIndex(vertical_filter_index, height);
  const int vertical_taps = GetNumTapsInFilter(filter_index);
  const ptrdiff_t src_stride = reference_stride;
  const auto* src = static_cast<const uint8_t*>(reference) -
                    (vertical_taps / 2 - 1) * src_stride;
  auto* dest = static_cast<uint8_t*>(prediction);
  const ptrdiff_t dest_stride = pred_stride;
  const int filter_id = (subpixel_y >> 6) & kSubPixelMask;
  // First filter is always a copy.
  if (filter_id == 0) {
    // Move |src| down the actual values and not the start of the context.
    src = static_cast<const uint8_t*>(reference);
    int y = 0;
    do {
      memcpy(dest, src, width * sizeof(src[0]));
      src += src_stride;
      dest += dest_stride;
    } while (++y < height);
    return;
  }

  uint8x8_t taps[8];
  for (int k = 0; k < kSubPixelTaps; ++k) {
    taps[k] = vreinterpret_u8_s8(
        vabs_s8(vdup_n_s8(kHalfSubPixelFilters[filter_index][filter_id][k])));
  }

  if (filter_index == 0) {  // 6 tap.
    if (width == 2) {
      FilterVertical2xH<0>(src, src_stride, dest, dest_stride, height,
                           taps + 1);
    } else if (width == 4) {
      FilterVertical4xH<0>(src, src_stride, dest, dest_stride, height,
                           taps + 1);
    } else {
      FilterVertical<0>(src, src_stride, dest, dest_stride, width, height,
                        taps + 1);
    }
  } else if ((filter_index == 1) &
             ((filter_id == 1) | (filter_id == 15))) {  // 5 tap.
    if (width == 2) {
      FilterVertical2xH<1>(src, src_stride, dest, dest_stride, height,
                           taps + 1);
    } else if (width == 4) {
      FilterVertical4xH<1>(src, src_stride, dest, dest_stride, height,
                           taps + 1);
    } else {
      FilterVertical<1>(src, src_stride, dest, dest_stride, width, height,
                        taps + 1);
    }
  } else if ((filter_index == 1) &
             ((filter_id == 7) | (filter_id == 8) |
              (filter_id == 9))) {  // 6 tap with weird negative taps.
    if (width == 2) {
      FilterVertical2xH<1, /*is_compound=*/false,
                        /*negative_outside_taps=*/true>(
          src, src_stride, dest, dest_stride, height, taps + 1);
    } else if (width == 4) {
      FilterVertical4xH<1,
                        /*negative_outside_taps=*/true>(
          src, src_stride, dest, dest_stride, height, taps + 1);
    } else {
      FilterVertical<1, /*negative_outside_taps=*/true>(
          src, src_stride, dest, dest_stride, width, height, taps + 1);
    }
  } else if (filter_index == 2) {  // 8 tap.
    if (width == 2) {
      FilterVertical2xH<2>(src, src_stride, dest, dest_stride, height, taps);
    } else if (width == 4) {
      FilterVertical4xH<2>(src, src_stride, dest, dest_stride, height, taps);
    } else {
      FilterVertical<2>(src, src_stride, dest, dest_stride, width, height,
                        taps);
    }
  } else if (filter_index == 3) {  // 2 tap.
    if (width == 2) {
      FilterVertical2xH<3>(src, src_stride, dest, dest_stride, height,
                           taps + 3);
    } else if (width == 4) {
      FilterVertical4xH<3>(src, src_stride, dest, dest_stride, height,
                           taps + 3);
    } else {
      FilterVertical<3>(src, src_stride, dest, dest_stride, width, height,
                        taps + 3);
    }
  } else if (filter_index == 4) {  // 4 tap.
    // Outside taps are negative.
    if (width == 2) {
      FilterVertical2xH<4>(src, src_stride, dest, dest_stride, height,
                           taps + 2);
    } else if (width == 4) {
      FilterVertical4xH<4>(src, src_stride, dest, dest_stride, height,
                           taps + 2);
    } else {
      FilterVertical<4>(src, src_stride, dest, dest_stride, width, height,
                        taps + 2);
    }
  } else {
    // 4 tap. When |filter_index| == 1 the |filter_id| values listed below map
    // to 4 tap filters.
    assert(filter_index == 5 ||
           (filter_index == 1 &&
            (filter_id == 2 || filter_id == 3 || filter_id == 4 ||
             filter_id == 5 || filter_id == 6 || filter_id == 10 ||
             filter_id == 11 || filter_id == 12 || filter_id == 13 ||
             filter_id == 14)));
    // According to GetNumTapsInFilter() this has 6 taps but here we are
    // treating it as though it has 4.
    if (filter_index == 1) src += src_stride;
    if (width == 2) {
      FilterVertical2xH<5>(src, src_stride, dest, dest_stride, height,
                           taps + 2);
    } else if (width == 4) {
      FilterVertical4xH<5>(src, src_stride, dest, dest_stride, height,
                           taps + 2);
    } else {
      FilterVertical<5>(src, src_stride, dest, dest_stride, width, height,
                        taps + 2);
    }
  }
}

void ConvolveCompoundCopy_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int /*horizontal_filter_index*/, const int /*vertical_filter_index*/,
    const int inter_round_bits_vertical, const int /*subpixel_x*/,
    const int /*subpixel_y*/, const int width, const int height,
    void* prediction, const ptrdiff_t pred_stride) {
  const auto* src = static_cast<const uint8_t*>(reference);
  const ptrdiff_t src_stride = reference_stride;
  auto* dest = static_cast<uint16_t*>(prediction);
  const int compound_round_offset = (1 << kBitdepth8) + (1 << (kBitdepth8 - 1));
  const uint16x8_t v_compound_round_offset = vdupq_n_u16(compound_round_offset);
  const int final_shift = kInterRoundBitsVertical - inter_round_bits_vertical;
  const int16x8_t v_final_shift = vdupq_n_s16(final_shift);

  if (width >= 16) {
    int y = 0;
    do {
      int x = 0;
      do {
        const uint8x16_t v_src = vld1q_u8(&src[x]);
        const uint16x8_t v_offset_lo =
            vaddw_u8(v_compound_round_offset, vget_low_u8(v_src));
        const uint16x8_t v_offset_hi =
            vaddw_u8(v_compound_round_offset, vget_high_u8(v_src));
        const uint16x8_t v_dest_lo = vshlq_u16(v_offset_lo, v_final_shift);
        const uint16x8_t v_dest_hi = vshlq_u16(v_offset_hi, v_final_shift);
        vst1q_u16(&dest[x], v_dest_lo);
        x += 8;
        vst1q_u16(&dest[x], v_dest_hi);
        x += 8;
      } while (x < width);
      src += src_stride;
      dest += pred_stride;
    } while (++y < height);
  } else if (width == 8) {
    int y = 0;
    do {
      const uint8x8_t v_src = vld1_u8(&src[0]);
      const uint16x8_t v_offset = vaddw_u8(v_compound_round_offset, v_src);
      vst1q_u16(&dest[0], vshlq_u16(v_offset, v_final_shift));
      src += src_stride;
      dest += pred_stride;
    } while (++y < height);
  } else if (width == 4) {
    const uint8x8_t zero = vdup_n_u8(0);
    int y = 0;
    do {
      const uint8x8_t v_src = LoadLo4(&src[0], zero);
      const uint16x8_t v_offset = vaddw_u8(v_compound_round_offset, v_src);
      vst1_u16(&dest[0], vget_low_u16(vshlq_u16(v_offset, v_final_shift)));
      src += src_stride;
      dest += pred_stride;
    } while (++y < height);
  } else {  // width == 2
    assert(width == 2);
    int y = 0;
    do {
      dest[0] = (src[0] + compound_round_offset) << final_shift;
      dest[1] = (src[1] + compound_round_offset) << final_shift;
      src += src_stride;
      dest += pred_stride;
    } while (++y < height);
  }
}

// Input 8 bits and output 16 bits.
template <int min_width, int filter_index, bool negative_outside_taps = false>
void FilterCompoundVertical(const uint8_t* src, const ptrdiff_t src_stride,
                            uint16_t* dst, const ptrdiff_t dst_stride,
                            const int width, const int height,
                            const uint8x8_t* const taps,
                            const int inter_round_bits_vertical) {
  const int num_taps = GetNumTapsInFilter(filter_index);
  const int next_row = num_taps - 1;
  const uint16x8_t compound_round_offset = vdupq_n_u16(
      (1 << (kBitdepth8 + 3)) >> (inter_round_bits_vertical - kFilterBits));
  const int bits_shift = kFilterBits - kHorizontalOffset;
  const int16x8_t v_vertical_shift =
      vdupq_n_s16(-(inter_round_bits_vertical - bits_shift - 1));

  int x = 0;
  do {
    const uint8_t* src_x = src + x;
    uint8x8_t srcs[8];
    srcs[0] = vld1_u8(src_x);
    src_x += src_stride;
    if (num_taps >= 4) {
      srcs[1] = vld1_u8(src_x);
      src_x += src_stride;
      srcs[2] = vld1_u8(src_x);
      src_x += src_stride;
      if (num_taps >= 6) {
        srcs[3] = vld1_u8(src_x);
        src_x += src_stride;
        srcs[4] = vld1_u8(src_x);
        src_x += src_stride;
        if (num_taps == 8) {
          srcs[5] = vld1_u8(src_x);
          src_x += src_stride;
          srcs[6] = vld1_u8(src_x);
          src_x += src_stride;
        }
      }
    }

    int y = 0;
    do {
      srcs[next_row] = vld1_u8(src_x);
      src_x += src_stride;

      const uint16x8_t sums =
          SumVerticalTaps8To16<filter_index, negative_outside_taps>(srcs, taps);
      const uint16x8_t shifted = vqrshlq_u16(sums, v_vertical_shift);
      // In order to keep the sum in 16 bits we added an offset to the sum
      // (1 << (bitdepth + kFilterBits - 2) == 1 << 13). This ensures that the
      // results will never be negative.
      // Normally ConvolveCompoundVertical would add |compound_round_offset| at
      // the end. Instead we use that to compensate for the initial offset.
      // (1 << (bitdepth + 3)) + (1 << (bitdepth + 2)) == (1 << 11) + (1 << 10)
      // After taking into account the shift above:
      // RightShiftWithRounding(LeftShift(sum, bits_shift),
      //                        inter_round_bits_vertical)
      // where bits_shift == kFilterBits - kInterRoundBitsHorizontal == 4
      // and inter_round_bits_vertical == 7
      // and simplifying it to RightShiftWithRounding(sum, 2)
      // we see that the initial offset of 1 << 13 >> 3 == 1 << 10 and
      // |compound_round_offset| can be simplified to 1 << 11.
      const uint16x8_t offset = vaddq_u16(shifted, compound_round_offset);

      if (min_width == 4) {
        vst1_u16(dst + x + y * dst_stride, vget_low_u16(offset));
      } else {
        vst1q_u16(dst + x + y * dst_stride, offset);
      }

      srcs[0] = srcs[1];
      if (num_taps >= 4) {
        srcs[1] = srcs[2];
        srcs[2] = srcs[3];
        if (num_taps >= 6) {
          srcs[3] = srcs[4];
          srcs[4] = srcs[5];
          if (num_taps == 8) {
            srcs[5] = srcs[6];
            srcs[6] = srcs[7];
          }
        }
      }
    } while (++y < height);
    x += 8;
  } while (x < width);
}

void ConvolveCompoundVertical_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int /*horizontal_filter_index*/, const int vertical_filter_index,
    const int inter_round_bits_vertical, const int /*subpixel_x*/,
    const int subpixel_y, const int width, const int height, void* prediction,
    const ptrdiff_t pred_stride) {
  const int filter_index = GetFilterIndex(vertical_filter_index, height);
  const int vertical_taps = GetNumTapsInFilter(filter_index);
  const ptrdiff_t src_stride = reference_stride;
  const auto* src = static_cast<const uint8_t*>(reference) -
                    (vertical_taps / 2 - 1) * src_stride;
  auto* dest = static_cast<uint16_t*>(prediction);
  const int filter_id = (subpixel_y >> 6) & kSubPixelMask;

  uint8x8_t taps[8];
  for (int k = 0; k < kSubPixelTaps; ++k) {
    taps[k] = vreinterpret_u8_s8(
        vabs_s8(vdup_n_s8(kHalfSubPixelFilters[filter_index][filter_id][k])));
  }

  if (filter_index == 0) {  // 6 tap.
    if (width == 2) {
      FilterVertical2xH<0, /*is_compound=*/true>(src, src_stride, dest,
                                                 pred_stride, height, taps + 1);
    } else if (width == 4) {
      FilterCompoundVertical<4, 0>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 1, inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 0>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 1, inter_round_bits_vertical);
    }
  } else if ((filter_index == 1) &
             ((filter_id == 1) | (filter_id == 15))) {  // 5 tap.
    if (width == 2) {
      FilterVertical2xH<1, /*is_compound=*/true>(src, src_stride, dest,
                                                 pred_stride, height, taps + 1);
    } else if (width == 4) {
      FilterCompoundVertical<4, 1>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 1, inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 1>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 1, inter_round_bits_vertical);
    }
  } else if ((filter_index == 1) &
             ((filter_id == 7) | (filter_id == 8) |
              (filter_id == 9))) {  // 6 tap with weird negative taps.
    if (width == 2) {
      FilterVertical2xH<1, /*is_compound=*/true,
                        /*negative_outside_taps=*/true>(
          src, src_stride, dest, pred_stride, height, taps + 1);
    } else if (width == 4) {
      FilterCompoundVertical<4, 1, true>(src, src_stride, dest, pred_stride,
                                         width, height, taps + 1,
                                         inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 1, true>(src, src_stride, dest, pred_stride,
                                         width, height, taps + 1,
                                         inter_round_bits_vertical);
    }
  } else if (filter_index == 2) {  // 8 tap.
    if (width == 2) {
      FilterVertical2xH<2, /*is_compound=*/true>(src, src_stride, dest,
                                                 pred_stride, height, taps);
    } else if (width == 4) {
      FilterCompoundVertical<4, 2>(src, src_stride, dest, pred_stride, width,
                                   height, taps, inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 2>(src, src_stride, dest, pred_stride, width,
                                   height, taps, inter_round_bits_vertical);
    }
  } else if (filter_index == 3) {  // 2 tap.
    if (width == 2) {
      FilterVertical2xH<3, /*is_compound=*/true>(src, src_stride, dest,
                                                 pred_stride, height, taps + 3);
    } else if (width == 4) {
      FilterCompoundVertical<4, 3>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 3, inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 3>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 3, inter_round_bits_vertical);
    }
  } else if (filter_index == 4) {  // 4 tap.
    if (width == 2) {
      FilterVertical2xH<4, /*is_compound=*/true>(src, src_stride, dest,
                                                 pred_stride, height, taps + 2);
    } else if (width == 4) {
      FilterCompoundVertical<4, 4>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 2, inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 4>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 2, inter_round_bits_vertical);
    }
  } else {
    // 4 tap. When |filter_index| == 1 the |filter_id| values listed below map
    // to 4 tap filters.
    assert(filter_index == 5 ||
           (filter_index == 1 &&
            (filter_id == 2 || filter_id == 3 || filter_id == 4 ||
             filter_id == 5 || filter_id == 6 || filter_id == 10 ||
             filter_id == 11 || filter_id == 12 || filter_id == 13 ||
             filter_id == 14)));
    // According to GetNumTapsInFilter() this has 6 taps but here we are
    // treating it as though it has 4.
    if (filter_index == 1) src += src_stride;
    if (width == 2) {
      FilterVertical2xH<5, /*is_compound=*/true>(src, src_stride, dest,
                                                 pred_stride, height, taps + 2);
    } else if (width == 4) {
      FilterCompoundVertical<4, 5>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 2, inter_round_bits_vertical);
    } else {
      FilterCompoundVertical<8, 5>(src, src_stride, dest, pred_stride, width,
                                   height, taps + 2, inter_round_bits_vertical);
    }
  }
}

void ConvolveCompoundHorizontal_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int horizontal_filter_index, const int /*vertical_filter_index*/,
    const int inter_round_bits_vertical, const int subpixel_x,
    const int /*subpixel_y*/, const int width, const int height,
    void* prediction, const ptrdiff_t pred_stride) {
  const int filter_index = GetFilterIndex(horizontal_filter_index, width);
  const auto* src = static_cast<const uint8_t*>(reference) - kHorizontalOffset;
  auto* dest = static_cast<uint16_t*>(prediction);

  DoHorizontalPass</*is_2d=*/false, /*is_compound=*/true>(
      src, reference_stride, dest, pred_stride, width, height, subpixel_x,
      filter_index, inter_round_bits_vertical);
}

void ConvolveCompound2D_NEON(const void* const reference,
                             const ptrdiff_t reference_stride,
                             const int horizontal_filter_index,
                             const int vertical_filter_index,
                             const int inter_round_bits_vertical,
                             const int subpixel_x, const int subpixel_y,
                             const int width, const int height,
                             void* prediction, const ptrdiff_t pred_stride) {
  // The output of the horizontal filter, i.e. the intermediate_result, is
  // guaranteed to fit in int16_t.
  uint16_t
      intermediate_result[kMaxSuperBlockSizeInPixels *
                          (kMaxSuperBlockSizeInPixels + kSubPixelTaps - 1)];

  // Horizontal filter.
  // Filter types used for width <= 4 are different from those for width > 4.
  // When width > 4, the valid filter index range is always [0, 3].
  // When width <= 4, the valid filter index range is always [4, 5].
  // Similarly for height.
  const int horiz_filter_index = GetFilterIndex(horizontal_filter_index, width);
  const int vert_filter_index = GetFilterIndex(vertical_filter_index, height);
  const int vertical_taps = GetNumTapsInFilter(vert_filter_index);
  const int intermediate_height = height + vertical_taps - 1;
  const ptrdiff_t src_stride = reference_stride;
  const auto* const src = static_cast<const uint8_t*>(reference) -
                          (vertical_taps / 2 - 1) * src_stride -
                          kHorizontalOffset;

  DoHorizontalPass</*is_2d=*/true, /*is_compound=*/true>(
      src, src_stride, intermediate_result, width, width, intermediate_height,
      subpixel_x, horiz_filter_index, kFilterBits);

  // Vertical filter.
  auto* dest = static_cast<uint16_t*>(prediction);
  const int filter_id = ((subpixel_y & 1023) >> 6) & kSubPixelMask;

  const ptrdiff_t dest_stride = pred_stride;
  const int16x8_t taps =
      vmovl_s8(vld1_s8(kHalfSubPixelFilters[vert_filter_index][filter_id]));

  if (vertical_taps == 8) {
    if (width == 2) {
      Filter2DVertical2xH<8, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else if (width == 4) {
      Filter2DVertical4xH<8, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else {
      Filter2DVertical<8, /*is_compound=*/true>(
          intermediate_result, dest, dest_stride, width, height, taps,
          inter_round_bits_vertical);
    }
  } else if (vertical_taps == 6) {
    if (width == 2) {
      Filter2DVertical2xH<6, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else if (width == 4) {
      Filter2DVertical4xH<6, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else {
      Filter2DVertical<6, /*is_compound=*/true>(
          intermediate_result, dest, dest_stride, width, height, taps,
          inter_round_bits_vertical);
    }
  } else if (vertical_taps == 4) {
    if (width == 2) {
      Filter2DVertical2xH<4, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else if (width == 4) {
      Filter2DVertical4xH<4, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else {
      Filter2DVertical<4, /*is_compound=*/true>(
          intermediate_result, dest, dest_stride, width, height, taps,
          inter_round_bits_vertical);
    }
  } else {  // |vertical_taps| == 2
    if (width == 2) {
      Filter2DVertical2xH<2, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else if (width == 4) {
      Filter2DVertical4xH<2, /*is_compound=*/true>(intermediate_result, dest,
                                                   dest_stride, height, taps,
                                                   inter_round_bits_vertical);
    } else {
      Filter2DVertical<2, /*is_compound=*/true>(
          intermediate_result, dest, dest_stride, width, height, taps,
          inter_round_bits_vertical);
    }
  }
}

inline void HalfAddHorizontal(const uint8_t* src, uint8_t* dst) {
  const uint8x16_t left = vld1q_u8(src);
  const uint8x16_t right = vld1q_u8(src + 1);
  vst1q_u8(dst, vrhaddq_u8(left, right));
}

template <int width>
inline void IntraBlockCopyHorizontal(const uint8_t* src,
                                     const ptrdiff_t src_stride,
                                     const int height, uint8_t* dst,
                                     const ptrdiff_t dst_stride) {
  const ptrdiff_t src_remainder_stride = src_stride - (width - 16);
  const ptrdiff_t dst_remainder_stride = dst_stride - (width - 16);

  int y = 0;
  do {
    HalfAddHorizontal(src, dst);
    if (width >= 32) {
      src += 16;
      dst += 16;
      HalfAddHorizontal(src, dst);
      if (width >= 64) {
        src += 16;
        dst += 16;
        HalfAddHorizontal(src, dst);
        src += 16;
        dst += 16;
        HalfAddHorizontal(src, dst);
        if (width == 128) {
          src += 16;
          dst += 16;
          HalfAddHorizontal(src, dst);
          src += 16;
          dst += 16;
          HalfAddHorizontal(src, dst);
          src += 16;
          dst += 16;
          HalfAddHorizontal(src, dst);
          src += 16;
          dst += 16;
          HalfAddHorizontal(src, dst);
        }
      }
    }
    src += src_remainder_stride;
    dst += dst_remainder_stride;
  } while (++y < height);
}

void ConvolveIntraBlockCopyHorizontal_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int /*horizontal_filter_index*/, const int /*vertical_filter_index*/,
    const int /*inter_round_bits_vertical*/, const int /*subpixel_x*/,
    const int /*subpixel_y*/, const int width, const int height,
    void* const prediction, const ptrdiff_t pred_stride) {
  const auto* src = static_cast<const uint8_t*>(reference);
  auto* dest = static_cast<uint8_t*>(prediction);

  if (width == 128) {
    IntraBlockCopyHorizontal<128>(src, reference_stride, height, dest,
                                  pred_stride);
  } else if (width == 64) {
    IntraBlockCopyHorizontal<64>(src, reference_stride, height, dest,
                                 pred_stride);
  } else if (width == 32) {
    IntraBlockCopyHorizontal<32>(src, reference_stride, height, dest,
                                 pred_stride);
  } else if (width == 16) {
    IntraBlockCopyHorizontal<16>(src, reference_stride, height, dest,
                                 pred_stride);
  } else if (width == 8) {
    int y = 0;
    do {
      const uint8x8_t left = vld1_u8(src);
      const uint8x8_t right = vld1_u8(src + 1);
      vst1_u8(dest, vrhadd_u8(left, right));

      src += reference_stride;
      dest += pred_stride;
    } while (++y < height);
  } else if (width == 4) {
    uint8x8_t left = vdup_n_u8(0);
    uint8x8_t right = vdup_n_u8(0);
    int y = 0;
    do {
      left = LoadLo4(src, left);
      right = LoadLo4(src + 1, right);
      src += reference_stride;
      left = LoadHi4(src, left);
      right = LoadHi4(src + 1, right);
      src += reference_stride;

      const uint8x8_t result = vrhadd_u8(left, right);

      StoreLo4(dest, result);
      dest += pred_stride;
      StoreHi4(dest, result);
      dest += pred_stride;
      y += 2;
    } while (y < height);
  } else {
    assert(width == 2);
    uint8x8_t left = vdup_n_u8(0);
    uint8x8_t right = vdup_n_u8(0);
    int y = 0;
    do {
      left = Load2<0>(src, left);
      right = Load2<0>(src + 1, right);
      src += reference_stride;
      left = Load2<1>(src, left);
      right = Load2<1>(src + 1, right);
      src += reference_stride;

      const uint8x8_t result = vrhadd_u8(left, right);

      Store2<0>(dest, result);
      dest += pred_stride;
      Store2<1>(dest, result);
      dest += pred_stride;
      y += 2;
    } while (y < height);
  }
}

template <int width>
inline void IntraBlockCopyVertical(const uint8_t* src,
                                   const ptrdiff_t src_stride, const int height,
                                   uint8_t* dst, const ptrdiff_t dst_stride) {
  const ptrdiff_t src_remainder_stride = src_stride - (width - 16);
  const ptrdiff_t dst_remainder_stride = dst_stride - (width - 16);
  uint8x16_t row[8], below[8];

  row[0] = vld1q_u8(src);
  if (width >= 32) {
    src += 16;
    row[1] = vld1q_u8(src);
    if (width >= 64) {
      src += 16;
      row[2] = vld1q_u8(src);
      src += 16;
      row[3] = vld1q_u8(src);
      if (width == 128) {
        src += 16;
        row[4] = vld1q_u8(src);
        src += 16;
        row[5] = vld1q_u8(src);
        src += 16;
        row[6] = vld1q_u8(src);
        src += 16;
        row[7] = vld1q_u8(src);
      }
    }
  }
  src += src_remainder_stride;

  int y = 0;
  do {
    below[0] = vld1q_u8(src);
    if (width >= 32) {
      src += 16;
      below[1] = vld1q_u8(src);
      if (width >= 64) {
        src += 16;
        below[2] = vld1q_u8(src);
        src += 16;
        below[3] = vld1q_u8(src);
        if (width == 128) {
          src += 16;
          below[4] = vld1q_u8(src);
          src += 16;
          below[5] = vld1q_u8(src);
          src += 16;
          below[6] = vld1q_u8(src);
          src += 16;
          below[7] = vld1q_u8(src);
        }
      }
    }
    src += src_remainder_stride;

    vst1q_u8(dst, vrhaddq_u8(row[0], below[0]));
    row[0] = below[0];
    if (width >= 32) {
      dst += 16;
      vst1q_u8(dst, vrhaddq_u8(row[1], below[1]));
      row[1] = below[1];
      if (width >= 64) {
        dst += 16;
        vst1q_u8(dst, vrhaddq_u8(row[2], below[2]));
        row[2] = below[2];
        dst += 16;
        vst1q_u8(dst, vrhaddq_u8(row[3], below[3]));
        row[3] = below[3];
        if (width >= 128) {
          dst += 16;
          vst1q_u8(dst, vrhaddq_u8(row[4], below[4]));
          row[4] = below[4];
          dst += 16;
          vst1q_u8(dst, vrhaddq_u8(row[5], below[5]));
          row[5] = below[5];
          dst += 16;
          vst1q_u8(dst, vrhaddq_u8(row[6], below[6]));
          row[6] = below[6];
          dst += 16;
          vst1q_u8(dst, vrhaddq_u8(row[7], below[7]));
          row[7] = below[7];
        }
      }
    }
    dst += dst_remainder_stride;
  } while (++y < height);
}

void ConvolveIntraBlockCopyVertical_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int /*horizontal_filter_index*/, const int /*vertical_filter_index*/,
    const int /*inter_round_bits_vertical*/, const int /*subpixel_x*/,
    const int /*subpixel_y*/, const int width, const int height,
    void* const prediction, const ptrdiff_t pred_stride) {
  const auto* src = static_cast<const uint8_t*>(reference);
  auto* dest = static_cast<uint8_t*>(prediction);

  if (width == 128) {
    IntraBlockCopyVertical<128>(src, reference_stride, height, dest,
                                pred_stride);
  } else if (width == 64) {
    IntraBlockCopyVertical<64>(src, reference_stride, height, dest,
                               pred_stride);
  } else if (width == 32) {
    IntraBlockCopyVertical<32>(src, reference_stride, height, dest,
                               pred_stride);
  } else if (width == 16) {
    IntraBlockCopyVertical<16>(src, reference_stride, height, dest,
                               pred_stride);
  } else if (width == 8) {
    uint8x8_t row, below;
    row = vld1_u8(src);
    src += reference_stride;

    int y = 0;
    do {
      below = vld1_u8(src);
      src += reference_stride;

      vst1_u8(dest, vrhadd_u8(row, below));
      dest += pred_stride;

      row = below;
    } while (++y < height);
  } else if (width == 4) {
    uint8x8_t row = vdup_n_u8(0);
    uint8x8_t below = vdup_n_u8(0);
    row = LoadLo4(src, row);
    src += reference_stride;

    int y = 0;
    do {
      below = LoadLo4(src, below);
      src += reference_stride;

      StoreLo4(dest, vrhadd_u8(row, below));
      dest += pred_stride;

      row = below;
    } while (++y < height);
  } else {
    assert(width == 2);
    uint8x8_t row = vdup_n_u8(0);
    uint8x8_t below = vdup_n_u8(0);
    row = Load2<0>(src, row);
    src += reference_stride;
    int y = 0;
    do {
      below = Load2<0>(src, below);
      src += reference_stride;

      Store2<0>(dest, vrhadd_u8(row, below));
      dest += pred_stride;

      row = below;
    } while (++y < height);
  }
}

template <int width>
inline void IntraBlockCopy2D(const uint8_t* src, const ptrdiff_t src_stride,
                             const int height, uint8_t* dst,
                             const ptrdiff_t dst_stride) {
  const ptrdiff_t src_remainder_stride = src_stride - (width - 8);
  const ptrdiff_t dst_remainder_stride = dst_stride - (width - 8);
  uint16x8_t row[16];
  row[0] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
  if (width >= 16) {
    src += 8;
    row[1] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
    if (width >= 32) {
      src += 8;
      row[2] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
      src += 8;
      row[3] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
      if (width >= 64) {
        src += 8;
        row[4] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        src += 8;
        row[5] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        src += 8;
        row[6] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        src += 8;
        row[7] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        if (width == 128) {
          src += 8;
          row[8] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[9] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[10] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[11] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[12] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[13] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[14] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          src += 8;
          row[15] = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        }
      }
    }
  }
  src += src_remainder_stride;

  int y = 0;
  do {
    const uint16x8_t below_0 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
    vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[0], below_0), 2));
    row[0] = below_0;
    if (width >= 16) {
      src += 8;
      dst += 8;

      const uint16x8_t below_1 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
      vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[1], below_1), 2));
      row[1] = below_1;
      if (width >= 32) {
        src += 8;
        dst += 8;

        const uint16x8_t below_2 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[2], below_2), 2));
        row[2] = below_2;
        src += 8;
        dst += 8;

        const uint16x8_t below_3 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
        vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[3], below_3), 2));
        row[3] = below_3;
        if (width >= 64) {
          src += 8;
          dst += 8;

          const uint16x8_t below_4 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[4], below_4), 2));
          row[4] = below_4;
          src += 8;
          dst += 8;

          const uint16x8_t below_5 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[5], below_5), 2));
          row[5] = below_5;
          src += 8;
          dst += 8;

          const uint16x8_t below_6 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[6], below_6), 2));
          row[6] = below_6;
          src += 8;
          dst += 8;

          const uint16x8_t below_7 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
          vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[7], below_7), 2));
          row[7] = below_7;
          if (width == 128) {
            src += 8;
            dst += 8;

            const uint16x8_t below_8 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[8], below_8), 2));
            row[8] = below_8;
            src += 8;
            dst += 8;

            const uint16x8_t below_9 = vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[9], below_9), 2));
            row[9] = below_9;
            src += 8;
            dst += 8;

            const uint16x8_t below_10 =
                vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[10], below_10), 2));
            row[10] = below_10;
            src += 8;
            dst += 8;

            const uint16x8_t below_11 =
                vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[11], below_11), 2));
            row[11] = below_11;
            src += 8;
            dst += 8;

            const uint16x8_t below_12 =
                vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[12], below_12), 2));
            row[12] = below_12;
            src += 8;
            dst += 8;

            const uint16x8_t below_13 =
                vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[13], below_13), 2));
            row[13] = below_13;
            src += 8;
            dst += 8;

            const uint16x8_t below_14 =
                vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[14], below_14), 2));
            row[14] = below_14;
            src += 8;
            dst += 8;

            const uint16x8_t below_15 =
                vaddl_u8(vld1_u8(src), vld1_u8(src + 1));
            vst1_u8(dst, vrshrn_n_u16(vaddq_u16(row[15], below_15), 2));
            row[15] = below_15;
          }
        }
      }
    }
    src += src_remainder_stride;
    dst += dst_remainder_stride;
  } while (++y < height);
}

void ConvolveIntraBlockCopy2D_NEON(
    const void* const reference, const ptrdiff_t reference_stride,
    const int /*horizontal_filter_index*/, const int /*vertical_filter_index*/,
    const int /*inter_round_bits_vertical*/, const int /*subpixel_x*/,
    const int /*subpixel_y*/, const int width, const int height,
    void* const prediction, const ptrdiff_t pred_stride) {
  const auto* src = static_cast<const uint8_t*>(reference);
  auto* dest = static_cast<uint8_t*>(prediction);
  // Note: allow vertical access to height + 1. Because this function is only
  // for u/v plane of intra block copy, such access is guaranteed to be within
  // the prediction block.

  if (width == 128) {
    IntraBlockCopy2D<128>(src, reference_stride, height, dest, pred_stride);
  } else if (width == 64) {
    IntraBlockCopy2D<64>(src, reference_stride, height, dest, pred_stride);
  } else if (width == 32) {
    IntraBlockCopy2D<32>(src, reference_stride, height, dest, pred_stride);
  } else if (width == 16) {
    IntraBlockCopy2D<16>(src, reference_stride, height, dest, pred_stride);
  } else if (width == 8) {
    IntraBlockCopy2D<8>(src, reference_stride, height, dest, pred_stride);
  } else if (width == 4) {
    uint8x8_t left = vdup_n_u8(0);
    uint8x8_t right = vdup_n_u8(0);

    left = LoadLo4(src, left);
    right = LoadLo4(src + 1, right);
    src += reference_stride;

    uint16x4_t row = vget_low_u16(vaddl_u8(left, right));

    int y = 0;
    do {
      left = LoadLo4(src, left);
      right = LoadLo4(src + 1, right);
      src += reference_stride;
      left = LoadHi4(src, left);
      right = LoadHi4(src + 1, right);
      src += reference_stride;

      const uint16x8_t below = vaddl_u8(left, right);

      const uint8x8_t result = vrshrn_n_u16(
          vaddq_u16(vcombine_u16(row, vget_low_u16(below)), below), 2);
      StoreLo4(dest, result);
      dest += pred_stride;
      StoreHi4(dest, result);
      dest += pred_stride;

      row = vget_high_u16(below);
      y += 2;
    } while (y < height);
  } else {
    uint8x8_t left = vdup_n_u8(0);
    uint8x8_t right = vdup_n_u8(0);

    left = Load2<0>(src, left);
    right = Load2<0>(src + 1, right);
    src += reference_stride;

    uint16x4_t row = vget_low_u16(vaddl_u8(left, right));

    int y = 0;
    do {
      left = Load2<0>(src, left);
      right = Load2<0>(src + 1, right);
      src += reference_stride;
      left = Load2<2>(src, left);
      right = Load2<2>(src + 1, right);
      src += reference_stride;

      const uint16x8_t below = vaddl_u8(left, right);

      const uint8x8_t result = vrshrn_n_u16(
          vaddq_u16(vcombine_u16(row, vget_low_u16(below)), below), 2);
      Store2<0>(dest, result);
      dest += pred_stride;
      Store2<2>(dest, result);
      dest += pred_stride;

      row = vget_high_u16(below);
      y += 2;
    } while (y < height);
  }
}

void Init8bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(8);
  assert(dsp != nullptr);
  dsp->convolve[0][0][0][1] = ConvolveHorizontal_NEON;
  dsp->convolve[0][0][1][0] = ConvolveVertical_NEON;
  dsp->convolve[0][0][1][1] = Convolve2D_NEON;

  dsp->convolve[0][1][0][0] = ConvolveCompoundCopy_NEON;
  dsp->convolve[0][1][0][1] = ConvolveCompoundHorizontal_NEON;
  dsp->convolve[0][1][1][0] = ConvolveCompoundVertical_NEON;
  dsp->convolve[0][1][1][1] = ConvolveCompound2D_NEON;

  dsp->convolve[1][0][0][1] = ConvolveIntraBlockCopyHorizontal_NEON;
  dsp->convolve[1][0][1][0] = ConvolveIntraBlockCopyVertical_NEON;
  dsp->convolve[1][0][1][1] = ConvolveIntraBlockCopy2D_NEON;

  dsp->convolve_scale[0] = ConvolveScale2D_NEON<false>;
  dsp->convolve_scale[1] = ConvolveScale2D_NEON<true>;
}

}  // namespace
}  // namespace low_bitdepth

void ConvolveInit_NEON() { low_bitdepth::Init8bpp(); }

}  // namespace dsp
}  // namespace libgav1

#else   // !LIBGAV1_ENABLE_NEON

namespace libgav1 {
namespace dsp {

void ConvolveInit_NEON() {}

}  // namespace dsp
}  // namespace libgav1
#endif  // LIBGAV1_ENABLE_NEON
