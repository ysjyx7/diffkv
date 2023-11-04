#pragma once

#include "blob_format.h"
#include "util/mutexlock.h"
#include <algorithm>
#include <atomic>
#include <bits/stdint-uintn.h>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace rocksdb {
namespace titandb {

class RangeInfo {
public:
  void assign(const std::string &tmp_bound, bool left) {
    range_bounds.assign(std::move(tmp_bound));
    is_left = left;
  }
  void assign(const RangeInfo &range_info) {
    this->assign(range_info.range_bounds, range_info.is_left);
  }
  const std::string &data() const { return range_bounds; }

  bool is_left_bound() const { return is_left; }
  void operator=(const RangeInfo &other) { this->assign(other); }
  bool operator<(const RangeInfo &other) {
    int is_smaller = range_bounds.compare(other.range_bounds);
    if (is_smaller < 0)
      return true;
    if (is_smaller == 0 && is_left == true)
      return true;
    return false;
  }

private:
  std::string range_bounds;
  bool is_left = false;
};

class ScanRangeMonitor {
public:
  ScanRangeMonitor(std::uint32_t max_size = 1000) : max_size_(max_size) {
    range_buffer_list_.reserve(max_size_);
    range_sorted_list_.reserve(max_size_);
  }
  void AddRangeInfo(const RangeInfo &range_info) {
    MutexLock l(&mutex_);
    range_buffer_list_.push_back(range_info);
    current_count_ += 1;
    if (current_count_ < max_size_) {
      return;
    }
    if (ready_ == false) {
      ready_ = true;
    }
    current_count_ = 0;
    std::sort(range_buffer_list_.begin(), range_buffer_list_.end());
    range_buffer_list_.swap(range_sorted_list_);
    BuildRangeOverlapCache();
    range_buffer_list_.clear();
  }

  std::uint32_t GetRangeOverlapDegree(const BlobFileMeta *file_range) {
    if (!ready_) {
      return 0;
    }
    MutexLock l(&mutex_);
    if (file_range->largest_key() < range_overlap_cache_.begin()->first) {
      return 0;
    }
    uint32_t left_bound_count =
        range_overlap_cache_.lower_bound(file_range->largest_key())
            ->second.left_bound_count;
    if (file_range->smallest_key() < range_overlap_cache_.begin()->first) {
      return left_bound_count;
    }
    uint32_t right_bound_count =
        range_overlap_cache_.lower_bound(file_range->smallest_key())
            ->second.right_bound_count;
    return left_bound_count - right_bound_count;
  }

private:
  struct PrevBoundInfo {
    std::uint32_t left_bound_count;
    std::uint32_t right_bound_count;
  };

  void BuildRangeOverlapCache() {
    PrevBoundInfo prev_bound_info = {.left_bound_count = 0,
                                     .right_bound_count = 0};
    for (auto &i : range_sorted_list_) {
      if (i.is_left_bound()) {
        prev_bound_info.left_bound_count += 1;
      } else {
        prev_bound_info.right_bound_count += 1;
      }
      if (range_overlap_cache_.find(i.data()) == range_overlap_cache_.end()) {
        range_overlap_cache_.insert({i.data(), prev_bound_info});
      } else {
        range_overlap_cache_[i.data()].right_bound_count =
            prev_bound_info.right_bound_count;
      }
    }
  }

  friend class DBScanMonitorTest;

  std::uint32_t max_size_ = 0;
  std::uint32_t current_count_ = 0;
  bool ready_ = false;
  std::vector<RangeInfo> range_buffer_list_;
  std::vector<RangeInfo> range_sorted_list_;
  std::map<std::string, PrevBoundInfo> range_overlap_cache_;
  port::Mutex mutex_;
};
} // namespace titandb
} // namespace rocksdb