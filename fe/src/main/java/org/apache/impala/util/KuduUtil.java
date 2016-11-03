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

package org.apache.impala.util;

import java.io.StringReader;
import java.util.HashSet;
import java.util.List;
import javax.json.Json;
import javax.json.JsonArray;
import javax.json.JsonReader;

import org.apache.impala.catalog.Catalog;
import org.apache.impala.catalog.ScalarType;
import org.apache.impala.catalog.Type;
import org.apache.impala.common.ImpalaRuntimeException;
import org.apache.impala.thrift.TDistributeByRangeParam;
import org.apache.impala.thrift.TRangeLiteral;
import org.apache.impala.thrift.TRangeLiteralList;
import com.google.common.base.Splitter;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;
import com.google.common.collect.Sets;
import org.apache.kudu.ColumnSchema;
import org.apache.kudu.Schema;
import org.apache.kudu.client.PartialRow;

import static java.lang.String.format;

public class KuduUtil {

  private static final String SPLIT_KEYS_ERROR_MESSAGE = "Error parsing splits keys.";
  private static final String KUDU_TABLE_NAME_PREFIX = "impala::";

  /**
   * Parses split keys from statements.
   *
   * Split keys are expected to be in json, as an array of arrays, in the form:
   * '[[value1_col1, value1_col2, ...], [value2_col1, value2_col2, ...], ...]'
   *
   * Each inner array corresponds to a split key and should have one matching entry for
   * each key column specified in 'schema'.
   */
  public static List<PartialRow> parseSplits(Schema schema, String kuduSplits)
      throws ImpalaRuntimeException {

    // If there are no splits return early.
    if (kuduSplits == null || kuduSplits.isEmpty()) return ImmutableList.of();

    ImmutableList.Builder<PartialRow> splitRows = ImmutableList.builder();

    // ...Otherwise parse the splits. We're expecting splits in the format of a list of
    // lists of keys. We only support specifying splits for int and string keys
    // (currently those are the only type of keys allowed in Kudu too).
    try {
      JsonReader jr = Json.createReader(new StringReader(kuduSplits));
      JsonArray keysList = jr.readArray();
      for (int i = 0; i < keysList.size(); i++) {
        PartialRow splitRow = new PartialRow(schema);
        JsonArray compoundKey = keysList.getJsonArray(i);
        if (compoundKey.size() != schema.getPrimaryKeyColumnCount()) {
          throw new ImpalaRuntimeException(SPLIT_KEYS_ERROR_MESSAGE +
              " Wrong number of keys.");
        }
        for (int j = 0; j < compoundKey.size(); j++) {
          setKey(schema.getColumnByIndex(j).getType(), compoundKey, j, splitRow);
        }
        splitRows.add(splitRow);
      }
    } catch (ImpalaRuntimeException e) {
      throw e;
    } catch (Exception e) {
      throw new ImpalaRuntimeException(SPLIT_KEYS_ERROR_MESSAGE + " Problem parsing json"
          + ": " + e.getMessage(), e);
    }

    return splitRows.build();
  }

  /**
   * Given the TDistributeByRangeParam from the CREATE statement, creates the
   * appropriate split rows.
   */
  public static List<PartialRow> parseSplits(Schema schema,
      TDistributeByRangeParam param) throws ImpalaRuntimeException {
    ImmutableList.Builder<PartialRow> splitRows = ImmutableList.builder();
    for (TRangeLiteralList literals : param.getSplit_rows()) {
      PartialRow splitRow = new PartialRow(schema);
      List<TRangeLiteral> literalValues = literals.getValues();
      for (int i = 0; i < literalValues.size(); ++i) {
        String colName = param.getColumns().get(i);
        ColumnSchema col = schema.getColumn(colName);
        setKey(col.getType(), literalValues.get(i), schema.getColumnIndex(colName),
            colName, splitRow);
      }
      splitRows.add(splitRow);
    }
    return splitRows.build();
  }

  /**
   * Sets the value in 'key' at 'pos', given the json representation.
   */
  private static void setKey(org.apache.kudu.Type type, JsonArray array, int pos,
      PartialRow key) throws ImpalaRuntimeException {
    switch (type) {
      case INT8: key.addByte(pos, (byte) array.getInt(pos)); break;
      case INT16: key.addShort(pos, (short) array.getInt(pos)); break;
      case INT32: key.addInt(pos, array.getInt(pos)); break;
      case INT64: key.addLong(pos, array.getJsonNumber(pos).longValue()); break;
      case STRING: key.addString(pos, array.getString(pos)); break;
      default:
        throw new ImpalaRuntimeException("Key columns not supported for type: "
            + type.toString());
    }
  }

  /**
   * Sets the value in 'key' at 'pos', given the range literal.
   */
  private static void setKey(org.apache.kudu.Type type, TRangeLiteral literal, int pos,
      String colName, PartialRow key) throws ImpalaRuntimeException {
    switch (type) {
      case INT8:
        checkCorrectType(literal.isSetInt_literal(), type, colName, literal);
        key.addByte(pos, (byte) literal.getInt_literal());
        break;
      case INT16:
        checkCorrectType(literal.isSetInt_literal(), type, colName, literal);
        key.addShort(pos, (short) literal.getInt_literal());
        break;
      case INT32:
        checkCorrectType(literal.isSetInt_literal(), type, colName, literal);
        key.addInt(pos, (int) literal.getInt_literal());
        break;
      case INT64:
        checkCorrectType(literal.isSetInt_literal(), type, colName, literal);
        key.addLong(pos, literal.getInt_literal());
        break;
      case STRING:
        checkCorrectType(literal.isSetString_literal(), type, colName, literal);
        key.addString(pos, literal.getString_literal());
        break;
      default:
        throw new ImpalaRuntimeException("Key columns not supported for type: "
            + type.toString());
    }
  }

  /**
   * If correctType is true, returns. Otherwise throws a formatted error message
   * indicating problems with the type of the literal of the range literal.
   */
  private static void checkCorrectType(boolean correctType, org.apache.kudu.Type t,
      String colName, TRangeLiteral literal) throws ImpalaRuntimeException {
    if (correctType) return;
    throw new ImpalaRuntimeException(
        format("Expected %s literal for column '%s' got '%s'", t.getName(), colName,
            toString(literal)));
  }

  /**
   * Parses a string of the form "a, b, c" and returns a set of values split by ',' and
   * stripped of the whitespace.
   */
  public static HashSet<String> parseKeyColumns(String cols) {
    return Sets.newHashSet(Splitter.on(",").trimResults().split(cols.toLowerCase()));
  }

  public static List<String> parseKeyColumnsAsList(String cols) {
    return Lists.newArrayList(Splitter.on(",").trimResults().split(cols.toLowerCase()));
  }

  public static boolean isSupportedKeyType(org.apache.impala.catalog.Type type) {
    return type.isIntegerType() || type.isStringType();
  }

  /**
   * Return the name that should be used in Kudu when creating a table, assuming a custom
   * name was not provided.
   */
  public static String getDefaultCreateKuduTableName(String metastoreDbName,
      String metastoreTableName) {
    return KUDU_TABLE_NAME_PREFIX + metastoreDbName + "." + metastoreTableName;
  }

  /**
   * Converts a given Impala catalog type to the Kudu type. Throws an exception if the
   * type cannot be converted.
   */
  public static org.apache.kudu.Type fromImpalaType(Type t)
      throws ImpalaRuntimeException {
    if (!t.isScalarType()) {
      throw new ImpalaRuntimeException(format(
          "Type %s is not supported in Kudu", t.toSql()));
    }
    ScalarType s = (ScalarType) t;
    switch (s.getPrimitiveType()) {
      case TINYINT: return org.apache.kudu.Type.INT8;
      case SMALLINT: return org.apache.kudu.Type.INT16;
      case INT: return org.apache.kudu.Type.INT32;
      case BIGINT: return org.apache.kudu.Type.INT64;
      case BOOLEAN: return org.apache.kudu.Type.BOOL;
      case STRING: return org.apache.kudu.Type.STRING;
      case DOUBLE: return org.apache.kudu.Type.DOUBLE;
      case FLOAT: return org.apache.kudu.Type.FLOAT;
        /* Fall through below */
      case INVALID_TYPE:
      case NULL_TYPE:
      case TIMESTAMP:
      case BINARY:
      case DATE:
      case DATETIME:
      case DECIMAL:
      case CHAR:
      case VARCHAR:
      default:
        throw new ImpalaRuntimeException(format(
            "Type %s is not supported in Kudu", s.toSql()));
    }
  }

  public static Type toImpalaType(org.apache.kudu.Type t)
      throws ImpalaRuntimeException {
    switch (t) {
      case BOOL: return Type.BOOLEAN;
      case DOUBLE: return Type.DOUBLE;
      case FLOAT: return Type.FLOAT;
      case INT8: return Type.TINYINT;
      case INT16: return Type.SMALLINT;
      case INT32: return Type.INT;
      case INT64: return Type.BIGINT;
      case STRING: return Type.STRING;
      default:
        throw new ImpalaRuntimeException(String.format(
            "Kudu type '%s' is not supported in Impala", t.getName()));
    }
  }

  /**
   * Returns the string value of the RANGE literal.
   */
  static String toString(TRangeLiteral l) throws ImpalaRuntimeException {
    if (l.isSetString_literal()) return String.valueOf(l.string_literal);
    if (l.isSetInt_literal()) return String.valueOf(l.int_literal);
    throw new ImpalaRuntimeException("Unsupported type for RANGE literal.");
  }
}
