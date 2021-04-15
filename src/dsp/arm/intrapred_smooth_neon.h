/*
 * Copyright 2021 The libgav1 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBGAV1_SRC_DSP_ARM_INTRAPRED_SMOOTH_NEON_H_
#define LIBGAV1_SRC_DSP_ARM_INTRAPRED_SMOOTH_NEON_H_

#include "src/dsp/dsp.h"
#include "src/utils/cpu.h"

namespace libgav1 {
namespace dsp {

// Initializes Dsp::intra_predictors[][kIntraPredictorSmooth.*].
// This function is not thread-safe.
void IntraPredSmoothInit_NEON();

}  // namespace dsp
}  // namespace libgav1

#if LIBGAV1_ENABLE_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x4_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x4_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x4_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize4x8_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x8_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x8_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize4x16_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x16_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize4x16_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize8x4_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x4_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x4_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize8x8_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x8_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x8_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize8x16_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x16_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x16_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize8x32_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x32_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize8x32_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize16x4_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x4_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x4_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize16x8_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x8_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x8_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize16x16_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x16_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x16_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize16x32_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x32_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x32_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize16x64_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x64_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize16x64_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize32x8_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x8_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x8_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize32x16_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x16_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x16_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize32x32_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x32_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x32_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize32x64_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x64_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize32x64_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize64x16_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize64x16_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize64x16_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize64x32_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize64x32_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize64x32_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp8bpp_TransformSize64x64_IntraPredictorSmooth LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize64x64_IntraPredictorSmoothVertical \
  LIBGAV1_CPU_NEON
#define LIBGAV1_Dsp8bpp_TransformSize64x64_IntraPredictorSmoothHorizontal \
  LIBGAV1_CPU_NEON

// 10bpp
#define LIBGAV1_Dsp10bpp_TransformSize4x4_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize4x8_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize4x16_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize8x4_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize8x8_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize8x16_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize8x32_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize16x4_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize16x8_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize16x16_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize16x32_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize16x64_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize32x8_IntraPredictorSmooth LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize32x16_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize32x32_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize32x64_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize64x16_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize64x32_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#define LIBGAV1_Dsp10bpp_TransformSize64x64_IntraPredictorSmooth \
  LIBGAV1_CPU_NEON

#endif  // LIBGAV1_ENABLE_NEON

#endif  // LIBGAV1_SRC_DSP_ARM_INTRAPRED_SMOOTH_NEON_H_
