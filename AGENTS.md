# AGENTS.md

## Build system

- **GN + Ninja** — not CMake, Make, or Bazel. GN generates build files; Ninja builds them.
- Dependencies are managed by **gclient** (`DEPS` file), not git submodules.
- The `.gn` file sets `buildconfig = "//build/BUILDCONFIG.gn"` and `script_executable = "python3"`.

```bash
# Standard build
gn gen out/Default
ninja -C out/Default

# Set a GN arg (also editable in out/Default/args.gn)
gn args out/Default --args='is_debug=true target_cpu="x64"'
```

## Build flavor (`crashpad_dependencies`)

Set via the GN arg `crashpad_dependencies`. Valid values: `"standalone"` (default), `"chromium"`, `"fuchsia"`, `"dart"`, `"external"`. This controls which build targets exist and which platform configs apply. When `"standalone"`, individual test executables are separate targets; when `"chromium"` or `"fuchsia"`, there is a single `crashpad_tests` umbrella target.

## Testing

```bash
# Run all tests (canonical command)
python build/run_tests.py out/Default

# Run a single test executable
out/Default/crashpad_minidump_test

# Filter tests within an executable
out/Default/crashpad_minidump_test --gtest_filter=MinidumpStringWriter*

# Filter tests via run_tests.py
python build/run_tests.py out/Default --gtest_filter MinidumpStringWriter\*
```

Test executables: `crashpad_client_test`, `crashpad_handler_test`, `crashpad_minidump_test`, `crashpad_snapshot_test`, `crashpad_test_test`, `crashpad_util_test`. Tests use **Google Test + Google Mock** (via `//test:gtest_main` and `//test:googlemock_main`).

## Architecture

| Dir | Purpose |
|-----|---------|
| `client/` | Crash-reporting client library (`crashpad_client.h`) |
| `handler/` | Out-of-process crash handler (`crashpad_handler`) |
| `minidump/` | Minidump file writers |
| `snapshot/` | System and process snapshot capture |
| `util/` | Platform-agnostic utilities (file, net, process, thread, etc.) |
| `compat/` | Platform-specific compatibility headers (not a library, just includes) |
| `test/` | Test utilities (`main_arguments`, `multiprocess_exec`, etc.) |
| `tools/` | CLI tools (`crashpad_database_util`, `generate_dump`, etc.) |
| `third_party/` | gclient-managed dependencies (do not edit these by hand) |
| `build/` | Build config: `BUILDCONFIG.gn`, `crashpad_buildconfig.gni`, `test.gni`, `run_tests.py` |
| `infra/config/` | Lucicfg config and PRESUBMIT.py for Luci CI |

## Code conventions

- **C++ style**: Chromium (`BasedOnStyle: Chromium` in `.clang-format`), with `BinPackArguments: false`. Python: Google style (`.style.yapf`).
- **Include guards**: `CRASHPAD_DIR_FILE_H_` (e.g. `CRASHPAD_CLIENT_CRASHPAD_CLIENT_H_`).
- **Namespace**: `crashpad` with sub-namespaces (e.g. `crashpad::test`).
- **Includes**: Absolute-path style from repo root — `#include "util/file/file_io.h"` — not relative.
- **Platform-specific sources**: Suffixed by platform (e.g. `*_linux.cc`, `*_mac.cc`, `*_win.cc`, `*_ios.mm`, `*_fuchsia.cc`). Selected via GN conditionals like `crashpad_is_linux`, `crashpad_is_win`, etc. (see `build/crashpad_buildconfig.gni`).
- **`BUILD.gn` files**: Every directory with source code has one. New files must be listed in the appropriate `BUILD.gn` `sources`.
- **License header**: All source files have the Apache 2.0 boilerplate (see existing files for the exact text).

## Code review workflow

- Reviews use Chromium's Gerrit: `git cl upload`. See `doc/developing.md` for full details.
- `codereview.settings` sets `GERRIT_HOST` and points to `chromium-review.googlesource.com`.
- `infra/config/PRESUBMIT.py` checks Lucicfg config changes.
