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

#ifndef CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_FORMAT_H_
#define CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_FORMAT_H_

#include <cstdint>

namespace crashpad {

// Minidump user-extension stream carrying a minimal V8 heap snapshot, so a
// stack-only dump can still resolve JIT frames (function/script names + line
// numbers) without a full ~1 GB memory dump.
//
// The collector runs in the out-of-process Crashpad handler against a suspended
// renderer and captures only the regions the decoder walks:
//   JSFunction -> SFI -> name/Script -> line_ends, plus the isolate slice and
//   (optionally) the external-pointer table + external-string targets.
//
// Wire format (little-endian):
//   V8HeapExtensionHeader header;
//   V8HeapRegion regions[header.region_count];
//   uint8_t region_bytes[];   // concatenated, in region order
// Each V8HeapRegion.file_offset is the byte offset of that region's bytes from
// the start of the stream (i.e. immediately after the region table).

// 'V8HE' packed little-endian as a uint32 (in-memory bytes: 'V','8','H','E').
// Above the 0x0000..0xffff reserved range and outside Crashpad's own
// 0x43500001..0x4350ffff block.
inline constexpr uint32_t kMinidumpStreamTypeV8Heap = 0x45483856u;

inline constexpr uint32_t kV8HeapStreamVersion = 1;

struct V8HeapExtensionHeader {
  uint32_t stream_type;   // == kMinidumpStreamTypeV8Heap
  uint32_t version;       // == kV8HeapStreamVersion
  uint64_t cage_base;     // V8 pointer-compression cage base (RO space start)
  uint64_t isolate_va;    // v8 isolate address (for EPT-base discovery)
  uint32_t region_count;  // number of V8HeapRegion entries that follow
  uint32_t flags;         // bit0: strategy B (whole old space) [unused in v1]
                          // bit1: partial heap (external-string targets omitted)
};

static_assert(sizeof(V8HeapExtensionHeader) == 8 + 8 + 8 + 8,
              "V8HeapExtensionHeader layout drift");

struct V8HeapRegion {
  uint64_t va;           // virtual address in the crashed process
  uint64_t size;         // region length in bytes
  uint64_t file_offset;  // offset of region bytes from the start of the stream
};

static_assert(sizeof(V8HeapRegion) == 24, "V8HeapRegion layout drift");

}  // namespace crashpad

#endif  // CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_FORMAT_H_
