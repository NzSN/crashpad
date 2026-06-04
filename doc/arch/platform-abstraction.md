# Crashpad Platform Abstraction

Crashpad supports Linux, Android, ChromeOS, macOS, iOS, tvOS, Windows, and Fuchsia. Platform differences are managed through four complementary mechanisms.

## 1. GN Build Conditionals

`build/crashpad_buildconfig.gni` defines boolean flags for platform detection:

```gni
crashpad_is_mac       # macOS
crashpad_is_ios       # iOS
crashpad_is_tvos      # tvOS (subset of iOS)
crashpad_is_apple     # macOS + iOS + tvOS
crashpad_is_win       # Windows
crashpad_is_linux     # Linux
crashpad_is_android   # Android
crashpad_is_fuchsia   # Fuchsia
crashpad_is_posix     # Non-Windows (Apple + Linux + Android + Fuchsia)
```

Every `BUILD.gn` file uses these to conditionally include platform-specific sources:

```gni
if (crashpad_is_linux || crashpad_is_android) {
  sources += [
    "my_class_linux.cc",
    "my_class_linux.h",
  ]
}
if (crashpad_is_win) {
  sources += [
    "my_class_win.cc",
    "my_class_win.h",
  ]
}
```

When built inside Chromium or Fuchsia trees, these are derived from the host build's platform variables (`is_mac`, `is_win`, etc.) rather than `mini_chromium`'s own detection.

## 2. File Naming Conventions

Platform-specific source files use suffixes that match the GN conditionals:

| Suffix | Platform | Example |
|--------|----------|---------|
| `_linux.cc` / `_linux.h` | Linux | `crashpad_client_linux.cc` |
| `_mac.cc` / `_mac.h` | macOS | `crashpad_client_mac.cc` |
| `_win.cc` / `_win.h` | Windows | `crashpad_client_win.cc` |
| `_fuchsia.cc` / `_fuchsia.h` | Fuchsia | `crashpad_client_fuchsia.cc` |
| `_ios.mm` / `_ios.h` | iOS (ObjC++) | `crashpad_client_ios.mm` |
| `_posix.cc` / `_posix.h` | All POSIX platforms | `signals.cc` in `util/posix/` |

Files without a platform suffix contain cross-platform logic or abstract interfaces (`.h` files defining pure virtual classes).

Code within a shared file uses `BUILDFLAG(IS_WIN)`, `BUILDFLAG(IS_LINUX)`, etc. to conditionally compile platform-specific sections.

## 3. The `compat/` Layer

`compat/` provides platform-specific **header shims** — it does not compile any object code. Different include directories are added to the compiler's search path per target platform:

```
compat/
├── mac/          Apple: Availability.h, mach/mach.h, Mach-O headers
├── ios/          iOS: Mach .defs files for exception types
├── linux/        Linux/Android: signal.h, sys/mman.h, sys/ptrace.h
├── android/      Android: elf.h
├── win/          Windows: getopt.h, strings.h, time.h stubs
├── non_win/      Non-Windows: stub headers (dbghelp.h, windows.h)
└── non_mac/      Non-Apple: stub mach/mach.h header
```

The GN config `compat_config` selects which directories to include based on the platform:

```gni
if (crashpad_is_apple) {
  include_dirs += [ "mac" ]
}
if (crashpad_is_linux || crashpad_is_android) {
  include_dirs += [ "linux" ]
}
if (crashpad_is_win) {
  include_dirs += [ "win" ]
} else {
  include_dirs += [ "non_win" ]
}
```

This allows code to `#include <mach/mach.h>` unconditionally — the right header is resolved at compile time based on the platform.

## 4. Abstract Interfaces with Platform Implementations

The primary pattern for platform abstraction: define a pure virtual interface, implement it for each platform.

### ProcessSnapshot

```
snapshot/process_snapshot.h          # Abstract ProcessSnapshot interface
snapshot/linux/process_snapshot_linux.h   # ptrace-based implementation
snapshot/mac/process_snapshot_mac.h       # Mach VM-based implementation
snapshot/win/process_snapshot_win.h       # ReadProcessMemory-based
snapshot/fuchsia/process_snapshot_fuchsia.h
snapshot/ios/process_snapshot_ios.h       # In-process
```

### CrashReportDatabase

```
client/crash_report_database.h           # Abstract interface
client/crash_report_database_generic.cc  # File-based (Linux, Android, Fuchsia)
client/crash_report_database_mac.mm      # macOS with Apple locking
client/crash_report_database_win.cc      # Windows with Win32 locking
```

### CrashpadClient

```
client/crashpad_client.h              # Single header with #if BUILDFLAG()
client/crashpad_client_linux.cc       # Linux StartHandler() impl
client/crashpad_client_mac.cc         # macOS StartHandler() impl
client/crashpad_client_win.cc         # Windows StartHandler() impl
client/crashpad_client_fuchsia.cc     # Fuchsia StartHandler() impl
client/crashpad_client_ios.cc         # iOS in-process impl
```

Here the pattern uses a shared header with `#if BUILDFLAG(...)` guards for platform-specific method signatures, plus separate `.cc` files for each implementation.

### Exception Handling

Each platform has a matching pair of handler and server classes:

| Platform | Handler Delegate | Server |
|----------|-----------------|--------|
| Linux/Android | `handler/linux/crash_report_exception_handler.{h,cc}` | `handler/linux/exception_handler_server.{h,cc}` |
| macOS | `handler/mac/crash_report_exception_handler.{h,cc}` | `handler/mac/exception_handler_server.{h,cc}` |
| Windows | `handler/win/crash_report_exception_handler.{h,cc}` | `util/win/exception_handler_server.{h,cc}` |

## 5. `crashpad_dependencies` Mode

The GN arg `crashpad_dependencies` controls how Crashpad builds and what dependencies it uses:

| Value | Use Case | Test Targets | Dependency Source |
|-------|----------|-------------|-------------------|
| `"standalone"` | Default; standalone builds | Individual test executables | `mini_chromium` via `DEPS` |
| `"chromium"` | Built inside Chromium tree | Single `crashpad_tests` umbrella target | Chromium's `//base`, `//testing` |
| `"fuchsia"` | Built inside Fuchsia tree | Single `crashpad_tests` + Fuchsia component | Fuchsia SDK via build integration |
| `"external"` | Embedded in external projects | Individual test executables | External project provides mini_chromium path |
| `"dart"` | Dart VM embedding | Individual test executables | External project provides mini_chromium path |

This changes:
- How `mini_chromium`'s import path is resolved (`mini_chromium_import_root` changes)
- Whether tests are one executable per subsystem or a single aggregated binary
- Whether Fuchsia-specific test packaging rules apply
- Whether platform flags come from `mini_chromium` or the parent build

## 6. OS-Specific IPC and Memory Access

The fundamental difference between platforms is how the handler accesses the client's memory:

| Platform | Client→Handler IPC | Handler reads client memory |
|----------|-------------------|---------------------------|
| Linux | `AF_UNIX` socket pair + `ExceptionHandlerProtocol` | `ptrace()` + `/proc/pid/mem` |
| Android | Same as Linux | `ptrace()` + `/proc/pid/mem` |
| macOS | Mach exception ports + `ChildPortHandshake` | `mach_vm_read()` via `task_for_pid()` |
| Windows | Named pipe + `RegistrationProtocol` | `ReadProcessMemory()` |
| Fuchsia | Exception port on default job | `zx_process_read_memory()` |
| iOS | None (in-process handler) | Direct pointer access |

These are encapsulated by the `ProcessSnapshot` platform implementations and the `ExceptionHandlerServer` / `ExceptionHandlerClient` platform-specific classes.
