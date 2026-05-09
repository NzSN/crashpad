# Crashpad Architecture Overview

> Crashpad v0.8.0 — Chromium's cross-platform crash-reporting system.

---

## 1. Project Layout

```
crashpad/
├── client/          Crashpad client library (linked into your app)
├── handler/         Crashpad handler (separate process, crash catcher)
├── snapshot/        Process snapshot layer (OS-agnostic interfaces + platform impls)
├── minidump/        Minidump serialization (writes Snapshots to .dmp files)
├── util/            Utility libraries (file I/O, net, Mach, process, threading)
├── compat/          System compatibility headers (fill POSIX/API gaps)
├── third_party/     mini_chromium, googletest, zlib, cpp-httplib, …
├── build/           GN build configs
├── test/            Integration & multi-process tests
├── tools/           Helper tools (macOS exception catch tool, etc.)
├── doc/             Design docs
├── infra/           CI/CD config
└── BUILD.gn         Top-level GN build file
```

---

## 2. Three-Layer Architecture

```
┌──────────────────────────────────────────────────┐
│              Embedding Application               │  ← Your app
├──────────────────────────────────────────────────┤
│  Client (client/)    crash-reporting client lib  │  ← Linked into app
│  Handler (handler/)  standalone crash processor  │  ← Separate process
├──────────────────────────────────────────────────┤
│  Snapshot (snapshot/)   process state interfaces │
│  Minidump (minidump/)   .dmp file serialization  │
│  Database (client/)     crash report storage     │
├──────────────────────────────────────────────────┤
│  Util (util/)          low-level helpers         │
│  Compat (compat/)      OS header polyfills       │
│  Third-party (third_party/)                      │
└──────────────────────────────────────────────────┘
```

---

## 3. Key Concepts: Handler Process vs Client Process

Crashpad uses a **dual-process model** (except iOS):

```
┌───────────────────────┐          IPC          ┌───────────────────────────┐
│    CLIENT PROCESS      │                       │     HANDLER PROCESS        │
│                        │                       │                            │
│  Your application      │   crash notification  │  crashpad_handler (.exe)   │
│  + CrashpadClient lib  │ ───────────────────►  │  + ExceptionHandlerServer  │
│  + CrashpadInfo struct │                       │  + CrashReportDatabase     │
│                        │   (handler reads      │  + CrashReportUploadThread │
│    ↑  CRASH!           │    client memory)     │                            │
│    │  send signal      │ ◄───────────────────  │  ① Suspend client         │
│    │  wait for handler │                       │  ② Read client memory     │
│                        │                       │  ③ Write minidump         │
│                        │                       │  ④ Upload to server       │
│                        │                       │  ⑤ Resume/kill client     │
└───────────────────────┘                       └───────────────────────────┘
```

### Client Process
- **Who**: Your application process.
- **What it does**: Registers with handler at startup. On crash, runs **minimal code** (one IPC message) then waits.
- **Crash-time code**: signal handler (Linux), UEF (Windows), or **zero** (macOS — kernel forwards directly).

### Handler Process
- **Who**: `crashpad_handler` — a dedicated background process.
- **What it does**: Receives crash notifications, suspends the crashed client, reads its memory, writes a minidump, stores it, uploads it.
- **Why separate**: Safety (crashed process state is corrupted), reliability (handler can survive), isolation (one handler serves many clients).

### iOS Exception
iOS **does not allow** a separate handler process. The app handles crashes **in-process**:
1. Install Mach exception handler + `SIGABRT` handler inside the app.
2. On crash, write a lightweight **intermediate dump** file.
3. Process terminates.
4. On next app launch, the intermediate dump is converted to a full minidump and uploaded.

---

## 4. Crash Dump Capture — Complete Flow

```
 ┌───────────────────────────────────────────────────────┐
 │  STEP 1: Crash Notification (OS-specific IPC)         │
 │  Kernel/signal delivers crash to handler process       │
 └──────────────────────┬────────────────────────────────┘
                        │
 ┌──────────────────────▼────────────────────────────────┐
 │  STEP 2: Suspend Target Process                       │
 │  Freeze all threads in the crashed client              │
 │  macOS: task_suspend()   Windows: NtSuspendProcess()  │
 │  Linux: ptrace attach    iOS: task_suspend()           │
 └──────────────────────┬────────────────────────────────┘
                        │
 ┌──────────────────────▼────────────────────────────────┐
 │  STEP 3: Create ProcessSnapshot                       │
 │  Cross-process read: threads, registers, modules,      │
 │  memory maps, annotations, handles                     │
 └──────────────────────┬────────────────────────────────┘
                        │
 ┌──────────────────────▼────────────────────────────────┐
 │  STEP 4: Write Minidump                               │
 │  Serialize snapshot → Windows Minidump format → disk   │
 │  Two-phase: write with Signature=0, then seek back     │
 │  to write real signature (prevent partial-reads)       │
 └──────────────────────┬────────────────────────────────┘
                        │
 ┌──────────────────────▼────────────────────────────────┐
 │  STEP 5: Store + Notify Upload                        │
 │  Add to CrashReportDatabase → upload_thread picks up   │
 │  Upload via HTTP(S) using Breakpad wire protocol       │
 └───────────────────────────────────────────────────────┘
```

### Platform IPC Mechanisms

| Platform | Crash Notification Mechanism |
|----------|----------------------------|
| **macOS** | Mach exception port — kernel sends Mach message directly to handler; **zero code** runs in crashing process |
| **Windows** | UnhandledExceptionFilter (UEF) in client writes exception info to shared struct, signals an Event handle |
| **Linux/Android** | Signal handler (`SIGSEGV`, `SIGABRT`, etc.) sends `ClientInformation` over Unix socket pair |
| **iOS** | In-process Mach exception + signal handler; writes intermediate dump file |

### ProcessReader — How Cross-Process Memory Is Read

| Platform | Memory Read Primitive |
|----------|----------------------|
| **macOS** | `ProcessReaderMac` — `mach_vm_read()` |
| **Windows** | `ProcessReaderWin` — `ReadProcessMemory()` |
| **Linux** | `ProcessReaderLinux` — `ptrace(PTRACE_PEEKDATA)` or `/proc/pid/mem` |

---

## 5. ProcessSnapshot — What Gets Captured

The abstract `ProcessSnapshot` interface captures:

```
ProcessSnapshot
 ├── SystemSnapshot          OS version, CPU architecture, timezone
 ├── ThreadSnapshot[]        Thread ID, register context (CPU state), stack memory
 ├── ModuleSnapshot[]        Loaded code modules (exe, dll, so, dylib) + CrashpadInfo annotations
 ├── ExceptionSnapshot       Exception type, code, address, triggering thread
 ├── MemoryMapRegionSnapshot[]  /proc/pid/maps or VirtualQuery regions
 ├── MemorySnapshot[]        Extra memory ranges (referenced from registers/stacks)
 ├── HandleSnapshot[]        Open file descriptors / handles
 └── Annotations             CrashpadInfo key-value pairs
```

---

## 6. Minidump — Stream Layout

`MinidumpFileWriter` serializes the snapshot to disk in this order (optimized for data locality):

```
MINIDUMP_HEADER (Signature initially 0, rewritten at end)
 ├── Stream: SystemInfo          OS / CPU info
 ├── Stream: MiscInfo            Process start time, uptime
 ├── Stream: ThreadList          Threads + stack memory
 ├── Stream: Exception           Exception record (if present)
 ├── Stream: ModuleList          Loaded modules + CrashpadInfo
 ├── Stream: UnloadedModuleList
 ├── Stream: CrashpadInfo        Crashpad extensions (report ID, client ID, annotations)
 ├── Stream: MemoryInfoList      Memory map regions
 ├── Stream: HandleData          Open handles
 ├── Stream: UserStreams         Embedder-defined custom data
 └── Stream: MemoryList          Extra memory (placed last for truncation safety)
```

---

## 7. HandlerMain() — The Handler's Main Function

`handler/handler_main.cc` is the entry point for the handler process. Its full lifecycle:

```cpp
int HandlerMain(int argc, char* argv[],
                const UserStreamDataSources* user_stream_sources) {

    // ── Phase 1: Self-protection ──
    InstallCrashHandler();   // Handler can crash too — record it

    // ── Phase 2: Parse arguments ──
    Options options;
    // --database=PATH        (required) crash DB location
    // --url=URL              upload server
    // --annotation=K=V       process-level annotations
    // --pipe-name=NAME       Windows named pipe
    // --initial-client-fd=N  Linux socket fd
    // --mach-service=NAME    macOS bootstrap service name
    // --monitor-self         spawn 2nd handler to watch this one
    GetOpt(argc, argv, &options);

    // ── Phase 3: Self-monitoring (optional) ──
    if (options.monitor_self) {
        // Spawn a second Crashpad handler to catch crashes in THIS handler
        CrashpadClient crashpad_client;
        crashpad_client.StartHandler(exe, db, url, ...);
    }

    // ── Phase 4: Open crash report database ──
    std::unique_ptr<CrashReportDatabase> database =
        CrashReportDatabase::Initialize(options.database);
    // Contains: settings.dat, pending/ dir, completed/ dir

    // ── Phase 5: Start background upload thread ──
    CrashReportUploadThread upload_thread(database, url, opts);
    upload_thread.Start();  // Periodically scans for pending reports, uploads them

    // ── Phase 6: Start prune thread ──
    PruneCrashReportThread prune_thread(database, ...);
    prune_thread.Start();   // Removes old reports to respect size/age limits

    // ── Phase 7: Create exception handler (callback for crashes) ──
    CrashReportExceptionHandler exception_handler(
        database, &upload_thread, &annotations, user_stream_sources);

    // ── Phase 8: Run IPC server — BLOCKS until shutdown ──
    ExceptionHandlerServer server(...);
    server.InitializeWithClient(socket_or_port);
    server.Run(&exception_handler);
    //    ↑
    //    macOS:     mach_msg() loop waiting for Mach exception messages
    //    Linux:     poll() loop on Unix sockets
    //    Windows:   WaitForMultipleObjects() on events/named pipes

    return EXIT_SUCCESS;
}
```

The actual crash processing happens when `ExceptionHandlerServer` receives a message and calls back into `CrashReportExceptionHandler`:

```
server.Run()  →  [crash received]  →  exception_handler.HandleException()
                                           │
                                           ├─ Suspend process
                                           ├─ CaptureSnapshot()
                                           ├─ MinidumpFileWriter::WriteEverything()
                                           ├─ database->FinishedWritingCrashReport()
                                           └─ upload_thread->ReportPending(uuid)
```

---

## 8. CrashReportDatabase

The database stores crash reports on disk:

```
<database_dir>/
├── settings.dat          Binary file: client UUID, upload-enabled flag
├── pending/              Minidumps waiting for upload
│   └── <uuid>.dmp
├── completed/            Minidumps already uploaded
│   └── <uuid>.dmp
└── metadata              (Windows only) per-report state
```

Per-report metadata (upload attempts, server ID, etc.) is stored:
- **macOS/iOS**: Filesystem extended attributes on the .dmp file
- **Windows**: Separate `metadata` binary file
- **Linux**: Filesystem extended attributes

---

## 9. Upload Flow

```
CrashReportUploadThread (background WorkerThread)
  │
  ├─ ReportPending(uuid)        ← called by exception handler
  │
  └─ DoWork() [periodic timer]
       │
       ├─ ProcessPendingReports()
       │    ├─ Scan database for pending reports
       │    ├─ RateLimitCheck (1 upload/hour)
       │    └─ For each report:
       │         ├─ UploadReport()
       │         │    └─ HTTP(S) POST to Breakpad server
       │         │       Content-Type: multipart/form-data
       │         │       Parts: crash keys + minidump (gzip'd)
       │         ├─ On success: mark completed, store server ID
       │         └─ On failure: update attempt count, may retry
       │
       └─ [iOS only] ShouldRateLimitRetry() per-report backoff
```

---

## 10. CrashpadInfo — Client Annotations

Each executable module can embed a `CrashpadInfo` structure in a special named section. On crash, the handler crawls all modules looking for `CrashpadInfo` to collect:

```cpp
struct CrashpadInfo {
    uint32_t signature;              // Magic number
    uint32_t size;                   // For version compatibility
    uint32_t version;

    // Client-controlled annotations (key-value pairs, e.g. "channel=beta")
    SimpleStringDictionary* simple_annotations;

    // Memory ranges to include in dump (beyond defaults)
    SimpleAddressRangeBag* extra_memory_ranges;

    // Custom minidump streams
    UserDataMinidumpStreamListEntry* custom_streams;

    // Behavior flags
    TriState crashpad_handler_behavior;     // enabled / disabled
    TriState system_crash_reporter_forwarding; // forward to OS reporter too
    uint64_t gather_indirectly_referenced_memory; // memory capture cap
};
```

---

## 11. Platform Adaptation Pattern

Every OS-specific component follows the pattern: **abstract interface + platform implementation**.

| Component | Interface | macOS | Windows | Linux | iOS | Fuchsia |
|-----------|-----------|-------|---------|-------|-----|---------|
| ProcessSnapshot | `snapshot/process_snapshot.h` | `snapshot/mac/process_snapshot_mac.*` | `snapshot/win/process_snapshot_win.*` | `snapshot/linux/process_snapshot_linux.*` | `snapshot/ios/` | `snapshot/fuchsia/` |
| ProcessReader | per-platform only | `snapshot/mac/process_reader_mac.*` | `snapshot/win/process_reader_win.*` | `snapshot/linux/process_reader_linux.*` | — | — |
| ExceptionHandler | `handler/*/crash_report_exception_handler.*` | Mach exception-based | UEF + named pipe | Signals + socket | In-process | — |
| CrashpadClient | `client/crashpad_client.h` | `client/crashpad_client_mac.cc` | `client/crashpad_client_win.cc` | `client/crashpad_client_linux.cc` | `client/crashpad_client_ios.cc` | `client/crashpad_client_fuchsia.cc` |
| CrashReportDatabase | `client/crash_report_database.h` | `*_mac.mm` | `*_win.cc` | `*_generic.cc` | — | — |

---

## 12. Design Principles

| Principle | Implementation |
|-----------|---------------|
| **Safety** | One-way data flow (handler reads client memory, never writes). All reads use bounded, alignment-checked accessors. |
| **No code in crashed process** | Snapshot capture is entirely out-of-process via OS memory-read primitives. |
| **Privacy** | User consent required before upload. Client UUID, not user identity. Database stores upload-enabled flag. |
| **Extensibility** | `UserStreamDataSources` allow embedders to add custom minidump streams. `CrashpadInfo` allows per-client annotations and memory ranges. |
| **Cross-bitness** | 64-bit handler can capture 32-bit clients (Windows limited: 32-bit handler can only capture 32-bit clients). |
| **Partial write safety** | Minidump header signature written as `0` initially, rewritten after all streams complete. Incomplete files are never mistaken for valid dumps. |
| **Self-monitoring** | `--monitor-self` spawns a second handler to catch crashes in the primary handler. |

---

## 13. Dependencies

- **`mini_chromium`**: Subset of Chromium `//base` (logging, files, strings, threading, synchronization).
- **`googletest`**: Unit testing framework.
- **`zlib`**: Compression for minidump uploads.
- **`cpp-httplib`**: HTTP client library.
- **System APIs**: Mach (macOS/iOS), Win32/NT (Windows), ptrace/procfs (Linux), signals (POSIX).

When built inside Chromium, `mini_chromium` is replaced by Chromium's full `//base`.

---

## 14. Quick Reference: Key Files

| File | Role |
|------|------|
| `client/crashpad_client.h` | Client API: `StartHandler()`, `SetHandlerIPCPipe()`, `DumpWithoutCrash()` |
| `handler/handler_main.h` | `HandlerMain()` — handler process entry point |
| `handler/main.cc` | `main()` → `HandlerMain()` |
| `handler/*/crash_report_exception_handler.*` | Platform-specific: crash → suspend → snapshot → minidump |
| `handler/*/exception_handler_server.*` | Platform-specific IPC server loop |
| `snapshot/process_snapshot.h` | Abstract interface for process state |
| `snapshot/*/process_snapshot_*.cc` | Platform-specific snapshot implementations |
| `snapshot/*/process_reader_*.cc` | Platform-specific cross-process memory readers |
| `minidump/minidump_file_writer.*` | Root minidump writer, `InitializeFromSnapshot()` |
| `client/crash_report_database.*` | Database: `PrepareNewCrashReport()`, `FinishedWritingCrashReport()` |
| `handler/crash_report_upload_thread.*` | Background upload with rate limiting |
