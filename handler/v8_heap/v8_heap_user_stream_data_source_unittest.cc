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
#include "snapshot/test/test_module_snapshot.h"
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
  // V8 15.6 Windows sandbox: 8-byte entries. Handle 0x40 is index 1, so the
  // resource entry is at byte 8. Tag 2 << 49 is kExternalStringResourceTag.
  // Bit 48 is the mark bit and is outside the tag mask. The payload keeps the
  // low 48 bits and the top byte (include/v8-internal.h).
  std::vector<uint8_t> ept(32, 0);
  const uint64_t entry = kResource | (2ULL << 49) | (1ULL << 48);
  std::memcpy(&ept[8], &entry, 8);
  mem->AddRegion(kEptBase, std::move(ept));
  // resource: vtable at +0, cached_data_ at +8 (first member after the vptr).
  std::vector<uint8_t> res(64, 0);
  const uint64_t vtable = kModuleBase + 0x1000;
  std::memcpy(&res[0], &vtable, 8);
  const uint64_t chars_ptr = kChars;
  std::memcpy(&res[8], &chars_ptr, 8);
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

// Drives ProduceStreamData. Numeric fixtures come from the V8 15.6.75.25
// sources (instance-type.h, v8-internal.h, isolate-data.h), not from
// v8_layout.h. The isolate address is intentionally 8 bytes past a 4 KiB
// boundary and not on a 256 KiB V8 page.
class V8HeapExternalStringsTest : public testing::Test {
 protected:
  static constexpr uint64_t kCage = 0x100000000ULL;
  static constexpr uint64_t kIsolatePage = 0x200000000ULL;
  static constexpr uint64_t kIsolate = kIsolatePage + 0x1008;
  // Internals::kIsolateSharedExternalPointerTableAddressOffset for this pin.
  static constexpr uint64_t kSharedTableSlot = 0x2F0;
  // V8 15.2 slot. A reader that still uses it must not win.
  static constexpr uint64_t kStaleTableSlot = 0x300;
  static constexpr uint64_t kTableObject = 0x250000000ULL;
  static constexpr uint64_t kTable = 0x300000000ULL;
  static constexpr uint64_t kDecoyObject = 0x260000000ULL;
  static constexpr uint64_t kDecoyTable = 0x310000000ULL;
  static constexpr uint64_t kResource = 0x400000000ULL;
  static constexpr uint64_t kChars = 0x500000000ULL;
  // Bit 56 is inside kExternalPointerPayloadMask (0xff00ffffffffffff) and
  // outside the old 48-bit mask.
  static constexpr uint64_t kHighChars = 0x0100000500000000ULL;
  static constexpr uint64_t kWrongChars = 0x700000000ULL;
  static constexpr uint64_t kModule = 0x7ff800000000ULL;
  static constexpr uint64_t kJsf = kCage + 0x500000;
  static constexpr uint64_t kStack = 0x7ff700000000ULL;
  static constexpr size_t kPage = 256 * 1024;
  // instance-type.h: external=2, one-byte=8, not-internalized=0x20.
  static constexpr uint16_t kExternalOneByte = 0x2A;
  static constexpr uint16_t kExternalTwoByte = 0x22;
  static constexpr uint16_t kUncachedBit = 0x10;
  // Sliced is representation 3, so bit 1 is set but it is not external.
  static constexpr uint16_t kSlicedOneByte = 0x2B;

  static void Put(std::vector<uint8_t>& bytes,
                  size_t off,
                  uint64_t value,
                  size_t size = 8) {
    ASSERT_LE(off + size, bytes.size());
    for (size_t i = 0; i < size; ++i)
      bytes[off + i] = static_cast<uint8_t>(value >> (8 * i));
  }

  struct Options {
    bool uncached = false;
    bool invalid_direct = false;
    bool missing_direct = false;
    bool broken_data = false;
    bool resource_cache = true;
    bool scan_candidate = true;
    bool wrong_tag = false;
    bool two_byte = false;
    bool stale_slot = false;
    bool sliced = false;
    bool overlong = false;
    bool high_chars = false;
  };

  std::vector<uint8_t> Capture(const Options& options) {
    auto memory = std::make_unique<FakeProcessMemory>();
    std::vector<uint8_t> ro(kPage, 0);
    uint16_t instance_type = options.two_byte ? kExternalTwoByte
                                              : kExternalOneByte;
    if (options.uncached)
      instance_type = static_cast<uint16_t>(instance_type | kUncachedBit);
    if (options.sliced)
      instance_type = kSlicedOneByte;
    // Name map is the compressed pointer 0x201, so the instance type is at
    // cage + 0x200 + 8.
    Put(ro, 0x208, instance_type, 2);
    memory->AddRegion(kCage, std::move(ro));

    // Character length is independent of v8_layout::kMaxNameLen. 3000 two-byte
    // characters are 6000 bytes, so a capture that clamps the byte length to
    // the 4096 character cap drops the tail. 4097 is one past that cap and is
    // fully readable, so a missing cap check would still store the payload.
    const uint32_t char_len = options.overlong ? 4097u
                              : options.two_byte ? 3000u
                                                 : 5u;
    std::vector<uint8_t> heap(kPage, 0);
    Put(heap, 0, 0x101, 4);       // JSFunction.map
    Put(heap, 16, 0x500101, 4);   // JSFunction.shared_function_info
    Put(heap, 0x100, 0x101, 4);   // SFI.map
    Put(heap, 0x114, 0x500201, 4);  // SFI.script at +20
    Put(heap, 0x200, 0x101, 4);   // Script.map
    Put(heap, 0x208, 0x500301, 4);  // Script.name at +8
    Put(heap, 0x300, 0x201, 4);   // name.map
    Put(heap, 0x308, char_len, 4);
    Put(heap, 0x30c, 0x40, 4);  // resource handle, index 1
    // Uncached strings have no resource_data_. This word is the next object.
    // As a handle (0xc0) it would select the decoy payload at entry index 3.
    Put(heap, 0x310, options.uncached ? 0xc0 : 0x80, 4);
    memory->AddRegion(kJsf, std::move(heap));

    std::vector<uint8_t> stack(8, 0);
    Put(stack, 0, kJsf | 1);
    memory->AddRegion(kStack, std::move(stack));
    auto thread = std::make_unique<TestThreadSnapshot>();
    auto stack_snapshot = std::make_unique<TestMemorySnapshot>();
    stack_snapshot->SetAddress(kStack);
    stack_snapshot->SetSize(8);
    thread->SetStack(std::move(stack_snapshot));

    std::vector<uint8_t> isolate(kPage, 0);
    const uint64_t isolate_off = kIsolate - kIsolatePage;
    if (!options.missing_direct)
      Put(isolate, isolate_off + kSharedTableSlot, kTableObject);
    if (options.stale_slot)
      Put(isolate, isolate_off + kStaleTableSlot, kDecoyObject);
    if (options.scan_candidate)
      Put(isolate, isolate_off + 0x40, kTable);
    memory->AddRegion(kIsolatePage, std::move(isolate));

    std::vector<uint8_t> table_object(64, 0);
    Put(table_object, 0, options.invalid_direct ? kWrongTable() : kTable);
    memory->AddRegion(kTableObject, std::move(table_object));
    if (options.stale_slot) {
      std::vector<uint8_t> decoy_object(64, 0);
      Put(decoy_object, 0, kDecoyTable);
      memory->AddRegion(kDecoyObject, std::move(decoy_object));
    }

    const uint64_t chars = options.high_chars ? kHighChars : kChars;
    auto fill_ept = [&](bool swapped_tags) {
      std::vector<uint8_t> ept(4096, 0);
      const uint64_t resource_tag = swapped_tags ? (3ULL << 49) : (2ULL << 49);
      const uint64_t data_tag = swapped_tags ? (2ULL << 49) : (3ULL << 49);
      const uint64_t data_ptr =
          options.broken_data ? kWrongChars + 0x100000 : chars;
      Put(ept, 8, kResource | resource_tag | (1ULL << 48));
      Put(ept, 16, data_ptr | data_tag);
      Put(ept, 24, kWrongChars | data_tag);
      return ept;
    };
    memory->AddRegion(kTable, fill_ept(false));
    if (options.invalid_direct) {
      // A zero table is corrupt. Swapped tags are the wrong-tag case. Neither
      // may satisfy the direct-route tag and payload checks.
      memory->AddRegion(kWrongTable(),
                        options.wrong_tag ? fill_ept(true)
                                          : std::vector<uint8_t>(4096, 0));
    }
    if (options.stale_slot) {
      std::vector<uint8_t> decoy(4096, 0);
      Put(decoy, 8, kResource | (2ULL << 49) | (1ULL << 48));
      Put(decoy, 16, kWrongChars | (3ULL << 49));
      memory->AddRegion(kDecoyTable, std::move(decoy));
    }

    std::vector<uint8_t> resource(64, 0);
    Put(resource, 0, kModule + 0x1000);
    if (options.resource_cache)
      Put(resource, 8, chars);
    memory->AddRegion(kResource, std::move(resource));

    std::vector<uint8_t> payload;
    if (options.two_byte) {
      payload.reserve(static_cast<size_t>(char_len) * 2);
      for (uint32_t i = 0; i < char_len; ++i) {
        payload.push_back('a');
        payload.push_back(0);
      }
    } else if (options.overlong) {
      payload.assign(char_len, 'a');
    } else {
      payload.assign({'h', 'e', 'l', 'l', 'o'});
    }
    memory->AddRegion(chars, payload);
    memory->AddRegion(kWrongChars, std::vector<uint8_t>(10, 'x'));

    auto module = std::make_unique<TestModuleSnapshot>();
    module->SetAddressAndSize(kModule, 0x100000);
    TestProcessSnapshot snapshot;
    snapshot.SetAnnotationsSimpleMap(
        {{"v8_isolate_address", "0x200001008"},
         {"v8_ro_space_firstpage_address", "0x100000000"}});
    snapshot.SetProcessMemory(std::move(memory));
    snapshot.AddThread(std::move(thread));
    snapshot.AddModule(std::move(module));
    V8HeapUserStreamDataSource source;
    auto stream = source.ProduceStreamData(&snapshot);
    EXPECT_TRUE(stream);
    if (!stream)
      return {};
    CapturingDelegate delegate;
    EXPECT_TRUE(stream->ReadStreamData(&delegate));
    return delegate.out_;
  }

  static uint64_t kWrongTable() { return 0x600000000ULL; }

  static std::vector<uint8_t> BytesAt(const std::vector<uint8_t>& stream,
                                      uint64_t address) {
    if (stream.size() < 32)
      return {};
    const uint32_t count = U32LE(&stream[24]);
    for (uint32_t i = 0; i < count; ++i) {
      const size_t entry = 32 + static_cast<size_t>(i) * 24;
      if (entry + 24 > stream.size())
        return {};
      const uint64_t va = U64LE(&stream[entry]);
      const uint64_t size = U64LE(&stream[entry + 8]);
      const uint64_t offset = U64LE(&stream[entry + 16]);
      if (va == address && offset <= stream.size() &&
          size <= stream.size() - offset) {
        return std::vector<uint8_t>(stream.begin() + static_cast<size_t>(offset),
                                    stream.begin() + static_cast<size_t>(offset + size));
      }
    }
    return {};
  }

  const std::vector<uint8_t> expected_ = {'h', 'e', 'l', 'l', 'o'};
};

TEST_F(V8HeapExternalStringsTest, PreservesUnalignedIsolateAndCachedChars) {
  Options options;
  options.resource_cache = false;
  options.scan_candidate = false;
  const auto stream = Capture(options);
  ASSERT_GE(stream.size(), 24u);
  EXPECT_EQ(U64LE(&stream[16]), kIsolate);
  EXPECT_EQ(BytesAt(stream, kChars), expected_);
}

TEST_F(V8HeapExternalStringsTest, CachedDataDoesNotRequireResourceCache) {
  Options options;
  options.resource_cache = false;
  options.scan_candidate = false;
  EXPECT_EQ(BytesAt(Capture(options), kChars), expected_);
}

TEST_F(V8HeapExternalStringsTest, UncachedStringIgnoresFollowingHeapWord) {
  Options options;
  options.uncached = true;
  const auto stream = Capture(options);
  EXPECT_EQ(BytesAt(stream, kChars), expected_);
  EXPECT_TRUE(BytesAt(stream, kWrongChars).empty());
}

TEST_F(V8HeapExternalStringsTest, InvalidDirectBaseFallsBackToScan) {
  Options options;
  options.invalid_direct = true;
  EXPECT_EQ(BytesAt(Capture(options), kChars), expected_);
}

TEST_F(V8HeapExternalStringsTest, WrongTagsFallBackToScan) {
  Options options;
  options.invalid_direct = true;
  options.wrong_tag = true;
  const auto stream = Capture(options);
  EXPECT_EQ(BytesAt(stream, kChars), expected_);
  EXPECT_TRUE(BytesAt(stream, kWrongChars).empty());
}

TEST_F(V8HeapExternalStringsTest, MissingDirectTableFallsBackToScan) {
  Options options;
  options.missing_direct = true;
  EXPECT_EQ(BytesAt(Capture(options), kChars), expected_);
}

TEST_F(V8HeapExternalStringsTest, BrokenDataHandleUsesResourceCache) {
  Options options;
  options.broken_data = true;
  options.scan_candidate = false;
  EXPECT_EQ(BytesAt(Capture(options), kChars), expected_);
}

TEST_F(V8HeapExternalStringsTest, InvalidTableWithoutFallbackOmitsChars) {
  Options options;
  options.invalid_direct = true;
  options.scan_candidate = false;
  options.wrong_tag = true;
  const auto stream = Capture(options);
  EXPECT_TRUE(BytesAt(stream, kChars).empty());
  EXPECT_TRUE(BytesAt(stream, kWrongChars).empty());
}

TEST_F(V8HeapExternalStringsTest, CapturesTwoByteData) {
  Options options;
  options.resource_cache = false;
  options.two_byte = true;
  std::vector<uint8_t> expected;
  expected.reserve(6000);
  for (int i = 0; i < 3000; ++i) {
    expected.push_back('a');
    expected.push_back(0);
  }
  EXPECT_EQ(BytesAt(Capture(options), kChars), expected);
}

TEST_F(V8HeapExternalStringsTest, Stale15_2SlotDoesNotOverride15_6Slot) {
  Options options;
  options.stale_slot = true;
  options.resource_cache = false;
  options.scan_candidate = false;
  const auto stream = Capture(options);
  EXPECT_EQ(BytesAt(stream, kChars), expected_);
  EXPECT_TRUE(BytesAt(stream, kWrongChars).empty());
}

TEST_F(V8HeapExternalStringsTest, SlicedStringIsNotExternal) {
  Options options;
  options.sliced = true;
  options.resource_cache = false;
  options.scan_candidate = false;
  EXPECT_TRUE(BytesAt(Capture(options), kChars).empty());
}

TEST_F(V8HeapExternalStringsTest, OverlongNameIsNotCaptured) {
  Options options;
  options.overlong = true;
  options.resource_cache = false;
  options.scan_candidate = false;
  EXPECT_TRUE(BytesAt(Capture(options), kChars).empty());
}

TEST_F(V8HeapExternalStringsTest, PayloadKeepsTopByte) {
  Options options;
  options.high_chars = true;
  options.resource_cache = false;
  options.scan_candidate = false;
  EXPECT_EQ(BytesAt(Capture(options), kHighChars), expected_);
  EXPECT_TRUE(BytesAt(Capture(options), 0x500000000ULL).empty());
}

}  // namespace
}  // namespace test
}  // namespace crashpad
