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

package org.apache.impala.analysis;

import java.util.Collection;

import org.apache.impala.catalog.HdfsFileFormat;
import org.apache.impala.catalog.HdfsPartition;
import org.apache.impala.catalog.HdfsTable;
import org.apache.impala.catalog.KuduTable;
import org.apache.impala.catalog.RowFormat;
import org.apache.impala.catalog.Table;
import org.apache.impala.common.AnalysisException;
import org.apache.impala.thrift.TAlterTableParams;
import org.apache.impala.thrift.TAlterTableSetRowFormatParams;
import org.apache.impala.thrift.TAlterTableType;

/**
 * Represents an ALTER TABLE [PARTITION partitionSet] SET ROW FORMAT statement.
 */
public class AlterTableSetRowFormatStmt extends AlterTableSetStmt {
  private final RowFormat rowFormat_;

  public AlterTableSetRowFormatStmt(TableName tableName,
      PartitionSet partitionSet, RowFormat rowFormat) {
    super(tableName, partitionSet);
    rowFormat_ = rowFormat;
  }

  public RowFormat getRowFormat() { return rowFormat_; }

  @Override
  public TAlterTableParams toThrift() {
    TAlterTableParams params = super.toThrift();
    params.setAlter_type(TAlterTableType.SET_ROW_FORMAT);
    TAlterTableSetRowFormatParams rowFormatParams =
        new TAlterTableSetRowFormatParams(getRowFormat().toThrift());
    if (getPartitionSet() != null) {
      rowFormatParams.setPartition_set(getPartitionSet().toThrift());
    }
    params.setSet_row_format_params(rowFormatParams);
    return params;
  }

  @Override
  public void analyze(Analyzer analyzer) throws AnalysisException {
    super.analyze(analyzer);
    Table tbl = getTargetTable();
    if (!(tbl instanceof HdfsTable)) {
      throw new AnalysisException(String.format("ALTER TABLE SET ROW FORMAT is only " +
          "supported on HDFS tables. Conflicting table: %1$s", tbl.getFullName()));
    }
    if (partitionSet_ != null) {
      for (HdfsPartition partition: partitionSet_.getPartitions()) {
        if (partition.getFileFormat() != HdfsFileFormat.TEXT &&
            partition.getFileFormat() != HdfsFileFormat.SEQUENCE_FILE) {
          throw new AnalysisException(String.format("ALTER TABLE SET ROW FORMAT is " +
              "only supported on TEXT or SEQUENCE file formats.  " +
              "Conflicting partition/format: %1$s / %2$s", partition.getPartitionName(),
              HdfsFileFormat.fromHdfsInputFormatClass(
                  partition.getFileFormat().inputFormat()).name()));
        }
      }
    } else {
      HdfsFileFormat format = HdfsFileFormat.fromHdfsInputFormatClass(
          ((HdfsTable) tbl).getMetaStoreTable().getSd().getInputFormat());
      if (format != HdfsFileFormat.TEXT &&
          format != HdfsFileFormat.SEQUENCE_FILE) {
        throw new AnalysisException(String.format("ALTER TABLE SET ROW FORMAT is " +
            "only supported on TEXT or SEQUENCE file formats. Conflicting " +
            "table/format: %1$s / %2$s", tbl.getFullName(), format.name()));
      }
    }
  }
}
