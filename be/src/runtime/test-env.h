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

#ifndef IMPALA_RUNTIME_TEST_ENV
#define IMPALA_RUNTIME_TEST_ENV

#include "runtime/disk-io-mgr.h"
#include "runtime/buffered-block-mgr.h"
#include "runtime/exec-env.h"
#include "runtime/mem-tracker.h"
#include "runtime/runtime-state.h"
#include "runtime/query-state.h"

namespace impala {

/// Helper testing class that creates an environment with a buffered-block-mgr similar
/// to the one Impala's runtime is using.
class TestEnv {
 public:
  TestEnv();
  ~TestEnv();

  /// Reinitialize tmp_file_mgr with custom configuration. Only valid to call before
  /// query states have been created.
  void InitTmpFileMgr(const std::vector<std::string>& tmp_dirs, bool one_dir_per_device);

  /// Create a RuntimeState for a query with a new block manager and the given query
  /// options. The RuntimeState is owned by the TestEnv. Returns an error if
  /// CreateQueryState() has been called with the same query ID already.
  Status CreateQueryState(int64_t query_id, int max_buffers, int block_size,
      const TQueryOptions* query_options, RuntimeState** runtime_state);

  /// Destroy all RuntimeStates and block managers created by this TestEnv.
  void TearDownRuntimeStates();

  /// Calculate memory limit accounting for overflow and negative values.
  /// If max_buffers is -1, no memory limit will apply.
  int64_t CalculateMemLimit(int max_buffers, int block_size);

  /// Return total of mem tracker consumption for all queries.
  int64_t TotalQueryMemoryConsumption();

  ExecEnv* exec_env() { return exec_env_.get(); }
  MemTracker* io_mgr_tracker() { return io_mgr_tracker_.get(); }
  MetricGroup* metrics() { return metrics_.get(); }
  TmpFileMgr* tmp_file_mgr() { return tmp_file_mgr_.get(); }

 private:
  /// Recreate global metric groups.
  void InitMetrics();

  /// Global state for test environment.
  static boost::scoped_ptr<MetricGroup> static_metrics_;
  boost::scoped_ptr<ExecEnv> exec_env_;
  boost::scoped_ptr<MemTracker> io_mgr_tracker_;
  boost::scoped_ptr<MetricGroup> metrics_;
  boost::scoped_ptr<TmpFileMgr> tmp_file_mgr_;

  /// Per-query states with associated block managers. Key is the integer query ID passed
  /// to CreatePerQueryState().
  std::unordered_map<int64_t, std::shared_ptr<RuntimeState>> runtime_states_;
};

}

#endif
