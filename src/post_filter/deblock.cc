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
#include <atomic>

#include "src/post_filter.h"
#include "src/utils/blocking_counter.h"

namespace libgav1 {
namespace {

constexpr uint8_t HevThresh(int level) { return DivideBy16(level); }

// GetLoopFilterSize* functions depend on this exact ordering of the
// LoopFilterSize enums.
static_assert(dsp::kLoopFilterSize4 == 0, "");
static_assert(dsp::kLoopFilterSize6 == 1, "");
static_assert(dsp::kLoopFilterSize8 == 2, "");
static_assert(dsp::kLoopFilterSize14 == 3, "");

dsp::LoopFilterSize GetLoopFilterSizeY(int filter_length) {
  // |filter_length| must be a power of 2.
  assert((filter_length & (filter_length - 1)) == 0);
  // This code is the branch free equivalent of:
  //   if (filter_length == 4) return kLoopFilterSize4;
  //   if (filter_length == 8) return kLoopFilterSize8;
  //   return kLoopFilterSize14;
  return static_cast<dsp::LoopFilterSize>(
      MultiplyBy2(static_cast<int>(filter_length > 4)) +
      static_cast<int>(filter_length > 8));
}

constexpr dsp::LoopFilterSize GetLoopFilterSizeUV(int filter_length) {
  // For U & V planes, size is kLoopFilterSize4 if |filter_length| is 4,
  // otherwise size is kLoopFilterSize6.
  return static_cast<dsp::LoopFilterSize>(filter_length != 4);
}

// Returns true if skip and is_inter are true for the current block and the
// current row4x4/column4x4 is not in a block boundary. If we are at a block
// boundary, then |bp| and |bp_prev| will not be equal.
constexpr bool SkipFilter(const BlockParameters* const bp,
                          const BlockParameters* const bp_prev) {
  return bp->skip && bp->is_inter && bp == bp_prev;
}

// 7.14.5.
void ComputeDeblockFilterLevelsHelper(
    const ObuFrameHeader& frame_header, int segment_id, int level_index,
    const int8_t delta_lf[kFrameLfCount],
    uint8_t deblock_filter_levels[kNumReferenceFrameTypes][2]) {
  const int delta = delta_lf[frame_header.delta_lf.multi ? level_index : 0];
  uint8_t level = Clip3(frame_header.loop_filter.level[level_index] + delta, 0,
                        kMaxLoopFilterValue);
  const auto feature = static_cast<SegmentFeature>(
      kSegmentFeatureLoopFilterYVertical + level_index);
  level =
      Clip3(level + frame_header.segmentation.feature_data[segment_id][feature],
            0, kMaxLoopFilterValue);
  if (!frame_header.loop_filter.delta_enabled) {
    static_assert(sizeof(deblock_filter_levels[0][0]) == 1, "");
    memset(deblock_filter_levels, level, kNumReferenceFrameTypes * 2);
    return;
  }
  assert(frame_header.loop_filter.delta_enabled);
  const int shift = level >> 5;
  deblock_filter_levels[kReferenceFrameIntra][0] = Clip3(
      level +
          LeftShift(frame_header.loop_filter.ref_deltas[kReferenceFrameIntra],
                    shift),
      0, kMaxLoopFilterValue);
  // deblock_filter_levels[kReferenceFrameIntra][1] is never used. So it does
  // not have to be populated.
  for (int reference_frame = kReferenceFrameIntra + 1;
       reference_frame < kNumReferenceFrameTypes; ++reference_frame) {
    for (int mode_id = 0; mode_id < 2; ++mode_id) {
      deblock_filter_levels[reference_frame][mode_id] = Clip3(
          level +
              LeftShift(frame_header.loop_filter.ref_deltas[reference_frame] +
                            frame_header.loop_filter.mode_deltas[mode_id],
                        shift),
          0, kMaxLoopFilterValue);
    }
  }
}

}  // namespace

void PostFilter::ComputeDeblockFilterLevels(
    const int8_t delta_lf[kFrameLfCount],
    uint8_t deblock_filter_levels[kMaxSegments][kFrameLfCount]
                                 [kNumReferenceFrameTypes][2]) const {
  if (!DoDeblock()) return;
  for (int segment_id = 0;
       segment_id < (frame_header_.segmentation.enabled ? kMaxSegments : 1);
       ++segment_id) {
    int level_index = 0;
    for (; level_index < 2; ++level_index) {
      ComputeDeblockFilterLevelsHelper(
          frame_header_, segment_id, level_index, delta_lf,
          deblock_filter_levels[segment_id][level_index]);
    }
    for (; level_index < kFrameLfCount; ++level_index) {
      if (frame_header_.loop_filter.level[level_index] != 0) {
        ComputeDeblockFilterLevelsHelper(
            frame_header_, segment_id, level_index, delta_lf,
            deblock_filter_levels[segment_id][level_index]);
      }
    }
  }
}

bool PostFilter::GetHorizontalDeblockFilterEdgeInfo(int row4x4, int column4x4,
                                                    uint8_t* level, int* step,
                                                    int* filter_length) const {
  *step = kTransformHeight[inter_transform_sizes_[row4x4][column4x4]];
  if (row4x4 == 0) return false;

  const BlockParameters* bp = block_parameters_.Find(row4x4, column4x4);
  const uint8_t level_this = bp->deblock_filter_level[1];
  const int row4x4_prev = row4x4 - 1;
  assert(row4x4_prev >= 0);
  const BlockParameters* bp_prev =
      block_parameters_.Find(row4x4_prev, column4x4);
  const uint8_t level_prev = bp_prev->deblock_filter_level[1];
  *level = level_this;
  if (level_this == 0) {
    if (level_prev == 0) return false;
    *level = level_prev;
  }
  if (SkipFilter(bp, bp_prev)) return false;
  const int step_prev =
      kTransformHeight[inter_transform_sizes_[row4x4_prev][column4x4]];
  *filter_length = std::min(*step, step_prev);
  return true;
}

void PostFilter::GetHorizontalDeblockFilterEdgeInfoUV(
    int row4x4, int column4x4, bool* need_filter_u, bool* need_filter_v,
    uint8_t* level_u, uint8_t* level_v, int* step, int* filter_length) const {
  const int subsampling_x = subsampling_x_[kPlaneU];
  const int subsampling_y = subsampling_y_[kPlaneU];
  row4x4 = GetDeblockPosition(row4x4, subsampling_y);
  column4x4 = GetDeblockPosition(column4x4, subsampling_x);
  const BlockParameters* bp = block_parameters_.Find(row4x4, column4x4);
  *step = kTransformHeight[bp->uv_transform_size];
  if (row4x4 == subsampling_y) {
    *need_filter_u = false;
    *need_filter_v = false;
    return;
  }

  *need_filter_u = frame_header_.loop_filter.level[kPlaneU + 1] != 0;
  *need_filter_v = frame_header_.loop_filter.level[kPlaneV + 1] != 0;
  const int filter_id_u =
      kDeblockFilterLevelIndex[kPlaneU][kLoopFilterTypeHorizontal];
  const int filter_id_v =
      kDeblockFilterLevelIndex[kPlaneV][kLoopFilterTypeHorizontal];
  const int row4x4_prev = row4x4 - (1 << subsampling_y);
  assert(row4x4_prev >= 0);
  const BlockParameters* bp_prev =
      block_parameters_.Find(row4x4_prev, column4x4);

  if (*need_filter_u) {
    const uint8_t level_u_this = bp->deblock_filter_level[filter_id_u];
    *level_u = level_u_this;
    if (level_u_this == 0) {
      const uint8_t level_u_prev = bp_prev->deblock_filter_level[filter_id_u];
      if (level_u_prev == 0) {
        *need_filter_u = false;
      } else {
        *level_u = level_u_prev;
      }
    }
  }

  if (*need_filter_v) {
    const uint8_t level_v_this = bp->deblock_filter_level[filter_id_v];
    *level_v = level_v_this;
    if (level_v_this == 0) {
      const uint8_t level_v_prev = bp_prev->deblock_filter_level[filter_id_v];
      if (level_v_prev == 0) {
        *need_filter_v = false;
      } else {
        *level_v = level_v_prev;
      }
    }
  }

  if (!*need_filter_u && !*need_filter_v) return;

  if (SkipFilter(bp, bp_prev)) {
    *need_filter_u = false;
    *need_filter_v = false;
    return;
  }
  const int step_prev = kTransformHeight[bp_prev->uv_transform_size];
  *filter_length = std::min(*step, step_prev);
}

bool PostFilter::GetVerticalDeblockFilterEdgeInfo(
    int row4x4, int column4x4, BlockParameters* const* bp_ptr, uint8_t* level,
    int* step, int* filter_length) const {
  const BlockParameters* bp = *bp_ptr;
  *step = kTransformWidth[inter_transform_sizes_[row4x4][column4x4]];
  if (column4x4 == 0) return false;

  const int filter_id = 0;
  const uint8_t level_this = bp->deblock_filter_level[filter_id];
  const int column4x4_prev = column4x4 - 1;
  assert(column4x4_prev >= 0);
  const BlockParameters* bp_prev = *(bp_ptr - 1);
  const uint8_t level_prev = bp_prev->deblock_filter_level[filter_id];
  *level = level_this;
  if (level_this == 0) {
    if (level_prev == 0) return false;
    *level = level_prev;
  }

  if (SkipFilter(bp, bp_prev)) return false;
  const int step_prev =
      kTransformWidth[inter_transform_sizes_[row4x4][column4x4_prev]];
  *filter_length = std::min(*step, step_prev);
  return true;
}

void PostFilter::GetVerticalDeblockFilterEdgeInfoUV(
    int row4x4, int column4x4, BlockParameters* const* bp_ptr,
    bool* need_filter_u, bool* need_filter_v, uint8_t* level_u,
    uint8_t* level_v, int* step, int* filter_length) const {
  const int subsampling_x = subsampling_x_[kPlaneU];
  const int subsampling_y = subsampling_y_[kPlaneU];
  row4x4 = GetDeblockPosition(row4x4, subsampling_y);
  column4x4 = GetDeblockPosition(column4x4, subsampling_x);
  const BlockParameters* bp = *bp_ptr;
  *step = kTransformWidth[bp->uv_transform_size];
  if (column4x4 == subsampling_x) {
    *need_filter_u = false;
    *need_filter_v = false;
    return;
  }

  *need_filter_u = frame_header_.loop_filter.level[kPlaneU + 1] != 0;
  *need_filter_v = frame_header_.loop_filter.level[kPlaneV + 1] != 0;
  const int filter_id_u =
      kDeblockFilterLevelIndex[kPlaneU][kLoopFilterTypeVertical];
  const int filter_id_v =
      kDeblockFilterLevelIndex[kPlaneV][kLoopFilterTypeVertical];
  const BlockParameters* bp_prev = *(bp_ptr - (1 << subsampling_x));

  if (*need_filter_u) {
    const uint8_t level_u_this = bp->deblock_filter_level[filter_id_u];
    *level_u = level_u_this;
    if (level_u_this == 0) {
      const uint8_t level_u_prev = bp_prev->deblock_filter_level[filter_id_u];
      if (level_u_prev == 0) {
        *need_filter_u = false;
      } else {
        *level_u = level_u_prev;
      }
    }
  }

  if (*need_filter_v) {
    const uint8_t level_v_this = bp->deblock_filter_level[filter_id_v];
    *level_v = level_v_this;
    if (level_v_this == 0) {
      const uint8_t level_v_prev = bp_prev->deblock_filter_level[filter_id_v];
      if (level_v_prev == 0) {
        *need_filter_v = false;
      } else {
        *level_v = level_v_prev;
      }
    }
  }

  if (!*need_filter_u && !*need_filter_v) return;

  if (SkipFilter(bp, bp_prev)) {
    *need_filter_u = false;
    *need_filter_v = false;
    return;
  }
  const int step_prev = kTransformWidth[bp_prev->uv_transform_size];
  *filter_length = std::min(*step, step_prev);
}

void PostFilter::HorizontalDeblockFilter(int row4x4_start,
                                         int column4x4_start) {
  const int column_step = 1;
  const size_t src_step = MultiplyBy4(pixel_size_);
  const ptrdiff_t src_stride = frame_buffer_.stride(kPlaneY);
  uint8_t* src = GetSourceBuffer(kPlaneY, row4x4_start, column4x4_start);
  int row_step;
  uint8_t level;
  int filter_length;

  for (int column4x4 = 0; MultiplyBy4(column4x4_start + column4x4) < width_ &&
                          column4x4 < kNum4x4InLoopFilterUnit;
       column4x4 += column_step, src += src_step) {
    uint8_t* src_row = src;
    for (int row4x4 = 0; MultiplyBy4(row4x4_start + row4x4) < height_ &&
                         row4x4 < kNum4x4InLoopFilterUnit;
         row4x4 += row_step) {
      const bool need_filter = GetHorizontalDeblockFilterEdgeInfo(
          row4x4_start + row4x4, column4x4_start + column4x4, &level, &row_step,
          &filter_length);
      if (need_filter) {
        const dsp::LoopFilterSize size = GetLoopFilterSizeY(filter_length);
        dsp_.loop_filters[size][kLoopFilterTypeHorizontal](
            src_row, src_stride, outer_thresh_[level], inner_thresh_[level],
            HevThresh(level));
      }
      // TODO(chengchen): use shifts instead of multiplication.
      src_row += row_step * src_stride;
      row_step = DivideBy4(row_step);
    }
  }

  if (needs_chroma_deblock_) {
    const int8_t subsampling_x = subsampling_x_[kPlaneU];
    const int8_t subsampling_y = subsampling_y_[kPlaneU];
    const int column_step = 1 << subsampling_x;
    const ptrdiff_t src_stride_u = frame_buffer_.stride(kPlaneU);
    const ptrdiff_t src_stride_v = frame_buffer_.stride(kPlaneV);
    uint8_t* src_u = GetSourceBuffer(kPlaneU, row4x4_start, column4x4_start);
    uint8_t* src_v = GetSourceBuffer(kPlaneV, row4x4_start, column4x4_start);
    int row_step;
    bool need_filter_u;
    bool need_filter_v;
    uint8_t level_u;
    uint8_t level_v;
    int filter_length;

    for (int column4x4 = 0; MultiplyBy4(column4x4_start + column4x4) < width_ &&
                            column4x4 < kNum4x4InLoopFilterUnit;
         column4x4 += column_step, src_u += src_step, src_v += src_step) {
      uint8_t* src_row_u = src_u;
      uint8_t* src_row_v = src_v;
      for (int row4x4 = 0; MultiplyBy4(row4x4_start + row4x4) < height_ &&
                           row4x4 < kNum4x4InLoopFilterUnit;
           row4x4 += row_step) {
        GetHorizontalDeblockFilterEdgeInfoUV(
            row4x4_start + row4x4, column4x4_start + column4x4, &need_filter_u,
            &need_filter_v, &level_u, &level_v, &row_step, &filter_length);
        const dsp::LoopFilterSize size = GetLoopFilterSizeUV(filter_length);
        if (need_filter_u) {
          dsp_.loop_filters[size][kLoopFilterTypeHorizontal](
              src_row_u, src_stride_u, outer_thresh_[level_u],
              inner_thresh_[level_u], HevThresh(level_u));
        }
        if (need_filter_v) {
          dsp_.loop_filters[size][kLoopFilterTypeHorizontal](
              src_row_v, src_stride_v, outer_thresh_[level_v],
              inner_thresh_[level_v], HevThresh(level_v));
        }
        src_row_u += row_step * src_stride_u;
        src_row_v += row_step * src_stride_v;
        row_step = DivideBy4(row_step << subsampling_y);
      }
    }
  }
}

void PostFilter::VerticalDeblockFilter(int row4x4_start, int column4x4_start) {
  const ptrdiff_t row_stride = MultiplyBy4(frame_buffer_.stride(kPlaneY));
  const ptrdiff_t src_stride = frame_buffer_.stride(kPlaneY);
  uint8_t* src = GetSourceBuffer(kPlaneY, row4x4_start, column4x4_start);
  int column_step;
  uint8_t level;
  int filter_length;

  BlockParameters* const* bp_row_base =
      block_parameters_.Address(row4x4_start, column4x4_start);
  const int bp_stride = block_parameters_.columns4x4();
  for (int row4x4 = 0; MultiplyBy4(row4x4_start + row4x4) < height_ &&
                       row4x4 < kNum4x4InLoopFilterUnit;
       ++row4x4, src += row_stride, bp_row_base += bp_stride) {
    uint8_t* src_row = src;
    BlockParameters* const* bp = bp_row_base;
    for (int column4x4 = 0; MultiplyBy4(column4x4_start + column4x4) < width_ &&
                            column4x4 < kNum4x4InLoopFilterUnit;
         column4x4 += column_step, bp += column_step) {
      const bool need_filter = GetVerticalDeblockFilterEdgeInfo(
          row4x4_start + row4x4, column4x4_start + column4x4, bp, &level,
          &column_step, &filter_length);
      if (need_filter) {
        const dsp::LoopFilterSize size = GetLoopFilterSizeY(filter_length);
        dsp_.loop_filters[size][kLoopFilterTypeVertical](
            src_row, src_stride, outer_thresh_[level], inner_thresh_[level],
            HevThresh(level));
      }
      src_row += column_step * pixel_size_;
      column_step = DivideBy4(column_step);
    }
  }

  if (needs_chroma_deblock_) {
    const int8_t subsampling_x = subsampling_x_[kPlaneU];
    const int8_t subsampling_y = subsampling_y_[kPlaneU];
    const int row_step = 1 << subsampling_y;
    uint8_t* src_u = GetSourceBuffer(kPlaneU, row4x4_start, column4x4_start);
    uint8_t* src_v = GetSourceBuffer(kPlaneV, row4x4_start, column4x4_start);
    const ptrdiff_t src_stride_u = frame_buffer_.stride(kPlaneU);
    const ptrdiff_t src_stride_v = frame_buffer_.stride(kPlaneV);
    const ptrdiff_t row_stride_u = MultiplyBy4(frame_buffer_.stride(kPlaneU));
    const ptrdiff_t row_stride_v = MultiplyBy4(frame_buffer_.stride(kPlaneV));
    const LoopFilterType type = kLoopFilterTypeVertical;
    int column_step;
    uint8_t level_u, level_v;
    bool need_filter_u, need_filter_v;
    int filter_length;

    BlockParameters* const* bp_row_base = block_parameters_.Address(
        GetDeblockPosition(row4x4_start, subsampling_y),
        GetDeblockPosition(column4x4_start, subsampling_x));
    const int bp_stride = block_parameters_.columns4x4() * row_step;
    for (int row4x4 = 0; MultiplyBy4(row4x4_start + row4x4) < height_ &&
                         row4x4 < kNum4x4InLoopFilterUnit;
         row4x4 += row_step, src_u += row_stride_u, src_v += row_stride_v,
             bp_row_base += bp_stride) {
      uint8_t* src_row_u = src_u;
      uint8_t* src_row_v = src_v;
      BlockParameters* const* bp = bp_row_base;
      for (int column4x4 = 0;
           MultiplyBy4(column4x4_start + column4x4) < width_ &&
           column4x4 < kNum4x4InLoopFilterUnit;
           column4x4 += column_step, bp += column_step) {
        GetVerticalDeblockFilterEdgeInfoUV(
            row4x4_start + row4x4, column4x4_start + column4x4, bp,
            &need_filter_u, &need_filter_v, &level_u, &level_v, &column_step,
            &filter_length);
        const dsp::LoopFilterSize size = GetLoopFilterSizeUV(filter_length);
        if (need_filter_u) {
          dsp_.loop_filters[size][type](
              src_row_u, src_stride_u, outer_thresh_[level_u],
              inner_thresh_[level_u], HevThresh(level_u));
        }
        if (need_filter_v) {
          dsp_.loop_filters[size][type](
              src_row_v, src_stride_v, outer_thresh_[level_v],
              inner_thresh_[level_v], HevThresh(level_v));
        }
        src_row_u += column_step * pixel_size_;
        src_row_v += column_step * pixel_size_;
        column_step = DivideBy4(column_step << subsampling_x);
      }
    }
  }
}

void PostFilter::ApplyDeblockFilterForOneSuperBlockRow(int row4x4_start,
                                                       int sb4x4) {
  assert(row4x4_start >= 0);
  assert(DoDeblock());
  for (int y = 0; y < sb4x4; y += 16) {
    const int row4x4 = row4x4_start + y;
    if (row4x4 >= frame_header_.rows4x4) break;
    int column4x4;
    for (column4x4 = 0; column4x4 < frame_header_.columns4x4;
         column4x4 += kNum4x4InLoopFilterUnit) {
      // First apply vertical filtering
      VerticalDeblockFilter(row4x4, column4x4);

      // Delay one superblock to apply horizontal filtering.
      if (column4x4 != 0) {
        HorizontalDeblockFilter(row4x4, column4x4 - kNum4x4InLoopFilterUnit);
      }
    }
    // Horizontal filtering for the last 64x64 block.
    HorizontalDeblockFilter(row4x4, column4x4 - kNum4x4InLoopFilterUnit);
  }
}

void PostFilter::DeblockFilterWorker(int jobs_per_plane,
                                     const Plane* /*planes*/,
                                     int /*num_planes*/,
                                     std::atomic<int>* job_counter,
                                     DeblockFilter deblock_filter) {
  const int total_jobs = jobs_per_plane;
  int job_index;
  while ((job_index = job_counter->fetch_add(1, std::memory_order_relaxed)) <
         total_jobs) {
    const int row_unit = job_index % jobs_per_plane;
    const int row4x4 = row_unit * kNum4x4InLoopFilterUnit;
    for (int column4x4 = 0; column4x4 < frame_header_.columns4x4;
         column4x4 += kNum4x4InLoopFilterUnit) {
      (this->*deblock_filter)(row4x4, column4x4);
    }
  }
}

void PostFilter::ApplyDeblockFilterThreaded() {
  const int jobs_per_plane = DivideBy16(frame_header_.rows4x4 + 15);
  const int num_workers = thread_pool_->num_threads();
  std::array<Plane, kMaxPlanes> planes;
  planes[0] = kPlaneY;
  int num_planes = 1;
  for (int plane = kPlaneU; plane < planes_; ++plane) {
    if (frame_header_.loop_filter.level[plane + 1] != 0) {
      planes[num_planes++] = static_cast<Plane>(plane);
    }
  }
  // The vertical filters are not dependent on each other. So simply schedule
  // them for all possible rows.
  //
  // The horizontal filter for a row/column depends on the vertical filter being
  // finished for the blocks to the top and to the right. To work around
  // this synchronization, we simply wait for the vertical filter to finish for
  // all rows. Now, the horizontal filters can also be scheduled
  // unconditionally similar to the vertical filters.
  //
  // The only synchronization involved is to know when the each directional
  // filter is complete for the entire frame.
  for (auto& type : {kLoopFilterTypeVertical, kLoopFilterTypeHorizontal}) {
    const DeblockFilter deblock_filter = deblock_filter_func_[type];
    std::atomic<int> job_counter(0);
    BlockingCounter pending_workers(num_workers);
    for (int i = 0; i < num_workers; ++i) {
      thread_pool_->Schedule([this, jobs_per_plane, &planes, num_planes,
                              &job_counter, deblock_filter,
                              &pending_workers]() {
        DeblockFilterWorker(jobs_per_plane, planes.data(), num_planes,
                            &job_counter, deblock_filter);
        pending_workers.Decrement();
      });
    }
    // Run the jobs on the current thread.
    DeblockFilterWorker(jobs_per_plane, planes.data(), num_planes, &job_counter,
                        deblock_filter);
    // Wait for the threadpool jobs to finish.
    pending_workers.Wait();
  }
}

void PostFilter::ApplyDeblockFilter(LoopFilterType loop_filter_type,
                                    int row4x4_start, int column4x4_start,
                                    int column4x4_end, int sb4x4) {
  assert(row4x4_start >= 0);
  assert(DoDeblock());

  column4x4_end = std::min(column4x4_end, frame_header_.columns4x4);
  if (column4x4_start >= column4x4_end) return;

  const DeblockFilter deblock_filter = deblock_filter_func_[loop_filter_type];
  const int sb_height4x4 =
      std::min(sb4x4, frame_header_.rows4x4 - row4x4_start);
  for (int y = 0; y < sb_height4x4; y += kNum4x4InLoopFilterUnit) {
    const int row4x4 = row4x4_start + y;
    for (int column4x4 = column4x4_start; column4x4 < column4x4_end;
         column4x4 += kNum4x4InLoopFilterUnit) {
      (this->*deblock_filter)(row4x4, column4x4);
    }
  }
}

}  // namespace libgav1
