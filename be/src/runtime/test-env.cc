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

#include "runtime/test-env.h"
#include "util/disk-info.h"
#include "util/impalad-metrics.h"

#include "gutil/strings/substitute.h"

#include <memory>

#include "common/names.h"

using boost::scoped_ptr;

namespace impala {

scoped_ptr<MetricGroup> TestEnv::static_metrics_;

TestEnv::TestEnv() {
  if (static_metrics_ == NULL) {
    static_metrics_.reset(new MetricGroup("test-env-static-metrics"));
    ImpaladMetrics::CreateMetrics(static_metrics_.get());
  }
  exec_env_.reset(new ExecEnv);
  exec_env_->InitForFeTests();
  io_mgr_tracker_.reset(new MemTracker(-1));
  exec_env_->disk_io_mgr()->Init(io_mgr_tracker_.get());
  InitMetrics();
  tmp_file_mgr_.reset(new TmpFileMgr);
  tmp_file_mgr_->Init(metrics_.get());
}

void TestEnv::InitMetrics() {
  metrics_.reset(new MetricGroup("test-env-metrics"));
}

void TestEnv::InitTmpFileMgr(const vector<string>& tmp_dirs, bool one_dir_per_device) {
  // Need to recreate metrics to avoid error when registering metric twice.
  InitMetrics();
  tmp_file_mgr_.reset(new TmpFileMgr);
  tmp_file_mgr_->InitCustom(tmp_dirs, one_dir_per_device, metrics_.get());
}

TestEnv::~TestEnv() {
  // Queries must be torn down first since they are dependent on global state.
  TearDownRuntimeStates();
  exec_env_.reset();
  io_mgr_tracker_.reset();
  tmp_file_mgr_.reset();
  metrics_.reset();
}

void TestEnv::TearDownRuntimeStates() {
  for (auto& runtime_state : runtime_states_) runtime_state.second->ReleaseResources();
  runtime_states_.clear();
}

int64_t TestEnv::CalculateMemLimit(int max_buffers, int block_size) {
  DCHECK_GE(max_buffers, -1);
  if (max_buffers == -1) return -1;
  return max_buffers * static_cast<int64_t>(block_size);
}

int64_t TestEnv::TotalQueryMemoryConsumption() {
  int64_t total = 0;
  for (const auto& runtime_state : runtime_states_) {
    total += runtime_state.second->query_mem_tracker()->consumption();
  }
  return total;
}

Status TestEnv::CreateQueryState(int64_t query_id, int max_buffers, int block_size,
    const TQueryOptions* query_options, RuntimeState** runtime_state) {
  // Enforce invariant that each query ID can be registered at most once.
  if (runtime_states_.find(query_id) != runtime_states_.end()) {
    return Status(Substitute("Duplicate query id found: $0", query_id));
  }

  TQueryCtx query_ctx;
  if (query_options != nullptr) query_ctx.client_request.query_options = *query_options;
  query_ctx.query_id.hi = 0;
  query_ctx.query_id.lo = query_id;
  *runtime_state = new RuntimeState(query_ctx, exec_env_.get());
  (*runtime_state)->InitMemTrackers(nullptr, -1);

  shared_ptr<BufferedBlockMgr> mgr;
  RETURN_IF_ERROR(BufferedBlockMgr::Create(*runtime_state,
      (*runtime_state)->query_mem_tracker(), (*runtime_state)->runtime_profile(),
      tmp_file_mgr_.get(), CalculateMemLimit(max_buffers, block_size), block_size, &mgr));
  (*runtime_state)->set_block_mgr(mgr);

  runtime_states_[query_id] = shared_ptr<RuntimeState>(*runtime_state);
  return Status::OK();
}

}
