# Crashpad Layers

## Client Layer (`client/`)

The client library is linked into the application being monitored. Its main entry point is `CrashpadClient` (`client/crashpad_client.h`).

### Core Classes

**`CrashpadClient`**
- Primary public API. Applications call `StartHandler()` to begin crash monitoring.
- `StartHandler()` launches the `crashpad_handler` binary and performs a platform-specific handshake:
  - **Linux**: forks the handler, sets up `AF_UNIX` socket pair, installs signal handlers
  - **macOS**: starts handler via `ChildPortHandshake`, sets Mach exception ports
  - **Windows**: launches handler process, sets up named pipe, installs unhandled exception filter
  - **Fuchsia**: binds to the default job's exception port
  - **iOS**: initializes in-process handler (no separate process)

**`Annotation`** (`client/annotation.h`)
- Base class for name-value pairs stored in the crashing process's static memory.
- All annotations form a global linked list (`AnnotationList`) so the handler can find them by reading the client's memory.
- `StringAnnotation<MaxSize>` is a template for NUL-terminated string annotations.
- Maximum value size: 5 × 4096 bytes. Maximum name length: 256 bytes.

**`CrashpadInfo`** (`client/crashpad_info.h`)
- Structure embedded at a known location in the client's address space.
- Contains: `simple_annotations` pointer, `annotation_objects` pointer, `client_id`, `module_list`, `crashpad_handler_behavior`, `system_crash_reporter_forwarding`, `user_data_minidump_stream_head`.
- The handler reads this structure to locate annotations and other metadata in the crashed process.

**`CrashReportDatabase`** (`client/crash_report_database.h`)
- Abstract interface for managing crash report files and metadata on disk.
- Three-stage lifecycle:
  1. **New** — `PrepareNewCrashReport()` opens a writer for a new report
  2. **Pending** — `FinishedWritingCrashReport()` marks report ready for upload
  3. **Completed** — `RecordUploadComplete()` or `SkipReportUpload()` marks it done
- Each report has a UUID, file path, creation time, upload attempt count, and optional attachments.
- Platform implementations:
  - `crash_report_database_generic.cc` — Linux, Android, Fuchsia (file-based)
  - `crash_report_database_mac.mm` — macOS (file-based, with locking)
  - `crash_report_database_win.cc` — Windows (file-based, with locking)

**`Settings`** (`client/settings.h`)
- Persistent key-value store located alongside the database.
- Keys: `client_id` (UUID), `uploads_enabled` (boolean), `last_upload_attempt_time`.
- `SettingsReader` provides read-only access; `Settings` extends with write access.
- Concurrency controlled via file locking (`flock` or lockfiles).

### Build Targets
- `client:common` — annotations, database, settings, ring buffer, CrashpadInfo
- `client:client` — CrashpadClient + platform-specific implementations
- `client:pthread_create` — Linux hook to auto-initialize signal stacks per thread

---

## Handler Layer (`handler/`)

The handler is a separate process (`crashpad_handler` binary). It receives crash notifications, captures process state, writes minidumps, and uploads reports.

### Entry Points

- **POSIX** — `main()` in `handler/main.cc` calls `HandlerMain()`
- **Windows** — `wWinMain()` in `handler/main.cc` adapts wide-char args to `HandlerMain()`
- **Android** — `CrashpadHandlerMain()` in `handler/crashpad_handler_main.cc` is the `extern "C"` entry for `app_process` / dynamic linker loading

### `HandlerMain()` Flow (`handler/handler_main.cc`)

1. Parse command-line options: `--database`, `--url`, `--annotation`, `--attachment`, platform-specific connection parameters (`--initial-client-fd`, `--handshake-fd`, `--pipe-name`, etc.)
2. Initialize `CrashReportDatabase` at the configured path
3. Optionally start `CrashReportUploadThread` if `--url` is provided
4. Create a platform-specific exception handler delegate (`CrashReportExceptionHandler`)
5. Start `ExceptionHandlerServer` to listen for client crash notifications
6. Optionally start `PruneCrashReportThread` for cleaning old reports
7. Optionally `MonitorSelf()` — starts a second handler to catch crashes in the primary handler

### Key Handler Classes

**`CrashReportExceptionHandler`** (platform-specific: `handler/{linux,mac,win}/`)
- Implements `ExceptionHandlerServer::Delegate`
- `HandleException()` is called when the server receives a crash dump request
- Calls `CaptureSnapshot()` to create a `ProcessSnapshot` of the crashed client
- Writes the minidump via `MinidumpFileWriter::InitializeFromSnapshot()` and `WriteEverything()`
- Stores the report in the `CrashReportDatabase` and notifies the upload thread

**`ExceptionHandlerServer`** (platform-specific: `handler/{linux,mac}/` or `util/win/`)
- Listens for client connections over the platform IPC channel
- Each client sends crash context information (addresses, ports, handles)
- Delegates crash handling to the platform's `CrashReportExceptionHandler`

**`CrashReportUploadThread`** (`handler/crash_report_upload_thread.h`)
- Runs on a background `WorkerThread`
- Periodically scans database for pending reports (every 15 minutes by default)
- Immediately triggered via `ReportPending(uuid)` when a new crash is processed
- Uploads reports via HTTP multipart POST to the configured server URL
- Three outcomes per report: `kSuccess` (uploaded), `kPermanentFailure` (abandon), `kRetry` (retry later)
- Options: rate limiting, gzip compression, client identification in URL

**`MinidumpToUploadParameters`** (`handler/minidump_to_upload_parameters.h`)
- Extracts key-value annotations from a `ProcessSnapshot` for HTTP form parameters
- Combines annotations from: process simple annotations, module simple annotations, module annotation objects
- Adds `"guid"` (client ID) and `"list_annotations"` keys

**`UserStreamDataSource`** (`handler/user_stream_data_source.h`)
- Extension point for embedders to inject custom minidump streams
- `ProduceStreamData()` is called for each crash; returns optional byte stream data

**`PruneCrashReportsThread`** (`handler/prune_crash_reports_thread.h`)
- Background thread that periodically prunes old crash reports from the database

### Build Targets
- `handler:common` — upload thread, upload rate limit, minidump-to-upload-params, user stream source
- `handler:handler` — HandlerMain + platform-specific exception handlers + servers
- `handler:crashpad_handler` — executable (`main.cc`)

---

## Snapshot Layer (`snapshot/`)

The snapshot layer captures a frozen picture of the crashed process at the moment of the crash. It uses abstract interfaces with platform-specific implementations.

### Abstract Interfaces

| Interface | Purpose |
|-----------|---------|
| `ProcessSnapshot` | Top-level: PID, parent PID, timestamps, UUIDs, annotations, system snapshot, modules, threads, exception, memory map, handles, extra memory |
| `SystemSnapshot` | OS name, version, build; CPU architecture and features; timezone; NX/DEP status |
| `ThreadSnapshot` | CPU context (registers), stack memory, thread ID, name, priority, thread-specific data |
| `ModuleSnapshot` | Path, base address, size, UUID/build ID, version, annotations, extra memory ranges |
| `ExceptionSnapshot` | Exception code, flags, address, number of parameters; CPU context of the crashing thread |
| `MemorySnapshot` | Address, size; uses a `Delegate` pattern for lazy memory reading |
| `CPUContext` | Union type covering x86, x86_64, ARM, ARM64, MIPS, MIPS64, RISCV64 register sets |

### Platform-Specific Implementations

| Subdirectory | Primary Classes | Memory Access |
|-------------|----------------|---------------|
| `snapshot/linux/` | `ProcessSnapshotLinux`, `SystemSnapshotLinux`, `ProcessReaderLinux` | `ptrace()` + `/proc/pid/` |
| `snapshot/mac/` | `ProcessSnapshotMac`, `ProcessReaderMac`, Mach-O readers | `mach_vm_read()` + `task_for_pid()` |
| `snapshot/win/` | `ProcessSnapshotWin`, `ProcessReaderWin`, PE readers | `ReadProcessMemory()` |
| `snapshot/fuchsia/` | `ProcessSnapshotFuchsia`, `ProcessReaderFuchsia` | `zx_process_read_memory()` |
| `snapshot/ios/` | Intermediate dump-based snapshots | In-process direct memory |

**Shared subdirectories:**
- `snapshot/elf/` — ELF image reader (Linux, Android, Fuchsia)
- `snapshot/crashpad_types/` — Crashpad-specific data readers (`CrashpadInfo`, `ImageAnnotationReader`)
- `snapshot/sanitized/` — `ProcessSnapshotSanitized` wraps another snapshot, redacting sensitive fields
- `snapshot/minidump/` — Reads a minidump file as a `ProcessSnapshot` (converts minidump-to-snapshot)

### Snapshot Pattern (Linux Example)
1. `PtraceConnection` is established (via `ptrace(PTRACE_ATTACH)`) to the client
2. `ProcessReaderLinux` reads `/proc/[pid]/` files and enumerates threads, modules, memory maps
3. `ProcessSnapshotLinux::Initialize()` composes the snapshot from reader data:
   - Threads: `ThreadSnapshotLinux` with CPU context from `PTRACE_GETREGS`
   - Modules: `ModuleSnapshotElf` reading ELF headers and build IDs
   - Exception: `ExceptionSnapshotLinux` with signal info
   - System: `SystemSnapshotLinux` from `uname()` and CPUID

### Build Targets
- `snapshot:context` — `CPUContext` + `CPUArchitecture` (only part that `minidump` depends on)
- `snapshot:snapshot` — full snapshot library with all platform implementations

---

## Minidump Layer (`minidump/`)

Writes [Windows minidump format](https://msdn.microsoft.com/en-us/library/windows/desktop/ms680369.aspx) files.

### Base: `MinidumpWritable` (`minidump/minidump_writable.h`)

Abstract base class for all minidump content. Uses a **composite pattern**: objects form a tree where parent objects manage children.

**Four-state lifecycle:**
1. `kStateMutable` — can add children and set data
2. `kStateFrozen` — sizes are known, RVAs/locations can be resolved
3. `kStateWritable` — absolute file offset is known
4. `kStateWritten` — data has been serialized

**Two-phase writing:**
- `kPhaseEarly` — most data (headers, directories, metadata)
- `kPhaseLate` — large memory regions for spatial locality in the output file

**Automatic RVA resolution:** Child objects call `RegisterRVA()` or `RegisterLocationDescriptor()` on their parents during the freeze phase. When file offsets are assigned during `WillWriteAtOffset()`, all registered pointers are automatically updated.

### Root: `MinidumpFileWriter` (`minidump/minidump_file_writer.h`)

The root of the writable tree. Writes `MINIDUMP_HEADER` and `MINIDUMP_DIRECTORY`.

`InitializeFromSnapshot(process_snapshot)` populates child streams in order:
1. SystemInfo
2. MiscInfo
3. ThreadList (from `ThreadSnapshot`s)
4. Exception (from `ExceptionSnapshot`, if present)
5. ModuleList (from `ModuleSnapshot`s)
6. UnloadedModuleList
7. CrashpadInfo (annotations, module list)
8. MemoryInfoList
9. HandleData
10. User extension streams
11. MemoryList (thread stacks, extra memory ranges)

`AddStream()` / `AddUserExtensionStream()` support adding custom data streams.

### Stream Writers

Each minidump stream type has a dedicated writer class. Key ones:

| Writer | Stream Type | Source |
|--------|------------|--------|
| `MinidumpSystemInfoWriter` | SystemInfo | `SystemSnapshot` |
| `MinidumpThreadListWriter` / `MinidumpThreadWriter` | ThreadList | `ThreadSnapshot` |
| `MinidumpModuleListWriter` / `MinidumpModuleWriter` | ModuleList | `ModuleSnapshot` |
| `MinidumpExceptionWriter` | Exception | `ExceptionSnapshot` |
| `MinidumpMemoryListWriter` / `MinidumpMemoryWriter` | MemoryList | `MemorySnapshot` |
| `MinidumpStringWriter` | strings | Raw string data |
| `MinidumpAnnotationWriter` | annotations | Annotation key-value pairs |
| `MinidumpMiscInfoWriter` | MiscInfo | Process times, counters |

### Build Targets
- `minidump:format` — minidump format constants/extensions (used by `snapshot/minidump/`)
- `minidump:minidump` — full writer library

Note: `minidump` depends on `snapshot:context` (CPUContext) but NOT on `snapshot:snapshot`. This avoids a circular dependency since `snapshot/minidump/` converts minidump files back into snapshots.

---

## Util Layer (`util/`)

Platform-agnostic utility library organized into subdirectories.

| Subdirectory | Purpose |
|-------------|---------|
| `file/` | `FileReader`, `FileWriter`, `FileIO` (platform shim), `DirectoryReader`, `StringFile`, `DelimitedFileReader`, `ScopedRemoveFile` |
| `net/` | `HTTPTransport`, `HTTPMultipartBuilder`, `HTTPBody` (+ `HTTPBodyGzip`), `URL` parsing |
| `process/` | `ProcessMemory` (cross-platform), `ProcessMemoryRange`, `ProcessMemorySanitized` |
| `misc/` | `UUID`, `CaptureContext`, `Metrics`, `Clock`, `Paths`, `RandomString`, `RangeSet`, `Zlib`, time utilities |
| `thread/` | `Thread`, `WorkerThread`, `Stoppable` interface |
| `synchronization/` | `Semaphore`, `ScopedSpinGuard` |
| `stream/` | `OutputStreamInterface`, `FileOutputStream`, `LogOutputStream`, `ZlibOutputStream`, `Base94OutputStream`, `FileEncoder` |
| `string/` | `SplitString` |
| `stdlib/` | `StringNumberConversion`, `MapInsert`, `Strlcpy`, `Strnlen`, `ThreadSafeVector`, `AlignedAllocator` |
| `numeric/` | `CheckedRange`, `CheckedAddressRange`, `InRangeCast`, `Int128` |

**Platform-specific subdirectories:**
- `util/linux/` — `ExceptionHandlerClient`, `PtraceConnection`, `Ptracer`, `Socket`, `/proc` readers
- `util/mac/` — `MachMessage`, `ExceptionPorts`, `Bootstrap`, `ChildPortHandshake`, `ServiceManagement`
- `util/win/` — `ExceptionHandlerServer`, `InitialClientData`, `ProcessInfo`, `RegistrationProtocol`, `SessionEndWatcher`, `ScopedHandle`
- `util/posix/` — `Signals`, `ScopedDir`, `ScopedMmap`, `SpawnSubprocess`, `DropPrivileges`
- `util/fuchsia/` — `KoidUtilities`, `ScopedTaskSuspend`
- `util/ios/` — intermediate dump processing, `ScopedBackgroundTask`, `ScopedVMMap`

### Build Targets
- `util:util` — core utility library (depends on `compat`, zlib, `mini_chromium:base`)
- `util:net` — separate network target so client code doesn't link HTTP code
- `util:no_cfi_icall` — small target for CFI-safe indirection

---

## Compat Layer (`compat/`)

Not a compilable library — provides platform-specific compatibility headers.

- **Apple**: `compat/mac/` — `Availability.h`, Mach headers, Mach-O headers, `sys/resource.h`
- **Linux/Android**: `compat/linux/` — `signal.h`, `sys/mman.h`, `sys/ptrace.h`, `sys/user.h`
- **Android**: `compat/android/` — `elf.h`
- **Windows**: `compat/win/` — `getopt.h`, `strings.h`, `time.h`, Windows compatibility stubs
- **Non-Win**: `compat/non_win/` — stub headers (`dbghelp.h`, `minwinbase.h`, `windows.h`) for cross-compilation

Selected via GN conditionals in `compat/BUILD.gn`. Different include directories are added per target platform.
