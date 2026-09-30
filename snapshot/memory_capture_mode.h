// Copyright 2014 The Crashpad Authors
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

#ifndef CRASHPAD_SNAPSHOT_MEMORY_CAPTURE_MODE_H_
#define CRASHPAD_SNAPSHOT_MEMORY_CAPTURE_MODE_H_

namespace crashpad {

//! \brief Selects memory capture and serialization policy.
enum class MemoryCaptureMode {
  kPartial,
  kFullMemory,
};

}  // namespace crashpad

#endif  // CRASHPAD_SNAPSHOT_MEMORY_CAPTURE_MODE_H_
