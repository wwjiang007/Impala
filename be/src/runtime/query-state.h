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


#ifndef IMPALA_RUNTIME_QUERY_STATE_H
#define IMPALA_RUNTIME_QUERY_STATE_H

#include <boost/thread/mutex.hpp>
#include <unordered_map>

#include "common/object-pool.h"
#include "gen-cpp/ImpalaInternalService_types.h"
#include "gen-cpp/Types_types.h"
#include "util/uid-util.h"
#include "common/atomic.h"

namespace impala {

class FragmentInstanceState;

/// Central class for all backend execution state (example: the FragmentInstanceStates
/// of the individual fragment instances) created for a particular query.
/// This class contains or makes accessible state that is shared across fragment
/// instances; in contrast, fragment instance-specific state is collected in
/// FragmentInstanceState.
///
/// The lifetime of an instance of this class is dictated by a reference count.
/// Any thread that executes on behalf of a query, and accesses any of its state,
/// must obtain a reference to the corresponding QueryState and hold it for at least the
/// duration of that access. The reference is obtained and released via
/// QueryExecMgr::Get-/ReleaseQueryState() or via QueryState::ScopedRef (the latter
/// for references limited to the scope of a single function or block).
/// As long as the reference count is greater than 0, all query state (contained
/// either in this class or accessible through this class, such as the
/// FragmentInstanceStates) is guaranteed to be alive.
///
/// Thread-safe, unless noted otherwise.
class QueryState {
 public:
  /// Use this class to obtain a QueryState for the duration of a function/block,
  /// rather than manually via QueryExecMgr::Get-/ReleaseQueryState().
  /// Pattern:
  /// {
  ///   QueryState::ScopedRef qs(qid);
  ///   if (qs->query_state() == nullptr) <do something, such as return>
  ///   ...
  /// }
  class ScopedRef {
   public:
    ScopedRef(const TUniqueId& query_id);
    ~ScopedRef();

    /// may return nullptr
    QueryState* get() const { return query_state_; }
    QueryState* operator->() const { return query_state_; }

   private:
    QueryState* query_state_;
    DISALLOW_COPY_AND_ASSIGN(ScopedRef);
  };

  /// a shared pool for all objects that have query lifetime
  ObjectPool* obj_pool() { return &obj_pool_; }

  /// This TQueryCtx was copied from the first fragment instance which led to the
  /// creation of this QueryState. For all subsequently arriving fragment instances the
  /// desc_tbl in this context will be incorrect, therefore query_ctx().desc_tbl should
  /// not be used. This restriction will go away with the switch to a per-query exec
  /// rpc.
  const TQueryCtx& query_ctx() const { return query_ctx_; }

  const TUniqueId& query_id() const { return query_ctx_.query_id; }

  /// Registers a new FInstanceState.
  void RegisterFInstance(FragmentInstanceState* fis);

  /// Returns the instance state or nullptr if the instance id has not previously
  /// been registered. The returned FIS is valid for the duration of the QueryState.
  FragmentInstanceState* GetFInstanceState(const TUniqueId& instance_id);

 private:
  friend class QueryExecMgr;
  friend class TestEnv;

  static const int DEFAULT_BATCH_SIZE = 1024;

  TQueryCtx query_ctx_;

  ObjectPool obj_pool_;
  AtomicInt32 refcnt_;

  boost::mutex fis_map_lock_;  // protects fis_map_

  /// map from instance id to its state (owned by obj_pool_)
  std::unordered_map<TUniqueId, FragmentInstanceState*> fis_map_;

  /// Create QueryState w/ copy of query_ctx and refcnt of 0.
  QueryState(const TQueryCtx& query_ctx);
};

}

#endif
