// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/mem-tracker.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/lexical_cast.hpp>
#include <gperftools/malloc_extension.h>
#include <gutil/strings/substitute.h>

#include "runtime/bufferpool/reservation-tracker-counters.h"
#include "runtime/exec-env.h"
#include "runtime/runtime-state.h"
#include "util/debug-util.h"
#include "util/mem-info.h"
#include "util/pretty-printer.h"
#include "util/uid-util.h"

#include "common/names.h"

using boost::algorithm::join;
using namespace strings;

namespace impala {

const string MemTracker::COUNTER_NAME = "PeakMemoryUsage";
MemTracker::RequestTrackersMap MemTracker::request_to_mem_trackers_;
MemTracker::PoolTrackersMap MemTracker::pool_to_mem_trackers_;
SpinLock MemTracker::static_mem_trackers_lock_;

AtomicInt64 MemTracker::released_memory_since_gc_;

// Name for request pool MemTrackers. '$0' is replaced with the pool name.
const string REQUEST_POOL_MEM_TRACKER_LABEL_FORMAT = "RequestPool=$0";

MemTracker::MemTracker(
    int64_t byte_limit, const string& label, MemTracker* parent, bool log_usage_if_zero)
  : limit_(byte_limit),
    label_(label),
    parent_(parent),
    consumption_(&local_counter_),
    local_counter_(TUnit::BYTES),
    consumption_metric_(NULL),
    auto_unregister_(false),
    log_usage_if_zero_(log_usage_if_zero),
    num_gcs_metric_(NULL),
    bytes_freed_by_last_gc_metric_(NULL),
    bytes_over_limit_metric_(NULL),
    limit_metric_(NULL) {
  if (parent != NULL) parent_->AddChildTracker(this);
  Init();
}

MemTracker::MemTracker(RuntimeProfile* profile, int64_t byte_limit,
    const std::string& label, MemTracker* parent)
  : limit_(byte_limit),
    label_(label),
    parent_(parent),
    consumption_(profile->AddHighWaterMarkCounter(COUNTER_NAME, TUnit::BYTES)),
    local_counter_(TUnit::BYTES),
    consumption_metric_(NULL),
    auto_unregister_(false),
    log_usage_if_zero_(true),
    num_gcs_metric_(NULL),
    bytes_freed_by_last_gc_metric_(NULL),
    bytes_over_limit_metric_(NULL),
    limit_metric_(NULL) {
  if (parent != NULL) parent_->AddChildTracker(this);
  Init();
}

MemTracker::MemTracker(
    UIntGauge* consumption_metric, int64_t byte_limit, const string& label)
  : limit_(byte_limit),
    label_(label),
    parent_(NULL),
    consumption_(&local_counter_),
    local_counter_(TUnit::BYTES),
    consumption_metric_(consumption_metric),
    auto_unregister_(false),
    log_usage_if_zero_(true),
    num_gcs_metric_(NULL),
    bytes_freed_by_last_gc_metric_(NULL),
    bytes_over_limit_metric_(NULL),
    limit_metric_(NULL) {
  Init();
}

void MemTracker::Init() {
  DCHECK_GE(limit_, -1);
  // populate all_trackers_ and limit_trackers_
  MemTracker* tracker = this;
  while (tracker != NULL) {
    all_trackers_.push_back(tracker);
    if (tracker->has_limit()) limit_trackers_.push_back(tracker);
    tracker = tracker->parent_;
  }
  DCHECK_GT(all_trackers_.size(), 0);
  DCHECK_EQ(all_trackers_[0], this);
}

void MemTracker::AddChildTracker(MemTracker* tracker) {
  lock_guard<mutex> l(child_trackers_lock_);
  tracker->child_tracker_it_ = child_trackers_.insert(child_trackers_.end(), tracker);
}

void MemTracker::UnregisterFromParent() {
  DCHECK(parent_ != NULL);
  lock_guard<mutex> l(parent_->child_trackers_lock_);
  parent_->child_trackers_.erase(child_tracker_it_);
  child_tracker_it_ = parent_->child_trackers_.end();
}

void MemTracker::EnableReservationReporting(const ReservationTrackerCounters& counters) {
  reservation_counters_.reset(new ReservationTrackerCounters);
  *reservation_counters_ = counters;
}

int64_t MemTracker::GetPoolMemReserved() const {
  // Pool trackers should have a pool_name_ and no limit.
  DCHECK(!pool_name_.empty());
  DCHECK_EQ(limit_, -1);

  int64_t mem_reserved = 0L;
  lock_guard<mutex> l(child_trackers_lock_);
  for (list<MemTracker*>::const_iterator it = child_trackers_.begin();
       it != child_trackers_.end(); ++it) {
    int64_t child_limit = (*it)->limit();
    if (child_limit > 0) {
      // Make sure we don't overflow if the query limits are set to ridiculous values.
      mem_reserved += std::min(child_limit, MemInfo::physical_mem());
    } else {
      DCHECK_EQ(child_limit, -1);
      mem_reserved += (*it)->consumption();
    }
  }
  return mem_reserved;
}

MemTracker* MemTracker::GetRequestPoolMemTracker(const string& pool_name,
    MemTracker* parent) {
  DCHECK(!pool_name.empty());
  lock_guard<SpinLock> l(static_mem_trackers_lock_);
  PoolTrackersMap::iterator it = pool_to_mem_trackers_.find(pool_name);
  if (it != pool_to_mem_trackers_.end()) {
    MemTracker* tracker = it->second;
    DCHECK(pool_name == tracker->pool_name_);
    return tracker;
  } else {
    if (parent == NULL) return NULL;
    // First time this pool_name registered, make a new object.
    MemTracker* tracker = new MemTracker(
        -1, Substitute(REQUEST_POOL_MEM_TRACKER_LABEL_FORMAT, pool_name), parent);
    tracker->auto_unregister_ = true;
    tracker->pool_name_ = pool_name;
    pool_to_mem_trackers_[pool_name] = tracker;
    return tracker;
  }
}

shared_ptr<MemTracker> MemTracker::GetQueryMemTracker(
    const TUniqueId& id, int64_t byte_limit, MemTracker* parent) {
  if (byte_limit != -1) {
    if (byte_limit > MemInfo::physical_mem()) {
      LOG(WARNING) << "Memory limit "
                   << PrettyPrinter::Print(byte_limit, TUnit::BYTES)
                   << " exceeds physical memory of "
                   << PrettyPrinter::Print(MemInfo::physical_mem(), TUnit::BYTES);
    }
    VLOG_QUERY << "Using query memory limit: "
               << PrettyPrinter::Print(byte_limit, TUnit::BYTES);
  }

  lock_guard<SpinLock> l(static_mem_trackers_lock_);
  RequestTrackersMap::iterator it = request_to_mem_trackers_.find(id);
  if (it != request_to_mem_trackers_.end()) {
    // Return the existing MemTracker object for this id, converting the weak ptr
    // to a shared ptr.
    shared_ptr<MemTracker> tracker = it->second.lock();
    DCHECK_EQ(tracker->limit_, byte_limit);
    DCHECK(id == tracker->query_id_);
    DCHECK(parent == tracker->parent_);
    return tracker;
  } else {
    // First time this id registered, make a new object.  Give a shared ptr to
    // the caller and put a weak ptr in the map.
    shared_ptr<MemTracker> tracker = make_shared<MemTracker>(
        byte_limit, Substitute("Query($0)", lexical_cast<string>(id)), parent);
    tracker->auto_unregister_ = true;
    tracker->query_id_ = id;
    request_to_mem_trackers_[id] = tracker;
    return tracker;
  }
}

MemTracker::~MemTracker() {
  DCHECK_EQ(consumption_->current_value(), 0) << label_ << "\n" << GetStackTrace();
  lock_guard<SpinLock> l(static_mem_trackers_lock_);
  if (auto_unregister_) UnregisterFromParent();
  // Erase the weak ptr reference from the map.
  request_to_mem_trackers_.erase(query_id_);
  // Per-pool trackers should live the entire lifetime of the impalad process, but
  // remove the element from the map in case this changes in the future.
  pool_to_mem_trackers_.erase(pool_name_);
}

void MemTracker::RegisterMetrics(MetricGroup* metrics, const string& prefix) {
  num_gcs_metric_ = metrics->AddCounter<int64_t>(Substitute("$0.num-gcs", prefix), 0);

  // TODO: Consider a total amount of bytes freed counter
  bytes_freed_by_last_gc_metric_ = metrics->AddGauge<int64_t>(
      Substitute("$0.bytes-freed-by-last-gc", prefix), -1);

  bytes_over_limit_metric_ = metrics->AddGauge<int64_t>(
      Substitute("$0.bytes-over-limit", prefix), -1);

  limit_metric_ = metrics->AddGauge<int64_t>(Substitute("$0.limit", prefix), limit_);
}

void MemTracker::RefreshConsumptionFromMetric() {
  DCHECK(consumption_metric_ != NULL);
  DCHECK(parent_ == NULL);
  consumption_->Set(consumption_metric_->value());
}

// Calling this on the query tracker results in output like:
//
//  Query(4a4c81fedaed337d:4acadfda00000000) Limit=10.00 GB Total=508.28 MB Peak=508.45 MB
//    Fragment 4a4c81fedaed337d:4acadfda00000000: Total=8.00 KB Peak=8.00 KB
//      EXCHANGE_NODE (id=4): Total=0 Peak=0
//      DataStreamRecvr: Total=0 Peak=0
//    Block Manager: Limit=6.68 GB Total=394.00 MB Peak=394.00 MB
//    Fragment 4a4c81fedaed337d:4acadfda00000006: Total=233.72 MB Peak=242.24 MB
//      AGGREGATION_NODE (id=1): Total=139.21 MB Peak=139.84 MB
//      HDFS_SCAN_NODE (id=0): Total=93.94 MB Peak=102.24 MB
//      DataStreamSender (dst_id=2): Total=45.99 KB Peak=85.99 KB
//    Fragment 4a4c81fedaed337d:4acadfda00000003: Total=274.55 MB Peak=274.62 MB
//      AGGREGATION_NODE (id=3): Total=274.50 MB Peak=274.50 MB
//      EXCHANGE_NODE (id=2): Total=0 Peak=0
//      DataStreamRecvr: Total=45.91 KB Peak=684.07 KB
//      DataStreamSender (dst_id=4): Total=680.00 B Peak=680.00 B
//
// If 'reservation_metrics_' are set, we ge a more granular breakdown:
//   TrackerName: Limit=5.00 MB BufferPoolUsed/Reservation=0/5.00 MB OtherMemory=1.04 MB
//                Total=6.04 MB Peak=6.45 MB
//
string MemTracker::LogUsage(const string& prefix) const {
  if (!log_usage_if_zero_ && consumption() == 0) return "";

  stringstream ss;
  ss << prefix << label_ << ":";
  if (CheckLimitExceeded()) ss << " memory limit exceeded.";
  if (limit_ > 0) ss << " Limit=" << PrettyPrinter::Print(limit_, TUnit::BYTES);

  int64_t total = consumption();
  int64_t peak = consumption_->value();
  if (reservation_counters_ != NULL) {
    int64_t reservation = reservation_counters_->peak_reservation->current_value();
    int64_t used_reservation =
        reservation_counters_->peak_used_reservation->current_value();
    int64_t reservation_limit = reservation_counters_->reservation_limit->value();
    ss << " BufferPoolUsed/Reservation="
       << PrettyPrinter::Print(used_reservation, TUnit::BYTES) << "/"
       << PrettyPrinter::Print(reservation, TUnit::BYTES);
    if (reservation_limit != numeric_limits<int64_t>::max()) {
      ss << " BufferPoolLimit=" << PrettyPrinter::Print(reservation_limit, TUnit::BYTES);
    }
    ss << " OtherMemory=" << PrettyPrinter::Print(total - reservation, TUnit::BYTES);
  }
  ss << " Total=" << PrettyPrinter::Print(total, TUnit::BYTES)
     << " Peak=" << PrettyPrinter::Print(peak, TUnit::BYTES);

  stringstream prefix_ss;
  prefix_ss << prefix << "  ";
  string new_prefix = prefix_ss.str();
  lock_guard<mutex> l(child_trackers_lock_);
  string child_trackers_usage = LogUsage(new_prefix, child_trackers_);
  if (!child_trackers_usage.empty()) ss << "\n" << child_trackers_usage;
  return ss.str();
}

string MemTracker::LogUsage(const string& prefix, const list<MemTracker*>& trackers) {
  vector<string> usage_strings;
  for (list<MemTracker*>::const_iterator it = trackers.begin();
      it != trackers.end(); ++it) {
    string usage_string = (*it)->LogUsage(prefix);
    if (!usage_string.empty()) usage_strings.push_back(usage_string);
  }
  return join(usage_strings, "\n");
}

Status MemTracker::MemLimitExceeded(RuntimeState* state, const std::string& details,
    int64_t failed_allocation_size) {
  Status status = Status::MemLimitExceeded();
  status.AddDetail(details);
  if (state != NULL) state->LogMemLimitExceeded(this, failed_allocation_size);
  return status;
}

bool MemTracker::GcMemory(int64_t max_consumption) {
  if (max_consumption < 0) return true;
  lock_guard<mutex> l(gc_lock_);
  if (consumption_metric_ != NULL) consumption_->Set(consumption_metric_->value());
  uint64_t pre_gc_consumption = consumption();
  // Check if someone gc'd before us
  if (pre_gc_consumption < max_consumption) return false;
  if (num_gcs_metric_ != NULL) num_gcs_metric_->Increment(1);

  // Try to free up some memory
  for (int i = 0; i < gc_functions_.size(); ++i) {
    gc_functions_[i]();
    if (consumption_metric_ != NULL) RefreshConsumptionFromMetric();
    if (consumption() <= max_consumption) break;
  }

  if (bytes_freed_by_last_gc_metric_ != NULL) {
    bytes_freed_by_last_gc_metric_->set_value(pre_gc_consumption - consumption());
  }
  return consumption() > max_consumption;
}

void MemTracker::GcTcmalloc() {
#ifndef ADDRESS_SANITIZER
  released_memory_since_gc_.Store(0);
  MallocExtension::instance()->ReleaseFreeMemory();
#else
  // Nothing to do if not using tcmalloc.
#endif
}

}
