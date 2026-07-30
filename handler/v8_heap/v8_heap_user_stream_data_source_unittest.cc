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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "handler/v8_heap/v8_heap_capture.h"
#include "handler/v8_heap/v8_heap_format.h"
#include "minidump/minidump_user_extension_stream_data_source.h"
#include "snapshot/test/test_memory_snapshot.h"
#include "snapshot/test/test_process_snapshot.h"
#include "snapshot/test/test_thread_snapshot.h"
#include "util/misc/address_types.h"
#include "util/process/process_memory.h"

namespace crashpad {
namespace test {
namespace {

// A ProcessMemory backed by an in-memory map of regions, so Capture() can be
// driven without a real (or suspended) process. Read() succeeds only for ranges
// fully contained in one added region; everything else is "unreadable".
class FakeProcessMemory : public ProcessMemory {
 public:
  void AddRegion(VMAddress va, std::vector<uint8_t> bytes) {
    regions_[va] = std::move(bytes);
  }

  ssize_t ReadUpTo(VMAddress address, size_t size, void* buffer) const override {
    for (const auto& [base, bytes] : regions_) {
      if (address >= base) {
        const uint64_t offset = address - base;
        if (offset + size <= bytes.size()) {
          std::memcpy(buffer, bytes.data() + offset, size);
          return static_cast<ssize_t>(size);
        }
      }
    }
    return -1;  // unreadable
  }

 private:
  std::map<VMAddress, std::vector<uint8_t>> regions_;
};

// Captures the bytes handed to ReadStreamData() for inspection.
class CapturingDelegate
    : public MinidumpUserExtensionStreamDataSource::Delegate {
 public:
  bool ExtensionStreamDataSourceRead(const void* data, size_t size) override {
    out_.assign(static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + size);
    return true;
  }

  std::vector<uint8_t> out_;
};

uint32_t U32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t U64LE(const uint8_t* p) {
  return static_cast<uint64_t>(U32LE(p)) |
         (static_cast<uint64_t>(U32LE(p + 4)) << 32);
}

// Drives the full ProduceStreamData path against a fake snapshot and checks the
// emitted V8HE stream: header fields + region table + region bytes.
TEST(V8HeapUserStreamDataSource, CapturesRoAndIsolateFromFakeSnapshot) {
  constexpr uint64_t kCageBase = 0x000100000000ULL;  // high 32 bits set
  constexpr uint64_t kIsolateVa = 0x000200000000ULL;
  constexpr uint64_t kPageSize = 256 * 1024;

  // V8 annotations exactly as Blink/base::debug publish them.
  std::map<std::string, std::string> annotations = {
      {"v8_isolate_address", "0x000200000000"},
      {"v8_ro_space_firstpage_address", "0x000100000000"},
  };

  auto mem = std::make_unique<FakeProcessMemory>();
  mem->AddRegion(kCageBase, std::vector<uint8_t>(kPageSize, 0xAA));
  mem->AddRegion(kIsolateVa, std::vector<uint8_t>(kPageSize, 0xBB));

  TestProcessSnapshot snapshot;
  snapshot.SetAnnotationsSimpleMap(annotations);
  snapshot.SetProcessMemory(std::move(mem));

  V8HeapUserStreamDataSource source;
  auto data_source = source.ProduceStreamData(&snapshot);
  ASSERT_TRUE(data_source);
  EXPECT_EQ(data_source->stream_type(),
            static_cast<MinidumpStreamType>(kMinidumpStreamTypeV8Heap));

  CapturingDelegate delegate;
  ASSERT_TRUE(data_source->ReadStreamData(&delegate));
  const std::vector<uint8_t>& b = delegate.out_;
  ASSERT_GE(b.size(), 32u + 2u * 24u);

  // Header.
  EXPECT_EQ(U32LE(&b[0]), kMinidumpStreamTypeV8Heap);
  EXPECT_EQ(U32LE(&b[4]), kV8HeapStreamVersion);
  EXPECT_EQ(U64LE(&b[8]), kCageBase);
  EXPECT_EQ(U64LE(&b[16]), kIsolateVa);
  const uint32_t region_count = U32LE(&b[24]);
  EXPECT_EQ(U32LE(&b[28]), 0u);  // flags: full heap

  // Region table: expect one RO page at the cage base and one isolate page,
  // each carrying its marker byte. Unreadable tails are trimmed.
  bool found_ro = false;
  bool found_isolate = false;
  for (uint32_t i = 0; i < region_count; ++i) {
    const uint8_t* r = &b[32 + i * 24];
    const uint64_t va = U64LE(r);
    const uint64_t size = U64LE(r + 8);
    const uint64_t off = U64LE(r + 16);
    ASSERT_LE(static_cast<size_t>(off) + static_cast<size_t>(size), b.size());
    if (va == kCageBase) {
      found_ro = true;
      EXPECT_EQ(size, kPageSize);
      EXPECT_EQ(b[off], 0xAA);
    } else if (va == kIsolateVa) {
      found_isolate = true;
      EXPECT_EQ(size, kPageSize);
      EXPECT_EQ(b[off], 0xBB);
    }
  }
  EXPECT_TRUE(found_ro);
  EXPECT_TRUE(found_isolate);
}

// A process without V8 annotations (e.g. the browser process) must opt out —
// no stream emitted.
TEST(V8HeapUserStreamDataSource, OptsOutWithoutV8Annotations) {
  TestProcessSnapshot snapshot;  // no annotations, no memory
  V8HeapUserStreamDataSource source;
  auto data_source = source.ProduceStreamData(&snapshot);
  EXPECT_FALSE(data_source);
}

// The chase follows the decoder's compressed-pointer chain (JSFunction -> SFI ->
// Script -> line_ends) from a stack-derived JSFunction, capturing each hop's
// page. Within-cage refs are 32-bit; each hop is validated by its map landing
// in RO space, so the null name_or_scope / name fields (even values) are not
// followed.
TEST(V8HeapUserStreamDataSource, ChaseFollowsCompressedChain) {
  constexpr uint64_t kCage = 0x000100000000ULL;
  constexpr uint64_t kIsolate = 0x000200000000ULL;
  constexpr uint64_t kStack = 0x00007FF700000000ULL;
  constexpr uint64_t kPageSize = 256 * 1024;
  constexpr uint64_t kJsf = kCage + 0x500000;
  constexpr uint64_t kSfi = kCage + 0x600000;
  constexpr uint64_t kScript = kCage + 0x700000;
  constexpr uint64_t kLineEnds = kCage + 0x800000;
  // Compressed map pointer: offset 0x100 (inside the RO span) | tag bit.
  constexpr uint32_t kMap = 0x101;

  auto setu32 = [](std::vector<uint8_t>& p, size_t off, uint32_t v) {
    p[off] = v & 0xFF;
    p[off + 1] = (v >> 8) & 0xFF;
    p[off + 2] = (v >> 16) & 0xFF;
    p[off + 3] = (v >> 24) & 0xFF;
  };

  auto mem = std::make_unique<FakeProcessMemory>();
  mem->AddRegion(kCage, std::vector<uint8_t>(kPageSize, 0));      // RO page
  mem->AddRegion(kIsolate, std::vector<uint8_t>(kPageSize, 0));  // isolate slice

  // Stack holding one tagged JSFunction full pointer.
  std::vector<uint8_t> stack(kPageSize, 0);
  const uint64_t jsf_tagged = kJsf | 1ULL;
  for (int i = 0; i < 8; ++i)
    stack[i] = (jsf_tagged >> (8 * i)) & 0xFF;
  mem->AddRegion(kStack, std::move(stack));

  // JSFunction: map@0, SharedFunctionInfo compressed@+16 -> SFI.
  std::vector<uint8_t> jsf_page(kPageSize, 0);
  setu32(jsf_page, 0, kMap);
  setu32(jsf_page, 16, 0x600001);
  mem->AddRegion(kJsf, std::move(jsf_page));

  // SFI: map@0, Script compressed@+20 -> Script (+12 name_or_scope left null).
  std::vector<uint8_t> sfi_page(kPageSize, 0);
  setu32(sfi_page, 0, kMap);
  setu32(sfi_page, 20, 0x700001);
  mem->AddRegion(kSfi, std::move(sfi_page));

  // Script: map@0, line_ends compressed@+28 -> line_ends (+8 name left null).
  std::vector<uint8_t> script_page(kPageSize, 0);
  setu32(script_page, 0, kMap);
  setu32(script_page, 28, 0x800001);
  mem->AddRegion(kScript, std::move(script_page));

  std::vector<uint8_t> le_page(kPageSize, 0);
  setu32(le_page, 0, kMap);
  mem->AddRegion(kLineEnds, std::move(le_page));

  // Thread whose Stack() reports (kStack, kPageSize); the bytes are served by
  // the ProcessMemory above.
  auto thread = std::make_unique<TestThreadSnapshot>();
  auto stack_snap = std::make_unique<TestMemorySnapshot>();
  stack_snap->SetAddress(kStack);
  stack_snap->SetSize(kPageSize);
  thread->SetStack(std::move(stack_snap));

  TestProcessSnapshot snapshot;
  snapshot.SetAnnotationsSimpleMap({{"v8_isolate_address", "0x000200000000"},
                                    {"v8_ro_space_firstpage_address", "0x000100000000"}});
  snapshot.SetProcessMemory(std::move(mem));
  snapshot.AddThread(std::move(thread));

  V8HeapUserStreamDataSource source;
  auto data_source = source.ProduceStreamData(&snapshot);
  ASSERT_TRUE(data_source);

  CapturingDelegate delegate;
  ASSERT_TRUE(data_source->ReadStreamData(&delegate));
  const auto& b = delegate.out_;
  ASSERT_GE(b.size(), 32u + 24u);

  const uint32_t region_count = U32LE(&b[24]);
  std::vector<uint64_t> vas;
  for (uint32_t i = 0; i < region_count; ++i)
    vas.push_back(U64LE(&b[32 + i * 24]));
  auto has = [&](uint64_t va) {
    return std::find(vas.begin(), vas.end(), va) != vas.end();
  };
  EXPECT_TRUE(has(kJsf));
  EXPECT_TRUE(has(kSfi));
  EXPECT_TRUE(has(kScript));
  EXPECT_TRUE(has(kLineEnds));
}

// FindEptBase locates the external pointer table by scanning the isolate slice
// for a pointer whose entry for |handle| resolves to a resource with a module
// vtable and a printable char payload.
TEST(V8HeapUserStreamDataSource, FindEptBaseLocatesTable) {
  constexpr uint64_t kEptBase = 0x0000000300000000ULL;
  constexpr uint64_t kResource = 0x0000000400000000ULL;
  constexpr uint64_t kChars = 0x0000000500000000ULL;
  constexpr uint64_t kModuleBase = 0x00007FF800000000ULL;
  constexpr uint64_t kModuleSize = 0x00100000;
  constexpr uint32_t kHandle = 0x40;  // idx = handle >> 6 = 1
  constexpr uint32_t kLen = 5;
  const std::string kName = "hello";

  auto mem = std::make_unique<FakeProcessMemory>();
  // EPT: entry at ept_base + 16*idx = ept_base+16 holds the resource pointer.
  std::vector<uint8_t> ept(32, 0);
  const uint64_t entry = kResource;
  std::memcpy(&ept[16], &entry, 8);
  mem->AddRegion(kEptBase, std::move(ept));
  // resource: vtable in-module at +0, chars pointer at +16.
  std::vector<uint8_t> res(64, 0);
  const uint64_t vtable = kModuleBase + 0x1000;
  std::memcpy(&res[0], &vtable, 8);
  const uint64_t chars_ptr = kChars;
  std::memcpy(&res[16], &chars_ptr, 8);
  mem->AddRegion(kResource, std::move(res));
  // chars: printable, one-byte, length kLen.
  std::vector<uint8_t> chars(kName.begin(), kName.end());
  mem->AddRegion(kChars, std::move(chars));
  // Isolate slice: the EPT base at offset 0.
  std::vector<uint8_t> isolate(8, 0);
  std::memcpy(&isolate[0], &kEptBase, 8);

  std::vector<std::pair<uint64_t, uint64_t>> module_ranges = {
      {kModuleBase, kModuleSize}};

  EXPECT_EQ(V8HeapCapture::FindEptBase(*mem,
                                       isolate.data(),
                                       isolate.size(),
                                       kHandle,
                                       kLen,
                                       /*one_byte=*/true,
                                       module_ranges),
            kEptBase);
}

}  // namespace
}  // namespace test
}  // namespace crashpad
