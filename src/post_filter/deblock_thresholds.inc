// Copyright 2020 The libgav1 Authors
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

// Thresholds for the deblocking filter. Precomputed values of part of Section
// 7.14.4 for all possible values of sharpness.

constexpr uint8_t kInnerThresh[8][kMaxLoopFilterValue + 1] = {
    {1,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
     32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
     48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63},
    {1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8},
    {1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
     7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
     7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7},
    {1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
     6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
     6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
     5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
     5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5},
    {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4,
     4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
     4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
     3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
     3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
    {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
     2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
     2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}};

constexpr uint8_t kOuterThresh[8][kMaxLoopFilterValue + 1] = {
    {5,   7,   10,  13,  16,  19,  22,  25,  28,  31,  34,  37,  40,
     43,  46,  49,  52,  55,  58,  61,  64,  67,  70,  73,  76,  79,
     82,  85,  88,  91,  94,  97,  100, 103, 106, 109, 112, 115, 118,
     121, 124, 127, 130, 133, 136, 139, 142, 145, 148, 151, 154, 157,
     160, 163, 166, 169, 172, 175, 178, 181, 184, 187, 190, 193},
    {5,   7,   9,   11,  14,  16,  19,  21,  24,  26,  29,  31,  34,
     36,  39,  41,  44,  46,  48,  50,  52,  54,  56,  58,  60,  62,
     64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  86,  88,
     90,  92,  94,  96,  98,  100, 102, 104, 106, 108, 110, 112, 114,
     116, 118, 120, 122, 124, 126, 128, 130, 132, 134, 136, 138},
    {5,   7,   9,   11,  14,  16,  19,  21,  24,  26,  29,  31,  34,
     36,  39,  41,  43,  45,  47,  49,  51,  53,  55,  57,  59,  61,
     63,  65,  67,  69,  71,  73,  75,  77,  79,  81,  83,  85,  87,
     89,  91,  93,  95,  97,  99,  101, 103, 105, 107, 109, 111, 113,
     115, 117, 119, 121, 123, 125, 127, 129, 131, 133, 135, 137},
    {5,   7,   9,   11,  14,  16,  19,  21,  24,  26,  29,  31,  34,
     36,  38,  40,  42,  44,  46,  48,  50,  52,  54,  56,  58,  60,
     62,  64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  86,
     88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 108, 110, 112,
     114, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134, 136},
    {5,   7,   9,   11,  14,  16,  19,  21,  24,  26,  29,  31,  33,
     35,  37,  39,  41,  43,  45,  47,  49,  51,  53,  55,  57,  59,
     61,  63,  65,  67,  69,  71,  73,  75,  77,  79,  81,  83,  85,
     87,  89,  91,  93,  95,  97,  99,  101, 103, 105, 107, 109, 111,
     113, 115, 117, 119, 121, 123, 125, 127, 129, 131, 133, 135},
    {5,   7,   9,   11,  13,  15,  17,  19,  22,  24,  26,  28,  31,
     33,  35,  37,  40,  42,  44,  46,  48,  50,  52,  54,  56,  58,
     60,  62,  64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,
     86,  88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 108, 110,
     112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134},
    {5,   7,   9,   11,  13,  15,  17,  19,  22,  24,  26,  28,  31,
     33,  35,  37,  39,  41,  43,  45,  47,  49,  51,  53,  55,  57,
     59,  61,  63,  65,  67,  69,  71,  73,  75,  77,  79,  81,  83,
     85,  87,  89,  91,  93,  95,  97,  99,  101, 103, 105, 107, 109,
     111, 113, 115, 117, 119, 121, 123, 125, 127, 129, 131, 133},
    {5,   7,   9,   11,  13,  15,  17,  19,  22,  24,  26,  28,  30,
     32,  34,  36,  38,  40,  42,  44,  46,  48,  50,  52,  54,  56,
     58,  60,  62,  64,  66,  68,  70,  72,  74,  76,  78,  80,  82,
     84,  86,  88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 108,
     110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132}};