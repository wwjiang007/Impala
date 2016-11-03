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

#include "exec/kudu-table-sink.h"

#include <sstream>
#include <thrift/protocol/TDebugProtocol.h>

#include "exec/kudu-util.h"
#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "gen-cpp/ImpalaInternalService_constants.h"
#include "gutil/gscoped_ptr.h"
#include "runtime/mem-tracker.h"
#include "runtime/row-batch.h"
#include "util/runtime-profile-counters.h"

#include "common/names.h"

DEFINE_int32(kudu_session_timeout_seconds, 60, "Timeout set on the Kudu session. "
    "How long to wait before considering a write failed.");
DEFINE_int32(kudu_mutation_buffer_size, 100 * 1024 * 1024, "The size (bytes) of the "
    "Kudu client buffer for mutations.");

using kudu::client::KuduColumnSchema;
using kudu::client::KuduSchema;
using kudu::client::KuduClient;
using kudu::client::KuduRowResult;
using kudu::client::KuduTable;
using kudu::client::KuduInsert;
using kudu::client::KuduUpdate;
using kudu::client::KuduError;

namespace impala {

const static string& ROOT_PARTITION_KEY =
    g_ImpalaInternalService_constants.ROOT_PARTITION_KEY;

// Send 7MB buffers to Kudu, matching a hard-coded size in Kudu (KUDU-1693).
const static int INDIVIDUAL_BUFFER_SIZE = 7 * 1024 * 1024;

KuduTableSink::KuduTableSink(const RowDescriptor& row_desc,
    const vector<TExpr>& select_list_texprs,
    const TDataSink& tsink)
    : DataSink(row_desc),
      table_id_(tsink.table_sink.target_table_id),
      select_list_texprs_(select_list_texprs),
      sink_action_(tsink.table_sink.action),
      kudu_table_sink_(tsink.table_sink.kudu_table_sink),
      kudu_error_counter_(NULL),
      rows_written_(NULL),
      rows_written_rate_(NULL) {
  DCHECK(KuduIsAvailable());
}

Status KuduTableSink::PrepareExprs(RuntimeState* state) {
  // From the thrift expressions create the real exprs.
  RETURN_IF_ERROR(Expr::CreateExprTrees(state->obj_pool(), select_list_texprs_,
                                        &output_expr_ctxs_));
  // Prepare the exprs to run.
  RETURN_IF_ERROR(
      Expr::Prepare(output_expr_ctxs_, state, row_desc_, expr_mem_tracker_.get()));
  return Status::OK();
}

Status KuduTableSink::Prepare(RuntimeState* state, MemTracker* mem_tracker) {
  RETURN_IF_ERROR(DataSink::Prepare(state, mem_tracker));
  SCOPED_TIMER(profile()->total_time_counter());
  RETURN_IF_ERROR(PrepareExprs(state));

  // Get the kudu table descriptor.
  TableDescriptor* table_desc = state->desc_tbl().GetTableDescriptor(table_id_);
  DCHECK(table_desc != NULL);

  // In debug mode try a dynamic cast. If it fails it means that the
  // TableDescriptor is not an instance of KuduTableDescriptor.
  DCHECK(dynamic_cast<const KuduTableDescriptor*>(table_desc))
      << "TableDescriptor must be an instance KuduTableDescriptor.";
  table_desc_ = static_cast<const KuduTableDescriptor*>(table_desc);

  // Add a 'root partition' status in which to collect write statistics
  TInsertPartitionStatus root_status;
  root_status.__set_num_modified_rows(0L);
  root_status.__set_id(-1L);
  state->per_partition_status()->insert(make_pair(ROOT_PARTITION_KEY, root_status));

  // Add counters
  kudu_error_counter_ = ADD_COUNTER(profile(), "TotalKuduFlushErrors", TUnit::UNIT);
  rows_written_ = ADD_COUNTER(profile(), "RowsWritten", TUnit::UNIT);
  kudu_apply_timer_ = ADD_TIMER(profile(), "KuduApplyTimer");
  rows_written_rate_ = profile()->AddDerivedCounter(
      "RowsWrittenRate", TUnit::UNIT_PER_SECOND,
      bind<int64_t>(&RuntimeProfile::UnitsPerSecond, rows_written_,
      profile()->total_time_counter()));

  return Status::OK();
}

Status KuduTableSink::Open(RuntimeState* state) {
  RETURN_IF_ERROR(Expr::Open(output_expr_ctxs_, state));

  kudu::client::KuduClientBuilder b;
  for (const string& address: table_desc_->kudu_master_addresses()) {
    b.add_master_server_addr(address);
  }

  KUDU_RETURN_IF_ERROR(b.Build(&client_), "Unable to create Kudu client");

  KUDU_RETURN_IF_ERROR(client_->OpenTable(table_desc_->table_name(), &table_),
      "Unable to open Kudu table");

  session_ = client_->NewSession();
  session_->SetTimeoutMillis(FLAGS_kudu_session_timeout_seconds * 1000);

  // KuduSession Set* methods here and below return a status for API compatibility.
  // As long as the Kudu client is statically linked, these shouldn't fail and thus these
  // calls could also DCHECK status is OK for debug builds (while still returning errors
  // for release).
  KUDU_RETURN_IF_ERROR(session_->SetFlushMode(
      kudu::client::KuduSession::AUTO_FLUSH_BACKGROUND), "Unable to set flush mode");

  const int32_t buf_size = FLAGS_kudu_mutation_buffer_size;
  if (buf_size < 1024 * 1024) {
    return Status(strings::Substitute(
        "Invalid kudu_mutation_buffer_size: '$0'. Must be greater than 1MB.", buf_size));
  }
  KUDU_RETURN_IF_ERROR(session_->SetMutationBufferSpace(buf_size),
      "Couldn't set mutation buffer size");

  // Configure client memory used for buffering.
  // Internally, the Kudu client keeps one or more buffers for writing operations. When a
  // single buffer is flushed, it is locked (that space cannot be reused) until all
  // operations within it complete, so it is important to have a number of buffers. In
  // our testing, we found that allowing a total of 100MB of buffer space to provide good
  // results; this is the default.  Then, because of some existing 8MB limits in Kudu, we
  // want to have that total space broken up into 7MB buffers (INDIVIDUAL_BUFFER_SIZE).
  // The mutation flush watermark is set to flush every INDIVIDUAL_BUFFER_SIZE.
  int num_buffers = FLAGS_kudu_mutation_buffer_size / INDIVIDUAL_BUFFER_SIZE;
  if (num_buffers == 0) num_buffers = 1;
  KUDU_RETURN_IF_ERROR(session_->SetMutationBufferFlushWatermark(1.0 / num_buffers),
      "Couldn't set mutation buffer watermark");

  // No limit on the buffer count since the settings above imply a max number of buffers.
  // Note that the Kudu client API has a few too many knobs for configuring the size and
  // number of these buffers; there are a few ways to accomplish similar behaviors.
  KUDU_RETURN_IF_ERROR(session_->SetMutationBufferMaxNum(0),
      "Couldn't set mutation buffer count");
  return Status::OK();
}

kudu::client::KuduWriteOperation* KuduTableSink::NewWriteOp() {
  if (sink_action_ == TSinkAction::INSERT) {
    return table_->NewInsert();
  } else if (sink_action_ == TSinkAction::UPDATE) {
    return table_->NewUpdate();
  } else {
    DCHECK(sink_action_ == TSinkAction::DELETE) << "Sink type not supported: "
        << sink_action_;
    return table_->NewDelete();
  }
}

Status KuduTableSink::Send(RuntimeState* state, RowBatch* batch) {
  SCOPED_TIMER(profile()->total_time_counter());
  ExprContext::FreeLocalAllocations(output_expr_ctxs_);
  RETURN_IF_ERROR(state->CheckQueryState());

  // Collect all write operations and apply them together so the time in Apply() can be
  // easily timed.
  vector<unique_ptr<kudu::client::KuduWriteOperation>> write_ops;

  int rows_added = 0;
  // Since everything is set up just forward everything to the writer.
  for (int i = 0; i < batch->num_rows(); ++i) {
    TupleRow* current_row = batch->GetRow(i);
    unique_ptr<kudu::client::KuduWriteOperation> write(NewWriteOp());

    for (int j = 0; j < output_expr_ctxs_.size(); ++j) {
      int col = kudu_table_sink_.referenced_columns.empty() ?
          j : kudu_table_sink_.referenced_columns[j];

      void* value = output_expr_ctxs_[j]->GetValue(current_row);

      // If the value is NULL and no explicit column references are provided, the column
      // should be ignored, else it's explicitly set to NULL.
      if (value == NULL) {
        if (!kudu_table_sink_.referenced_columns.empty()) {
          KUDU_RETURN_IF_ERROR(write->mutable_row()->SetNull(col),
              "Could not add Kudu WriteOp.");
        }
        continue;
      }

      PrimitiveType type = output_expr_ctxs_[j]->root()->type().type;
      switch (type) {
        case TYPE_VARCHAR:
        case TYPE_STRING: {
          StringValue* sv = reinterpret_cast<StringValue*>(value);
          kudu::Slice slice(reinterpret_cast<uint8_t*>(sv->ptr), sv->len);
          KUDU_RETURN_IF_ERROR(write->mutable_row()->SetString(col, slice),
              "Could not add Kudu WriteOp.");
          break;
        }
        case TYPE_FLOAT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetFloat(col, *reinterpret_cast<float*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_DOUBLE:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetDouble(col, *reinterpret_cast<double*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_BOOLEAN:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetBool(col, *reinterpret_cast<bool*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_TINYINT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt8(col, *reinterpret_cast<int8_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_SMALLINT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt16(col, *reinterpret_cast<int16_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_INT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt32(col, *reinterpret_cast<int32_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_BIGINT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt64(col, *reinterpret_cast<int64_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        default:
          return Status(TErrorCode::IMPALA_KUDU_TYPE_MISSING, TypeToString(type));
      }
    }
    write_ops.push_back(move(write));
  }

  {
    SCOPED_TIMER(kudu_apply_timer_);
    for (auto&& write: write_ops) {
      KUDU_RETURN_IF_ERROR(session_->Apply(write.release()), "Error applying Kudu Op.");
      ++rows_added;
    }
  }

  COUNTER_ADD(rows_written_, rows_added);
  RETURN_IF_ERROR(CheckForErrors(state));
  return Status::OK();
}

Status KuduTableSink::CheckForErrors(RuntimeState* state) {
  if (session_->CountPendingErrors() == 0) return Status::OK();

  vector<KuduError*> errors;
  Status status = Status::OK();

  // Get the pending errors from the Kudu session. If errors overflowed the error buffer
  // we can't be sure all errors can be ignored, so an error status will be reported.
  // TODO: Make sure Kudu handles conflict errors properly if IGNORE is set (KUDU-1563).
  bool error_overflow = false;
  session_->GetPendingErrors(&errors, &error_overflow);
  if (UNLIKELY(error_overflow)) {
    status = Status("Error overflow in Kudu session.");
  }

  // The memory for the errors is manually managed. Iterate over all errors and delete
  // them accordingly.
  for (int i = 0; i < errors.size(); ++i) {
    kudu::Status e = errors[i]->status();
    // If the sink has the option "ignore_not_found_or_duplicate" set, duplicate key or
    // key already present errors from Kudu in INSERT, UPDATE, or DELETE operations will
    // be ignored.
    if (!kudu_table_sink_.ignore_not_found_or_duplicate ||
        ((sink_action_ == TSinkAction::DELETE && !e.IsNotFound()) ||
            (sink_action_ == TSinkAction::UPDATE && !e.IsNotFound()) ||
            (sink_action_ == TSinkAction::INSERT && !e.IsAlreadyPresent()))) {
      if (status.ok()) {
        status = Status(strings::Substitute(
            "Kudu error(s) reported, first error: $0", e.ToString()));
      }
    }
    if (e.IsNotFound()) {
      state->LogError(ErrorMsg::Init(TErrorCode::KUDU_KEY_NOT_FOUND,
          table_desc_->table_name()));
    } else if (e.IsAlreadyPresent()) {
      state->LogError(ErrorMsg::Init(TErrorCode::KUDU_KEY_ALREADY_PRESENT,
          table_desc_->table_name()));
    } else {
      state->LogError(ErrorMsg::Init(TErrorCode::KUDU_SESSION_ERROR,
          table_desc_->table_name(), e.ToString()));
    }
    delete errors[i];
  }
  COUNTER_ADD(kudu_error_counter_, errors.size());
  return status;
}

Status KuduTableSink::FlushFinal(RuntimeState* state) {
  kudu::Status flush_status = session_->Flush();

  // Flush() may return an error status but any errors will also be reported by
  // CheckForErrors(), so it's safe to ignore and always call CheckForErrors.
  if (!flush_status.ok()) {
    VLOG_RPC << "Ignoring Flush() error status: " << flush_status.ToString();
  }
  Status status = CheckForErrors(state);
  (*state->per_partition_status())[ROOT_PARTITION_KEY].__set_num_modified_rows(
      rows_written_->value() - kudu_error_counter_->value());
  (*state->per_partition_status())[ROOT_PARTITION_KEY].__set_kudu_latest_observed_ts(
      client_->GetLatestObservedTimestamp());
  return status;
}

void KuduTableSink::Close(RuntimeState* state) {
  if (closed_) return;
  SCOPED_TIMER(profile()->total_time_counter());
  Expr::Close(output_expr_ctxs_, state);
  DataSink::Close(state);
  closed_ = true;
}

}  // namespace impala
