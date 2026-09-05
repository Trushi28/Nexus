# Design notes

The rationale behind specific choices in Nexus, plus what's honestly
still missing. See the main [README](../README.md) for the feature
list and build instructions.

## Notable design choices

- **x2APIC-first**: the Local APIC driver goes through MSRs when
  x2APIC is available — no MMIO mapping needed for the LAPIC on that
  path. The I/O APIC has no x2APIC equivalent, so it always goes
  through `vmm_map_mmio()`.
- **Own page tables, not the bootloader's**: Limine's tables are
  reclaimable memory as far as the kernel is concerned, so Nexus
  builds a fresh PML4 and switches to it early. EFER.NXE has to be
  enabled *before* that switch — an NX-marked PTE is a reserved-bit
  fault, not a no-op, until NXE is set (see `cpu/cpu.c`).
- **A `.bss`-backed boot stack**: `_start` moves off whatever stack
  Limine handed it onto one living in the kernel's own `.bss`, before
  any C code runs — guaranteed mapped both before and after the later
  CR3 switch, since Limine already has to map the whole kernel image
  to execute the first instruction, and Nexus's own tables map the
  identical range. See `boot/start.S`.
- **AP release is fire-and-forget**: the BSP signals every AP's
  `goto_address` and moves on rather than blocking on a timeout — the
  natural timing primitive (the LAPIC timer) isn't ticking yet at that
  point in boot. `smp_online_cpu_count()` reflects however many have
  checked in at any later point.
- **Dropping to ring 3 without disturbing `this_cpu()`**: the IRETQ
  trampoline that starts a ring-3 task (`task_bootstrap_user()`,
  `sched/sched.c`) never touches DS/ES/FS/GS. IRETQ doesn't reload
  them, and long mode doesn't enforce segment-limit/DPL checks against
  them for ordinary memory references — but GS carries this CPU's
  `GS_BASE`-backed `cpu_local` pointer, set via `wrmsr()` rather than a
  segment load, so it survives the privilege change untouched.
- **Higher half shared, lower half owned**: a fresh process address
  space (`vmm_new_address_space()`) copies the kernel's upper-half
  PML4 entries wholesale — cheap and correct, since each copied entry
  just points at an already-shared sub-table. Physical pages stay
  reachable through the direct map regardless of which CR3 is loaded,
  so the ELF loader never needs to switch address spaces to populate a
  process's memory.
- **A dedicated wait primitive for `run`**: the original single-slot
  `wait_queue_block()`/`wait_queue_wake()` (built for
  `keyboard_getc()`, a repeating producer) has a lost-wakeup race
  that's harmless there but would be a permanent hang for a one-shot
  event like process exit. `sched_wait_task()`/`task_exit()` instead
  rendezvous under a dedicated lock (`wait_lock`).
- **Scheduler wake-up has exactly one writer per list**: any task
  leaving `TASK_RUNNING` needs to become visible to other CPUs again
  eventually (the ready queue, the sleep list, or a wake reaching a
  blocked task) — but only once this CPU's own `context_switch()` away
  from it has actually completed. `cpu_local::parked_head` is the only
  place a task is ever republished after leaving `TASK_RUNNING`; it's
  touched only by its owning CPU, and drained only at the top of that
  CPU's own next `schedule()` call, which is the one place that can
  prove the switch away already happened. External wakers (including
  `wake_blocked_task()`) only ever flip a state flag, never enqueue
  directly. Cost: up to one quantum (~10ms) of added wake latency, in
  exchange for a mechanism that can't corrupt itself under any
  interleaving. See `sched/sched.c`.
- **Table-driven shell dispatch**: `shell/shell.c`'s `commands[]` is
  the single source of truth for every command's name, handler, usage
  string, and blurb — help text, dispatch, tab completion, and fuzzy
  "did you mean" suggestions all read from the same table instead of
  drifting independently.
- **Non-POSIX shell heuristics**: a synonym table lets several
  spellings of a command execute identically (`list`==`ls`, `tasks`==`ps`,
  ...); an unrecognised command gets a "did you mean" via integer
  Levenshtein distance (no floats anywhere in this build); a
  lazy-decay integer frecency score per command powers `topcmds` and
  suggestion tie-breaking.
- **UTF-8 decoding lives at exactly one layer**: `klib/utf8.c`'s
  `utf8_decode()` is used only by `console_puts()` — it decodes a
  formatted string in one pass, renders a recognised box-drawing
  codepoint through `nx_box8x8.h`'s table, and falls back to a single
  `?` for anything else. `console_putc()` stays a raw single-byte
  pass-through, since every direct caller only ever hands it one
  already-known ASCII byte. `serial_puts()` deliberately isn't
  UTF-8-aware — a real terminal on the other end already understands
  UTF-8 natively.
- **Graph FS persistence is a whole-graph snapshot**, not a journaled
  on-disk structure: `graph_save_to_disk()`/`graph_load_from_disk()`
  serialize/deserialize the entire in-memory graph in one shot.
  Deliberately simple — prove the boring, obviously-correct version
  first. Node IDs are preserved exactly across a save/load
  round-trip; refcounts are never stored, only recomputed by replaying
  the same `graph_link()`/`sstring_set()` calls normal operation uses.
- **Graph FS deletion is refcounting with a mark-and-sweep backstop**:
  `graph_unlink()`/`sstring_unset()`/`graph_node_delete()` free a node
  the instant its refcount hits 0, cascading iteratively (an explicit
  worklist, not recursion, so a long chain can't blow the kernel
  stack). Refcounting alone can't reclaim a reference cycle, so `ggc`
  backstops it with a real mark-and-sweep: BFS-mark everything
  reachable from every current anchor, free whatever the sweep never
  reached. `gclear` shares the same sweep after first dropping every
  anchor, so it correctly empties the graph completely, cycles
  included.
- **`sys_brk` shrink is a real unmap**: `vmm_unmap_page_in()`/
  `vmm_unmap_range_in()` walk to each leaf PTE, clear it, `invlpg` it
  locally, and return the physical frame to the PMM. Only a local
  invalidation, not a full shootdown — a user address space belongs to
  exactly one task, and a task only ever runs on one CPU at a time.
- **`O_TRUNC` is an optional `vnode_ops` entry**, not tmpfs-specific
  logic bolted onto `vfs_open()` — a future filesystem that can't
  support truncation fails `open()` cleanly via a NULL function
  pointer instead of needing a filesystem-type check in the VFS layer.

## Known limitations

- No `fork`/`exec` in the POSIX sense — `split()`/`exec()` cover the
  same ground with non-POSIX semantics (see the README); a spawned
  process is created fully-formed from an ELF path via
  `run`/`process_spawn`, with no argv/envp passing.
- No network stack.
- The direct map and kernel image use plain 4KiB pages, not huge
  pages — simpler and safer to hand off unverified, at some
  page-table-memory and TLB cost.
- The framebuffer is mapped write-back, not write-combining, in the
  direct map, for the same simplicity tradeoff.
- `open()`'s `O_RDWR`/`O_WRONLY`/`O_RDONLY` are enforced; there's no
  `O_APPEND` yet.
- The ELF loader only accepts static, non-PIE `ET_EXEC` binaries — no
  dynamic linking, no relocations, no PIE.
- The NVMe driver is single-namespace, single-I/O-queue, and caps a
  single request at ~2MiB (one PRP-list page, not chained). Buffers
  must be page-aligned. No timeout on command completion.
- virtio-blk is a second block-device driver behind the same HAL as
  NVMe, but is compile-tested only — not yet verified against real
  hardware or QEMU.
- Tab completion walks the classic VFS namespace generically via
  `vfs_readdir()`, so ordinary paths complete fine. It doesn't
  complete a bare graph-native argument (an sstring name or numeric ID
  handed to `sstring`/`glink`/`grm` when not written as a path).
- `graph_load_from_disk()` refuses to run against a non-empty
  in-memory graph — run `gclear` first to reload mid-session. A node
  created after a `gload` draws its ID from the restored counter,
  which can numerically reuse an ID that existed earlier in the same
  session (never one currently live). Persistence is capped at
  `GRAPH_SNAPSHOT_MAX_BYTES` (1MiB by default), and one coarse lock
  covers the whole graph.
- UTF-8 decoding only renders a codepoint that matches one of
  `nx_box8x8.h`'s 22 box-drawing glyphs — everything else valid-but-
  unmapped (accented Latin, CJK, emoji) falls back to `?`. No
  combining-character support, no wide/fullwidth glyph-width
  accounting. Keyboard input is still pure single-byte ASCII (Scan
  Code Set 1 US QWERTY has no way to produce anything else), so
  there's no way to type a non-ASCII character in, only to display one
  that arrived some other way (typically GraphFS file content).
- No per-task stdin isolation — every ring-3 task shares one keyboard
  ring buffer with no foreground-job concept, which makes interactive
  stdin-reading programs unsuitable as /bin/init-supervised services.

## Kernel command line

Set from `limine.conf`'s `cmdline:` lines, or edited directly:

- `nosmp` — stay uniprocessor, don't release any APs.
- `selftest` — after boot, automatically `run` `/bin/hello` in ring 3
  and report pass/fail before handing off to the shell.
- `kshell` — boot into the ring-0 kernel shell (`shell/shell.c`)
  instead of nsh (`/bin/nsh`), the default interactive target. The
  kernel shell is where every privileged/diagnostic command lives
  (`lspci`, `meminfo`, `reboot`/`shutdown`, the native graph commands
  with `gsync`/`gload`/`ggc`/`gclear`) — nsh only has what's reachable
  through the ordinary syscall ABI.

## Build dependencies

The top-level `Makefile` downloads a pinned Limine release and builds
its host tool + bootloader stages. `kernel/get-deps` separately
fetches `limine.h` (BSD-2-Clause, from the Limine protocol repo) at
build time — not vendored, so the build always tracks current
upstream. See `LICENSE` for the full accounting.
