# Crashpad Data Flow

This document traces a crash from signal delivery to upload, using Linux as the reference platform. The same logical flow applies across platforms, with different IPC and memory-access mechanisms.

## Lifecycle Overview

```
Application Install ──> Crash Signal ──> IPC to Handler ──> Snapshot Capture
                                                                │
Server Upload <────────────────── Background Upload Thread <────┤
                                                                │
                             CrashReportDatabase (disk) <───────┤
                                         Minidump File <────────┘
```

## Phase 1: Startup — Installing Crash Handlers

1. Application calls `CrashpadClient::StartHandler(handler_path, database_path, url, annotations)`
2. `StartHandler()` forks/execs the `crashpad_handler` binary with command-line arguments:
   - `--database=PATH`, `--url=URL`, `--annotation=KEY=VALUE`, `--initial-client-fd=FD`
3. Client and handler establish an IPC channel:
   - Linux: `AF_UNIX` socket pair from `socketpair()`
4. Client installs signal handlers (`SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGTRAP`, `SIGSYS`, `SIGXCPU`, `SIGXFSZ`) via `Signals::InstallHandler()`
5. Client calls `prctl(PR_SET_PTRACER, handler_pid)` to allow the handler to `ptrace` it
6. Handler starts `ExceptionHandlerServer`, begins listening for client connections
7. Handler starts `CrashReportUploadThread` on a background `WorkerThread`

## Phase 2: Crash Signal — In the Client Process

1. A signal (e.g., `SIGSEGV`) is delivered to the client process
2. The signal handler runs on a pre-allocated alternate signal stack (`sigaltstack()`):
   - The alternate stack ensures the handler can run even if the main stack overflowed
3. The signal handler reads `siginfo_t` and `ucontext_t` from the signal frame
4. It populates an `ExceptionInformation` structure with pointers into the signal frame
5. It calls `ExceptionHandlerClient::RequestCrashDump()`, sending a message over the socket:
   - Message type: `kTypeCrashDumpRequest`
   - Contains: client process ID, addresses of `siginfo_t` and `ucontext_t` in the handler's address space
6. The handler is async-signal-safe: uses `futex`-based synchronization (not mutexes), avoids heap allocation, uses only raw system calls via `linux-syscall-support`

## Phase 3: Snapshot Capture — In the Handler Process

1. `ExceptionHandlerServer` receives the client message and calls `CrashReportExceptionHandler::HandleException(pid, uid, info)`
2. Handler attaches to the client via `ptrace(PTRACE_ATTACH, pid)` and waits for `SIGSTOP`
3. `CaptureSnapshot()` is called:
   - Creates a `DirectPtraceConnection` (or uses `PtraceBroker` for multiple clients)
   - Creates `ProcessReaderLinux` which reads:
     - `/proc/[pid]/maps` — memory map
     - `/proc/[pid]/auxv` — ELF auxiliary vector (used to locate the ELF interpreter)
     - `/proc/[pid]/exe` — executable path
     - `/proc/[pid]/mem` + `ptrace` — process memory and register state
   - Creates `ProcessSnapshotLinux::Initialize()`:
     - Threads: enumerates threads via `/proc/[pid]/task/`, reads each thread's registers via `PTRACE_GETREGS`
     - Modules: reads each mapped ELF object's build ID, dynamic annotations
     - Exception: reads signal info from the client's `ExceptionInformation` structure
     - System: reads `uname()`, `sysinfo()`, CPUID
     - Annotations: reads `CrashpadInfo` structure from client memory, follows linked lists of `Annotation` objects
   - Optionally wraps in `ProcessSnapshotSanitized` to redact sensitive data

## Phase 4: Minidump Writing

1. `CrashReportDatabase::PrepareNewCrashReport()` allocates a new report with a UUID
2. Creates `MinidumpFileWriter` and calls `InitializeFromSnapshot(process_snapshot)`
3. `InitializeFromSnapshot()` adds child writables in order:
   ```
   SystemInfo → MiscInfo → ThreadList → Exception → ModuleList →
   UnloadedModuleList → CrashpadInfo → MemoryInfoList → HandleData →
   UserExtensionStreams → MemoryList
   ```
4. `WriteEverything()` transitions through the writable lifecycle:
   - **Freeze**: computes all sizes, resolves RVA pointers within the tree
   - **Write Early**: writes headers, directories, metadata to the output stream
   - **Write Late**: writes large memory regions for better spatial locality
5. The minidump is written to the database's new report file
6. `FinishedWritingCrashReport()` marks the report as Pending
7. Upload thread is notified via `ReportPending(uuid)`
8. Handler sends `kCrashDumpComplete` back to client over the socket

## Phase 5: Client Process Termination

1. Client's signal handler receives the `kCrashDumpComplete` response
2. Restores the original signal handler (or resets to `SIG_DFL`)
3. Re-raises the signal so the kernel delivers it again
4. The process dies with the original signal; the default action produces a core dump or terminates

## Phase 6: Report Upload

1. `CrashReportUploadThread::DoWork()` runs on the background `WorkerThread`
2. `ProcessPendingReports()` scans the database for pending reports
3. For each pending report:
   - Calls `CrashReportDatabase::GetReportForUploading()` to lock the report
   - Reads the minidump file
   - Calls `MinidumpToUploadParameters::StringFromSnapshot()` to extract HTTP form parameters:
     - `prod` — product name from annotations
     - `ver` — product version from annotations
     - `guid` — client UUID
     - Process-level and module-level annotations as form fields
   - Builds an HTTP multipart POST: `HTTPMultipartBuilder`
     - Field: `"upload_file_minidump"` with the minidump as file attachment
     - Additional fields from `MinidumpToUploadParameters`
   - Sends via `HTTPTransport::ExecuteSynchronously(url, body)`
   - On success: calls `RecordUploadComplete()` or `RecordUploadAttempt()`
   - On retryable failure: updates attempt count, keeps report as Pending
   - On permanent failure: marks as completed without upload

## Phase 7: Report Pruning

1. `PruneCrashReportsThread` periodically runs on its own background thread
2. Calls `prune_crash_reports::PruneCrashReportDatabase()` which:
   - Removes reports older than a configurable age
   - Limits total database size
   - Cleans upload-related lockfiles

## Signals and Exceptions by Platform

| Platform | Crash Mechanism | Client-side Trigger |
|----------|----------------|---------------------|
| Linux/Android/ChromeOS | POSIX signals (`SIGSEGV`, etc.) | `Signals::InstallHandler()` + `ExceptionHandlerClient::RequestCrashDump()` |
| macOS | Mach exceptions (`EXC_CRASH`, `EXC_RESOURCE`, `EXC_GUARD`) | Task exception port set to handler's Mach port |
| Windows | Structured exceptions (`EXCEPTION_ACCESS_VIOLATION`, etc.) | `SetUnhandledExceptionFilter()` |
| Fuchsia | Job exception ports | Exception channel bound to default job |
| iOS | Mach exceptions (in-process) | In-process exception handler writes intermediate dumps |

## IPC Mechanisms by Platform

| Platform | Client → Handler Channel | Handler Accesses Client |
|----------|-------------------------|------------------------|
| Linux/Android/ChromeOS | `AF_UNIX` socket pair | `ptrace()` |
| macOS | Mach exception ports | `mach_vm_read()` via `task_for_pid()` |
| Windows | Named pipe (`\\.\pipe\crashpad_*`) | `ReadProcessMemory()` |
| Fuchsia | Exception port on default job | `zx_process_read_memory()` |
| iOS | None (same process) | Direct memory access |
