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

#include <string>

#include "common/thread-debug-info.h"
#include "testutil/gtest-util.h"

#include "common/names.h"

namespace impala {

TEST(ThreadDebugInfo, Ids) {
  // This test checks if SetInstanceId() stores the
  // string representation of a TUniqueId correctly.
  ThreadDebugInfo thread_debug_info;
  TUniqueId uid;
  uid.hi = 123;
  uid.lo = 456;
  thread_debug_info.SetInstanceId(uid);
  string uid_str = PrintId(uid);

  EXPECT_EQ(uid_str, thread_debug_info.GetInstanceId());
}

TEST(ThreadDebugInfo, ThreadName) {
  // Checks if we can store the thread name.
  // If thread name is too long, the prefix of the thread name is stored.
  ThreadDebugInfo thread_debug_info;
  string thread_name = "thread-1";
  thread_debug_info.SetThreadName(thread_name);

  EXPECT_EQ(thread_name, thread_debug_info.GetThreadName());

  string a_255(255, 'a');
  string b_255(255, 'b');
  string a_b = a_255 + b_255;
  thread_debug_info.SetThreadName(a_b);
  string stored_name = thread_debug_info.GetThreadName();

  // Let's check if we stored the corrects parts of thread name
  string expected = a_255.substr(0, 244) + "..." + b_255.substr(0, 8);
  EXPECT_EQ(expected, stored_name);
}

TEST(ThreadDebugInfo, Global) {
  // Checks if the constructor of the local ThreadDebugInfo object set the
  // global pointer to itself.
  ThreadDebugInfo thread_debug_info;
  ThreadDebugInfo* global_thread_debug_info = GetThreadDebugInfo();

  EXPECT_EQ(&thread_debug_info, global_thread_debug_info);
}

}

IMPALA_TEST_MAIN();
