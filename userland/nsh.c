#include "syscall.h"
#include "ulib.h"

#define LINE_MAX 200
#define WORD_MAX 24
#define SUGGEST_MAX 3
#define SUGGEST_DIST_MAX 3

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef void (*cmd_handler_t)(const char *args);

struct nsh_command {
  const char *name;
  cmd_handler_t handler;
  const char *usage;
  const char *blurb;
  unsigned frecency_score;
};

static void cmd_help(const char *args);
static void cmd_echo(const char *args);
static void cmd_ps(const char *args);
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_cd(const char *args);
static void cmd_run(const char *args);
static void cmd_exec(const char *args);
static void cmd_whoami(const char *args);
static void cmd_drop(const char *args);
static void cmd_jobs(const char *args);
static void cmd_kill(const char *args);
static void cmd_meminfo(const char *args);
static void cmd_cpuinfo(const char *args);
static void cmd_reboot(const char *args);
static void cmd_shutdown(const char *args);
static void cmd_gsync(const char *args);
static void cmd_gload(const char *args);
static void cmd_topcmds(const char *args);
static void cmd_exit(const char *args);

/* Same non-POSIX naming philosophy as the kernel shell (shell.c):
 * several spellings, one canonical handler. Several of these
 * deliberately match shell.c's OWN synonym table 1:1 (mem/free,
 * cpu, poweroff/halt, save/load) so a habit picked up in one shell
 * carries straight over to the other. */
struct nsh_synonym {
  const char *alias;
  const char *canonical;
};

static const struct nsh_synonym synonyms[] = {
    {"list", "ls"},           {"dir", "ls"},        {"type", "cat"},
    {"read", "cat"},          {"spawn", "run"},     {"start", "run"},
    {"tasks", "ps"},          {"procs", "ps"},      {"who", "ps"},
    {"say", "echo"},          {"quit", "exit"},     {"logout", "exit"},
    {"mem", "meminfo"},       {"free", "meminfo"},  {"cpu", "cpuinfo"},
    {"poweroff", "shutdown"}, {"halt", "shutdown"}, {"save", "gsync"},
    {"load", "gload"},
};

static struct nsh_command commands[] = {
    {"help", cmd_help, "", "this text", 0},
    {"echo", cmd_echo, "<text>", "print text back", 0},
    {"ps", cmd_ps, "", "list every live task", 0},
    {"ls", cmd_ls, "[path]", "list a directory (default: cwd)", 0},
    {"cat", cmd_cat, "<path>", "print a file's contents", 0},
    {"cd", cmd_cd, "[path]",
     "change this shell's working directory (default: /); supports .. "
     "and relative paths",
     0},
    {"run", cmd_run, "<path>",
     "spawn an ELF binary and wait for it (append & to background it)", 0},
    {"exec", cmd_exec, "<path>",
     "replace THIS shell's own image with an ELF binary -- same pid, no return "
     "on success",
     0},
    {"whoami", cmd_whoami, "", "print this shell's own clearance (uid)", 0},
    {"drop", cmd_drop, "<uid>",
     "narrow this shell's own clearance -- one-way, root-only", 0},
    {"jobs", cmd_jobs, "", "list background jobs spawned with 'run ... &'", 0},
    {"kill", cmd_kill, "<pid>", "signal a task to exit at its next syscall", 0},
    {"meminfo", cmd_meminfo, "", "physical memory + heap usage", 0},
    {"cpuinfo", cmd_cpuinfo, "", "list online CPUs and APIC mode", 0},
    {"reboot", cmd_reboot, "",
     "save if dirty, then reset the machine. Root only", 0},
    {"shutdown", cmd_shutdown, "",
     "save if dirty, then power off (ACPI, falling back as needed). "
     "Root only",
     0},
    {"gsync", cmd_gsync, "", "save the graph filesystem to disk. Root only", 0},
    {"gload", cmd_gload, "",
     "reload the graph filesystem from disk (only if empty). Root only", 0},
    {"topcmds", cmd_topcmds, "", "your most-used commands this session", 0},
    {"exit", cmd_exit, "[code]", "leave the ring-3 shell", 0},
};

static const char *skip_spaces(const char *s) {
  while (*s == ' ') {
    s++;
  }
  return s;
}

static void print_error(const char *msg) {
  u_color_error();
  u_print(msg);
  u_color_reset();
}

static const char *normalize_verb(const char *verb, char *scratch,
                                  size_t scratch_size) {
  for (size_t i = 0; i < ARRAY_LEN(synonyms); i++) {
    if (u_strcmp(verb, synonyms[i].alias) == 0) {
      u_strncpy(scratch, synonyms[i].canonical, scratch_size - 1);
      scratch[scratch_size - 1] = '\0';
      return scratch;
    }
  }
  return verb;
}

/* Plain integer Levenshtein distance -- same algorithm and same
 * reason as shell.c's edit_distance(): no floats anywhere in this
 * build (-mno-sse/-mno-mmx/-mno-80387), and these strings are a
 * handful of characters, nothing here needs to be fast. */
static int edit_distance(const char *a, const char *b) {
  int la = (int)u_strlen(a);
  int lb = (int)u_strlen(b);
  if (la > WORD_MAX) {
    la = WORD_MAX;
  }
  if (lb > WORD_MAX) {
    lb = WORD_MAX;
  }

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

static void suggest_commands(const char *typed) {
  if (*typed == '\0') {
    return;
  }

  const struct nsh_command *matches[SUGGEST_MAX];
  int match_dist[SUGGEST_MAX];
  int match_count = 0;

  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    int d = edit_distance(typed, commands[i].name);
    if (d > SUGGEST_DIST_MAX) {
      continue;
    }
    if (match_count == SUGGEST_MAX && d >= match_dist[SUGGEST_MAX - 1]) {
      continue;
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

  u_print("did you mean: ");
  for (int i = 0; i < match_count; i++) {
    u_print(matches[i]->name);
    if (i + 1 < match_count) {
      u_print(", ");
    }
  }
  u_print("\n");
}

#define MAX_JOBS 8
#define NX_TASK_STATE_DEAD                                                     \
  5 /* mirrors enum task_state in sched/sched.h --                             \
        see abi/task_info.h's comment on why this                              \
        crosses the user/kernel boundary as a bare                             \
        numeric constant, not a shared enum. */

struct bg_job {
  bool used;
  unsigned job_id;
  int pid;
  char name[32];
};
static struct bg_job jobs[MAX_JOBS];
static unsigned next_job_id = 1;

static bool strip_trailing_background(char *s) {
  size_t len = u_strlen(s);
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
    nx_task_info_t info;
    if (!u_find_task(jobs[i].pid, &info)) {
      jobs[i].used = false; /* gone -- nothing left to wait() on */
      continue;
    }
    if (info.state != NX_TASK_STATE_DEAD) {
      continue;
    }

    int pid = jobs[i].pid;
    char name[sizeof(jobs[i].name)];
    u_strncpy(name, jobs[i].name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    unsigned job_id = jobs[i].job_id;
    jobs[i].used = false;

    int code = u_wait(pid);

    char idbuf[16], pidbuf[16], codebuf[16];
    u_itoa((int)job_id, idbuf);
    u_itoa(pid, pidbuf);
    u_itoa(code, codebuf);

    u_print("[");
    u_print(idbuf);
    u_print("] pid ");
    u_print(pidbuf);
    u_print(" (");
    u_print(name);
    u_print(") done, exit code ");
    u_print(codebuf);
    u_print("\n");
  }
}

static void cmd_help(const char *args) {
  (void)args;
  u_print("nsh -- ring-3 Nexus shell. Commands:\n");
  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    u_print("  ");
    u_print(commands[i].name);
    if (commands[i].usage[0] != '\0') {
      u_print(" ");
      u_print(commands[i].usage);
    }
    u_print(" -- ");
    u_print(commands[i].blurb);
    u_print("\n");
  }
  u_print("\nUnrecognised commands get a fuzzy 'did you mean'. No tab\n"
          "completion or quote parsing here yet -- v1.\n");
}

static void cmd_echo(const char *args) {
  u_print(args);
  u_print("\n");
}

static void cmd_ps(const char *args) {
  (void)args;
  static const char *state_names[] = {
      "ready", "running", "sleeping", "blocked", "exiting", "dead",
  };
  u_print("  ");
  u_print_left("PID", 6);
  u_print_left("NAME", 22);
  u_print_left("STATE", 10);
  u_print_left("RING", 8);
  u_print("UID\n");

  nx_task_info_t info;
  unsigned idx = 0;
  while (u_ps(idx, &info)) {
    char buf[16];
    u_itoa((int)info.pid, buf);
    u_print("  ");
    u_print_left(buf, 6);
    u_print_left(info.name, 22);
    u_print_left(info.state < ARRAY_LEN(state_names) ? state_names[info.state]
                                                     : "?",
                 10);
    u_print_left(info.is_user ? "user" : "kernel", 8);
    u_itoa((int)info.uid, buf);
    u_print(buf);
    u_print("\n");
    idx++;
  }
}

static void cmd_ls(const char *args) {
  const char *path = (*args == '\0') ? "." : args;
  char name[64];
  unsigned i = 0;
  bool any = false;
  while (u_readdir(path, i, name, sizeof(name))) {
    u_print("  ");
    u_print(name);
    u_print("\n");
    i++;
    any = true;
  }
  if (!any) {
    u_print("(empty, or no such directory)\n");
  }
}

/* No local cwd tracking here at all -- SYS_chdir commits the new cwd
 * kernel-side (struct task::cwd, cpu/syscall.c), and print_prompt()
 * reads it back fresh via u_getcwd() on every prompt. Nothing else
 * can ever change this task's cwd out from under it, so there's
 * nothing to keep in sync locally -- the kernel's copy is the only
 * copy. */
static void cmd_cd(const char *args) {
  const char *target = (*args == '\0') ? "/" : args;
  if (u_chdir(target) < 0) {
    print_error("cd: no such directory\n");
  }
}

static void cmd_cat(const char *args) {
  if (*args == '\0') {
    u_print("usage: cat <path>\n");
    return;
  }
  int fd = u_open(args, O_RDONLY);
  if (fd < 0) {
    print_error("cat: no such file\n");
    return;
  }
  char buf[129];
  int n;
  while ((n = u_read(fd, buf, sizeof(buf) - 1)) > 0) {
    buf[n] = '\0';
    u_print(buf);
  }
  u_print("\n");
  u_close(fd);
}

static void cmd_run(const char *args) {
  if (*args == '\0') {
    u_print("usage: run <path> [&]\n");
    return;
  }

  char pathbuf[LINE_MAX];
  u_strncpy(pathbuf, args, sizeof(pathbuf) - 1);
  pathbuf[sizeof(pathbuf) - 1] = '\0';

  bool background = strip_trailing_background(pathbuf);

  int pid = u_spawn(pathbuf);
  if (pid < 0) {
    print_error("run: couldn't spawn '");
    u_print(pathbuf);
    u_print("'\n");
    return;
  }

  if (!background) {
    int code = u_wait(pid);
    char buf[16];
    u_print("[");
    u_print(pathbuf);
    u_print(" exited with code ");
    u_itoa(code, buf);
    u_print(buf);
    u_print("]\n");
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
    print_error("run: job table full -- see 'jobs', or wait for one to "
                "finish\n");
    return;
  }

  jobs[slot].used = true;
  jobs[slot].job_id = next_job_id++;
  jobs[slot].pid = pid;
  u_strncpy(jobs[slot].name, pathbuf, sizeof(jobs[slot].name) - 1);
  jobs[slot].name[sizeof(jobs[slot].name) - 1] = '\0';

  char idbuf[16], pidbuf[16];
  u_itoa((int)jobs[slot].job_id, idbuf);
  u_itoa(pid, pidbuf);
  u_print("[");
  u_print(idbuf);
  u_print("] pid ");
  u_print(pidbuf);
  u_print("\n");
}

static void cmd_exec(const char *args) {
  if (*args == '\0') {
    u_print("usage: exec <path>\n");
    return;
  }
  u_exec(args);
  /* Only reaches here if exec() failed -- on success this shell's own
   * image has already been replaced and control never returns here. */
  print_error("exec: couldn't exec '");
  u_print(args);
  u_print("'\n");
}

static void cmd_jobs(const char *args) {
  (void)args;
  reap_finished_jobs();
  bool any = false;
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].used) {
      continue;
    }
    char idbuf[16], pidbuf[16];
    u_itoa((int)jobs[i].job_id, idbuf);
    u_itoa(jobs[i].pid, pidbuf);
    u_print("  [");
    u_print(idbuf);
    u_print("]  pid ");
    u_print(pidbuf);
    u_print("  ");
    u_print(jobs[i].name);
    u_print("\n");
    any = true;
  }
  if (!any) {
    print_error("(no background jobs)\n");
  }
}

static void cmd_kill(const char *args) {
  if (*args == '\0') {
    u_print("usage: kill <pid>\n");
    return;
  }
  int pid = u_atoi(args);
  if (u_kill(pid) < 0) {
    print_error("kill: no such ring-3 task\n");
    return;
  }
  u_print("kill: signalled -- exits at its next syscall\n");
}

/* Renders a byte count in MiB -- u_itoa() only takes a plain `int`,
 * so anything finer-grained (KiB, exact bytes) risks overflowing it
 * on a machine with more than ~2GiB of RAM; MiB keeps every realistic
 * value comfortably inside int32 range. Coarser than shell.c's own
 * fmt_bytes()/print_usage_bar() (which have ksnprintf() and a real
 * framebuffer bar to work with), but plenty for a quick userland
 * check. */
static void print_mib(unsigned long bytes) {
  char buf[16];
  u_itoa((int)(bytes / (1024 * 1024)), buf);
  u_print(buf);
  u_print(" MiB");
}

static void cmd_meminfo(const char *args) {
  (void)args;
  nx_sysinfo_t info;
  if (!u_sysinfo(&info)) {
    print_error("meminfo: couldn't read system info\n");
    return;
  }
  u_print("physical: ");
  print_mib(info.mem_used_bytes);
  u_print(" / ");
  print_mib(info.mem_total_bytes);
  u_print(" used\n");
  u_print("heap:     ");
  print_mib(info.heap_used_bytes);
  u_print(" / ");
  print_mib(info.heap_capacity_bytes);
  u_print(" used\n");
}

static void cmd_cpuinfo(const char *args) {
  (void)args;
  nx_sysinfo_t info;
  if (!u_sysinfo(&info)) {
    print_error("cpuinfo: couldn't read system info\n");
    return;
  }
  char buf[16];
  u_itoa((int)info.cpu_count, buf);
  u_print(buf);
  u_print(" cpu(s) known, x2APIC ");
  u_print(info.x2apic ? "enabled\n" : "not in use\n");
}

/* u_reboot()/u_shutdown() never return on success -- the print below
 * only ever gets a chance to read as "did nothing" if the syscall
 * itself was refused (not root; see 'whoami'/'drop'). */
static void cmd_reboot(const char *args) {
  (void)args;
  u_print("rebooting...\n");
  u_reboot();
  print_error("reboot: refused -- root only (see 'whoami'/'drop')\n");
}

static void cmd_shutdown(const char *args) {
  (void)args;
  u_print("shutting down...\n");
  u_shutdown();
  print_error("shutdown: refused -- root only (see 'whoami'/'drop')\n");
}

static void cmd_gsync(const char *args) {
  (void)args;
  if (u_gsync() < 0) {
    print_error("gsync: failed -- no block device, not root, or nothing to "
                "save to\n");
    return;
  }
  u_print("graph saved\n");
}

static void cmd_gload(const char *args) {
  (void)args;
  if (u_gload() < 0) {
    print_error("gload: failed -- no saved graph, not root, or the graph "
                "isn't empty (gload only ever loads into an empty graph)\n");
    return;
  }
  u_print("graph loaded\n");
}

static void cmd_whoami(const char *args) {
  (void)args;
  char buf[16];
  u_itoa((int)u_getuid(), buf);
  u_print("uid ");
  u_print(buf);
  u_print(u_getuid() == 0 ? " (root)\n" : " (restricted)\n");
}

static void cmd_drop(const char *args) {
  if (*args == '\0') {
    u_print("usage: drop <uid>\n");
    return;
  }
  unsigned target = (unsigned)u_atoi(args);
  if (u_setuid(target) < 0) {
    print_error("drop: refused -- only uid 0 may drop, and only downward "
                "(see 'whoami')\n");
    return;
  }
  char buf[16];
  u_itoa((int)target, buf);
  u_print("now running as uid ");
  u_print(buf);
  u_print("\n");
}

static void cmd_topcmds(const char *args) {
  (void)args;
  unsigned order[ARRAY_LEN(commands)];
  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    order[i] = (unsigned)i;
  }
  for (size_t i = 1; i < ARRAY_LEN(commands); i++) {
    unsigned key = order[i];
    unsigned key_score = commands[key].frecency_score;
    size_t j = i;
    while (j > 0 && commands[order[j - 1]].frecency_score < key_score) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }

  u_print("most-used commands this session:\n");
  bool any = false;
  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    if (commands[order[i]].frecency_score == 0) {
      continue;
    }
    u_print("  ");
    u_print(commands[order[i]].name);
    u_print("\n");
    any = true;
  }
  if (!any) {
    u_print("  (nothing yet -- run a few commands first)\n");
  }
}

static bool g_should_exit = false;
static int g_exit_code = 0;

static void cmd_exit(const char *args) {
  g_exit_code = (*args != '\0') ? u_atoi(args) : 0;
  g_should_exit = true;
}

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

  size_t rest_len = u_strlen(rest);
  while (rest_len > 0 && (uint8_t)rest[rest_len - 1] <= ' ') {
    rest_len--;
  }
  rest[rest_len] = '\0';

  char scratch[16];
  const char *verb = normalize_verb(start, scratch, sizeof(scratch));

  for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
    if (u_strcmp(verb, commands[i].name) == 0) {
      commands[i].frecency_score = (commands[i].frecency_score >> 1) + 100;
      commands[i].handler(rest);
      return;
    }
  }

  u_color_error();
  u_print("unknown command: ");
  u_print(start);
  u_print(" (try 'help')\n");
  u_color_reset();
  suggest_commands(start);
}

// ------------------------------- banner / prompt --------------------------
#define BANNER_WIDTH 62

static void banner_rule_top(void) {
  u_color_accent();
  u_print("\xE2\x95\x94"); /* U+2554 ╔ */
  for (int i = 0; i < BANNER_WIDTH; i++) {
    u_print("\xE2\x95\x90"); /* U+2550 ═ */
  }
  u_print("\xE2\x95\x97\n"); /* U+2557 ╗ */
  u_color_reset();
}

static void banner_rule_bottom(void) {
  u_color_accent();
  u_print("\xE2\x95\x9A"); /* U+255A ╚ */
  for (int i = 0; i < BANNER_WIDTH; i++) {
    u_print("\xE2\x95\x90"); /* U+2550 ═ */
  }
  u_print("\xE2\x95\x9D\n"); /* U+255D ╝ */
  u_color_reset();
}

static void banner_line(const char *text) {
  u_color_accent();
  u_print("\xE2\x95\x91 "); /* U+2551 ║ */
  u_color_reset();
  u_print_left(text, BANNER_WIDTH - 2);
  u_color_accent();
  u_print(" \xE2\x95\x91\n");
  u_color_reset();
}

static void banner_rule(void) {
  u_putc('+');
  for (int i = 0; i < BANNER_WIDTH; i++) {
    u_putc('-');
  }
  u_putc('+');
  u_putc('\n');
}

static void print_banner(void) {
  banner_rule_top();
  banner_line("NEXUS -- nsh, the default ring-3 shell");
  banner_line("type 'help' for commands, 'exit' to leave");
  banner_line("need lspci / loom / native graph admin? boot with 'kshell'");
  banner_rule_bottom();

  nx_sysinfo_t info;
  if (u_sysinfo(&info)) {
    char buf[16];
    u_itoa((int)info.cpu_count, buf);
    u_print(buf);
    u_print(" cpu(s) online, x2APIC ");
    u_print(info.x2apic ? "on\n\n" : "off\n\n");
  } else {
    u_print("\n");
  }
}

/* Shows this task's OWN cwd, read fresh from the kernel every time --
 * see cmd_cd()'s comment for why there's no local copy to keep in
 * sync. Falls back to "?" rather than silently omitting it if
 * u_getcwd() somehow fails (it shouldn't -- LINE_MAX is comfortably
 * larger than struct task::cwd's own TASK_CWD_MAX). */
static void print_prompt(void) {
  char cwd[LINE_MAX];
  if (!u_getcwd(cwd, sizeof(cwd))) {
    u_strncpy(cwd, "?", sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
  }
  u_print("nsh:");
  u_print(cwd);
  u_print("> ");
}

int main(void) {
  print_banner();

  char line[LINE_MAX];
  while (!g_should_exit) {
    reap_finished_jobs();
    print_prompt();
    u_read_line(line, sizeof(line));
    dispatch(line);
  }

  return g_exit_code;
}
