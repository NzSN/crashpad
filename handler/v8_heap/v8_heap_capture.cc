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

#include "handler/v8_heap/v8_heap_capture.h"

#include "handler/v8_heap/v8_layout.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "base/logging.h"
#include "snapshot/memory_snapshot.h"
#include "snapshot/module_snapshot.h"
#include "snapshot/process_snapshot.h"
#include "snapshot/thread_snapshot.h"
#include "util/process/process_memory.h"

namespace crashpad {

namespace {

// V8 heap page size (v8::Page::kPageSize / MemoryChunk).
constexpr uint64_t kV8PageSize = 256 * 1024;
// Pointer-compression cage size.
constexpr uint64_t kCageSize = 1ULL << 32;

// RO space capture span from the cage base (RO is typically 1-4 MB; the
// unreadable tail is skipped by chunk reads).
constexpr uint64_t kRoSpaceCaptureSpan = 4 * 1024 * 1024;
// Isolate slice (find_ept_base needs roughly this much; 1 MB covered it in the
// reference dump).
constexpr uint64_t kIsolateSliceSpan = 1 * 1024 * 1024;

// Chase bounds: 64 pages bounds the worst-case chase size.
constexpr uint32_t kMaxChasePages = 64;
// EPT reservation capture cap (design doc §5).
constexpr uint64_t kEptReservationCap = 8 * 1024 * 1024;
// Global backstop on total captured regions.
constexpr size_t kMaxRegions = 512;

constexpr uint64_t kCageBaseMask = 0xFFFFFFFF00000000ULL;

inline uint64_t AlignDownToPage(uint64_t va) {
  return va & ~(kV8PageSize - 1);
}

inline uint64_t ReadU64LE(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

bool AddWouldOverflow(uint64_t base, uint64_t size) {
  return size > std::numeric_limits<uint64_t>::max() - base;
}

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Parses a hex annotation value (optional 0x/0X prefix), mirroring the
// dump_js_stack tool's strtoull(_, _, 0).
std::optional<uint64_t> ParseHex(const std::string& s) {
  if (s.empty())
    return std::nullopt;
  errno = 0;
  char* end = nullptr;
  unsigned long long v = strtoull(s.c_str(), &end, 0);
  if (end == s.c_str())
    return std::nullopt;
  return static_cast<uint64_t>(v);
}

// Annotation lookup helpers. V8 keys are published by V8->Blink->base::debug as
// crash keys, surfacing as module AnnotationObjects() and (sometimes) the
// process AnnotationsSimpleMap(). Mirrors dump_js_stack.cc.
std::optional<std::string> FindAnnotationExact(const ProcessSnapshot& snapshot,
                                               const std::string& key) {
  for (const auto& [k, v] : snapshot.AnnotationsSimpleMap()) {
    if (k == key)
      return v;
  }
  for (const auto* mod : snapshot.Modules()) {
    for (const auto& obj : mod->AnnotationObjects()) {
      if (obj.name == key) {
        return std::string(reinterpret_cast<const char*>(obj.value.data()),
                           obj.value.size());
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> FindAnnotationContaining(
    const ProcessSnapshot& snapshot,
    const std::string& needle) {
  for (const auto& [k, v] : snapshot.AnnotationsSimpleMap()) {
    if (k.find(needle) != std::string::npos)
      return v;
  }
  for (const auto* mod : snapshot.Modules()) {
    for (const auto& obj : mod->AnnotationObjects()) {
      if (obj.name.find(needle) != std::string::npos) {
        return std::string(reinterpret_cast<const char*>(obj.value.data()),
                           obj.value.size());
      }
    }
  }
  return std::nullopt;
}

}  // namespace

V8HeapCapture::V8HeapCapture() = default;
V8HeapCapture::~V8HeapCapture() = default;

void V8HeapCapture::Capture(const ProcessSnapshot& snapshot) {
  const ProcessMemory* memory = snapshot.Memory();
  if (!memory)
    return;

  auto isolate_str = FindAnnotationExact(snapshot, "v8_isolate_address");
  auto ro_str = FindAnnotationContaining(snapshot, "ro_space");
  if (!isolate_str || !ro_str)
    return;  // not a V8/renderer process

  auto isolate = ParseHex(*isolate_str);
  auto ro_first = ParseHex(*ro_str);
  if (!isolate || !ro_first || !*isolate || !*ro_first)
    return;

  isolate_va_ = *isolate;
  // Pointer-compression cage base = high 32 bits of the RO-space first page
  // (RO space begins at the cage start in shared-cage builds). Mirrors
  // v8_frame_reader.cc.
  cage_base_ = *ro_first & kCageBaseMask;

  // 1. Read-only space: maps + internalized short names (every instance-type
  //    check and minified name lives here).
  CaptureRange(*memory, cage_base_, kRoSpaceCaptureSpan);
  // 2. Isolate slice: needed by the EPT-base discovery scan (decode-side).
  CaptureRange(*memory, isolate_va_, kIsolateSliceSpan);
  // 3. Heap pages reachable from the stacks via the decoder's object chain
  //    (JSFunction -> SFI -> Script -> name / line_ends). Within-cage refs are
  //    compressed 32-bit pointers, followed at known offsets.
  ChaseFromStacks(snapshot, *memory);

  // 4. External-string names (eval / new Function) resolve through the
  //    external pointer table; their resource/char targets live outside the
  //    cage and are chased individually.
  for (const auto* mod : snapshot.Modules())
    module_ranges_.emplace_back(mod->Address(), mod->Size());
  CaptureExternalStrings(*memory);

  captured_ = !regions_.empty();
  if (captured_) {
    LOG(INFO) << "V8HE: captured " << regions_.size()
              << " regions, cage=0x" << std::hex << cage_base_ << std::dec
              << " isolate=0x" << std::hex << isolate_va_ << std::dec;
  }
}

void V8HeapCapture::CaptureRange(const ProcessMemory& memory,
                                 uint64_t va,
                                 uint64_t size) {
  if (size == 0 || AddWouldOverflow(va, size))
    return;
  // Read in 4 KiB sub-chunks and coalesce contiguous readable chunks into a
  // single region. A committed region may be smaller than a V8 page (RO space
  // is observed as a 64 KiB read-only region), so a full 256 KiB page read
  // would overshoot into unmapped space and fail — dropping RO entirely.
  // Coalescing keeps each committed span as one contiguous region (the decoder
  // reads Maps from RO and scans the isolate as one region for EPT discovery).
  constexpr uint64_t kSubChunk = 4 * 1024;
  const uint64_t start = va & ~(kSubChunk - 1);
  const uint64_t end = va + size;
  uint64_t run_start = 0;
  std::vector<uint8_t> run;
  for (uint64_t a = start; a < end;) {
    std::vector<uint8_t> buf(kSubChunk);
    if (memory.Read(a, kSubChunk, buf.data())) {
      if (run.empty())
        run_start = a;
      run.insert(run.end(), buf.begin(), buf.end());
    } else {
      if (!run.empty()) {
        AddPage(run_start, std::move(run));
        run.clear();
        if (regions_.size() >= kMaxRegions)
          break;
      }
    }
    if (AddWouldOverflow(a, kSubChunk))
      break;
    a += kSubChunk;
  }
  if (!run.empty())
    AddPage(run_start, std::move(run));
}

void V8HeapCapture::ChaseFromStacks(const ProcessSnapshot& snapshot,
                                    const ProcessMemory& memory) {
  // Seed: scan each thread stack for full (64-bit) tagged cage pointers. The
  // stack holds JSFunction pointers as full pointers; each candidate is probed
  // as a JSFunction by following the decoder's chain. Non-objects are rejected
  // by IsValidHeapObject (map must land in RO space), so random stack words
  // never chase garbage.
  for (const auto* thread : snapshot.Threads()) {
    const MemorySnapshot* stack = thread ? thread->Stack() : nullptr;
    if (!stack || stack->Size() == 0)
      continue;
    std::vector<uint8_t> buf(stack->Size());
    if (!memory.Read(stack->Address(), stack->Size(), buf.data()))
      continue;
    for (size_t i = 0; i + 8 <= buf.size(); i += 8) {
      const uint64_t w = ReadU64LE(&buf[i]);
      if (!IsCageHeapPointer(w))
        continue;
      const uint64_t jsf = w & ~1ULL;
      if (!CaptureObjectPage(memory, jsf) || !IsValidHeapObject(memory, jsf))
        continue;

      // JSFunction -> SharedFunctionInfo.
      const auto sfi =
          FollowCompressed(memory, jsf, v8_layout::kJsfSharedFunctionInfo);
      if (!sfi)
        continue;
      // SharedFunctionInfo -> name_or_scope_info and Script.
      FollowCompressed(memory, *sfi, v8_layout::kSfiNameOrScopeInfo);
      const auto script = FollowCompressed(memory, *sfi, v8_layout::kSfiScript);
      if (script) {
        // Script -> name and line_ends.
        const auto name =
            FollowCompressed(memory, *script, v8_layout::kScriptName);
        if (name)
          MaybeRecordExternalString(*name);  // stage 4: external strings
        FollowCompressed(memory, *script, v8_layout::kScriptLineEnds);
      }
    }
  }
}

bool V8HeapCapture::CaptureObjectPage(const ProcessMemory& memory, uint64_t va) {
  const uint64_t page = AlignDownToPage(va);
  if (regions_.count(page))
    return true;  // already captured
  if (chase_pages_captured_ >= kMaxChasePages)
    return false;  // chase cap reached
  std::vector<uint8_t> buf(kV8PageSize);
  if (!memory.Read(page, kV8PageSize, buf.data()))
    return false;  // freed/unmapped page
  AddPage(page, std::move(buf));
  ++chase_pages_captured_;
  return true;
}

std::optional<uint64_t> V8HeapCapture::FollowCompressed(
    const ProcessMemory& memory, uint64_t obj, uint64_t off) {
  // The field is a compressed (32-bit) tagged cage pointer.
  const auto c = ReadRegionsU32(obj + off);  // obj's page is already captured
  if (!c || (*c & 1) == 0)
    return std::nullopt;  // Smi/null
  const uint64_t target = cage_base_ + static_cast<uint64_t>(*c & ~1u);
  if (!CaptureObjectPage(memory, target) || !IsValidHeapObject(memory, target))
    return std::nullopt;
  return target;
}

bool V8HeapCapture::IsValidHeapObject(const ProcessMemory& memory,
                                      uint64_t va) {
  // A heap object's map field (compressed at +0) is a tagged pointer. V8 Map
  // objects live in old/map space, NOT RO space, so the map can be at any cage
  // offset — do NOT assume it is within the RO span. Capture the map's page
  // (the decoder reads the instance type at map+8) and validate by reading it.
  const auto map_c = ReadRegionsU32(va + v8_layout::kMapFieldOffset);
  if (!map_c || (*map_c & 1) == 0)
    return false;
  const uint64_t map = cage_base_ + static_cast<uint64_t>(*map_c & ~1u);
  if (!ReadRegionsU16(map + v8_layout::kMapInstanceTypeOffset))
    CaptureObjectPage(memory, map);
  return ReadRegionsU16(map + v8_layout::kMapInstanceTypeOffset).has_value();
}

const uint8_t* V8HeapCapture::ReadRegions(uint64_t va, size_t len) const {
  auto it = regions_.upper_bound(va);
  if (it == regions_.begin())
    return nullptr;
  --it;
  const auto& bytes = it->second.bytes;
  const uint64_t start = it->first;
  if (va < start)
    return nullptr;
  const uint64_t offset = va - start;
  if (offset > bytes.size() ||
      static_cast<uint64_t>(len) > bytes.size() - offset)
    return nullptr;
  return bytes.data() + static_cast<size_t>(offset);
}

std::optional<uint32_t> V8HeapCapture::ReadRegionsU32(uint64_t va) const {
  const uint8_t* p = ReadRegions(va, 4);
  if (!p)
    return std::nullopt;
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

std::optional<uint16_t> V8HeapCapture::ReadRegionsU16(uint64_t va) const {
  const uint8_t* p = ReadRegions(va, 2);
  if (!p)
    return std::nullopt;
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

bool V8HeapCapture::AddPage(uint64_t va, std::vector<uint8_t> bytes) {
  return regions_.emplace(va, Region{std::move(bytes)}).second;
}

bool V8HeapCapture::IsCageHeapPointer(uint64_t word) const {
  if (!(word & 1))
    return false;  // not a tagged heap pointer
  const uint64_t addr = word & ~1ULL;
  return addr >= cage_base_ && addr < cage_base_ + kCageSize;
}

void V8HeapCapture::MaybeRecordExternalString(uint64_t name_va) {
  const auto map_c = ReadRegionsU32(name_va + v8_layout::kMapFieldOffset);
  if (!map_c || (*map_c & 1) == 0)
    return;
  const uint64_t map = cage_base_ + (*map_c & ~1u);
  const auto itype = ReadRegionsU16(map + v8_layout::kMapInstanceTypeOffset);
  if (!itype || *itype >= v8_layout::kFirstNonStringType)
    return;
  // Representation is a 3-bit value. Sliced strings (tag 3) share bit 1 with
  // external strings (tag 2); compare the field, do not test the bit.
  if ((*itype & v8_layout::kStringRepresentationMask) !=
      v8_layout::kExternalStringTag) {
    return;
  }
  const auto handle = ReadRegionsU32(name_va + v8_layout::kStringChars);
  // Uncached external strings end after resource_. The following word belongs
  // to the next object, not to resource_data_.
  const bool uncached =
      (*itype & v8_layout::kStringUncachedExternalBit) != 0;
  const uint32_t data_handle =
      uncached ? 0
               : ReadRegionsU32(name_va + v8_layout::kStringResourceData)
                     .value_or(0);
  const auto len = ReadRegionsU32(name_va + v8_layout::kStringLength);
  if (!handle || *handle == 0 || !len || *len == 0 ||
      *len > v8_layout::kMaxNameLen) {
    return;
  }
  external_strings_.push_back({*handle,
                               *len,
                               data_handle,
                               (*itype & v8_layout::kStringOneByteBit) != 0});
}

void V8HeapCapture::CaptureExternalStrings(const ProcessMemory& memory) {
  if (external_strings_.empty())
    return;

  // The pinned build stores ExternalPointerTable* at a fixed IsolateData
  // slot. There is no runtime V8 version key, so a value at that slot is
  // used only when an entry has the expected tag and its payload is readable
  // for the recorded length. Anything else falls back to the scan.
  uint64_t ept_base = 0;
  std::optional<uint64_t> shared_table;
  if (!AddWouldOverflow(isolate_va_,
                        v8_layout::kIsolateSharedExternalPointerTableOffset)) {
    shared_table = ReadMemU64(
        memory,
        isolate_va_ + v8_layout::kIsolateSharedExternalPointerTableOffset);
    if (shared_table && *shared_table != 0 &&
        !AddWouldOverflow(*shared_table, 8)) {
      ept_base = ReadMemU64(memory, *shared_table).value_or(0);
    }
  }
  bool valid_base = false;
  for (const ExternalString& es : external_strings_) {
    const auto resource =
        ReadEptPointer(memory, ept_base, es.handle, v8_layout::kEptResourceTag);
    // resource+8 holds cached_data_; the following load reads 8 bytes.
    if (!resource || AddWouldOverflow(*resource, 16))
      continue;
    const size_t bytes =
        es.one_byte ? es.len : static_cast<size_t>(es.len) * 2;
    std::vector<uint8_t> probe(bytes);
    const auto readable = [&](std::optional<uint64_t> chars) {
      return chars && *chars != 0 && !AddWouldOverflow(*chars, bytes) &&
             memory.Read(*chars, bytes, probe.data());
    };
    if (readable(ReadEptPointer(memory,
                                ept_base,
                                es.data_handle,
                                v8_layout::kEptResourceDataTag)) ||
        readable(ReadMemU64(
            memory, *resource + v8_layout::kExternalResourceCachedDataOffset))) {
      valid_base = true;
      break;
    }
  }
  if (valid_base && shared_table) {
    // Keep the table object so a decoder can repeat the two-word load.
    CaptureBytes(memory, *shared_table, 64);
  } else {
    // Preserve gaps and the unaligned isolate origin. CaptureRange coalesces
    // 4 KiB chunks, which need not start on a 256 KiB V8 page.
    std::vector<uint8_t> isolate(kIsolateSliceSpan, 0);
    for (size_t off = 0; off + 8 <= isolate.size(); off += 8) {
      if (AddWouldOverflow(isolate_va_, off))
        break;
      const uint8_t* bytes = ReadRegions(isolate_va_ + off, 8);
      if (bytes)
        std::memcpy(isolate.data() + off, bytes, 8);
    }
    const ExternalString& seed = external_strings_.front();
    ept_base = FindEptBase(memory,
                           isolate.data(),
                           isolate.size(),
                           seed.handle,
                           seed.len,
                           seed.one_byte,
                           module_ranges_);
  }
  if (ept_base == 0)
    return;

  CaptureRange(memory, ept_base, kEptReservationCap);

  for (const ExternalString& es : external_strings_) {
    // Character length is already <= kMaxNameLen. Two-byte payloads are
    // twice that many bytes; do not truncate them to the character cap.
    const uint64_t byte_len =
        es.one_byte ? es.len : static_cast<uint64_t>(es.len) * 2;

    bool captured_chars = false;
    const auto data = ReadEptPointer(
        memory, ept_base, es.data_handle, v8_layout::kEptResourceDataTag);
    if (data)
      captured_chars = CaptureBytes(memory, *data, byte_len);

    const auto resource =
        ReadEptPointer(memory, ept_base, es.handle, v8_layout::kEptResourceTag);
    if (!resource || AddWouldOverflow(*resource, 16))
      continue;
    CaptureBytes(memory, *resource, 64);
    if (captured_chars)
      continue;
    const auto chars = ReadMemU64(
        memory, *resource + v8_layout::kExternalResourceCachedDataOffset);
    if (chars && *chars != 0)
      CaptureBytes(memory, *chars, byte_len);
  }
}

bool V8HeapCapture::CaptureBytes(const ProcessMemory& memory,
                                 uint64_t va,
                                 uint64_t size) {
  if (size == 0 || AddWouldOverflow(va, size))
    return false;
  if (regions_.count(va))
    return true;
  if (regions_.size() >= kMaxRegions)
    return false;
  std::vector<uint8_t> buf(size);
  if (!memory.Read(va, size, buf.data()))
    return false;
  AddPage(va, std::move(buf));
  return true;
}

uint64_t V8HeapCapture::FindEptBase(
    const ProcessMemory& memory,
    const uint8_t* isolate,
    size_t isolate_size,
    uint32_t handle,
    uint32_t len,
    bool one_byte,
    const std::vector<std::pair<uint64_t, uint64_t>>& module_ranges) {
  const auto in_module = [&module_ranges](uint64_t va) {
    for (const auto& [base, size] : module_ranges) {
      if (AddWouldOverflow(base, size))
        continue;
      if (va >= base && va < base + size)
        return true;
    }
    return false;
  };
  // Pass 1 rejects payloads inside the candidate table's own window; pass 2
  // accepts any validated payload. Mirrors forensicator find_ept_base.
  constexpr uint64_t kCandidateWindow = 2ULL << 20;
  for (const bool reject_internal : {true, false}) {
    for (size_t off = 0; off + 8 <= isolate_size; off += 8) {
      uint64_t b;
      std::memcpy(&b, isolate + off, 8);
      const auto pointer =
          ReadEptPointer(memory, b, handle, v8_layout::kEptResourceTag);
      if (!pointer || AddWouldOverflow(*pointer, 16))
        continue;
      const uint64_t resource = *pointer;
      if (reject_internal &&
          (AddWouldOverflow(b, kCandidateWindow) ||
           (resource >= b && resource < b + kCandidateWindow))) {
        continue;
      }
      if (in_module(resource))
        continue;  // resource must be a heap object, not module code
      const auto vtable = ReadMemU64(memory, resource);
      if (!vtable || !in_module(*vtable))
        continue;  // resource's vtable must be inside a loaded module
      const auto chars = ReadMemU64(
          memory, resource + v8_layout::kExternalResourceCachedDataOffset);
      if (!chars)
        continue;
      if (IsPrintableChars(memory, *chars, len, one_byte))
        return b;
    }
  }
  return 0;
}

std::optional<uint64_t> V8HeapCapture::ReadEptPointer(
    const ProcessMemory& memory,
    uint64_t base,
    uint32_t handle,
    uint64_t tag) {
  const uint32_t handle_mask = (1u << v8_layout::kEptIndexShift) - 1;
  if (base < 0x10000 || (base & 7) != 0 || handle == 0 ||
      (handle & handle_mask) != 0) {
    return std::nullopt;
  }
  const uint64_t offset =
      v8_layout::kEptEntrySize * (handle >> v8_layout::kEptIndexShift);
  // A shifted handle addresses one 8-byte entry inside the 512 MB reservation.
  if (offset > v8_layout::kEptReservationSize - 8 ||
      AddWouldOverflow(base, offset + 8)) {
    return std::nullopt;
  }
  const auto entry = ReadMemU64(memory, base + offset);
  if (!entry || (*entry & v8_layout::kEptTagMask) != tag)
    return std::nullopt;
  const uint64_t pointer = *entry & v8_layout::kEptPayloadMask;
  return pointer != 0 ? std::optional<uint64_t>(pointer) : std::nullopt;
}

std::optional<uint64_t> V8HeapCapture::ReadMemU64(const ProcessMemory& memory,
                                                  uint64_t va) {
  uint8_t b[8];
  if (!memory.Read(va, 8, b))
    return std::nullopt;
  uint64_t v;
  std::memcpy(&v, b, 8);
  return v;
}

bool V8HeapCapture::IsPrintableChars(const ProcessMemory& memory,
                                     uint64_t chars,
                                     uint32_t len,
                                     bool one_byte) {
  if (len == 0 || len > v8_layout::kMaxNameLen || chars == 0)
    return false;
  const size_t bytes = one_byte ? len : static_cast<size_t>(len) * 2;
  if (AddWouldOverflow(chars, bytes))
    return false;
  std::vector<uint8_t> buf(bytes);
  if (!memory.Read(chars, bytes, buf.data()))
    return false;
  const auto printable = [](uint32_t c) {
    return (c >= 0x20 && c <= 0x7e) || c >= 0x80;
  };
  if (one_byte) {
    for (uint8_t c : buf)
      if (!printable(c))
        return false;
  } else {
    for (size_t i = 0; i + 2 <= bytes; i += 2)
      if (!printable(buf[i] | (buf[i + 1] << 8)))
        return false;
  }
  return true;
}

std::vector<uint8_t> V8HeapCapture::Serialize() const {
  const uint32_t region_count = static_cast<uint32_t>(regions_.size());
  // Header = 4+4+8+8+4+4 = 32; each region table entry = 24.
  constexpr size_t kHeaderBytes = 32;
  const size_t table_bytes = static_cast<size_t>(region_count) * 24;
  const uint64_t data_start = kHeaderBytes + table_bytes;

  size_t total_data = 0;
  for (const auto& [va, region] : regions_)
    total_data += region.bytes.size();

  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + table_bytes + total_data);

  AppendU32(out, kMinidumpStreamTypeV8Heap);
  AppendU32(out, kV8HeapStreamVersion);
  AppendU64(out, cage_base_);
  AppendU64(out, isolate_va_);
  AppendU32(out, region_count);
  AppendU32(out, /*flags*/ 0);  // full heap (bit1 = partial heap, clear)

  uint64_t offset = data_start;
  for (const auto& [va, region] : regions_) {
    AppendU64(out, va);
    AppendU64(out, region.bytes.size());
    AppendU64(out, offset);
    offset += region.bytes.size();
  }

  for (const auto& [va, region] : regions_) {
    out.insert(out.end(), region.bytes.begin(), region.bytes.end());
  }

  return out;
}

}  // namespace crashpad
