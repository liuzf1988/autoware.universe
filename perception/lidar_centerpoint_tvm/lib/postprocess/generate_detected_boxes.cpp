// Copyright 2022 AutoCore Ltd., TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lidar_centerpoint_tvm/postprocess/generate_detected_boxes.hpp"

#include <lidar_centerpoint_tvm/postprocess/circle_nms.hpp>

#include <algorithm>
#include <cmath>
#include <thread>

namespace
{
const std::size_t THREAD_NUM_POST = 32;
}  // namespace

namespace autoware
{
namespace perception
{
namespace lidar_centerpoint_tvm
{

struct is_score_greater
{
  is_score_greater(float32_t t) : t_(t) {}
  bool8_t operator()(const Box3D & b) { return b.score > t_; }

private:
  float32_t t_{0.0};
};

struct is_kept
{
  bool8_t operator()(const bool8_t keep) { return keep; }
};

struct score_greater
{
  bool operator()(const Box3D & lb, const Box3D & rb) { return lb.score > rb.score; }
};

inline float32_t sigmoid(float32_t x) { return 1.0f / (1.0f + expf(-x)); }

void generateBoxes3D_worker(
  const float32_t * out_heatmap, const float32_t * out_offset, const float32_t * out_z,
  const float32_t * out_dim, const float32_t * out_rot, const float32_t * out_vel,
  const CenterPointConfig & config, Box3D * boxes3d, std::size_t thread_idx,
  std::size_t grids_per_thread)
{
  // generate boxes3d from the outputs of the network.
  // shape of out_*: (N, DOWN_GRID_SIZE_Y, DOWN_GRID_SIZE_X)
  // heatmap: N = class_size, offset: N = 2, z: N = 1, dim: N = 3, rot: N = 2, vel: N = 2
  for (std::size_t idx = 0; idx < grids_per_thread; idx++) {
    std::size_t grid_idx = thread_idx * grids_per_thread + idx;
    const auto down_grid_size = config.down_grid_size_y_ * config.down_grid_size_x_;
    if (grid_idx >= down_grid_size) return;

    const auto yi = grid_idx / config.down_grid_size_x_;
    const auto xi = grid_idx % config.down_grid_size_x_;

    int32_t label = -1;
    float32_t max_score = -1;
    for (std::size_t ci = 0; ci < config.class_size_; ci++) {
      float32_t score = sigmoid(out_heatmap[down_grid_size * ci + grid_idx]);
      if (score > max_score) {
        label = ci;
        max_score = score;
      }
    }

    const float32_t offset_x = out_offset[down_grid_size * 0 + grid_idx];
    const float32_t offset_y = out_offset[down_grid_size * 1 + grid_idx];
    const float32_t x =
      config.voxel_size_x_ * config.downsample_factor_ * (xi + offset_x) + config.range_min_x_;
    const float32_t y =
      config.voxel_size_y_ * config.downsample_factor_ * (yi + offset_y) + config.range_min_y_;
    const float32_t z = out_z[grid_idx];
    const float32_t w = out_dim[down_grid_size * 0 + grid_idx];
    const float32_t l = out_dim[down_grid_size * 1 + grid_idx];
    const float32_t h = out_dim[down_grid_size * 2 + grid_idx];
    const float32_t yaw_sin = out_rot[down_grid_size * 0 + grid_idx];
    const float32_t yaw_cos = out_rot[down_grid_size * 1 + grid_idx];
    const float32_t yaw_norm = sqrtf(yaw_sin * yaw_sin + yaw_cos * yaw_cos);
    const float32_t vel_x = out_vel[down_grid_size * 0 + grid_idx];
    const float32_t vel_y = out_vel[down_grid_size * 1 + grid_idx];

    boxes3d[grid_idx].label = label;
    boxes3d[grid_idx].score = yaw_norm >= config.yaw_norm_threshold_ ? max_score : 0.f;
    boxes3d[grid_idx].x = x;
    boxes3d[grid_idx].y = y;
    boxes3d[grid_idx].z = z;
    boxes3d[grid_idx].length = expf(l);
    boxes3d[grid_idx].width = expf(w);
    boxes3d[grid_idx].height = expf(h);
    boxes3d[grid_idx].yaw = atan2f(yaw_sin, yaw_cos);
    boxes3d[grid_idx].vel_x = vel_x;
    boxes3d[grid_idx].vel_y = vel_y;
  }
}

void generateDetectedBoxes3D(
  const float32_t * out_heatmap, const float32_t * out_offset, const float32_t * out_z,
  const float32_t * out_dim, const float32_t * out_rot, const float32_t * out_vel,
  const CenterPointConfig & config, std::vector<Box3D> & det_boxes3d)
{
  std::vector<std::thread> threadPool;
  const auto down_grid_size = config.down_grid_size_y_ * config.down_grid_size_x_;
  std::vector<Box3D> boxes3d(down_grid_size);
  std::size_t grids_per_thread = divup(down_grid_size, THREAD_NUM_POST);
  for (std::size_t idx = 0; idx < THREAD_NUM_POST; idx++) {
    std::thread worker(
      generateBoxes3D_worker, out_heatmap, out_offset, out_z, out_dim, out_rot, out_vel, config,
      &boxes3d.front(), idx, grids_per_thread);
    threadPool.push_back(std::move(worker));
  }
  for (std::size_t idx = 0; idx < THREAD_NUM_POST; idx++) {
    threadPool[idx].join();
  }

  // suppress by socre
  const auto num_det_boxes3d =
    std::count_if(boxes3d.begin(), boxes3d.end(), is_score_greater(config.score_threshold_));
  if (num_det_boxes3d == 0) {
    // construct boxes3d failed
    throw std::runtime_error("lidar_centerpoint_tvm: construct boxes3d failed");
  }
  std::vector<Box3D> det_boxes3d_nonms(num_det_boxes3d);
  std::copy_if(
    boxes3d.begin(), boxes3d.end(), det_boxes3d_nonms.begin(),
    is_score_greater(config.score_threshold_));

  // sort by score
  std::sort(det_boxes3d_nonms.begin(), det_boxes3d_nonms.end(), score_greater());

  // supress by NMS
  std::vector<bool8_t> final_keep_mask(num_det_boxes3d);
  const auto num_final_det_boxes3d =
    circleNMS(det_boxes3d_nonms, config.circle_nms_dist_threshold_, final_keep_mask);

  det_boxes3d.resize(num_final_det_boxes3d);
  std::size_t boxid = 0;
  for (std::size_t idx = 0; idx < final_keep_mask.size(); idx++) {
    if (final_keep_mask[idx]) {
      det_boxes3d[boxid] = det_boxes3d_nonms[idx];
      boxid++;
    }
  }
  // std::copy_if(det_boxes3d_nonms.begin(), det_boxes3d_nonms.end(), final_keep_mask.begin(),
  //   det_boxes3d.begin(), is_kept());
}

}  // namespace lidar_centerpoint_tvm
}  // namespace perception
}  // namespace autoware