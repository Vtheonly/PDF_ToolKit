# ADR-0006 — Guarded SIGBUS recovery for MmapHandle (task 1.1)

* **Status:** accepted (2026-10-09)
* **Context:** audit issue-1/task 1.1, step 4 — "Register a thread-safe
  `sigaction` handler for `SIGBUS` to catch asynchronous file truncation
  and throw `PdfToolkitException(ErrorCode::IoTruncated)`."
* **Supersedes:** none. Refines the audit's prescription (deviation filed
  as **P-014**, same class as P-011/P-012).

## Decision

1. **The typed exception is thrown from the normal stack, never from the
   signal handler.** Throwing a C++ exception from inside a signal
   handler is undefined behaviour (the unwinder may run on the signal
   stack mid-interruption; POSIX makes no provision for it). Instead,
   `MmapHandle::guarded()` establishes a `sigsetjmp` recovery point and
   publishes it in a thread-local; the SIGBUS handler `siglongjmp()`s
   back to that point and `guarded()` then throws
   `PdfToolkitException(IoTruncated)`. This delivers exactly the audit's
   acceptance criterion (controlled exception instead of a crash) without
   UB.
2. **Protection is opt-in per access region.** A blanket
   "any SIGBUS inside any mapping is converted" policy would also
   swallow faults the caller never asked to survive. `guarded(f)` is the
   only path that arms protection; faults outside `guarded()` keep the
   default disposition.
3. **Honesty contract (test-locked, `native/tests/test_mmap.cpp`):** the
   handler converts a fault to the typed exception **only** when
   (a) this thread has an active recovery point, AND (b) the fault
   address lies inside a *registered live* mapping. Foreign faults,
   faults with no recovery point, and faults after the handle died are
   re-raised with the disposition found at install time (restoring e.g.
   a sanitizer runtime's handler), so the process dies exactly as it
   would have without us. Never a blanket swallow.
4. **Registry: fixed 128-slot table with per-slot seqlocks.** The
   handler must probe "is this address in a live mapping?"
   async-signal-safely — no locks, no allocation. Each slot is
   `alignas(64)` `{atomic<uint64> version; begin; end}`; writers (handle
   ctor/dtor, serialized by a mutex the handler never takes) flip the
   version odd around mutations; the handler skips slots that are odd or
   changed mid-read. Skipping is fail-safe: a slot racing with its own
   (un)registration is treated as foreign → default disposition, which
   can only affect a fault racing that exact slot, never steady state.
   Exhaustion (129 live mappings) fails construction with `Internal`
   rather than mapping unprotected.
5. **`sigsetjmp` before publish.** The recovery point must be
   established *before* the thread-local publish, or a SIGBUS in the
   window would longjmp into an uninitialised buffer. In `guarded()` the
   `GuardInstall` lives inside the `if (sigsetjmp(...) == 0)` scope; the
   longjmp path skips its destructor, which is safe by construction
   because the handler nulls the thread-local first — the destructor's
   only effect.
6. **No nesting.** `guarded()` must not nest (last install wins; longjmp
   would skip inner destructors). Documented in the header; callers keep
   guards at one level.

## Consequences

* `guarded()` uses `sigjmp_buf`/`sigsetjmp`/`siglongjmp` — POSIX-only.
  On Windows `guarded()` runs the body unguarded; the SEH translation of
  `EXCEPTION_IN_PAGE_ERROR` is deferred (compile-only path, **U-011**).
* Re-raise path: restore previous disposition, unblock SIGBUS
  (`sigprocmask`), `raise(SIGBUS)`, with an `_exit(128+SIGBUS)` backstop
  if the previous disposition ignored the signal. Under the sanitizers
  the previous handler is the runtime's, which reports and dies with an
  exit code — the fork-based honesty tests accept that under
  `__SANITIZE_ADDRESS__/__SANITIZE_THREAD__` and assert exact
  death-by-signal otherwise.
* The guard table's thread-local pointer requires a TLS relocation model
  that is legal inside a shared object → `CMAKE_POSITION_INDEPENDENT_CODE
  ON` for the native tree (task 1.1 made this mandatory; previously the
  non-PIC static core linked into `pdftoolkit_ffi` by luck of having no
  TLS).
* Fork safety: children must not allocate after `fork()` before the
  fault (sanitizer runtimes hold post-fork locks); the honesty tests are
  written allocation-free in the child.

## Verification

`ctest --preset release` → `mmap` (15 cases: mapping semantics, error
mapping, moves, advice, the truncation acceptance criterion, three
fork-based honesty deaths, 128-slot capacity/recycling), plus the same
suite clean under ASan+UBSan (`-fno-sanitize-recover=all`) and TSan.
Recording: `docs/benchmarks/2026-10-09-phase1-mmap-baseline.md`.
