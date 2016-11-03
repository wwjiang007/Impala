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

package org.apache.impala.catalog;

import java.util.ArrayList;
import java.util.List;
import java.util.Set;

import javax.xml.bind.DatatypeConverter;

import org.apache.impala.analysis.ColumnDef;
import org.apache.impala.analysis.DistributeParam;
import org.apache.impala.analysis.ToSqlUtils;
import org.apache.impala.common.ImpalaRuntimeException;
import org.apache.impala.thrift.TCatalogObjectType;
import org.apache.impala.thrift.TColumn;
import org.apache.impala.thrift.TDistributeByHashParam;
import org.apache.impala.thrift.TDistributeByRangeParam;
import org.apache.impala.thrift.TDistributeParam;
import org.apache.impala.thrift.TKuduTable;
import org.apache.impala.thrift.TResultSet;
import org.apache.impala.thrift.TResultSetMetadata;
import org.apache.impala.thrift.TTable;
import org.apache.impala.thrift.TTableDescriptor;
import org.apache.impala.thrift.TTableType;
import org.apache.impala.util.KuduUtil;
import org.apache.impala.util.TResultRowBuilder;
import org.apache.impala.service.CatalogOpExecutor;

import com.google.common.base.Joiner;
import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;

import org.apache.hadoop.hive.common.StatsSetupConst;
import org.apache.hadoop.hive.metastore.IMetaStoreClient;
import org.apache.hadoop.hive.metastore.api.FieldSchema;
import org.apache.hadoop.hive.metastore.api.hive_metastoreConstants;
import org.apache.kudu.ColumnSchema;
import org.apache.kudu.client.KuduClient;
import org.apache.kudu.client.KuduException;
import org.apache.kudu.client.LocatedTablet;
import org.apache.kudu.client.PartitionSchema.HashBucketSchema;
import org.apache.kudu.client.PartitionSchema.RangeSchema;
import org.apache.kudu.client.PartitionSchema;
import org.apache.log4j.Logger;
import org.apache.thrift.TException;

/**
 * Representation of a Kudu table in the catalog cache.
 */
public class KuduTable extends Table {

  private static final Logger LOG = Logger.getLogger(KuduTable.class);

  // Alias to the string key that identifies the storage handler for Kudu tables.
  public static final String KEY_STORAGE_HANDLER =
      hive_metastoreConstants.META_TABLE_STORAGE;

  // Key to access the table name from the table properties.
  public static final String KEY_TABLE_NAME = "kudu.table_name";

  // Key to access the columns used to build the (composite) key of the table.
  // Deprecated - Used only for error checking.
  public static final String KEY_KEY_COLUMNS = "kudu.key_columns";

  // Key to access the master host from the table properties. Error handling for
  // this string is done in the KuduClient library.
  // TODO: Rename kudu.master_addresses to kudu.master_host will break compatibility
  // with older versions.
  public static final String KEY_MASTER_HOSTS = "kudu.master_addresses";

  // Kudu specific value for the storage handler table property keyed by
  // KEY_STORAGE_HANDLER.
  // TODO: Fix the storage handler name (see IMPALA-4271).
  public static final String KUDU_STORAGE_HANDLER =
      "com.cloudera.kudu.hive.KuduStorageHandler";

  // Key to specify the number of tablet replicas.
  public static final String KEY_TABLET_REPLICAS = "kudu.num_tablet_replicas";

  public static final long KUDU_RPC_TIMEOUT_MS = 50000;

  // Table name in the Kudu storage engine. It may not neccessarily be the same as the
  // table name specified in the CREATE TABLE statement; the latter
  // is stored in Table.name_. Reasons why KuduTable.kuduTableName_ and Table.name_ may
  // differ:
  // 1. For managed tables, 'kuduTableName_' is prefixed with 'impala::<db_name>' to
  // avoid conficts. TODO: Remove this when Kudu supports databases.
  // 2. The user may specify a table name using the 'kudu.table_name' table property.
  private String kuduTableName_;

  // Comma separated list of Kudu master hosts with optional ports.
  private String kuduMasters_;

  // Primary key column names.
  private final List<String> primaryKeyColumnNames_ = Lists.newArrayList();

  // Distribution schemes of this Kudu table. Both range and hash-based distributions are
  // supported.
  private final List<DistributeParam> distributeBy_ = Lists.newArrayList();

  protected KuduTable(org.apache.hadoop.hive.metastore.api.Table msTable,
      Db db, String name, String owner) {
    super(msTable, db, name, owner);
    kuduTableName_ = msTable.getParameters().get(KuduTable.KEY_TABLE_NAME);
    kuduMasters_ = msTable.getParameters().get(KuduTable.KEY_MASTER_HOSTS);
  }

  @Override
  public TCatalogObjectType getCatalogObjectType() { return TCatalogObjectType.TABLE; }

  @Override
  public String getStorageHandlerClassName() { return KUDU_STORAGE_HANDLER; }

  /**
   * Returns the columns in the order they have been created
   */
  @Override
  public ArrayList<Column> getColumnsInHiveOrder() { return getColumns(); }

  public static boolean isKuduTable(org.apache.hadoop.hive.metastore.api.Table msTbl) {
    return KUDU_STORAGE_HANDLER.equals(msTbl.getParameters().get(KEY_STORAGE_HANDLER));
  }

  public String getKuduTableName() { return kuduTableName_; }
  public String getKuduMasterHosts() { return kuduMasters_; }

  public List<String> getPrimaryKeyColumnNames() {
    return ImmutableList.copyOf(primaryKeyColumnNames_);
  }

  public List<DistributeParam> getDistributeBy() {
    return ImmutableList.copyOf(distributeBy_);
  }

  /**
   * Loads the metadata of a Kudu table.
   *
   * Schema and distribution schemes are loaded directly from Kudu whereas column stats
   * are loaded from HMS. The function also updates the table schema in HMS in order to
   * propagate alterations made to the Kudu table to HMS.
   */
  @Override
  public void load(boolean dummy /* not used */, IMetaStoreClient msClient,
      org.apache.hadoop.hive.metastore.api.Table msTbl) throws TableLoadingException {
    msTable_ = msTbl;
    // This is set to 0 for Kudu tables.
    // TODO: Change this to reflect the number of pk columns and modify all the
    // places (e.g. insert stmt) that currently make use of this parameter.
    numClusteringCols_ = 0;
    kuduTableName_ = msTable_.getParameters().get(KuduTable.KEY_TABLE_NAME);
    Preconditions.checkNotNull(kuduTableName_);
    kuduMasters_ = msTable_.getParameters().get(KuduTable.KEY_MASTER_HOSTS);
    Preconditions.checkNotNull(kuduMasters_);
    org.apache.kudu.client.KuduTable kuduTable = null;
    numRows_ = getRowCount(msTable_.getParameters());

    // Connect to Kudu to retrieve table metadata
    try (KuduClient kuduClient = new KuduClient.KuduClientBuilder(
        getKuduMasterHosts()).build()) {
      kuduTable = kuduClient.openTable(kuduTableName_);
    } catch (KuduException e) {
      LOG.error("Error accessing Kudu table " + kuduTableName_);
      throw new TableLoadingException(e.getMessage());
    }
    Preconditions.checkNotNull(kuduTable);

    // Load metadata from Kudu and HMS
    try {
      loadSchema(kuduTable);
      loadDistributeByParams(kuduTable);
      loadAllColumnStats(msClient);
    } catch (ImpalaRuntimeException e) {
      LOG.error("Error loading metadata for Kudu table: " + kuduTableName_);
      throw new TableLoadingException("Error loading metadata for Kudu table " +
          kuduTableName_, e);
    }

    // Update the table schema in HMS.
    try {
      long lastDdlTime = CatalogOpExecutor.calculateDdlTime(msTable_);
      msTable_.putToParameters("transient_lastDdlTime", Long.toString(lastDdlTime));
      msTable_.putToParameters(StatsSetupConst.DO_NOT_UPDATE_STATS,
          StatsSetupConst.TRUE);
      msClient.alter_table(msTable_.getDbName(), msTable_.getTableName(), msTable_);
    } catch (TException e) {
      throw new TableLoadingException(e.getMessage());
    }
  }

  /**
   * Loads the schema from the Kudu table including column definitions and primary key
   * columns. Replaces the columns in the HMS table with the columns from the Kudu table.
   * Throws an ImpalaRuntimeException if Kudu column data types cannot be mapped to
   * Impala data types.
   */
  private void loadSchema(org.apache.kudu.client.KuduTable kuduTable)
      throws ImpalaRuntimeException {
    Preconditions.checkNotNull(kuduTable);
    clearColumns();
    primaryKeyColumnNames_.clear();
    List<FieldSchema> cols = msTable_.getSd().getCols();
    cols.clear();
    int pos = 0;
    for (ColumnSchema colSchema: kuduTable.getSchema().getColumns()) {
      Type type = KuduUtil.toImpalaType(colSchema.getType());
      String colName = colSchema.getName();
      cols.add(new FieldSchema(colName, type.toSql().toLowerCase(), null));
      boolean isKey = colSchema.isKey();
      if (isKey) primaryKeyColumnNames_.add(colName);
      addColumn(new KuduColumn(colName, isKey, !isKey, type, null, pos));
      ++pos;
    }
  }

  private void loadDistributeByParams(org.apache.kudu.client.KuduTable kuduTable) {
    Preconditions.checkNotNull(kuduTable);
    PartitionSchema partitionSchema = kuduTable.getPartitionSchema();
    Preconditions.checkState(!colsByPos_.isEmpty());
    distributeBy_.clear();
    for (HashBucketSchema hashBucketSchema: partitionSchema.getHashBucketSchemas()) {
      List<String> columnNames = Lists.newArrayList();
      for (int colPos: hashBucketSchema.getColumnIds()) {
        columnNames.add(colsByPos_.get(colPos).getName());
      }
      distributeBy_.add(
          DistributeParam.createHashParam(columnNames, hashBucketSchema.getNumBuckets()));
    }
    RangeSchema rangeSchema = partitionSchema.getRangeSchema();
    List<Integer> columnIds = rangeSchema.getColumns();
    if (columnIds.isEmpty()) return;
    List<String> columnNames = Lists.newArrayList();
    for (int colPos: columnIds) columnNames.add(colsByPos_.get(colPos).getName());
    // We don't populate the split values because Kudu's API doesn't currently support
    // retrieving the split values for range partitions.
    // TODO: File a Kudu JIRA.
    distributeBy_.add(DistributeParam.createRangeParam(columnNames, null));
  }

  /**
   * Creates a temporary KuduTable object populated with the specified properties but has
   * an invalid TableId and is not added to the Kudu storage engine or the
   * HMS. This is used for CTAS statements.
   */
  public static KuduTable createCtasTarget(Db db,
      org.apache.hadoop.hive.metastore.api.Table msTbl, List<ColumnDef> columnDefs,
      List<String> primaryKeyColumnNames, List<DistributeParam> distributeParams) {
    KuduTable tmpTable = new KuduTable(msTbl, db, msTbl.getTableName(), msTbl.getOwner());
    int pos = 0;
    for (ColumnDef colDef: columnDefs) {
      tmpTable.addColumn(new Column(colDef.getColName(), colDef.getType(), pos++));
    }
    tmpTable.primaryKeyColumnNames_.addAll(primaryKeyColumnNames);
    tmpTable.distributeBy_.addAll(distributeParams);
    return tmpTable;
  }

  @Override
  public TTable toThrift() {
    TTable table = super.toThrift();
    table.setTable_type(TTableType.KUDU_TABLE);
    table.setKudu_table(getTKuduTable());
    return table;
  }

  @Override
  protected void loadFromThrift(TTable thriftTable) throws TableLoadingException {
    super.loadFromThrift(thriftTable);
    TKuduTable tkudu = thriftTable.getKudu_table();
    kuduTableName_ = tkudu.getTable_name();
    kuduMasters_ = Joiner.on(',').join(tkudu.getMaster_addresses());
    primaryKeyColumnNames_.clear();
    primaryKeyColumnNames_.addAll(tkudu.getKey_columns());
    loadDistributeByParamsFromThrift(tkudu.getDistribute_by());
  }

  private void loadDistributeByParamsFromThrift(List<TDistributeParam> params) {
    distributeBy_.clear();
    for (TDistributeParam param: params) {
      if (param.isSetBy_hash_param()) {
        TDistributeByHashParam hashParam = param.getBy_hash_param();
        distributeBy_.add(DistributeParam.createHashParam(
            hashParam.getColumns(), hashParam.getNum_buckets()));
      } else {
        Preconditions.checkState(param.isSetBy_range_param());
        TDistributeByRangeParam rangeParam = param.getBy_range_param();
        distributeBy_.add(DistributeParam.createRangeParam(rangeParam.getColumns(),
            null));
      }
    }
  }

  @Override
  public TTableDescriptor toThriftDescriptor(int tableId, Set<Long> referencedPartitions) {
    TTableDescriptor desc = new TTableDescriptor(tableId, TTableType.KUDU_TABLE,
        getTColumnDescriptors(), numClusteringCols_, kuduTableName_, db_.getName());
    desc.setKuduTable(getTKuduTable());
    return desc;
  }

  private TKuduTable getTKuduTable() {
    TKuduTable tbl = new TKuduTable();
    tbl.setKey_columns(Preconditions.checkNotNull(primaryKeyColumnNames_));
    tbl.setMaster_addresses(Lists.newArrayList(kuduMasters_.split(",")));
    tbl.setTable_name(kuduTableName_);
    Preconditions.checkNotNull(distributeBy_);
    for (DistributeParam distributeParam: distributeBy_) {
      tbl.addToDistribute_by(distributeParam.toThrift());
    }
    return tbl;
  }

  public boolean isPrimaryKeyColumn(String name) {
    return primaryKeyColumnNames_.contains(name);
  }

  public TResultSet getTableStats() throws ImpalaRuntimeException {
    TResultSet result = new TResultSet();
    TResultSetMetadata resultSchema = new TResultSetMetadata();
    result.setSchema(resultSchema);

    resultSchema.addToColumns(new TColumn("# Rows", Type.INT.toThrift()));
    resultSchema.addToColumns(new TColumn("Start Key", Type.STRING.toThrift()));
    resultSchema.addToColumns(new TColumn("Stop Key", Type.STRING.toThrift()));
    resultSchema.addToColumns(new TColumn("Leader Replica", Type.STRING.toThrift()));
    resultSchema.addToColumns(new TColumn("# Replicas", Type.INT.toThrift()));

    try (KuduClient client = new KuduClient.KuduClientBuilder(
        getKuduMasterHosts()).build()) {
      org.apache.kudu.client.KuduTable kuduTable = client.openTable(kuduTableName_);
      List<LocatedTablet> tablets =
          kuduTable.getTabletsLocations(KUDU_RPC_TIMEOUT_MS);
      for (LocatedTablet tab: tablets) {
        TResultRowBuilder builder = new TResultRowBuilder();
        builder.add("-1");   // The Kudu client API doesn't expose tablet row counts.
        builder.add(DatatypeConverter.printHexBinary(
            tab.getPartition().getPartitionKeyStart()));
        builder.add(DatatypeConverter.printHexBinary(
            tab.getPartition().getPartitionKeyEnd()));
        LocatedTablet.Replica leader = tab.getLeaderReplica();
        if (leader == null) {
          // Leader might be null, if it is not yet available (e.g. during
          // leader election in Kudu)
          builder.add("Leader n/a");
        } else {
          builder.add(leader.getRpcHost() + ":" + leader.getRpcPort().toString());
        }
        builder.add(tab.getReplicas().size());
        result.addToRows(builder.get());
      }

    } catch (Exception e) {
      throw new ImpalaRuntimeException("Could not communicate with Kudu.", e);
    }
    return result;
  }
}
