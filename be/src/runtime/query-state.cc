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


#include "runtime/query-state.h"

#include <boost/thread/locks.hpp>
#include <boost/thread/lock_guard.hpp>

#include "runtime/exec-env.h"
#include "runtime/fragment-instance-state.h"
#include "runtime/mem-tracker.h"
#include "runtime/query-exec-mgr.h"

#include "common/names.h"

using namespace impala;

QueryState::ScopedRef::ScopedRef(const TUniqueId& query_id) {
  DCHECK(ExecEnv::GetInstance()->query_exec_mgr() != nullptr);
  query_state_ = ExecEnv::GetInstance()->query_exec_mgr()->GetQueryState(query_id);
}

QueryState::ScopedRef::~ScopedRef() {
  if (query_state_ == nullptr) return;
  ExecEnv::GetInstance()->query_exec_mgr()->ReleaseQueryState(query_state_);
}

QueryState::QueryState(const TQueryCtx& query_ctx, const std::string& pool)
  : query_ctx_(query_ctx), refcnt_(0), prepared_(false), released_resources_(false) {
  TQueryOptions& query_options = query_ctx_.client_request.query_options;
  // max_errors does not indicate how many errors in total have been recorded, but rather
  // how many are distinct. It is defined as the sum of the number of generic errors and
  // the number of distinct other errors.
  if (query_options.max_errors <= 0) {
    query_options.max_errors = 100;
  }
  if (query_options.batch_size <= 0) {
    query_options.__set_batch_size(DEFAULT_BATCH_SIZE);
  }
  InitMemTrackers(pool);
}

void QueryState::ReleaseResources() {
  // Avoid dangling reference from the parent of 'query_mem_tracker_'.
  query_mem_tracker_->UnregisterFromParent();
  released_resources_ = true;
}

QueryState::~QueryState() {
  DCHECK(released_resources_);
}

Status QueryState::Prepare() {
  lock_guard<SpinLock> l(prepare_lock_);
  if (prepared_) {
    DCHECK(prepare_status_.ok());
    return Status::OK();
  }
  RETURN_IF_ERROR(prepare_status_);

  Status status;
  // Starting a new query creates threads and consumes a non-trivial amount of memory.
  // If we are already starved for memory, fail as early as possible to avoid consuming
  // more resources.
  MemTracker* process_mem_tracker = ExecEnv::GetInstance()->process_mem_tracker();
  if (process_mem_tracker->LimitExceeded()) {
    string msg = Substitute("Query $0 could not start because the backend Impala daemon "
                            "is over its memory limit",
        PrintId(query_id()));
    prepare_status_ = process_mem_tracker->MemLimitExceeded(NULL, msg, 0);
    return prepare_status_;
  }

  // TODO: IMPALA-3748: acquire minimum buffer reservation at this point.

  prepared_ = true;
  return Status::OK();
}

void QueryState::InitMemTrackers(const std::string& pool) {
  int64_t bytes_limit = -1;
  if (query_options().__isset.mem_limit && query_options().mem_limit > 0) {
    bytes_limit = query_options().mem_limit;
    VLOG_QUERY << "Using query memory limit from query options: "
               << PrettyPrinter::Print(bytes_limit, TUnit::BYTES);
  }
  query_mem_tracker_ =
      MemTracker::CreateQueryMemTracker(query_id(), query_options(), pool, &obj_pool_);
}

void QueryState::RegisterFInstance(FragmentInstanceState* fis) {
  VLOG_QUERY << "RegisterFInstance(): instance_id=" << PrintId(fis->instance_id());
  lock_guard<SpinLock> l(fis_map_lock_);
  DCHECK_EQ(fis_map_.count(fis->instance_id()), 0);
  fis_map_.insert(make_pair(fis->instance_id(), fis));
}

FragmentInstanceState* QueryState::GetFInstanceState(const TUniqueId& instance_id) {
  VLOG_FILE << "GetFInstanceState(): instance_id=" << PrintId(instance_id);
  lock_guard<SpinLock> l(fis_map_lock_);
  auto it = fis_map_.find(instance_id);
  return it != fis_map_.end() ? it->second : nullptr;
}
