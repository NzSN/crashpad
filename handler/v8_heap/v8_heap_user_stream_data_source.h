// Copyright 2026 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_USER_STREAM_DATA_SOURCE_H_
#define CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_USER_STREAM_DATA_SOURCE_H_

#include <memory>

#include "handler/user_stream_data_source.h"
#include "minidump/minidump_user_extension_stream_data_source.h"
#include "snapshot/process_snapshot.h"

namespace crashpad {

// UserStreamDataSource that attaches the minimal V8 heap snapshot
// (kMinidumpStreamTypeV8Heap) to each crash minidump. Runs in the out-of-process
// handler against a suspended client. Returns nullptr (no stream) for processes
// without V8 annotations, so registering it is safe for every crash type.
//
// Registered in run_as_crashpad_handler_win.cc alongside stability_report and
// gwp_asan sources.
class V8HeapUserStreamDataSource : public UserStreamDataSource {
 public:
  V8HeapUserStreamDataSource() = default;
  ~V8HeapUserStreamDataSource() override = default;

  V8HeapUserStreamDataSource(const V8HeapUserStreamDataSource&) = delete;
  V8HeapUserStreamDataSource& operator=(const V8HeapUserStreamDataSource&) =
      delete;

  // UserStreamDataSource:
  std::unique_ptr<MinidumpUserExtensionStreamDataSource> ProduceStreamData(
      ProcessSnapshot* process_snapshot) override;
};

}  // namespace crashpad

#endif  // CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_USER_STREAM_DATA_SOURCE_H_
