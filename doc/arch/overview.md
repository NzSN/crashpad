# Crashpad Architecture

Crashpad is a crash-reporting system comprising an **out-of-process crash handler** and a **client library** that captures process state at crash time with high fidelity. It produces [minidump](https://msdn.microsoft.com/en-us/library/windows/desktop/ms680369.aspx) files, stores them locally, and uploads them to a collection server.

## High-Level Design

Two cooperating programs run in separate processes:

```
[Client Process]                    [Handler Process]
      |                                    |
  crashpad client library          crashpad_handler binary
  (linked into app)                (fork/launched by client)
      |                                    |
  signal/exc handlers              exception handler server
  annotations, CrashpadInfo        snapshot capture
      |                                    |
      +-------- IPC ───────────────->      |
      |                           minidump writer
      |                           crash report database
      |                           upload thread ──> collection server
```

**Why out-of-process?** The client's state is corrupt during a crash. Running the handler in a separate process makes it reliable, and allows it to use platform debug interfaces (`ptrace`, Mach task ports, `ReadProcessMemory`) to inspect the crashed client.

## Layers

```
┌─────────────────────────────────────────────────────────┐
│                       Client App                         │
├─────────────────────────────────────────────────────────┤
│  client/     CrashpadClient, Annotations, Settings,       │
│              CrashReportDatabase, CrashpadInfo            │
├─────────────────────────────────────────────────────────┤
│  handler/    HandlerMain, CrashReportExceptionHandler,    │
│              CrashReportUploadThread                      │
├──────────────────┬──────────────────────────────────────┤
│  minidump/       │  snapshot/                           │
│  MinidumpFile-   │  ProcessSnapshot, ThreadSnapshot,    │
│  Writer, stream  │  ModuleSnapshot, ExceptionSnapshot,  │
│  writers         │  CPUContext, platform implementations │
├──────────────────┴──────────────────────────────────────┤
│  util/     FileIO, HTTPTransport, ProcessMemory,         │
│            WorkerThread, Semaphore, UUID, streams        │
├─────────────────────────────────────────────────────────┤
│  compat/   Platform-specific compatibility headers       │
└─────────────────────────────────────────────────────────┘
```

- **`client/`** — Library linked into the application. Sets up crash handlers and communicates with the handler process.
- **`handler/`** — The out-of-process crash handler binary. Accepts crash notifications, captures snapshots, writes minidumps, uploads reports.
- **`snapshot/`** — Abstract interfaces for process state (process, threads, modules, system, exception) with platform-specific implementations.
- **`minidump/`** — Minidump file format writers. Composes a tree of `MinidumpWritable` objects to produce a valid minidump file.
- **`util/`** — Cross-platform utilities: file I/O, HTTP, process memory access, threading, synchronization.
- **`compat/`** — Platform shim headers. Provides missing or compatibility declarations; does not compile any object code.

## Platforms

Crashpad supports:

| Platform | IPC | Client Memory Access | Snapshot Dir |
|----------|-----|---------------------|--------------|
| Linux/Android/ChromeOS | `AF_UNIX` socket + ExceptionHandlerProtocol | `ptrace()` | `snapshot/linux/` |
| macOS | Mach exception ports + ChildPortHandshake | `mach_vm_read()` / `task_for_pid()` | `snapshot/mac/` |
| Windows | Named pipe + RegistrationProtocol | `ReadProcessMemory()` | `snapshot/win/` |
| Fuchsia | Exception port bound to default job | `zx_process_read_memory()` | `snapshot/fuchsia/` |
| iOS | In-process (no IPC) | Direct memory access | `snapshot/ios/` |

See [Platform Abstraction](platform-abstraction.md) for details.

## Key Documents

- [Layers](layers.md) — Deep dive into each layer: client, handler, snapshot, minidump, util
- [Data Flow](data-flow.md) — End-to-end crash-to-upload flow (Linux example)
- [Platform Abstraction](platform-abstraction.md) — How platform differences are managed
- [Build Targets](build-targets.md) — GN build target dependency graph
