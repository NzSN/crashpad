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

#ifndef CRASHPAD_HANDLER_V8_HEAP_V8_LAYOUT_H_
#define CRASHPAD_HANDLER_V8_HEAP_V8_LAYOUT_H_

#include <cstdint>

namespace crashpad {
namespace v8_layout {

// Layout of the pinned V8 15.6.75.25 (Chromium 156.0.8078.75) Windows x64
// build. This is not a generic V8 decoder. Blink publishes no V8 version
// crash key (only isolate/RO/map/code addresses and dump-type), so a direct
// IsolateData slot is accepted only after tag and payload checks and is
// otherwise ignored in favor of the scan. Constants below were taken from
// that V8 tree with V8_COMPRESS_POINTERS, V8_ENABLE_SANDBOX, and
// V8_TARGET_ARCH_X64 / V8_TARGET_OS_WIN, and ENABLE_HUGEPAGE unset.

// ── Object-graph field offsets (compressed pointers, bytes) ──
// JSObject header is 3 compressed slots (Internals::kJSObjectHeaderSize).
// JSFunction then stores a 4-byte JSDispatchHandle before
// shared_function_info_, so the SFI slot stays at 16.
inline constexpr uint64_t kJsfSharedFunctionInfo = 16;
// SharedFunctionInfo: trusted pointer (4) + untrusted data (4) precede
// name_or_scope_info_. script_ follows outer_scope_info.
inline constexpr uint64_t kSfiNameOrScopeInfo = 12;
inline constexpr uint64_t kSfiScript = 20;
// Script extends an empty Struct (sizeof == HeapObject). Field order is
// source, name, line_offset, column_offset, context_data, script_type,
// line_ends. Each slot is one compressed pointer.
inline constexpr uint64_t kScriptName = 8;
inline constexpr uint64_t kScriptLineEnds = 28;

// Map is the compressed slot at +0. Internals::kMapInstanceTypeOffset is
// kApiTaggedSize + kApiInt32Size == 8 when pointers are compressed.
inline constexpr uint64_t kMapFieldOffset = 0;
inline constexpr uint64_t kMapInstanceTypeOffset = 8;

// String: map, raw hash, length, then the external resource handle.
// Internals::kStringResourceOffset is static_asserted against
// offsetof(ExternalString, resource_) and is 12 here. Cached ExternalString
// appends resource_data_ (another 4-byte handle). UncachedExternalString ends
// at resource_; the next heap word is not a field of this object.
inline constexpr uint64_t kStringLength = 8;
inline constexpr uint64_t kStringChars = 12;
inline constexpr uint64_t kStringResourceData = 16;
// instance-type.h: bits 7-15 are clear for every string. Bit 3 is the
// one-byte encoding. Bit 4 is the uncached-external tag. Representation is
// bits 0-2, and external is the value 2 (not merely bit 1: sliced is 3).
inline constexpr uint16_t kFirstNonStringType = 0x80;
inline constexpr uint16_t kStringRepresentationMask = 0x7;
inline constexpr uint16_t kExternalStringTag = 0x2;
inline constexpr uint16_t kStringOneByteBit = 0x08;
inline constexpr uint16_t kStringUncachedExternalBit = 0x10;
inline constexpr uint32_t kMaxNameLen = 4096;

// ── External pointer table (sandbox EPT) ──
// include/v8-internal.h, non-Android/non-iOS pointer-compression path:
// 8-byte entries, index shift 6, 512 MB reservation. The tag lives at bits
// 49-55 (mask 0x00fe000000000000). Bit 48 is the mark bit and is not part of
// the tag. The payload mask keeps bits 0-47 and 56-63.
inline constexpr uint64_t kEptEntrySize = 8;
inline constexpr uint32_t kEptIndexShift = 6;
inline constexpr uint64_t kEptReservationSize = 512ULL * 1024 * 1024;
inline constexpr uint64_t kEptPayloadMask = 0xff00ffffffffffffULL;
inline constexpr uint64_t kEptTagMask = 0x00fe000000000000ULL;
// ExternalPointerTag order: null=0, waiter-queue=1, resource=2, data=3.
inline constexpr uint64_t kEptResourceTag = 2ULL << 49;
inline constexpr uint64_t kEptResourceDataTag = 3ULL << 49;

// Isolate::isolate_data_ is at offset 0. Internals::
// kIsolateSharedExternalPointerTableAddressOffset is static_asserted against
// offsetof(shared_external_pointer_table_) and is 0x2F0 for this build:
// cage 8 + stack guard 64 + 8 flag bytes + regexp pointer 8 + two tier-0
// tables of 7 pointers + two 3-pointer LABs + last-young pointer + 5-pointer
// fast-call pad + fast pc/fp/api + long-task size_t + 28-pointer
// ThreadLocalTop + 24-byte HandleScopeData + 112-byte HandleScopeImplementer
// + 4 embedder slots + 32-byte per-isolate ExternalPointerTable.
// The slot holds ExternalPointerTable*. Its first word is the entry-array
// base (ExternalPointerTable::BaseOffset() == 0). This is not the V8 15.2
// slot at 0x300.
inline constexpr uint64_t kIsolateSharedExternalPointerTableOffset = 0x2F0;

// v8::String::ExternalOneByteStringResource / ExternalStringResource extend a
// vptr-only ExternalStringResourceBase. cached_data_ is the first data
// member, so it is at +8 on the Windows x64 ABI. Cached strings store the
// char pointer in resource_data_ instead; uncached cacheable resources keep
// it here.
inline constexpr uint64_t kExternalResourceCachedDataOffset = 8;

}  // namespace v8_layout
}  // namespace crashpad

#endif  // CRASHPAD_HANDLER_V8_HEAP_V8_LAYOUT_H_
