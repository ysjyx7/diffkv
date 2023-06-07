// Copyright (c) 2011 The LevelDB Authors. All rights reserved.

// Use of this source code is governed by a BSD-style license that can be

// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_HISTOGRAM_H_
#define STORAGE_LEVELDB_UTIL_HISTOGRAM_H_

#include <algorithm>
#include <cmath>
#include <iostream>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string>

namespace utils {
enum RecordUnit {
  h_nanoseconds = static_cast<int>(1e9),
  h_microseconds = static_cast<int>(1e6),
  h_milliseconds = static_cast<int>(1e3),
  h_seconds = static_cast<int>(1)
};

class Histogram {
public:
  Histogram(RecordUnit record_unit = RecordUnit::h_milliseconds) {
    record_unit_ = record_unit;
    switch (record_unit_) {
    case RecordUnit::h_seconds:
      snprintf(record_unit_name_, sizeof(record_unit_name_), "seconds");
      break;
    case RecordUnit::h_milliseconds:
      snprintf(record_unit_name_, sizeof(record_unit_name_), "milliseconds");
      break;
    case RecordUnit::h_microseconds:
      snprintf(record_unit_name_, sizeof(record_unit_name_), "microseconds");
      break;
    case RecordUnit::h_nanoseconds:
      snprintf(record_unit_name_, sizeof(record_unit_name_), "nanoseconds");
      break;
    default:
      snprintf(record_unit_name_, sizeof(record_unit_name_), "undefined");
    }
    this->Clear();
  }
  ~Histogram() { this->Clear(); }

  void Clear();
  void ClearInterval();
  void Add(double value);
  void Add_Fast(double value);
  void Merge(const Histogram &other);
  std::string ToString() const;
  std::string ToIntervalString() const;

public:
  double Median() const;
  double Sum() const { return sum_; }
  double Average() const {
    if (num_ == 0.0)
      return 0;
    return sum_ / num_;
  }
  double IntervalAverage() const {
    if (interval_num_ == 0.0)
      return 0;
    return interval_sum_ / interval_num_;
  }
  double Percentile(double p) const;
  double IntervalNum() const { return interval_num_; }
  double IntervalSum() const { return interval_sum_; }
  double IntervalPercentile(double p) const;
  double StandardDeviation() const;
  int GetRecordUnit() const;
  const char *GetRecordUnitName() const;

public:
  enum { kNumBuckets = 154 };
  static const double kBucketLimit[kNumBuckets];
  RecordUnit record_unit_;
  char record_unit_name_[20];
  double min_;
  double max_;
  double num_;
  long double sum_;
  double sum_squares_;
  std::mutex lock_;
  std::mutex interval_lock_;
  // use to record the interval recodings
  double interval_min_;
  double interval_max_;
  double interval_num_;
  long double interval_sum_;
  double interval_buckets_[kNumBuckets];

  double buckets_[kNumBuckets];
};

} // namespace utils

#endif // STORAGE_LEVELDB_UTIL_HISTOGRAM_H_
