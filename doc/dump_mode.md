# Selecting memory capture mode

The handler and `generate_dump` accept `--dump-mode=partial|full`.

- Omitted or `partial`: use Crashpad's standard selected-memory capture and
  MemoryList writer. Stacks, metadata, configured indirect memory, and explicitly
  registered extra ranges retain their existing behavior.
- `full`: Windows only. Also schedule eligible committed process regions and
  write a Memory64List stream with `MiniDumpWithFullMemory` set.

Invalid, empty, and repeated values fail argument parsing. Full mode on other
platforms fails startup. Selection is immutable for a handler instance and
applies to every client registered with that handler.

Embedding applications can append `--dump-mode=full` to the argument vector
already accepted by `CrashpadClient::StartHandler()`. Resolve this choice before
launch and preserve it when restarting the handler. Electron integrations must
explicitly forward the option at their handler startup boundary; this change
adds no JavaScript API.

This changes the fork's previous unconditional full-capture default to partial.
Existing deployments requiring full capture must add the explicit option.
An older handler without the switch will reject the new argument, so rollback
must restore matching binaries and startup arguments together.

Full mode excludes guard, inaccessible, and reserved regions. A scheduled read
can still fail; its payload uses `0xfe` placeholders to preserve offsets. A full
header therefore records capture policy, not a guarantee of complete reads.
Output failures stop report writing rather than trigger a partial fallback.
Full dumps can substantially increase suspension time and storage/upload size.

No database migration or upload-policy change is required. Partial and full
reports can coexist in the existing database.
