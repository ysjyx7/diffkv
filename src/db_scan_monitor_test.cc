#include "blob_format.h"
#include "third-party/gtest-1.7.0/fused-src/gtest/gtest.h"
#include "titan_stats.h"
#include "util.h"
#include <iostream>
#include <string>

#include "db_scan_monitor.h"
#include "test_util/testharness.h"

namespace rocksdb {
namespace titandb {

const auto scan_range_monitor_max_size = 10U;
const auto scan_range_test_padding = 10U;

class DBScanMonitorTest : public testing::Test {
public:
  DBScanMonitorTest() {
    scan_range_monitor_.reset(
        new ScanRangeMonitor(scan_range_monitor_max_size));
  };
  std::string BuildKey(uint32_t seed) {
    return std::to_string(seed + scan_range_test_padding);
  }
  void PrepareRangeData() {
    RangeInfo tmp_left;
    RangeInfo tmp_right;
    for (auto i = 0U; i < scan_range_monitor_max_size / 2; i++) {
      tmp_left.assign(BuildKey(i), true);
      tmp_right.assign(BuildKey(i + scan_range_monitor_max_size), false);
      scan_range_monitor_->AddRangeInfo(tmp_left);
      scan_range_monitor_->AddRangeInfo(tmp_right);
    }
  }
  bool GetReady() { return scan_range_monitor_->ready_; }

  uint32_t GetSortedListSize() {
    return scan_range_monitor_->range_sorted_list_.size();
  }

  uint32_t GetRangeOverlapCacheSize() {
    return scan_range_monitor_->range_overlap_cache_.size();
  }

  ScanRangeMonitor::PrevBoundInfo GetRangeOverlapCacheElem(std::uint32_t idx) {
    std::string key = BuildKey(idx);
    return scan_range_monitor_->range_overlap_cache_[key];
  }
  std::uint32_t GetRangeOverlapDegree(std::uint32_t min, std::uint32_t max) {
    std::string minium_key = BuildKey(min);
    std::string maxium_key = BuildKey(max);
    BlobFileMeta tmp_meta(0, 1 << 10, 1 << 10, 0, minium_key, maxium_key);
    return scan_range_monitor_->GetRangeOverlapDegree(&tmp_meta);
  }

private:
  std::unique_ptr<ScanRangeMonitor> scan_range_monitor_;
};

TEST_F(DBScanMonitorTest, AddReadyTest) {
  EXPECT_EQ(false, GetReady());
  PrepareRangeData();
  EXPECT_EQ(true, GetReady());
}

TEST_F(DBScanMonitorTest, AddSizeTest) {
  PrepareRangeData();
  EXPECT_EQ(scan_range_monitor_max_size, GetSortedListSize());
  PrepareRangeData();
  EXPECT_EQ(scan_range_monitor_max_size, GetSortedListSize());
}

TEST_F(DBScanMonitorTest, BuildRangeOverlapCacheTest) {
  PrepareRangeData();
  EXPECT_EQ(scan_range_monitor_max_size, GetRangeOverlapCacheSize());
  EXPECT_EQ(1U, GetRangeOverlapCacheElem(0).left_bound_count);
  EXPECT_EQ(0U, GetRangeOverlapCacheElem(0).right_bound_count);
  EXPECT_EQ(5U, GetRangeOverlapCacheElem(10).left_bound_count);
  EXPECT_EQ(1U, GetRangeOverlapCacheElem(10).right_bound_count);
}
TEST_F(DBScanMonitorTest, GetRangeOverlapDegree) {
  PrepareRangeData();
  EXPECT_EQ(4U, GetRangeOverlapDegree(10, 14));
  EXPECT_EQ(3U, GetRangeOverlapDegree(11, 14));
  EXPECT_EQ(2U, GetRangeOverlapDegree(12, 14));
  EXPECT_EQ(1U, GetRangeOverlapDegree(13, 14));
  EXPECT_EQ(0U, GetRangeOverlapDegree(14, 14));
}
} // namespace titandb
} // namespace rocksdb

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
