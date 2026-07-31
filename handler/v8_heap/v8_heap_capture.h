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

#ifndef CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_CAPTURE_H_
#define CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_CAPTURE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "handler/v8_heap/v8_heap_format.h"

namespace crashpad {

class ProcessSnapshot;
class ProcessMemory;

// Collects a minimal V8 heap snapshot from a crashed process, for emission as
// the kMinidumpStreamTypeV8Heap user-extension stream.
//
// Runs in the out-of-process Crashpad handler against a *suspended* client.
// All memory reads go through ProcessMemory (VirtualQuery-backed
// ReadProcessMemory), so an unreadable region is simply skipped — the handler
// cannot fault. Capture is best-effort and fails closed: a missing heap object
// degrades a frame to "no JS name", never to a wrong name.
class V8HeapCapture {
 public:
  V8HeapCapture();
  ~V8HeapCapture();

  V8HeapCapture(const V8HeapCapture&) = delete;
  V8HeapCapture& operator=(const V8HeapCapture&) = delete;

  // Runs the capture. No-op (leaves captured() == false) for processes without
  // V8 annotations — i.e. non-renderer crashes opt out entirely.
  void Capture(const ProcessSnapshot& snapshot);

  // Serializes the V8HE stream (header + region table + region bytes), all
  // little-endian. Returns an empty buffer unless capture succeeded.
  std::vector<uint8_t> Serialize() const;

  bool captured() const { return captured_; }
  uint64_t cage_base() const { return cage_base_; }
  uint64_t isolate_va() const { return isolate_va_; }

  // Scans the isolate slice for a pointer whose EPT entry for |handle| resolves
  // to a resource with a module vtable and a printable payload of |len| chars.
  // Returns 0 if not found. Exposed for unit testing.
  static uint64_t FindEptBase(const ProcessMemory& memory,
                              const uint8_t* isolate,
                              size_t isolate_size,
                              uint32_t handle,
                              uint32_t len,
                              bool one_byte,
                              const std::vector<std::pair<uint64_t, uint64_t>>&
                                  module_ranges);

 private:
  // Reads [va, va+size) in V8-page-sized chunks; unreadable chunks are skipped.
  // Used for RO space and the isolate slice.
  void CaptureRange(const ProcessMemory& memory, uint64_t va, uint64_t size);

  // Chases the decoder's object chain (JSFunction -> SharedFunctionInfo ->
  // Script -> {name, line_ends}) from each stack-derived cage pointer.
  // Within-cage references are compressed 32-bit pointers; each is followed at
  // the known field offsets in v8_layout.h and validated by its map landing in
  // RO space — so non-objects (random odd stack words) are rejected, never
  // chasing garbage.
  void ChaseFromStacks(const ProcessSnapshot& snapshot,
                       const ProcessMemory& memory);

  // Captures the V8 page containing |va| (if not already present and under the
  // chase cap). Returns true if the page is available in regions_.
  bool CaptureObjectPage(const ProcessMemory& memory, uint64_t va);

  // Reads the compressed pointer at |obj + off|; if it is a tagged cage
  // pointer, captures and validates the target object. Returns the target
  // address, or nullopt if the field is absent/invalid.
  std::optional<uint64_t> FollowCompressed(const ProcessMemory& memory,
                                           uint64_t obj,
                                           uint64_t off);

  // True if |va| looks like a heap object: its map field (compressed at +0) is
  // a tagged pointer whose target's instance type is readable. Captures the
  // map's page if needed (V8 Map objects live in old/map space, not RO).
  bool IsValidHeapObject(const ProcessMemory& memory, uint64_t va);

  // In-memory reads from already-captured regions_. Return nullopt if |va|
  // is not wholly contained in a captured region.
  std::optional<uint32_t> ReadRegionsU32(uint64_t va) const;
  std::optional<uint16_t> ReadRegionsU16(uint64_t va) const;

  // Returns a pointer into a captured region's bytes for [va, va+len), or
  // nullptr if not wholly contained in one. Pointers stay valid across later
  // AddPage calls (std::map nodes are stable).
  const uint8_t* ReadRegions(uint64_t va, size_t len) const;

  // Inserts a page unless one at |va| is already present. Returns true if added.
  bool AddPage(uint64_t va, std::vector<uint8_t> bytes);

  // True if |word| looks like a tagged full (64-bit) cage pointer (low bit set,
  // inside the 4 GB pointer-compression cage).
  bool IsCageHeapPointer(uint64_t word) const;

  // ── External-string / EPT capture (eval / new Function names) ──
  //
  // Script names that are external strings resolve through the sandbox
  // external pointer table (EPT): handle -> EPT entry -> resource -> chars.
  // The bytes for that chain live outside the cage, so they must be captured
  // explicitly. find_ept_base is ported from forensicator-core's analyzer/v8.rs
  // so the collector locates the same table the decoder will use.

  // If |name_va| is an external string, records its EPT handle/length for
  // CaptureExternalStrings. In-memory (the name's page is already captured).
  void MaybeRecordExternalString(uint64_t name_va);

  // Locates the EPT base, captures its reservation, and captures each recorded
  // external string's resource object + char buffer. Best-effort.
  void CaptureExternalStrings(const ProcessMemory& memory);

  // Captures [va, va+size) as a region of arbitrary size (resource/char
  // targets are not page-aligned). Returns true if the region is present.
  bool CaptureBytes(const ProcessMemory& memory, uint64_t va, uint64_t size);

  static std::optional<uint64_t> ReadMemU64(const ProcessMemory& memory,
                                            uint64_t va);
  static bool IsPrintableChars(const ProcessMemory& memory,
                               uint64_t chars,
                               uint32_t len,
                               bool one_byte);

  bool captured_ = false;
  uint64_t cage_base_ = 0;
  uint64_t isolate_va_ = 0;

  // Pages captured so far during the chase (bounded by kMaxChasePages),
  // independent of RO/isolate regions already in |regions_|.
  uint32_t chase_pages_captured_ = 0;

  // External strings discovered during the chase (Script.name objects whose
  // instance type has the external bit).
  struct ExternalString {
    uint32_t handle;
    uint32_t len;
    bool one_byte;
  };
  std::vector<ExternalString> external_strings_;

  // Loaded-module VA ranges, for the resource-vtable-in-module validation in
  // FindEptBase.
  std::vector<std::pair<uint64_t, uint64_t>> module_ranges_;

  struct Region {
    std::vector<uint8_t> bytes;
  };
  // Captured regions keyed by base VA; dedups overlaps by construction.
  std::map<uint64_t, Region> regions_;
};

}  // namespace crashpad

#endif  // CRASHPAD_HANDLER_V8_HEAP_V8_HEAP_CAPTURE_H_
