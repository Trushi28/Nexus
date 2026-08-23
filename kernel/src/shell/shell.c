#include "shell/shell.h"
#include "abi/syscall_nr.h"
#include "acpi/acpi.h"
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
#define CMD_NAME_MAX 16 // longest canonical command name + slop
#define WORD_MAX                                                               \
  24 // edit_distance() operates on at most this many chars per word
#define SUGGEST_MAX 3      /* "did you mean" shows at most this many */
#define SUGGEST_DIST_MAX 3 // beyond this edit distance, don't bother suggesting
#define COMPLETE_MAX_MATCHES 32 // tab-completion candidate cap

/* ------------------------------ working directory -------------------------
 * A real per-process cwd would live on struct task (sched/sched.h) and
 * cross the syscall boundary with a SYS_chdir -- worth doing once
 * something other than this one interactive shell task needs it, but
 * that's a bigger change (new syscall, ABI bump via abi/syscall_nr.h,
 * `make sync-abi`) than what "let me cd around" actually needs today.
 * This is deliberately the smaller, shell-local version instead: a
 * single static path this file's own command handlers consult, good
 * enough for one interactive task, with the real process-level version
 * left as later work if a second consumer ever shows up. */
static char cwd[LINE_MAX] = "/";

/* Joins `in` onto the shell's cwd (verbatim if `in` is already
 * absolute) and normalizes the result -- collapsing "." and empty
 * segments, and popping one component per ".." -- into `out`. Doesn't
 * touch the VFS at all; whether the result actually exists is up to
 * the caller to check (see cmd_cd()). Structurally the same split-
 * and-walk shape as fs/vfs.c's own walk()/split_path(), kept as its
 * own copy for the same reason vfs.c gives its two internal copies:
 * this is shell-local, cwd-aware path math that has no business
 * living in the VFS layer itself. */
#define CWD_MAX_DEPTH 16

static void resolve_path(const char *in, char *out, size_t out_max) {
  char combined[LINE_MAX * 2];
  if (in[0] == '/') {
    strncpy(combined, in, sizeof(combined) - 1);
    combined[sizeof(combined) - 1] = '\0';
  } else {
    ksnprintf(combined, sizeof(combined), "%s/%s", cwd, in);
  }

  char comps[CWD_MAX_DEPTH][64];
  int depth = 0;

  const char *p = combined;
  while (*p == '/') {
    p++;
  }
  while (*p != '\0' && depth < CWD_MAX_DEPTH) {
    size_t n = 0;
    while (*p != '\0' && *p != '/') {
      if (n + 1 < sizeof(comps[0])) {
        comps[depth][n++] = *p;
      }
      p++;
    }
    comps[depth][n] = '\0';

    if (strcmp(comps[depth], "..") == 0) {
      if (depth > 0) {
        depth--;
      }
    } else if (comps[depth][0] != '\0' && strcmp(comps[depth], ".") != 0) {
      depth++;
    }
    while (*p == '/') {
      p++;
    }
  }

  if (depth == 0) {
    strncpy(out, "/", out_max - 1);
    out[out_max - 1] = '\0';
    return;
  }

  size_t off = 0;
  out[0] = '\0';
  for (int i = 0; i < depth; i++) {
    int n = ksnprintf(out + off, out_max - off, "/%s", comps[i]);
    if (n < 0 || (size_t)n >= out_max - off) {
      break;
    }
    off += (size_t)n;
  }
}

/* ------------------------------- line editing --------------------------- */

static void print_prompt(void) {
  console_set_colors(NX_COLOR_ACCENT, NX_COLOR_BG);
  kprintf("nexus");
  console_set_colors(NX_COLOR_DIM, NX_COLOR_BG);
  kprintf(":%s", cwd);
  console_set_colors(NX_COLOR_FG, NX_COLOR_BG);
  kprintf("> ");
}

static void echo_backspace(void) {
  serial_puts("\b \b");
  console_backspace();
}

/* console_putc_box() (video/console.c) only ever draws to the
 * framebuffer console -- unlike kprintf(), it has no serial mirror
 * (see debug/log.c's kprintf(), which writes to both), so a plain
 * `debug=serial` boot or any other framebuffer-less run would lose
 * every border character drawn straight through it, leaving gaps
 * where the borders below should be. This mirrors a close ASCII
 * equivalent to serial alongside the real Unicode glyph, so serial
 * output still reads as a recognisable table/box instead of holes. */
static void box_putc(enum nx_box_glyph g, char serial_fallback) {
  serial_putc(serial_fallback);
  console_putc_box(g);
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

/* A plain "---...---\n" rule of `width` dashes -- used under table
 * headers (`ps`, `gnodes`) and section headers (`help`'s categories)
 * so they read as a header, not just another row. */
static void print_divider(uint32_t width) {
  for (uint32_t i = 0; i < width; i++) {
    box_putc(NX_BOX_H, '-');
  }
  kprintf("\n");
}

#define BOX_INNER_WIDTH 54

/* One line of the banner box -- "U+2551 text...padding U+2551" --
 * padded/truncated to exactly BOX_INNER_WIDTH columns so every border
 * lines up regardless of what's inside. video/nx_box8x8.h added real
 * double-line box-drawing glyphs a while back; this is the first
 * thing in the shell to actually use them instead of falling back to
 * plain -, |, +. */
static void print_box_line(const char *text) {
  box_putc(NX_BOX_DBL_V, '|');
  kprintf(" ");
  uint32_t n = 0;
  for (const char *p = text; *p != '\0' && n < BOX_INNER_WIDTH - 1; p++, n++) {
    kprintf("%c", *p);
  }
  for (uint32_t i = n; i < BOX_INNER_WIDTH; i++) {
    kprintf(" ");
  }
  box_putc(NX_BOX_DBL_V, '|');
  kprintf("\n");
}

/* Double-line box glyphs have different corners top vs bottom (U+2554
 * and U+2557 vs U+255A and U+255D) unlike the old ASCII '+', which was
 * the same character everywhere -- hence two functions instead of one
 * shared by both. */
static void print_box_border_top(void) {
  box_putc(NX_BOX_DBL_DR, '+');
  for (uint32_t i = 0; i < BOX_INNER_WIDTH + 1; i++) {
    box_putc(NX_BOX_DBL_H, '-');
  }
  box_putc(NX_BOX_DBL_DL, '+');
  kprintf("\n");
}

static void print_box_border_bottom(void) {
  box_putc(NX_BOX_DBL_UR, '+');
  for (uint32_t i = 0; i < BOX_INNER_WIDTH + 1; i++) {
    box_putc(NX_BOX_DBL_H, '-');
  }
  box_putc(NX_BOX_DBL_UL, '+');
  kprintf("\n");
}

/* Same idea as kprintf(), but in the shared "something's wrong" color
 * -- reserved for the handful of error messages a person is most
 * likely to hit while typing interactively (unknown command, bad
 * pid, missing file, job table full). Most of this file's other
 * error kprintf()s are deliberately left in the ordinary foreground
 * color -- see docs/Design.md's note on shell chrome for where that
 * line is drawn, rather than recoloring every kprintf() in the file
 * on the theory that "error-ish" isn't the same judgment call twice
 * in a row. */
static __attribute__((format(printf, 1, 2))) void print_error(const char *fmt,
                                                              ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  kvsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  console_set_colors(NX_COLOR_ERROR, NX_COLOR_BG);
  kprintf("%s", buf);
  console_set_colors(NX_COLOR_FG, NX_COLOR_BG);
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
static void cmd_cd(const char *args);
static void cmd_run(const char *args);
static void cmd_jobs(const char *args);
static void cmd_kill(const char *args);
static void cmd_matrix(const char *args);
static void cmd_reboot(const char *args);
static void cmd_shutdown(const char *args);
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
    {"list", "ls"},           {"dir", "ls"},
    {"type", "cat"},          {"read", "cat"},
    {"exec", "run"},          {"spawn", "run"},
    {"start", "run"},         {"tasks", "ps"},
    {"procs", "ps"},          {"who", "ps"},
    {"cls", "clear"},         {"clr", "clear"},
    {"devices", "lspci"},     {"pci", "lspci"},
    {"ver", "uname"},         {"version", "uname"},
    {"mem", "meminfo"},       {"free", "meminfo"},
    {"cpu", "cpuinfo"},       {"say", "echo"},
    {"nodes", "gnodes"},      {"anchors", "sstrings"},
    {"link", "glink"},        {"mk", "gmk"},
    {"new", "gmk"},           {"write", "gwrite"},
    {"info", "gnodeinfo"},    {"save", "gsync"},
    {"load", "gload"},        {"rm", "grm"},
    {"del", "grm"},           {"delete", "grm"},
    {"unlink", "gunlink"},    {"wipe", "gclear"},
    {"touch", "gtouch"},      {"gc", "ggc"},
    {"poweroff", "shutdown"}, {"halt", "shutdown"},
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
    {"ls", cmd_ls, "[path]", "list a directory (default: cwd)", NULL, 0},
    {"cat", cmd_cat, "<path>", "print a file's contents", NULL, 0},
    {"cd", cmd_cd, "[path]",
     "change the shell's working directory (default: /); supports .. and "
     "relative paths",
     NULL, 0},
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
    {"shutdown", cmd_shutdown, "",
     "save if dirty, then power off via ACPI (falls back to a QEMU-only "
     "hack, then a plain halt, if ACPI shutdown isn't available)",
     NULL, 0},
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

  /* Defaults to cwd, not "/" -- a bare (no-slash) argument completes
   * against wherever the shell currently is, same as ls/cat/run's own
   * relative-path handling (resolve_path()). A slash in the prefix
   * still overrides this with its own (possibly relative, possibly
   * absolute) directory fragment, resolved the same way. */
  const char *dir_path = cwd;
  char dir_buf[LINE_MAX];
  char resolved_dir[LINE_MAX];
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
      resolve_path(dir_buf, resolved_dir, sizeof(resolved_dir));
      dir_path = resolved_dir;
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

/* ------------------------------ history ----------------------------------*/
#define HISTORY_MAX 16

static char history[HISTORY_MAX][LINE_MAX];
static uint32_t history_len =
    0; /* entries currently stored, caps at HISTORY_MAX */
static uint32_t history_next = 0; /* ring slot the next push lands in */

static void history_push(const char *line) {
  if (line[0] == '\0') {
    return; /* don't clutter history with blank Enters */
  }
  if (history_len > 0) {
    uint32_t last = (history_next + HISTORY_MAX - 1) % HISTORY_MAX;
    if (strcmp(history[last], line) == 0) {
      return; /* skip an exact repeat of the most recent entry */
    }
  }
  strncpy(history[history_next], line, LINE_MAX - 1);
  history[history_next][LINE_MAX - 1] = '\0';
  history_next = (history_next + 1) % HISTORY_MAX;
  if (history_len < HISTORY_MAX) {
    history_len++;
  }
}

/* age 0 = most recently entered command, age 1 = the one before that,
 * and so on. NULL once `age` walks past however many entries exist. */
static const char *history_get(uint32_t age) {
  if (age >= history_len) {
    return NULL;
  }
  uint32_t idx = (history_next + HISTORY_MAX - 1 - age) % HISTORY_MAX;
  return history[idx];
}

/* Erases whatever's currently echoed (via `*len` backspaces) and
 * echoes `new_content` in its place, updating `buf`/`*len` to match.
 * This line editor has no cursor-movement support, so "replace the
 * line" always means a full erase-and-retype against the console. */
static void redraw_line(char *buf, uint32_t *len, uint32_t max,
                        const char *new_content) {
  while (*len > 0) {
    (*len)--;
    echo_backspace();
  }
  uint32_t n = 0;
  while (new_content[n] != '\0' && n + 1 < max) {
    buf[n] = new_content[n];
    kprintf("%c", new_content[n]);
    n++;
  }
  *len = n;
}

static uint32_t read_line(char *buf, uint32_t max) {
  uint32_t len = 0;
  bool browsing = false;
  uint32_t browse_age = 0;
  char saved_line[LINE_MAX]; /* what the user had typed before pressing Up */
  saved_line[0] = '\0';

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

    if (c == KEY_UP) {
      if (!browsing) {
        memcpy(saved_line, buf, len);
        saved_line[len] = '\0';
        if (history_get(0) == NULL) {
          continue; /* nothing recorded yet */
        }
        browsing = true;
        browse_age = 0;
      } else if (history_get(browse_age + 1) != NULL) {
        browse_age++;
      } else {
        continue; /* already at the oldest entry */
      }
      redraw_line(buf, &len, max, history_get(browse_age));
      continue;
    }

    if (c == KEY_DOWN) {
      if (!browsing) {
        continue;
      }
      if (browse_age == 0) {
        browsing = false;
        redraw_line(buf, &len, max, saved_line);
      } else {
        browse_age--;
        redraw_line(buf, &len, max, history_get(browse_age));
      }
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
      const char *label = commands[i].category ? commands[i].category : "core";
      kprintf("\n");
      console_set_colors(NX_COLOR_ACCENT, NX_COLOR_BG);
      kprintf("%s:\n", label);
      console_set_colors(NX_COLOR_FG, NX_COLOR_BG);
      print_divider((uint32_t)strlen(label) + 1);
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
          "get a fuzzy 'did you mean'. Tab completes commands and paths,\n"
          "up/down arrows recall previous ones.\n");
}

static void cmd_clear(const char *args) {
  (void)args;
  console_clear();
}

static void cmd_echo(const char *args) { kprintf("%s\n", args); }

/* A plain text usage bar -- "[####------]" -- filled proportionally
 * to used/total, in the accent color, with the empty remainder dimmed.
 * `width` is the number of '#'/'-' characters between the brackets. */
static void print_usage_bar(uint64_t used, uint64_t total, uint32_t width) {
  uint32_t filled = 0;
  if (total > 0) {
    filled = (uint32_t)((used * (uint64_t)width) / total);
    if (filled > width) {
      filled = width;
    }
  }
  kprintf("[");
  console_set_colors(NX_COLOR_ACCENT, NX_COLOR_BG);
  for (uint32_t i = 0; i < filled; i++) {
    kprintf("#");
  }
  console_set_colors(NX_COLOR_DIM, NX_COLOR_BG);
  for (uint32_t i = filled; i < width; i++) {
    kprintf("-");
  }
  console_set_colors(NX_COLOR_FG, NX_COLOR_BG);
  kprintf("]");
}

static void cmd_meminfo(const char *args) {
  (void)args;
  char total[32], used[32], free_[32], hused[32], hcap[32];
  fmt_bytes(total, sizeof(total), pmm_total_bytes());
  fmt_bytes(used, sizeof(used), pmm_used_bytes());
  fmt_bytes(free_, sizeof(free_), pmm_free_bytes());
  fmt_bytes(hused, sizeof(hused), heap_used_bytes());
  fmt_bytes(hcap, sizeof(hcap), heap_capacity_bytes());

  kprintf("physical: ");
  print_usage_bar(pmm_used_bytes(), pmm_total_bytes(), 30);
  kprintf(" %s / %s used (%s free)\n", used, total, free_);

  kprintf("heap:     ");
  print_usage_bar(heap_used_bytes(), heap_capacity_bytes(), 30);
  kprintf(" %s / %s used\n", hused, hcap);

  uint64_t task_slab_allocated, task_slab_pages;
  sched_task_cache_stats(&task_slab_allocated, &task_slab_pages);
  kprintf("task slab: %lu live, %lu page(s) committed\n", task_slab_allocated,
          task_slab_pages);
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

#define PS_COL_PID 4
#define PS_COL_NAME 18
#define PS_COL_STATE 9
#define PS_COL_RING 6

/* Draws one full horizontal rule of the `ps` table -- top border,
 * header divider, or bottom border, depending which corner/junction
 * glyphs the caller passes in (U+250C/U+252C/U+2510 for the top,
 * U+251C/U+253C/U+2524 for the header divider, U+2514/U+2534/U+2518
 * for the bottom -- see video/nx_box8x8.h). `+2` per column accounts
 * for the one space of padding print_ps_row() puts on each side of a
 * cell's content. */
static void ps_table_rule(enum nx_box_glyph left, enum nx_box_glyph mid,
                          enum nx_box_glyph right, enum nx_box_glyph fill) {
  static const uint32_t widths[] = {PS_COL_PID, PS_COL_NAME, PS_COL_STATE,
                                    PS_COL_RING};
  box_putc(left, '+');
  for (size_t c = 0; c < ARRAY_LEN(widths); c++) {
    for (uint32_t i = 0; i < widths[c] + 2; i++) {
      box_putc(fill, '-');
    }
    box_putc((c + 1 < ARRAY_LEN(widths)) ? mid : right, '+');
  }
  kprintf("\n");
}

/* One row of the `ps` table -- used for both the header (literal
 * "PID"/"NAME"/... strings) and every task row, so the two can never
 * drift out of alignment with each other. */
static void ps_table_row(const char *pid, const char *name, const char *state,
                         const char *ring) {
  box_putc(NX_BOX_V, '|');
  kprintf(" ");
  print_left(pid, PS_COL_PID);
  kprintf(" ");
  box_putc(NX_BOX_V, '|');
  kprintf(" ");
  print_left(name, PS_COL_NAME);
  kprintf(" ");
  box_putc(NX_BOX_V, '|');
  kprintf(" ");
  print_left(state, PS_COL_STATE);
  kprintf(" ");
  box_putc(NX_BOX_V, '|');
  kprintf(" ");
  print_left(ring, PS_COL_RING);
  kprintf(" ");
  box_putc(NX_BOX_V, '|');
  kprintf("\n");
}

static void ps_print_one(struct task *t, void *arg) {
  (void)arg;
  static const char *state_names[] = {
      "ready", "running", "sleeping", "blocked", "exiting", "dead",
  };
  char pidbuf[16];
  ksnprintf(pidbuf, sizeof(pidbuf), "%lu", t->id);
  ps_table_row(pidbuf, t->name, state_names[t->state],
               t->is_user ? "user" : "kernel");
}

static void cmd_ps(const char *args) {
  (void)args;
  ps_table_rule(NX_BOX_DR, NX_BOX_HD, NX_BOX_DL, NX_BOX_H);
  ps_table_row("PID", "NAME", "STATE", "RING");
  ps_table_rule(NX_BOX_VR, NX_BOX_VH, NX_BOX_VL, NX_BOX_H);
  sched_for_each_task(ps_print_one, NULL);
  ps_table_rule(NX_BOX_UR, NX_BOX_HU, NX_BOX_UL, NX_BOX_H);
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
  char resolved[LINE_MAX];
  resolve_path(*args == '\0' ? "." : args, resolved, sizeof(resolved));

  char name[64];
  uint32_t i = 0;
  bool any = false;
  while (vfs_readdir(resolved, i, name, sizeof(name))) {
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
  char resolved[LINE_MAX];
  resolve_path(args, resolved, sizeof(resolved));

  struct vfs_file *f;
  if (!vfs_open(resolved, O_RDONLY, &f)) {
    print_error("cat: %s: no such file\n", resolved);
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

/* Changes the shell's own cwd -- see resolve_path()'s comment for what
 * "changes" means here (a shell-local static, not a real per-process
 * field). Verifies the target actually resolves to a directory before
 * committing it, so a typo leaves the old cwd in place with a clear
 * error rather than silently pointing the prompt somewhere that will
 * just fail every subsequent relative lookup. */
static void cmd_cd(const char *args) {
  char resolved[LINE_MAX];
  resolve_path(*args == '\0' ? "/" : args, resolved, sizeof(resolved));

  struct vnode *n = vfs_lookup_path(resolved);
  if (n == NULL || n->type != VNODE_DIR) {
    print_error("cd: %s: no such directory\n", resolved);
    return;
  }

  strncpy(cwd, resolved, sizeof(cwd) - 1);
  cwd[sizeof(cwd) - 1] = '\0';
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

  char resolved[LINE_MAX];
  resolve_path(pathbuf, resolved, sizeof(resolved));
  strncpy(pathbuf, resolved, sizeof(pathbuf) - 1);
  pathbuf[sizeof(pathbuf) - 1] = '\0';

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
    print_error("run: job table full (%d slots) -- 'jobs' to see them, wait "
                "for one to finish, or 'kill' one first\n",
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
    print_error("kill: '%s' isn't a pid -- see 'jobs' or 'ps'\n", args);
    return;
  }
  uint64_t pid = 0;
  for (const char *p = args; *p; p++) {
    pid = pid * 10 + (uint64_t)(*p - '0');
  }

  struct task *t = sched_find_waitable_task(pid);
  if (t == NULL) {
    print_error("kill: no such ring-3 task with pid %lu\n", pid);
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
  console_set_colors(NX_COLOR_FG, NX_COLOR_BG);
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

static void cmd_shutdown(const char *args) {
  (void)args;
  if (graph_is_dirty()) {
    kprintf("graph has unsaved changes -- saving before shutdown...\n");
    graph_save_to_disk();
  }
  kprintf("shutting down...\n");
  timer_busy_wait_ms(50);
  acpi_shutdown(); /* never returns -- real ACPI / QEMU-hack / halt
                       fallback chain lives in acpi.c */
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
  kprintf("refcount: %u (incoming edges + sstring anchors + open fds)\n",
          n->refcount);
}

static void gnode_print_one(struct gnode *n, void *arg) {
  (void)arg;
  char idbuf[16], sizebuf[16], edgebuf[16];
  ksnprintf(idbuf, sizeof(idbuf), "#%lu", n->id);
  ksnprintf(sizebuf, sizeof(sizebuf), "size=%lu", n->size);
  ksnprintf(edgebuf, sizeof(edgebuf), "edges=%u", n->edge_count);

  kprintf("  ");
  print_left(idbuf, 9);
  print_left(n->label[0] ? n->label : "-", 20);
  print_left(sizebuf, 12);
  print_left(edgebuf, 10);
  kprintf("refs=%u\n", n->refcount);
}

static void cmd_gnodes(const char *args) {
  (void)args;
  kprintf("  ");
  print_left("ID", 9);
  print_left("LABEL", 20);
  kprintf("\n");
  print_divider(31);
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
  if (*args == '\0') {
    kprintf("usage: grm <ref>\n");
    return;
  }

  /* `args` names a live sstring anchor directly: an anchored node's
   * refcount is never 0 while its anchor is up (the anchor itself is
   * one of the references -- see docs/Design.md's note on graph FS
   * deletion), so falling straight through to graph_node_delete()
   * below would just refuse with a "still referenced" message a
   * person then has to go work around by hand with a separate
   * sstringrm call. Do that detach here instead, the same way
   * gtouch() already does the equivalent create-side step (auto-
   * creating every missing anchor/edge) rather than making the
   * caller drive it themselves first. */
  if (sstring_get(args) != NULL) {
    sstring_unset(args); /* logs its own outcome */
    return;
  }

  /* A deeper path (e.g. "photos/vacation"): the natural "remove
   * this" is unlinking it from its parent, not requiring a separate
   * gunlink call first -- same idea as the anchor case above, one
   * level down the path. Only kicks in when `args` actually contains
   * a '/'; a bare id or an unanchored label falls through to the
   * direct-delete path unchanged. */
  const char *last_slash = NULL;
  for (const char *p = args; *p; p++) {
    if (*p == '/') {
      last_slash = p;
    }
  }
  if (last_slash != NULL && last_slash != args) {
    char parent_path[128];
    size_t plen = (size_t)(last_slash - args);
    if (plen >= sizeof(parent_path)) {
      plen = sizeof(parent_path) - 1;
    }
    memcpy(parent_path, args, plen);
    parent_path[plen] = '\0';

    struct gnode *parent = graph_resolve(parent_path);
    if (parent != NULL) {
      graph_unlink(parent, last_slash + 1); /* logs its own outcome */
      return;
    }
    /* Parent didn't resolve as a graph path -- fall through and let
     * resolve_ref()/graph_node_delete() below give the ordinary
     * "no such node" treatment. */
  }

  struct gnode *n = resolve_ref(args);
  if (n == NULL) {
    kprintf("grm: no such node '%s'\n", args);
    return;
  }
  graph_node_delete(n); /* logs its own outcome -- a node still
                            referenced from somewhere OTHER than the
                            anchor/edge already handled above (e.g.
                            multiple incoming edges) still correctly
                            refuses here rather than force-deleting */
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

  print_error("unknown command: %s (try 'help')\n", start);
  suggest_commands(start);
}

void shell_task(void *arg) {
  (void)arg;
  char line[LINE_MAX];

  kprintf("\n");
  console_set_colors(NX_COLOR_ACCENT, NX_COLOR_BG);
  print_box_border_top();
  print_box_line(" NEXUS -- x86-64 kernel shell");
  char status[BOX_INNER_WIDTH + 1];
  ksnprintf(status, sizeof(status), " %u cpu(s) online, x2APIC %s", g_cpu_count,
            lapic_using_x2apic() ? "on" : "off");
  print_box_line(status);
  print_box_line(" type 'help' for commands, up/down for history");
  print_box_border_bottom();
  console_set_colors(NX_COLOR_FG, NX_COLOR_BG);
  kprintf("\n");

  for (;;) {
    reap_finished_jobs();
    print_prompt();
    read_line(line, sizeof(line));
    history_push(line); /* before dispatch() -- it mutates line in place */
    dispatch(line);
  }
}
