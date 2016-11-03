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

package org.apache.impala.service;

import java.lang.NumberFormatException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import com.google.common.base.Preconditions;
import com.google.common.base.Strings;

import org.apache.hadoop.hive.metastore.api.FieldSchema;
import org.apache.impala.analysis.ToSqlUtils;
import org.apache.impala.catalog.KuduTable;
import org.apache.impala.catalog.Table;
import org.apache.impala.catalog.TableNotFoundException;
import org.apache.impala.catalog.Type;
import org.apache.impala.common.ImpalaRuntimeException;
import org.apache.impala.thrift.TCreateTableParams;
import org.apache.impala.thrift.TDistributeParam;
import org.apache.impala.util.KuduUtil;
import org.apache.kudu.ColumnSchema.ColumnSchemaBuilder;
import org.apache.kudu.ColumnSchema;
import org.apache.kudu.Schema;
import org.apache.kudu.client.CreateTableOptions;
import org.apache.kudu.client.KuduClient;
import org.apache.kudu.client.PartialRow;
import org.apache.log4j.Logger;

/**
 * This is a helper for the CatalogOpExecutor to provide Kudu related DDL functionality
 * such as creating and dropping tables from Kudu.
 */
public class KuduCatalogOpExecutor {
  public static final Logger LOG = Logger.getLogger(KuduCatalogOpExecutor.class);

  /**
   * Create a table in Kudu with a schema equivalent to the schema stored in 'msTbl'.
   * Throws an exception if 'msTbl' represents an external table or if the table couldn't
   * be created in Kudu.
   */
  static void createManagedTable(org.apache.hadoop.hive.metastore.api.Table msTbl,
      TCreateTableParams params) throws ImpalaRuntimeException {
    Preconditions.checkState(!Table.isExternalTable(msTbl));
    String kuduTableName = msTbl.getParameters().get(KuduTable.KEY_TABLE_NAME);
    String masterHosts = msTbl.getParameters().get(KuduTable.KEY_MASTER_HOSTS);
    LOG.debug(String.format("Creating table '%s' in master '%s'", kuduTableName,
        masterHosts));
    try (KuduClient kudu = new KuduClient.KuduClientBuilder(masterHosts).build()) {
      // TODO: The IF NOT EXISTS case should be handled by Kudu to ensure atomicity.
      // (see KUDU-1710).
      if (kudu.tableExists(kuduTableName)) {
        if (params.if_not_exists) return;
        throw new ImpalaRuntimeException(String.format(
            "Table '%s' already exists in Kudu.", kuduTableName));
      }
      Schema schema = createTableSchema(msTbl, params);
      CreateTableOptions tableOpts = buildTableOptions(msTbl, params, schema);
      kudu.createTable(kuduTableName, schema, tableOpts);
    } catch (Exception e) {
      throw new ImpalaRuntimeException(String.format("Error creating table '%s'",
          kuduTableName), e);
    }
  }

  /**
   * Creates the schema of a new Kudu table.
   */
  private static Schema createTableSchema(
      org.apache.hadoop.hive.metastore.api.Table msTbl, TCreateTableParams params)
      throws ImpalaRuntimeException {
    Set<String> keyColNames = new HashSet<>(params.getPrimary_key_column_names());
    List<FieldSchema> fieldSchemas = msTbl.getSd().getCols();
    List<ColumnSchema> colSchemas = new ArrayList<>(fieldSchemas.size());
    for (FieldSchema fieldSchema : fieldSchemas) {
      Type type = Type.parseColumnType(fieldSchema.getType());
      Preconditions.checkState(type != null);
      org.apache.kudu.Type kuduType = KuduUtil.fromImpalaType(type);
      // Create the actual column and check if the column is a key column
      ColumnSchemaBuilder csb =
          new ColumnSchemaBuilder(fieldSchema.getName(), kuduType);
      boolean isKeyCol = keyColNames.contains(fieldSchema.getName());
      csb.key(isKeyCol);
      csb.nullable(!isKeyCol);
      colSchemas.add(csb.build());
    }
    return new Schema(colSchemas);
  }

  /**
   * Builds the table options of a new Kudu table.
   */
  private static CreateTableOptions buildTableOptions(
      org.apache.hadoop.hive.metastore.api.Table msTbl,
      TCreateTableParams params, Schema schema) throws ImpalaRuntimeException {
    CreateTableOptions tableOpts = new CreateTableOptions();
    // Set the distribution schemes
    List<TDistributeParam> distributeParams = params.getDistribute_by();
    if (distributeParams != null) {
      boolean hasRangePartitioning = false;
      for (TDistributeParam distParam : distributeParams) {
        if (distParam.isSetBy_hash_param()) {
          Preconditions.checkState(!distParam.isSetBy_range_param());
          tableOpts.addHashPartitions(distParam.getBy_hash_param().getColumns(),
              distParam.getBy_hash_param().getNum_buckets());
        } else {
          Preconditions.checkState(distParam.isSetBy_range_param());
          hasRangePartitioning = true;
          tableOpts.setRangePartitionColumns(
              distParam.getBy_range_param().getColumns());
          for (PartialRow partialRow :
              KuduUtil.parseSplits(schema, distParam.getBy_range_param())) {
            tableOpts.addSplitRow(partialRow);
          }
        }
      }
      // If no range-based distribution is specified in a CREATE TABLE statement, Kudu
      // generates one by default that includes all the primary key columns. To prevent
      // this from happening, explicitly set the range partition columns to be
      // an empty list.
      if (!hasRangePartitioning) {
        tableOpts.setRangePartitionColumns(Collections.<String>emptyList());
      }
    }

    // Set the number of table replicas, if specified.
    String replication = msTbl.getParameters().get(KuduTable.KEY_TABLET_REPLICAS);
    if (!Strings.isNullOrEmpty(replication)) {
      try {
        int r = Integer.parseInt(replication);
        Preconditions.checkState(r > 0);
        tableOpts.setNumReplicas(r);
      } catch (NumberFormatException e) {
        throw new ImpalaRuntimeException(String.format("Invalid number of table " +
            "replicas specified: '%s'", replication), e);
      }
    }
    return tableOpts;
  }

  /**
   * Drops the table in Kudu. If the table does not exist and 'ifExists' is false, a
   * TableNotFoundException is thrown. If the table exists and could not be dropped,
   * an ImpalaRuntimeException is thrown.
   */
  static void dropTable(org.apache.hadoop.hive.metastore.api.Table msTbl,
      boolean ifExists) throws ImpalaRuntimeException, TableNotFoundException {
    Preconditions.checkState(!Table.isExternalTable(msTbl));
    String tableName = msTbl.getParameters().get(KuduTable.KEY_TABLE_NAME);
    String masterHosts = msTbl.getParameters().get(KuduTable.KEY_MASTER_HOSTS);
    LOG.debug(String.format("Dropping table '%s' from master '%s'", tableName,
        masterHosts));
    try (KuduClient kudu = new KuduClient.KuduClientBuilder(masterHosts).build()) {
      Preconditions.checkState(!Strings.isNullOrEmpty(tableName));
      // TODO: The IF EXISTS case should be handled by Kudu to ensure atomicity.
      // (see KUDU-1710).
      if (kudu.tableExists(tableName)) {
        kudu.deleteTable(tableName);
      } else if (!ifExists) {
        throw new TableNotFoundException(String.format(
            "Table '%s' does not exist in Kudu master(s) '%s'.", tableName, masterHosts));
      }
    } catch (Exception e) {
      throw new ImpalaRuntimeException(String.format("Error dropping table '%s'",
          tableName), e);
    }
  }

  /**
   * Reads the column definitions from a Kudu table and populates 'msTbl' with
   * an equivalent schema. Throws an exception if any errors are encountered.
   */
  public static void populateColumnsFromKudu(
      org.apache.hadoop.hive.metastore.api.Table msTbl) throws ImpalaRuntimeException {
    org.apache.hadoop.hive.metastore.api.Table msTblCopy = msTbl.deepCopy();
    List<FieldSchema> cols = msTblCopy.getSd().getCols();
    String kuduTableName = msTblCopy.getParameters().get(KuduTable.KEY_TABLE_NAME);
    Preconditions.checkState(!Strings.isNullOrEmpty(kuduTableName));
    String masterHosts = msTblCopy.getParameters().get(KuduTable.KEY_MASTER_HOSTS);
    LOG.debug(String.format("Loading schema of table '%s' from master '%s'",
        kuduTableName, masterHosts));
    try (KuduClient kudu = new KuduClient.KuduClientBuilder(masterHosts).build()) {
      if (!kudu.tableExists(kuduTableName)) {
        throw new ImpalaRuntimeException(String.format("Table does not exist in Kudu: " +
            "'%s'", kuduTableName));
      }
      org.apache.kudu.client.KuduTable kuduTable = kudu.openTable(kuduTableName);
      // Replace the columns in the Metastore table with the columns from the recently
      // accessed Kudu schema.
      cols.clear();
      for (ColumnSchema colSchema : kuduTable.getSchema().getColumns()) {
        Type type = KuduUtil.toImpalaType(colSchema.getType());
        cols.add(new FieldSchema(colSchema.getName(), type.toSql().toLowerCase(), null));
      }
    } catch (Exception e) {
      throw new ImpalaRuntimeException(String.format("Error loading schema of table " +
          "'%s'", kuduTableName), e);
    }
    List<FieldSchema> newCols = msTbl.getSd().getCols();
    newCols.clear();
    newCols.addAll(cols);
  }

  /**
   * Validates the table properties of a Kudu table. It checks that the msTbl represents
   * a Kudu table (indicated by the Kudu storage handler), that the master
   * addresses point to valid Kudu masters, and that the table exists.
   * Throws an ImpalaRuntimeException if this is not the case.
   */
  public static void validateKuduTblExists(
      org.apache.hadoop.hive.metastore.api.Table msTbl) throws ImpalaRuntimeException {
    Map<String, String> properties = msTbl.getParameters();
    if (!KuduTable.isKuduTable(msTbl)) {
      throw new ImpalaRuntimeException(String.format("Table '%s' does not represent a " +
          "Kudu table. Expected storage_handler '%s' but found '%s'",
          msTbl.getTableName(), KuduTable.KUDU_STORAGE_HANDLER,
          properties.get(KuduTable.KEY_STORAGE_HANDLER)));
    }

    String masterHosts = properties.get(KuduTable.KEY_MASTER_HOSTS);
    Preconditions.checkState(!Strings.isNullOrEmpty(masterHosts));
    String kuduTableName = properties.get(KuduTable.KEY_TABLE_NAME);
    Preconditions.checkState(!Strings.isNullOrEmpty(kuduTableName));
    try (KuduClient kudu = new KuduClient.KuduClientBuilder(masterHosts).build()) {
      kudu.tableExists(kuduTableName);
    } catch (Exception e) {
      throw new ImpalaRuntimeException(String.format("Kudu table '%s' does not exist " +
          "on master '%s'", kuduTableName, masterHosts), e);
    }
  }
}
