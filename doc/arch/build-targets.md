# Build Target Dependency Graph

## Top-Level Targets (`BUILD.gn`)

The root `BUILD.gn` defines the test executables. The exact targets depend on `crashpad_dependencies`:

### Standalone / External / Dart Mode

```
crashpad_client_test    → client:client_test + test:googlemock_main
crashpad_handler_test   → handler:handler_test + test:googletest_main
crashpad_minidump_test  → minidump:minidump_test + test:googletest_main
crashpad_snapshot_test  → snapshot:snapshot_test + test:googlemock_main
crashpad_test_test      → test:test_test + test:googlemock_main
crashpad_util_test      → util:util_test + test:googlemock_main
```

### Chromium / Fuchsia Mode

A single umbrella target: `crashpad_tests` combining all subsystem test suites.

---

## Library Dependency Graph

Arrows show `deps` (compile-time dependency). "→ public_deps" means the dependency is also exposed to dependents via `public_deps`.

```
crashpad_handler (executable)
  → handler:handler
      → handler:common
      → client:client
      → minidump:minidump
      → snapshot:snapshot
      → tools:tool_support

handler:common
  → client:common
  → minidump:minidump
  → snapshot:snapshot
  → util
  → util:net

handler:handler  (also depends on handler:common)
  → client:client
  → minidump:minidump
  → snapshot:snapshot
  → tools:tool_support

client:client
  → client:common
  → util
  → compat
  ── public_deps → third_party/mini_chromium:base

client:common
  → util
  → compat

minidump:minidump
  → minidump:format
  → snapshot:context     ⬅── depends only on context, NOT full snapshot!
  → util
  ── public_deps → compat

minidump:format
  → snapshot:context
  → util
  ── public_deps → compat

snapshot:snapshot
  → snapshot:context
  → minidump:format      ⬅── depends on format, NOT full minidump writer!
  → client:common        (for CrashpadInfoClientOptions)
  → util
  → compat

snapshot:context
  → compat
  → util

util
  → compat
  → third_party/zlib
  ── public_deps → third_party/mini_chromium:base

util:net
  → util
  → compat

compat (headers only)
  ── public_deps → third_party/mini_chromium:base
  ── public_configs → .:crashpad_config
```

## Avoiding Circular Dependencies

The minidump and snapshot layers have a mutual dependency problem:
- `minidump` writes snapshot data → depends on `snapshot` types
- `snapshot` can read minidumps → depends on `minidump` constants

**Solution: split-level dependencies.**

1. `snapshot:context` contains only `CPUContext` and `CPUArchitecture` — pure data types with no snapshot logic.
2. `minidump:format` contains only `MINIDUMP_*` extension constants and `MinidumpContext` struct — pure format definitions with no writer logic.
3. `minidump:minidump` depends on `snapshot:context` (fine, it's just data types).
4. `snapshot:snapshot` depends on `minidump:format` (fine, it's just format constants).
5. Neither `minidump:minidump` depends on `snapshot:snapshot`, nor vice versa.

```
snapshot:context  ───────────────────────────────→  consumed by minidump writers
       ↕                                           ↕
snapshot:snapshot  ←── minidump:format ←── minidump:minidump
```

## Key Third-Party Dependencies

| Target | Provides |
|--------|----------|
| `third_party/mini_chromium:base` | `base::FilePath`, `base::TaskRunner`, `base::AutoReset`, logging, string utilities — Chromium-like base library, slimmed down |
| `third_party/googletest:gtest` | Google Test framework |
| `third_party/googletest:gmock` | Google Mock framework |
| `third_party/zlib:zlib` | Compression library (used by minidump compression and gzip uploads) |
| `third_party/lss:lss` | Linux Syscall Support — raw system call wrappers for async-signal-safe code |
| `third_party/getopt:getopt` | `getopt()` implementation for Windows |

## Test Support Targets

| Target | Purpose |
|--------|---------|
| `test:googlemock_main` | `main()` that invokes Google Mock test runner |
| `test:googletest_main` | `main()` that invokes Google Test runner |
| `test:test` | MultiprocessExec, ScopedTempDir, test helpers |
| `snapshot:test_support` | `TestProcessSnapshot`, test doubles for snapshot interfaces |
| `minidump:test_support` | Test utilities for minidump writer validation |

## GN Build Configuration Files

| File | Purpose |
|------|---------|
| `build/crashpad_buildconfig.gni` | Platform detection flags, `crashpad_static_library` template, `crashpad_executable` template |
| `build/test.gni` | `test()` template — generates executables (standalone) or XCTest bundles (iOS) |
| `build/crashpad_fuzzer_test.gni` | Fuzzer test template (libfuzzer integration) |
| `build/run_tests.py` | Test runner script — discovers and executes test binaries, supports Android and iOS |
