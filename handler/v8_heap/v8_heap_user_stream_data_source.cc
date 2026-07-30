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

#include "handler/v8_heap/v8_heap_user_stream_data_source.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "handler/v8_heap/v8_heap_capture.h"
#include "handler/v8_heap/v8_heap_format.h"

namespace crashpad {

namespace {

// A MinidumpUserExtensionStreamDataSource backed by an in-memory byte buffer.
// Production equivalent of the test-only BufferExtensionStreamDataSource
// (minidump/test/minidump_user_extension_stream_util.h) — we cannot depend on
// that target because it is testonly.
class V8HeapExtensionStreamDataSource final
    : public MinidumpUserExtensionStreamDataSource {
 public:
  explicit V8HeapExtensionStreamDataSource(std::vector<uint8_t> data)
      : MinidumpUserExtensionStreamDataSource(kMinidumpStreamTypeV8Heap),
        data_(std::move(data)) {}

  V8HeapExtensionStreamDataSource(const V8HeapExtensionStreamDataSource&) =
      delete;
  V8HeapExtensionStreamDataSource& operator=(
      const V8HeapExtensionStreamDataSource&) = delete;

  // MinidumpUserExtensionStreamDataSource:
  size_t StreamDataSize() override { return data_.size(); }

  bool ReadStreamData(Delegate* delegate) override {
    return delegate->ExtensionStreamDataSourceRead(
        data_.empty() ? nullptr : data_.data(), data_.size());
  }

 private:
  std::vector<uint8_t> data_;
};

}  // namespace

std::unique_ptr<MinidumpUserExtensionStreamDataSource>
V8HeapUserStreamDataSource::ProduceStreamData(
    ProcessSnapshot* process_snapshot) {
  V8HeapCapture capture;
  capture.Capture(*process_snapshot);
  if (!capture.captured())
    return nullptr;  // not a V8 process, or nothing to capture
  return std::make_unique<V8HeapExtensionStreamDataSource>(capture.Serialize());
}

}  // namespace crashpad
