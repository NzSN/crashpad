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

// V8 14.6 (Chromium 146 / Electron 41) layout constants. Mirrors
// forensicator-core's v8layout.rs `v14_6()`. Only the offsets the collector
// needs to chase the decoder's name/line chain are pinned here; everything
// else is captured raw and decoded out-of-process by the analyzer.

// ── Object-graph field offsets (compressed pointers, bytes) ──
// JSFunction.shared_function_info
inline constexpr uint64_t kJsfSharedFunctionInfo = 16;
// SharedFunctionInfo.name_or_scope_info
inline constexpr uint64_t kSfiNameOrScopeInfo = 12;
// SharedFunctionInfo.script
inline constexpr uint64_t kSfiScript = 20;
// Script.name
inline constexpr uint64_t kScriptName = 8;
// Script.line_ends
inline constexpr uint64_t kScriptLineEnds = 28;

// Map field is a compressed pointer at object +0; the instance type is u16 at
// Map + kMapInstanceTypeOffset. (The chase only needs the map to land in RO
// space — it does not decode instance types.)
inline constexpr uint64_t kMapFieldOffset = 0;
inline constexpr uint64_t kMapInstanceTypeOffset = 8;

// ── String layout (inline payload) ── map(0), raw_hash(4), length(8), chars(12)
inline constexpr uint64_t kStringLength = 8;
inline constexpr uint64_t kStringChars = 12;
inline constexpr uint16_t kStringITypeMax = 0x40;
inline constexpr uint16_t kStringOneByteBit = 0x08;
inline constexpr uint16_t kStringExternalBit = 0x02;
inline constexpr uint32_t kMaxNameLen = 4096;

// ── External pointer table (sandbox EPT) ──
inline constexpr uint64_t kEptEntrySize = 16;
inline constexpr uint32_t kEptIndexShift = 6;
inline constexpr uint64_t kEptPayloadMask = 0x0000FFFFFFFFFFFFULL;

}  // namespace v8_layout
}  // namespace crashpad

#endif  // CRASHPAD_HANDLER_V8_HEAP_V8_LAYOUT_H_
