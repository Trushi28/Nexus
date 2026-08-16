#include "shell/shell.h"
#include "apic/lapic.h"
#include "boot/requests.h"
#include "cpu/cpu.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "debug/serial.h"
#include "drivers/keyboard.h"
#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "fs/graph.h"
#include "fs/vfs.h"
#include "klib/klib.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/timer.h"
#include "video/console.h"
#include "video/fb.h"

#define LINE_MAX 200
#define CMD_NAME_MAX 16 /* longest canonical command name + slop */
#define WORD_MAX                                                               \
  24 /* edit_distance() operates on at most this many chars per word */
#define SUGGEST_MAX 3 /* "did you mean" shows at most this many */
#define SUGGEST_DIST_MAX                                                       \
  3 /* beyond this edit distance, don't bother suggesting */
#define COMPLETE_MAX_MATCHES 32 /* tab-completion candidate cap */

/* ------------------------------- line editing --------------------------- */

static void print_prompt(void) { kprintf("nexus> "); }

static void echo_backspace(void) {
  serial_puts("\b \b");
  console_backspace();
}

static const char *skip_spaces(const char *s) {
  while (*s == ' ') {
    s++;
  }
  return s;
}

static void fmt_bytes(char *out, size_t out_len, uint64_t bytes) {
  if (bytes >= 1024ULL * 1024 * 1024) {
    ksnprintf(out, out_len, "%lu MiB", bytes / (1024 * 1024));
  } else if (bytes >= 1024ULL * 1024) {
    ksnprintf(out, out_len, "%lu KiB", bytes / 1024);
  } else {
    ksnprintf(out, out_len, "%lu B", bytes);
  }
}

static void print_left(const char *s, int width) {
  int n = 0;
  for (const char *p = s; *p; p++) {
    n++;
  }
  kprintf("%s", s);
  for (int i = n; i < width; i++) {
    kprintf(" ");
  }
}

static bool same_category(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

/* ----------------------------- command table ----------------------------
 * Every shell command in one place: name, handler, a short usage suffix,
 * and a one-line blurb. `cmd_help()` renders itself from this table
 * instead of a hand-maintained string -- the exact class of bug that
 * caused help text to silently truncate a few builds back (a hand-
 * written cmd_help() overran kprintf()'s fixed buffer; see debug/log.c)
 * can't happen this way, since each entry prints through its own small
 * kprintf() call. `category` groups related commands under a shared
 * header in `help`'s output -- NULL is the default/core group; entries
 * sharing a category must stay adjacent in this table. `frecency_score`
 * is the only mutable field -- see dispatch()'s comment on how it's
 * updated, and cmd_topcmds()/suggest_commands() for what it's used for.
 * --------------------------------------------------------------------- */

typedef void (*cmd_handler_t)(const char *args);

struct shell_command {
  const char *name;
  cmd_handler_t handler;
  const char *usage;
  const char *blurb;
  const char *category;
  uint32_t frecency_score;
};

/* Forward declarations -- every handler is defined further down the
 * file, but `commands[]` below needs to take their addresses first. */
static void cmd_help(const char *args);
static void cmd_clear(const char *args);
static void cmd_echo(const char *args);
static void cmd_meminfo(const char *args);
static void cmd_cpuinfo(const char *args);
static void cmd_uptime(const char *args);
static void cmd_ps(const char *args);
static void cmd_lspci(const char *args);
static void cmd_uname(const char *args);
static void cmd_nvmeinfo(const char *args);
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_run(const char *args);
static void cmd_jobs(const char *args);
static void cmd_kill(const char *args);
static void cmd_matrix(const char *args);
static void cmd_reboot(const char *args);
static void cmd_aliases(const char *args);
static void cmd_topcmds(const char *args);
static void cmd_gmk(const char *args);
static void cmd_gtouch(const char *args);
static void cmd_sstring_set(const char *args);
static void cmd_sstrings(const char *args);
static void cmd_glink(const char *args);
static void cmd_gls(const char *args);
static void cmd_gcat(const char *args);
static void cmd_gwrite(const char *args);
static void cmd_gnodeinfo(const char *args);
static void cmd_gnodes(const char *args);
static void cmd_gsync(const char *args);
static void cmd_gload(const char *args);
static void cmd_grm(const char *args);
static void cmd_gunlink(const char *args);
static void cmd_sstringrm(const char *args);
static void cmd_gclear(const char *args);
static void cmd_ggc(const char *args);

/* ------------------------------- synonyms --------------------------------
 * Non-POSIX by design: several spellings of the same idea execute
 * identically rather than one blessed name -- see dispatch()'s
 * normalize_verb(). Purely a naming layer; every alias resolves to a
 * canonical entry in `commands[]` below before lookup. */
struct shell_synonym {
  const char *alias;
  const char *canonical;
};

static const struct shell_synonym synonyms[] = {
    {"list", "ls"},          {"dir", "ls"},       {"type", "cat"},
    {"read", "cat"},         {"exec", "run"},     {"spawn", "run"},
    {"start", "run"},        {"tasks", "ps"},     {"procs", "ps"},
    {"who", "ps"},           {"cls", "clear"},    {"clr", "clear"},
    {"devices", "lspci"},    {"pci", "lspci"},    {"ver", "uname"},
    {"version", "uname"},    {"mem", "meminfo"},  {"free", "meminfo"},
    {"cpu", "cpuinfo"},      {"say", "echo"},     {"nodes", "gnodes"},
    {"anchors", "sstrings"}, {"link", "glink"},   {"mk", "gmk"},
    {"new", "gmk"},          {"write", "gwrite"}, {"info", "gnodeinfo"},
    {"save", "gsync"},       {"load", "gload"},   {"rm", "grm"},
    {"del", "grm"},          {"delete", "grm"},   {"unlink", "gunlink"},
    {"wipe", "gclear"},      {"touch", "gtouch"}, {"gc", "ggc"},
};

static const char *normalize_verb(const char *verb, char *scratch,
                                  size_t scratch_size) {
  for (size_t i = 0; i < ARRAY_LEN(synonyms); i++) {
    if (strcmp(verb, synonyms[i].alias) == 0) {
      strncpy(scratch, synonyms[i].canonical, scratch_size - 1);
      scratch[scratch_size - 1] = '\0';
      return scratch;
    }
  }
  return verb;
}

static struct shell_command commands[] = {
    {"help", cmd_help, "", "this text", NULL, 0},
    {"clear", cmd_clear, "", "clear the screen", NULL, 0},
    {"echo", cmd_echo, "<text>", "print text back", NULL, 0},
    {"meminfo", cmd_meminfo, "", "physical memory + heap usage", NULL, 0},
    {"cpuinfo", cmd_cpuinfo, "", "list online CPUs and APIC mode", NULL, 0},
    {"uptime", cmd_uptime, "", "time since boot", NULL, 0},
    {"ps", cmd_ps, "", "list every live task", NULL, 0},
    {"lspci", cmd_lspci, "", "enumerate PCI devices", NULL, 0},
    {"uname", cmd_uname, "", "kernel/bootloader version info", NULL, 0},
    {"nvmeinfo", cmd_nvmeinfo, "",
     "NVMe namespace 1 info (if a controller was found)", NULL, 0},
    {"ls", cmd_ls, "[path]", "list a directory (default: /)", NULL, 0},
    {"cat", cmd_cat, "<path>", "print a file's contents", NULL, 0},
    {"run", cmd_run, "<path> [&]",
     "load and run an ELF binary in ring 3 (append & to background it)", NULL,
     0},
    {"jobs", cmd_jobs, "", "list background jobs spawned with 'run ... &'",
     NULL, 0},
    {"kill", cmd_kill, "<pid>",
     "signal a ring-3 task to exit at its next syscall", NULL, 0},
    {"matrix", cmd_matrix, "", "you know the one -- any key to stop", NULL, 0},
    {"reboot", cmd_reboot, "",
     "save if dirty,then reset the machine (8042 controller pulse)", NULL, 0},
    {"aliases", cmd_aliases, "", "list command shortcuts (list==ls, etc)", NULL,
     0},
    {"topcmds", cmd_topcmds, "", "your most-used commands, most frecent first",
     NULL, 0},

    {"gmk", cmd_gmk, "[label]", "create a new, unlinked graph node",
     "Graph FS (experimental)", 0},
    {"gtouch", cmd_gtouch, "<path>",
     "create (or reach) a node at a path -- auto-creates every "
     "missing anchor/edge along the way",
     "Graph FS (experimental)", 0},
    {"sstring", cmd_sstring_set, "<name> <ref>",
     "point a named anchor at a node (ref: id or path)",
     "Graph FS (experimental)", 0},
    {"sstrings", cmd_sstrings, "", "list assigned anchors",
     "Graph FS (experimental)", 0},
    {"glink", cmd_glink, "<ref> <edge> <ref>",
     "add a named edge between two nodes", "Graph FS (experimental)", 0},
    {"gls", cmd_gls, "<path>", "list a node's outgoing edges",
     "Graph FS (experimental)", 0},
    {"gcat", cmd_gcat, "<path>", "print a node's content",
     "Graph FS (experimental)", 0},
    {"gwrite", cmd_gwrite, "<ref> <text>", "write text into a node",
     "Graph FS (experimental)", 0},
    {"gnodeinfo", cmd_gnodeinfo, "<ref>",
     "id/label/size/edges/refcount for a node", "Graph FS (experimental)", 0},
    {"gnodes", cmd_gnodes, "", "flat listing of every graph node that exists",
     "Graph FS (experimental)", 0},
    {"gsync", cmd_gsync, "",
     "save the graph to disk (overwrites any previous save)",
     "Graph FS (experimental)", 0},
    {"gload", cmd_gload, "",
     "reload the graph from disk (only if currently empty)",
     "Graph FS (experimental)", 0},
    {"grm", cmd_grm, "<ref>", "delete a node (only if nothing references it)",
     "Graph FS (experimental)", 0},
    {"gunlink", cmd_gunlink, "<from-ref> <edge>",
     "remove a named edge (frees the target if now unreferenced)",
     "Graph FS (experimental)", 0},
    {"sstringrm", cmd_sstringrm, "<name>",
     "remove an sstring anchor (frees it if now unreferenced)",
     "Graph FS (experimental)", 0},
    {"ggc", cmd_ggc, "",
     "collect reference cycles unreachable from any anchor "
     "(mark-and-sweep)",
     "Graph FS (experimental)", 0},
    {"gclear", cmd_gclear, "",
     "wipe the entire graph (frees everything reachable)",
     "Graph FS (experimental)", 0},
};

/* --------------------------- fuzzy suggestions ---------------------------
 * Plain integer Levenshtein distance (no floats anywhere in this kernel
 * -- see the -mno-sse/-mno-mmx/-mno-80387 build flags) between the
 * mistyped word and every known command name, so an unrecognised
 * command can offer a real "did you mean" instead of just "unknown
 * command". Rolling two-row DP -- these strings are a handful of
 * characters, nothing here needs to be fast, just correct. */
static int edit_distance(const char *a, const char *b) {
  int la = (int)strlen(a);
  int lb = (int)strlen(b);
  if (la > WORD_MAX)
    la = WORD_MAX;
  if (lb > WORD_MAX)
    lb = WORD_MAX;

  int prev[WORD_MAX + 1];
  int cur[WORD_MAX + 1];

  for (int j = 0; j <= lb; j++) {
    prev[j] = j;
  }

  for (int i = 1; i <= la; i++) {
    cur[0] = i;
    for (int j = 1; j <= lb; j++) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      int del = prev[j] + 1;
      int ins = cur[j - 1] + 1;
      int sub = prev[j - 1] + cost;
      int m = del < ins ? del : ins;
      if (sub < m) {
        m = sub;
      }
      cur[j] = m;
    }
    for (int j = 0; j <= lb; j++) {
      prev[j] = cur[j];
    }
  }
  return prev[lb];
}

/* Collects up to SUGGEST_MAX commands within SUGGEST_DIST_MAX edits of
 * `typed`, closest first, breaking ties by frecency_score (a command
 * used often is a more likely typo target than one never touched this
 * boot) -- see dispatch()'s comment on how that score is maintained. */
static void suggest_commands(const char *typed) {
  if (*typed == '\0') {
    return;
  }

  const struct shell_command *matches[SUGGEST_MAX];
  int match_dist[SUGGEST_MAX];
  int match_count = 0;

  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    int d = edit_distance(typed, commands[i].name);
    if (d > SUGGEST_DIST_MAX) {
      continue;
    }
    if (match_count == SUGGEST_MAX && d >= match_dist[SUGGEST_MAX - 1]) {
      continue; /* no better than everything we're already keeping */
    }

    int pos = (match_count < SUGGEST_MAX) ? match_count : SUGGEST_MAX - 1;
    if (match_count < SUGGEST_MAX) {
      match_count++;
    }
    while (pos > 0 &&
           (match_dist[pos - 1] > d ||
            (match_dist[pos - 1] == d &&
             matches[pos - 1]->frecency_score < commands[i].frecency_score))) {
      match_dist[pos] = match_dist[pos - 1];
      matches[pos] = matches[pos - 1];
      pos--;
    }
    match_dist[pos] = d;
    matches[pos] = &commands[i];
  }

  if (match_count == 0) {
    return;
  }

  kprintf("did you mean: ");
  for (int i = 0; i < match_count; i++) {
    kprintf("%s%s", matches[i]->name, (i + 1 < match_count) ? ", " : "\n");
  }
}

/* ------------------------------ tab completion ---------------------------
 * Completes either the command verb (no space typed yet) or, for an
 * argument, the last '/'-delimited segment of a VFS path -- graph-FS
 * paths (gcat/gls/sstring targets) aren't wired up here yet, only the
 * classic vfs.c namespace ("/", tmpfs, the initrd). A single unambiguous
 * match is completed inline; multiple matches get listed and the prompt
 * line is redrawn so editing can continue. */
static void try_complete(char *buf, uint32_t *len, uint32_t max) {
  uint32_t word_start = *len;
  while (word_start > 0 && buf[word_start - 1] != ' ') {
    word_start--;
  }
  bool is_first_word = (word_start == 0);

  char prefix[LINE_MAX];
  uint32_t prefix_len = *len - word_start;
  memcpy(prefix, buf + word_start, prefix_len);
  prefix[prefix_len] = '\0';

  const char *dir_path = "/";
  char dir_buf[LINE_MAX];
  const char *seg_prefix = prefix;

  if (!is_first_word) {
    const char *last_slash = NULL;
    for (const char *p = prefix; *p; p++) {
      if (*p == '/') {
        last_slash = p;
      }
    }
    if (last_slash != NULL) {
      size_t dlen = (size_t)(last_slash - prefix);
      if (dlen == 0) {
        dir_buf[0] = '/';
        dir_buf[1] = '\0';
      } else {
        memcpy(dir_buf, prefix, dlen);
        dir_buf[dlen] = '\0';
      }
      dir_path = dir_buf;
      seg_prefix = last_slash + 1;
    }
  }

  char candidates[COMPLETE_MAX_MATCHES][64];
  uint32_t n = 0;

  if (is_first_word) {
    for (size_t i = 0; i < ARRAY_LEN(commands) && n < COMPLETE_MAX_MATCHES;
         i++) {
      if (str_has_prefix(commands[i].name, seg_prefix)) {
        strncpy(candidates[n], commands[i].name, sizeof(candidates[n]) - 1);
        candidates[n][sizeof(candidates[n]) - 1] = '\0';
        n++;
      }
    }
  } else {
    char name[64];
    uint32_t idx = 0;
    while (n < COMPLETE_MAX_MATCHES &&
           vfs_readdir(dir_path, idx, name, sizeof(name))) {
      if (str_has_prefix(name, seg_prefix)) {
        strncpy(candidates[n], name, sizeof(candidates[n]) - 1);
        candidates[n][sizeof(candidates[n]) - 1] = '\0';
        n++;
      }
      idx++;
    }
  }

  if (n == 0) {
    return; /* nothing matches -- silently ignore, no beep support */
  }

  if (n == 1) {
    const char *full = candidates[0];
    size_t full_len = strlen(full);
    size_t seg_len = strlen(seg_prefix);
    for (size_t i = seg_len; i < full_len && *len + 1 < max; i++) {
      buf[(*len)++] = full[i];
      kprintf("%c", full[i]);
    }
    return;
  }

  kprintf("\n");
  for (uint32_t i = 0; i < n; i++) {
    kprintf("  %s\n", candidates[i]);
  }
  buf[*len] = '\0';
  print_prompt();
  kprintf("%s", buf);
}

static uint32_t read_line(char *buf, uint32_t max) {
  uint32_t len = 0;
  for (;;) {
    char c = keyboard_getc();

    if (c == '\n') {
      kprintf("\n");
      buf[len] = '\0';
      return len;
    }

    if (c == '\b') {
      if (len > 0) {
        len--;
        echo_backspace();
      }
      continue;
    }

    if (c == '\t') {
      try_complete(buf, &len, max);
      continue;
    }

    if (len + 1 < max && c >= 0x20 && c < 0x7F) {
      buf[len++] = c;
      kprintf("%c", c);
    }
  }
}

/* ------------------------------ graph FS helpers -------------------------- */

static struct gnode *resolve_ref(const char *ref) {
  bool all_digits = (*ref != '\0');
  for (const char *p = ref; *p; p++) {
    if (*p < '0' || *p > '9') {
      all_digits = false;
      break;
    }
  }
  if (all_digits) {
    uint64_t id = 0;
    for (const char *p = ref; *p; p++) {
      id = id * 10 + (uint64_t)(*p - '0');
    }
    struct gnode *n = graph_find_by_id(id);
    if (n != NULL) {
      return n;
    }
  }
  return graph_resolve(ref);
}

/* ------------------------------ background jobs --------------------------
 * A small, fixed-size table of jobs started via `run <path> &`.
 * Deliberately shell-owned, not scheduler-owned: struct task doesn't
 * know or care whether something is "backgrounded" -- that's purely
 * bookkeeping this file maintains so `jobs`/`kill` have something to
 * list/target, and so a finished background job eventually gets
 * sched_wait_task()'d (freeing its kernel stack, struct task, and its
 * whole address space) by SOMEONE, since nothing else will. Reaped
 * lazily, once per prompt (see shell_task()'s loop) rather than by a
 * dedicated background task -- keeps this table single-threaded/
 * lock-free, at the cost of a finished job sitting as a zombie for
 * however long the user takes to hit Enter next. Real shells behave
 * the same way absent proper job-control signals. */
#define MAX_JOBS 8

struct bg_job {
  bool used;
  uint32_t job_id;
  struct task *task;
  char name[32];
};
static struct bg_job jobs[MAX_JOBS];
static uint32_t next_job_id = 1;

/* Strips a trailing `&` (and surrounding whitespace) in place --
 * the one bit of syntax this parser understands beyond "everything
 * after the verb is args, verbatim" (see docs/Design.md's note on
 * quoting). Mutates `s` directly; every caller here passes a handler's
 * `args`, which always points into shell_task()'s own `line` buffer,
 * not a string literal, so this is safe. */
static bool strip_trailing_background(char *s) {
  size_t len = strlen(s);
  while (len > 0 && s[len - 1] == ' ') {
    len--;
  }
  if (len == 0 || s[len - 1] != '&') {
    return false;
  }
  len--;
  while (len > 0 && s[len - 1] == ' ') {
    len--;
  }
  s[len] = '\0';
  return true;
}

static void reap_finished_jobs(void) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].used) {
      continue;
    }
    if (!sched_task_is_dead(jobs[i].task)) {
      continue;
    }

    /* Capture everything we need to print BEFORE calling
     * process_wait() -- that frees jobs[i].task (it's already
     * TASK_DEAD, so sched_wait_task() takes the non-blocking reap
     * path straight to kfree()), so touching the pointer again after
     * would be a use-after-free. */
    uint64_t pid = jobs[i].task->id;
    char name[sizeof(jobs[i].name)];
    strncpy(name, jobs[i].name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    uint32_t job_id = jobs[i].job_id;

    int code = process_wait(jobs[i].task);
    jobs[i].used = false;
    jobs[i].task = NULL;

    kprintf("[%u] pid %lu (%s) done, exit code %d\n", job_id, pid, name, code);
  }
}

/* -------------------------------- handlers -------------------------------- */

static void cmd_help(const char *args) {
  (void)args;
  kprintf("Nexus shell -- available commands:\n");

  const char *last_category = NULL;
  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    if (!same_category(commands[i].category, last_category)) {
      kprintf("\n%s:\n", commands[i].category ? commands[i].category : "core");
      last_category = commands[i].category;
    }
    char namebuf[32];
    if (commands[i].usage[0] != '\0') {
      ksnprintf(namebuf, sizeof(namebuf), "%s %s", commands[i].name,
                commands[i].usage);
    } else {
      ksnprintf(namebuf, sizeof(namebuf), "%s", commands[i].name);
    }
    kprintf("  ");
    print_left(namebuf, 26);
    kprintf("%s\n", commands[i].blurb);
  }

  kprintf("\n'aliases' lists shortcuts (list==ls, etc). Unknown commands\n"
          "get a fuzzy 'did you mean'. Tab completes commands and paths.\n");
}

static void cmd_clear(const char *args) {
  (void)args;
  console_clear();
}

static void cmd_echo(const char *args) { kprintf("%s\n", args); }

static void cmd_meminfo(const char *args) {
  (void)args;
  char total[32], used[32], free_[32], hused[32], hcap[32];
  fmt_bytes(total, sizeof(total), pmm_total_bytes());
  fmt_bytes(used, sizeof(used), pmm_used_bytes());
  fmt_bytes(free_, sizeof(free_), pmm_free_bytes());
  fmt_bytes(hused, sizeof(hused), heap_used_bytes());
  fmt_bytes(hcap, sizeof(hcap), heap_capacity_bytes());

  kprintf("physical: %s total, %s used, %s free\n", total, used, free_);
  kprintf("heap:     %s used, %s reserved\n", hused, hcap);
}

static void cmd_cpuinfo(const char *args) {
  (void)args;
  kprintf("%u CPU(s) known, x2APIC %s\n", g_cpu_count,
          lapic_using_x2apic() ? "enabled" : "not in use");
  for (uint32_t i = 0; i < g_cpu_count; i++) {
    struct cpu_local *c = g_cpus[i];
    kprintf("  cpu%u: lapic_id=%u %s %s\n", c->cpu_index, c->lapic_id,
            c->is_bsp ? "(BSP)" : "(AP) ",
            c->online || c->is_bsp ? "online" : "offline");
  }
}

static void cmd_uptime(const char *args) {
  (void)args;
  uint64_t ms = timer_uptime_ms();
  uint64_t s = ms / 1000;
  kprintf("up %lu:%02lu:%02lu.%03lu\n", s / 3600, (s / 60) % 60, s % 60,
          ms % 1000);
}

static void ps_print_one(struct task *t, void *arg) {
  (void)arg;
  static const char *state_names[] = {
      "ready", "running", "sleeping", "blocked", "exiting", "dead",
  };
  kprintf("  %4lu  ", t->id);
  print_left(t->name, 18);
  print_left(state_names[t->state], 11);
  kprintf("%s\n", t->is_user ? "user" : "kernel");
}

static void cmd_ps(const char *args) {
  (void)args;
  kprintf("  %4s  ", "PID");
  print_left("NAME", 18);
  print_left("STATE", 11);
  kprintf("%s\n", "RING");
  sched_for_each_task(ps_print_one, NULL);
}

static void cmd_lspci(const char *args) {
  (void)args;
  pci_scan();
  uint32_t n = pci_device_count();
  kprintf("%u device(s):\n", n);
  for (uint32_t i = 0; i < n; i++) {
    const struct pci_device *d = pci_device_at(i);
    kprintf("  %02x:%02x.%x  %04x:%04x  %s\n", d->bus, d->slot, d->func,
            d->vendor_id, d->device_id, pci_class_name(d->class_code));
  }
}

static void cmd_uname(const char *args) {
  (void)args;
  kprintf("Nexus OS -- x86-64, SMP, x2APIC\n");
  kprintf("bootloader: %s %s\n", g_boot.bootloader_name,
          g_boot.bootloader_version);
  kprintf("firmware:   %s\n",
          g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_X86BIOS ? "BIOS"
          : g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_EFI32 ? "UEFI (32-bit)"
          : g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_EFI64 ? "UEFI (64-bit)"
                                                               : "unknown");
}

static void cmd_nvmeinfo(const char *args) {
  (void)args;
  if (!nvme_available()) {
    kprintf("no NVMe controller available\n");
    return;
  }
  uint64_t sectors = nvme_sector_count();
  uint32_t sector_size = nvme_sector_size();
  char cap[32];
  fmt_bytes(cap, sizeof(cap), sectors * sector_size);
  kprintf("namespace 1: %lu sectors x %u bytes = %s\n", sectors, sector_size,
          cap);
}

static void cmd_ls(const char *args) {
  const char *path = (*args == '\0') ? "/" : args;
  char name[64];
  uint32_t i = 0;
  bool any = false;
  while (vfs_readdir(path, i, name, sizeof(name))) {
    kprintf("  %s\n", name);
    i++;
    any = true;
  }
  if (!any) {
    kprintf("(empty, or no such directory)\n");
  }
}

static void cmd_cat(const char *args) {
  if (*args == '\0') {
    kprintf("usage: cat <path>\n");
    return;
  }
  struct vfs_file *f;
  if (!vfs_open(args, false, &f)) {
    kprintf("cat: %s: no such file\n", args);
    return;
  }
  char buf[257];
  size_t n;
  while ((n = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {
    buf[n] = '\0';
    kprintf("%s", buf);
  }
  kprintf("\n");
  vfs_close(f);
}

static void cmd_run(const char *args) {
  if (*args == '\0') {
    kprintf("usage: run <path> [&]\n");
    return;
  }

  char pathbuf[LINE_MAX];
  strncpy(pathbuf, args, sizeof(pathbuf) - 1);
  pathbuf[sizeof(pathbuf) - 1] = '\0';

  bool background = strip_trailing_background(pathbuf);

  struct task *t = process_spawn(pathbuf, pathbuf);
  if (t == NULL) {
    return;
  }

  if (!background) {
    int code = process_wait(t);
    kprintf("[%s exited with code %d]\n", pathbuf, code);
    return;
  }

  int slot = -1;
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    kprintf("run: job table full (%d slots) -- 'jobs' to see them, wait for "
            "one to finish, or 'kill' one first\n",
            MAX_JOBS);
    return;
  }

  jobs[slot].used = true;
  jobs[slot].job_id = next_job_id++;
  jobs[slot].task = t;
  strncpy(jobs[slot].name, pathbuf, sizeof(jobs[slot].name) - 1);
  jobs[slot].name[sizeof(jobs[slot].name) - 1] = '\0';

  kprintf("[%u] pid %lu\n", jobs[slot].job_id, t->id);
}

static void cmd_jobs(const char *args) {
  (void)args;
  reap_finished_jobs();
  bool any = false;
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].used) {
      continue;
    }
    kprintf("  [%u]  pid %-6lu  %s\n", jobs[i].job_id, jobs[i].task->id,
            jobs[i].name);
    any = true;
  }
  if (!any) {
    kprintf("(no background jobs)\n");
  }
}

static void cmd_kill(const char *args) {
  if (*args == '\0') {
    kprintf("usage: kill <pid>\n");
    return;
  }
  bool all_digits = true;
  for (const char *p = args; *p; p++) {
    if (*p < '0' || *p > '9') {
      all_digits = false;
      break;
    }
  }
  if (!all_digits) {
    kprintf("kill: '%s' isn't a pid -- see 'jobs' or 'ps'\n", args);
    return;
  }
  uint64_t pid = 0;
  for (const char *p = args; *p; p++) {
    pid = pid * 10 + (uint64_t)(*p - '0');
  }

  struct task *t = sched_find_waitable_task(pid);
  if (t == NULL) {
    kprintf("kill: no such ring-3 task with pid %lu\n", pid);
    return;
  }
  sched_kill_task(t);
  kprintf("kill: signalled pid %lu -- it exits at its next syscall (see "
          "sched_kill_task() for why not instantly)\n",
          pid);
}

#define MATRIX_COLS_MAX 256

static void cmd_matrix(const char *args) {
  (void)args;
  if (!fb_available()) {
    kprintf("matrix: no framebuffer available\n");
    return;
  }

  uint32_t cols = console_cols();
  uint32_t rows = console_rows();
  if (cols == 0 || rows == 0) {
    return;
  }
  if (cols > MATRIX_COLS_MAX) {
    cols = MATRIX_COLS_MAX;
  }

  keyboard_flush();
  kprintf("(matrix rain -- press any key to stop)\n");

  int drop_row[MATRIX_COLS_MAX];
  uint32_t rng = (uint32_t)(timer_uptime_ms() * 2654435761ULL) | 1u;
  for (uint32_t c = 0; c < cols; c++) {
    rng = rng * 1103515245u + 12345u;
    drop_row[c] = -(int)(rng % rows);
  }

  console_set_colors(0x0033FF55, 0x00050805);

  while (!keyboard_haskey()) {
    for (uint32_t c = 0; c < cols; c++) {
      rng = rng * 1103515245u + 12345u;
      char glyph = (char)(0x21 + (rng >> 16) % (0x7E - 0x21));

      int r = drop_row[c];
      if (r >= 0 && (uint32_t)r < rows) {
        console_putc_at(c, (uint32_t)r, glyph);
      }
      int tail = r - 1;
      if (tail >= 0 && (uint32_t)tail < rows) {
        console_putc_at(c, (uint32_t)tail, ' ');
      }

      drop_row[c] = r + 1;
      rng = rng * 1103515245u + 12345u;
      if (drop_row[c] > (int)(rows + rng % rows)) {
        drop_row[c] = -(int)(rng % rows);
      }
    }
    sched_sleep_ms(45);
  }

  keyboard_flush();
  console_set_colors(0x00E0E0E0, 0x000B0E14);
  console_clear();
}

static void cmd_reboot(const char *args) {
  (void)args;
  if (graph_is_dirty()) {
    kprintf("graph has unsaved changes -- saving before reboot...\n");
    graph_save_to_disk(); /* logs its own outcome; a failed save still
                              falls through to reboot -- refusing to
                              reboot over a save failure would be
                              worse */
  }
  kprintf("rebooting...\n");
  timer_busy_wait_ms(50);
  uint8_t status;
  do {
    status = inb(0x64);
  } while (status & 0x02);
  outb(0x64, 0xFE);
  hang();
}

static void cmd_aliases(const char *args) {
  (void)args;
  kprintf("shortcuts -- identical effect to their canonical command:\n");
  for (size_t i = 0; i < ARRAY_LEN(synonyms); i++) {
    kprintf("  ");
    print_left(synonyms[i].alias, 10);
    kprintf("-> %s\n", synonyms[i].canonical);
  }
}

static void cmd_topcmds(const char *args) {
  (void)args;
  uint32_t order[ARRAY_LEN(commands)];
  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    order[i] = (uint32_t)i;
  }
  for (size_t i = 1; i < ARRAY_LEN(commands); i++) {
    uint32_t key = order[i];
    uint32_t key_score = commands[key].frecency_score;
    size_t j = i;
    while (j > 0 && commands[order[j - 1]].frecency_score < key_score) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }

  kprintf("most-used commands this boot (frecency-ranked):\n");
  uint32_t shown = 0;
  for (size_t i = 0; i < ARRAY_LEN(commands) && shown < 10; i++) {
    if (commands[order[i]].frecency_score == 0) {
      continue;
    }
    kprintf("  ");
    print_left(commands[order[i]].name, 14);
    kprintf("score=%u\n", commands[order[i]].frecency_score);
    shown++;
  }
  if (shown == 0) {
    kprintf("  (nothing yet -- run a few commands first)\n");
  }
}

static void cmd_gmk(const char *args) {
  struct gnode *n = graph_node_create(*args ? args : NULL);
  if (n == NULL) {
    kprintf("gmk: out of memory\n");
    return;
  }
  kprintf("created node #%lu%s%s\n", n->id, *args ? " " : "", args);
}

static void cmd_gtouch(const char *args) {
  if (*args == '\0') {
    kprintf("usage: gtouch <path>\n");
    return;
  }
  bool created;
  struct gnode *n = graph_touch(args, &created);
  if (n == NULL) {
    kprintf("gtouch: couldn't create '%s' (empty path, or out of memory)\n",
            args);
    return;
  }
  kprintf("%s -> #%lu%s\n", args, n->id,
          created ? " (created)" : " (already existed)");
}

static void cmd_sstring_set(const char *args) {
  char name[SSTRING_NAME_MAX];
  const char *p = args;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < sizeof(name)) {
    name[i++] = *p++;
  }
  name[i] = '\0';
  p = skip_spaces(p);
  if (name[0] == '\0' || *p == '\0') {
    kprintf("usage: sstring <name> <node-ref>\n");
    return;
  }
  struct gnode *target = resolve_ref(p);
  if (target == NULL) {
    kprintf("sstring: no such node '%s'\n", p);
    return;
  }
  if (!sstring_set(name, target)) {
    kprintf("sstring: table full\n");
    return;
  }
  kprintf("%s -> node #%lu\n", name, target->id);
}

static void cmd_sstrings(const char *args) {
  (void)args;
  char name[SSTRING_NAME_MAX];
  uint32_t i = 0;
  bool any = false;
  while (sstring_list(i, name, sizeof(name))) {
    struct gnode *n = sstring_get(name);
    kprintf("  %s -> #%lu\n", name, n->id);
    i++;
    any = true;
  }
  if (!any) {
    kprintf("(no sstring anchors assigned yet -- see 'sstring <name> "
            "<node-ref>')\n");
  }
}

static void cmd_glink(const char *args) {
  char from_ref[64], edge_name[GEDGE_NAME_MAX];
  const char *p = args;
  size_t i;

  i = 0;
  while (*p && *p != ' ' && i + 1 < sizeof(from_ref)) {
    from_ref[i++] = *p++;
  }
  from_ref[i] = '\0';
  p = skip_spaces(p);

  i = 0;
  while (*p && *p != ' ' && i + 1 < sizeof(edge_name)) {
    edge_name[i++] = *p++;
  }
  edge_name[i] = '\0';
  p = skip_spaces(p);

  if (from_ref[0] == '\0' || edge_name[0] == '\0' || *p == '\0') {
    kprintf("usage: glink <from-ref> <edge-name> <to-ref>\n");
    return;
  }

  struct gnode *from = resolve_ref(from_ref);
  struct gnode *to = resolve_ref(p);
  if (from == NULL) {
    kprintf("glink: no such node '%s'\n", from_ref);
    return;
  }
  if (to == NULL) {
    kprintf("glink: no such node '%s'\n", p);
    return;
  }
  graph_link(from, edge_name, to);
  kprintf("#%lu --%s--> #%lu\n", from->id, edge_name, to->id);
}

static void cmd_gls(const char *args) {
  struct gnode *n = resolve_ref(args);
  if (n == NULL) {
    kprintf("gls: no such node '%s'\n", args);
    return;
  }
  char name[GEDGE_NAME_MAX];
  uint32_t i = 0;
  bool any = false;
  while (graph_list_edges(n, i, name, sizeof(name))) {
    struct gnode *target = graph_edge_lookup(n, name);
    kprintf("  %s -> #%lu\n", name, target->id);
    i++;
    any = true;
  }
  if (!any) {
    kprintf("(no outgoing edges)\n");
  }
}

static void cmd_gcat(const char *args) {
  struct gnode *n = resolve_ref(args);
  if (n == NULL) {
    kprintf("gcat: no such node '%s'\n", args);
    return;
  }
  char buf[257];
  uint64_t off = 0;
  size_t got;
  while ((got = graph_read(n, off, buf, sizeof(buf) - 1)) > 0) {
    buf[got] = '\0';
    kprintf("%s", buf);
    off += got;
  }
  kprintf("\n");
}

static void cmd_gwrite(const char *args) {
  char ref[64];
  const char *p = args;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < sizeof(ref)) {
    ref[i++] = *p++;
  }
  ref[i] = '\0';
  p = skip_spaces(p);

  if (ref[0] == '\0' || *p == '\0') {
    kprintf("usage: gwrite <node-ref-or-path> <text>\n");
    return;
  }
  struct gnode *n = resolve_ref(ref);
  bool auto_created = false;
  if (n == NULL) {
    n = graph_touch(ref, &auto_created);
  }
  if (n == NULL) {
    kprintf("gwrite: no such node '%s' and couldn't create it\n", ref);
    return;
  }
  size_t len = strlen(p);
  graph_write(n, 0, p, len);
  kprintf("wrote %lu byte(s) to #%lu%s\n", (uint64_t)len, n->id,
          auto_created ? " (path auto-created)" : "");
}

static void cmd_gnodeinfo(const char *args) {
  struct gnode *n = resolve_ref(args);
  if (n == NULL) {
    kprintf("gnodeinfo: no such node '%s'\n", args);
    return;
  }
  kprintf("id:       #%lu\n", n->id);
  kprintf("label:    %s\n", n->label[0] ? n->label : "(none)");
  kprintf("size:     %lu byte(s)\n", n->size);
  kprintf("edges:    %u\n", n->edge_count);
  kprintf("refcount: %u (incoming edges + sstring anchors)\n", n->refcount);
}

static void gnode_print_one(struct gnode *n, void *arg) {
  (void)arg;
  kprintf("  #%-6lu ", n->id);
  print_left(n->label[0] ? n->label : "-", 20);
  kprintf("size=%-6lu edges=%-4u refs=%u\n", n->size, n->edge_count,
          n->refcount);
}

static void cmd_gnodes(const char *args) {
  (void)args;
  kprintf("  %-7s %-20s\n", "ID", "LABEL");
  graph_for_each_node(gnode_print_one, NULL);
}
static void cmd_gsync(const char *args) {
  (void)args;
  graph_save_to_disk(); /* logs its own success/failure */
}

static void cmd_gload(const char *args) {
  (void)args;
  graph_load_from_disk(); /* logs its own success/failure */
}
static void cmd_grm(const char *args) {
  struct gnode *n = resolve_ref(args);
  if (n == NULL) {
    kprintf("grm: no such node '%s'\n", args);
    return;
  }
  graph_node_delete(n); /* logs its own outcome */
}

static void cmd_gunlink(const char *args) {
  char from_ref[64];
  const char *p = args;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < sizeof(from_ref)) {
    from_ref[i++] = *p++;
  }
  from_ref[i] = '\0';
  p = skip_spaces(p);

  if (from_ref[0] == '\0' || *p == '\0') {
    kprintf("usage: gunlink <from-ref> <edge-name>\n");
    return;
  }
  struct gnode *from = resolve_ref(from_ref);
  if (from == NULL) {
    kprintf("gunlink: no such node '%s'\n", from_ref);
    return;
  }
  graph_unlink(from, p); /* logs its own outcome */
}

static void cmd_sstringrm(const char *args) {
  if (*args == '\0') {
    kprintf("usage: sstringrm <name>\n");
    return;
  }
  sstring_unset(args); /* logs its own outcome */
}

static void cmd_gclear(const char *args) {
  (void)args;
  graph_clear_all(); /* logs its own outcome */
}

static void cmd_ggc(const char *args) {
  (void)args;
  uint32_t freed = graph_collect_cycles();
  if (freed > 0) {
    kprintf("[graph] gc: freed %u node(s) unreachable from any anchor\n",
            freed);
  } else {
    kprintf("[graph] gc: nothing to collect\n");
  }
}

/* --------------------------------- dispatch ------------------------------- */

static void dispatch(char *line) {
  const char *start = skip_spaces(line);
  if (*start == '\0') {
    return;
  }

  char *rest = (char *)start;
  while (*rest != '\0' && *rest != ' ') {
    rest++;
  }
  if (*rest == ' ') {
    *rest = '\0';
    rest++;
    rest = (char *)skip_spaces(rest);
  }

  char scratch[CMD_NAME_MAX];
  const char *verb = normalize_verb(start, scratch, sizeof(scratch));

  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    if (strcmp(verb, commands[i].name) == 0) {
      commands[i].frecency_score = (commands[i].frecency_score >> 1) + 100;
      commands[i].handler(rest);
      return;
    }
  }

  kprintf("unknown command: %s (try 'help')\n", start);
  suggest_commands(start);
}

void shell_task(void *arg) {
  (void)arg;
  char line[LINE_MAX];

  kprintf("\nWelcome to Nexus. Type 'help' for a list of commands.\n");

  for (;;) {
    reap_finished_jobs();
    print_prompt();
    read_line(line, sizeof(line));
    dispatch(line);
  }
}
