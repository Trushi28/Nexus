# Design notes

The rationale behind a few specific choices in Nexus, plus what's
been verified and what's honestly still missing. See the main
[README](../README.md) for the feature list and build instructions.

## Notable design choices

- **x2APIC-first, seriously**: the Local APIC driver does everything
  through MSRs when x2APIC is available — no MMIO mapping needed at
  all for the LAPIC in that path. The I/O APIC still has to be MMIO
  (there's no x2APIC equivalent for it), so it goes through a small
  general-purpose `vmm_map_mmio()` the VMM exposes for exactly this.
- **Own page tables, not the bootloader's**: Limine's own tables are
  reclaimable/overwritable memory as far as the kernel is concerned, so
  Nexus builds a fresh PML4 and switches to it early, mapping the
  direct map + kernel image itself with real permissions. Getting this
  right needed EFER.NXE enabled *before* the switch (an NX-marked page
  table entry is a reserved-bit fault, not a no-op, until NXE is set) —
  see the comment in `cpu/cpu.c`.
- **A `.bss`-backed boot stack**: `_start` immediately moves off
  whatever stack Limine handed it and onto a stack living in the
  kernel's own `.bss`, before any C code runs. That stack is guaranteed
  mapped both before *and* after the later CR3 switch (Limine has to
  map the whole kernel image to execute the first instruction at all;
  Nexus's own tables map the identical range from the same Executable
  Address response) — see `boot/start.S`.
- **AP release is fire-and-forget**: the BSP signals every AP's
  `goto_address` and moves on with its own boot sequence rather than
  blocking on a timeout, since the natural timing primitive (the LAPIC
  timer) isn't ticking yet at that point in boot anyway. Query
  `smp_online_cpu_count()` any time after for however many have checked
  in.
- **Dropping to ring 3 without disturbing `this_cpu()`**: the IRETQ
  trampoline that starts a ring-3 task (`task_bootstrap_user()` in
  `sched/sched.c`) deliberately never touches DS/ES/FS/GS. IRETQ itself
  doesn't reload them, and 64-bit mode doesn't enforce segment-limit/
  DPL checks against them for ordinary memory references — but GS in
  particular still carries this CPU's `GS_BASE`-backed `cpu_local`
  pointer (`this_cpu()` reads `%gs:0`), and reloading the GS *selector*
  would zero that hidden base with nothing to restore it until an
  actual `swapgs`-based syscall entry exists. Leaving it alone
  sidesteps the whole issue: `GS_BASE` was set via `wrmsr()`, not a
  segment load, so it survives the privilege-level change untouched.
- **Higher half shared, lower half owned**: a fresh process address
  space (`vmm_new_address_space()`) copies the kernel's upper-half PML4
  entries wholesale rather than building them again — cheap (256
  qwords) and correct, since each copied entry just points at an
  already-shared sub-table. Physical pages stay reachable through the
  direct map regardless of which CR3 is loaded, so the ELF loader never
  needs to switch address spaces to populate a process's memory; it
  just `memcpy`s straight into `phys_to_virt()` of whatever it just
  mapped.
- **Why blocking `run` needed its own wait primitive**: the existing
  single-slot `wait_queue_block()`/`wait_queue_wake()` (built for
  `keyboard_getc()`, a single producer that fires repeatedly) has a
  lost-wakeup race that's harmless there — the next keystroke
  self-heals it — but would be a permanent hang for a one-shot event
  like process exit. `sched_wait_task()` and `task_exit()` instead
  rendezvous under a dedicated lock (`wait_lock` in `sched/sched.c`),
  closing the race properly rather than reusing a primitive that wasn't
  built for a single, non-repeating signal.
  - **A cross-cpu publish-before-safe race, present in three places, two
  of them since the very first scheduler code**: any time a task left
  TASK_RUNNING, whatever queue it was headed for (the ready queue on
  preemption, sleep_head on sched_sleep_ms(), or -- fixed in an
  earlier pass -- an external wake reaching a blocked task) could
  become visible to OTHER cpus before this cpu's own context_switch()
  had actually made that task's saved rsp valid to resume from. The
  preemption case (every task, every quantum) was the hottest and
  longest-standing instance and had never been touched by either
  earlier fix. Closed uniformly: cpu_local::parked_head is the only
  place a task is EVER made visible again after leaving TASK_RUNNING,
  it's only ever touched by its OWNING cpu, and it's drained (into the
  real ready queue, or into sleep_head) only at the top of that same
  cpu's own next schedule() call -- the one place that can prove the
  switch away already completed. A DIFFERENT bug turned up designing
  this unification: wake_blocked_task()'s earlier "fast path" (enqueue
  directly if switched_away was already true) could race the owning
  cpu's own drain loop over the same task->next field, corrupting
  parked_head and potentially double-enqueuing the same task. Fixed by
  removing that fast path entirely -- every external waker now only
  ever flips a state flag; the owning cpu's own drain loop is the sole
  writer to parked_head and the sole caller of enqueue_ready() for
  anything it holds. Cost: up to one quantum (~10ms) of added wake
  latency in the worst case, in exchange for a mechanism with exactly
  one writer per list under every interleaving. See sched/sched.c's
  wake_blocked_task() and schedule().
- **Table-driven shell dispatch, not an if/else chain**: `shell/shell.c`'s
  `commands[]` is the single source of truth for every command's name,
  handler, usage string, and one-line help blurb -- `cmd_help()` renders
  itself from it, `dispatch()` looks up against it, tab-completion and
  the fuzzy "did you mean" search it too. Replaces N separate
  str_has_prefix(cmd, "foo ") checks (which had a real bug: "echoFoo"
  used to trigger the echo branch, no word-boundary check) with one
  general verb/args split.
- **Non-POSIX shell heuristics**: a small synonym table lets several
  spellings of a command execute identically (list==ls, tasks==ps, ...
  -- see `aliases`); an unrecognised command gets a "did you mean"
  computed via integer Levenshtein distance (no floats -- this build is
  -mno-sse/-mno-mmx/-mno-80387); a lazy-decay integer frecency score
  per command (halve on use, add a fixed bonus -- no timers, no
  background pass) powers both `topcmds` and tie-breaking in
  suggestions. Tab completes command names always, and VFS path
  segments for the last argument.
  - **Graph FS persistence is a whole-graph snapshot, not a live on-disk
  structure**: graph_save_to_disk()/graph_load_from_disk() (fs/graph.c)
  serialize/deserialize the entire in-memory graph in one shot to/from
  the NVMe namespace -- "save game", not a journaled filesystem with
  its own block allocator. Kept deliberately this simple for the same
  reason NVMe went polled before MSI-X and PCI came before NVMe at
  all: prove the boring, obviously-correct version first. Node ids are
  preserved exactly across a save/load round-trip; refcounts are never
  stored -- they're fully recomputed by replaying the same
  graph_link()/sstring_set() calls used during normal operation, which
  also means a load exercises the identical code path a live gmk/glink
  session would, rather than a separate, easier-to-get-wrong
  reconstruction path.
  - **Graph FS deletion is plain refcounting, not a garbage collector**:
  graph_unlink()/sstring_unset()/graph_node_delete() (fs/graph.c) free
  a node the instant its refcount hits 0, cascading through whatever
  it pointed to. Iterative, not recursive (an explicit worklist via
  struct gnode::release_next, mirroring sched.c's pending_exit/
  parked_head pattern) so a long chain being released can't blow the
  kernel stack. No force-delete exists -- a still-referenced node
  refuses to go, since forcing it would leave dangling edges. The one
  permanent gap: reference cycles never reach refcount 0 through edge
  decrements and are never freed this way; graph_clear_all() (`gclear`)
  is the closest thing to a way out and honestly reports what's left
  over if something's cyclic. Closing that gap for real needs a
  mark-and-sweep pass, which is meaningfully more work and not v1
  scope.

## Known limitations

- No `fork`, no `exec` — a process is created fully-formed from an ELF
  path via `run`/`process_spawn`; there's no way for a running process
  to replace or duplicate itself, and no argv/envp passing.
- No background jobs — `run` blocks until the child exits, so there's
  no `&` and correspondingly no `kill`.
- No network stack.
- The direct map and kernel image are mapped with plain 4KiB pages,
  not 2MiB/1GiB huge pages. Simpler and safer to hand off unverified;
  costs a bit of extra page-table memory and TLB pressure.
- The framebuffer is mapped write-back (not write-combining) in the
  direct map, for the same reason — simplicity over the last bit of
  fill-rate performance.
- Syscalls that touch a user pointer (`write`, `read`, `open`,
  `readdir`) only do a shallow range check — "is this a plausible
  canonical low-half address" — not a real per-page walk of the
  caller's page tables with page-fault recovery (a proper
  `copy_from_user`/`copy_to_user`). A process that hands the kernel a
  syntactically-valid-but-unmapped pointer takes an unhandled page
  fault *in the kernel*, which today means a panic instead of just
  killing that one process. See the comment at the top of
  `cpu/syscall.c`.
- `sys_brk` can grow a process's heap but not shrink it (the pointer
  moves back; the pages stay mapped) — `vmm_free_user_space()` already
  shows the page-freeing walk if you want to wire it up.
- tmpfs is the only filesystem; there's no mount table yet, just a
  single root vnode — though the vnode_ops indirection is exactly what
  a mount table would dispatch through.
- The ELF loader only accepts static, non-PIE `ET_EXEC` binaries — no
  dynamic linking, no relocations, no PIE.
- The NVMe driver is single-namespace, single-I/O-queue, and caps a
  single request at ~2MiB (one PRP-list page, not chained). Buffers
  must be page-aligned. Command completion is MSI-X-driven (admin
  queue on vector 0, I/O queue on vector 1) with no timeout on the
  wait -- consistent with every other blocking wait in this kernel; see
  the comment on drivers/nvme.c's submit_and_wait().
  - Tab-completion's path matching only understands the classic VFS
  namespace ("/", tmpfs, the initrd) -- graph-FS paths (gcat/gls/
  sstring targets) don't complete yet.
- The graph filesystem (fs/graph.c) can free nodes now (grm/gunlink/
  sstringrm/gclear -- plain refcounting, see the design note above),
  but reference cycles permanently leak: a node only reachable through
  a cycle is never freed short of a reboot. graph_load_from_disk()
  still refuses to run against a non-empty in-memory graph -- run
  gclear first if you want to reload mid-session. A node created after
  a gload draws its id from the restored counter, which can numerically
  reuse an id that existed earlier in the same session (never one
  currently live -- just a cosmetic wrinkle worth knowing about).
  Persistence itself is still a whole-graph snapshot (gsync/gload), not
  continuous durability, capped at GRAPH_SNAPSHOT_MAX_BYTES (1MiB by
  default). One coarse lock covers the whole graph.

## Verification performed here

This was built and booted, repeatedly, in QEMU (TCG, `-M q35 -smp 4`)
— headless, with keyboard input scripted over QEMU's QMP socket
(`send-key`) and the console mirrored to serial for inspection, since
there's no way to type into a graphical PS/2 console from here
otherwise. What that covered:

- A `selftest`-cmdline boot: mounts tmpfs, unpacks the initrd, spawns
  `/bin/hello` in ring 3, waits for it, checks the exit code —
  `[selftest] PASS` on every run.
- `ps`, `ls`, `ls /bin`, `run /bin/hello`, `run /bin/sysinfo`, `matrix`
  (start and stop), all interactively, on the default (non-selftest)
  boot entry.
- `run /bin/guess` end to end: guessed every number 1 through the
  target in sequence (25, that run) to force many consecutive blocking
  `read` syscalls rather than just one — this is what caught and fixed
  a real bug (`kprintf`'s `%s` doesn't support a width/`-`-flag, so the
  first `ps` table came out with literal `%-16s` in it) and confirmed a
  more interesting property: a task can migrate to a *different* CPU
  core mid-syscall, between one blocking read and the next, and still
  resume correctly (right kernel stack via the per-CPU TSS.RSP0, right
  address space via the per-`schedule()` CR3 check) — observed directly
  in a debug build's tracing, not just inferred.
- SMP: all 4 vCPUs report online and ring-3 tasks visibly run on
  different cores across their own lifetime.

**Not covered:** real hardware, UEFI boot specifically (only BIOS was
exercised here), or anything KVM-accelerated (this environment has no
`/dev/kvm`, so every run was software-emulated TCG).

## Kernel command line

Set from `limine.conf`'s `cmdline:` lines, or edit it directly:

- `nosmp` — stay uniprocessor, don't release any APs.
- `selftest` — after boot, automatically `run` `/bin/hello` in ring 3
  and report pass/fail before handing off to the shell — see the
  "Nexus (self-test)" boot entry in `limine.conf`.

## Build dependencies

The top-level `GNUmakefile` downloads a pinned Limine release and
builds its host tool + bootloader stages. `kernel/get-deps` separately
fetches two small headers Nexus's own code depends on:

- `limine.h` — from the Limine protocol repo, BSD-2-Clause
- `font8x8_basic.h` — from `dhepper/font8x8`, Public Domain

Neither is vendored in this repo; both are fetched fresh at build time
so you always build against current upstream. See `LICENSE` for
exactly what and why.
