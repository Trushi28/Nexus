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
static void cmd_run(const char *args);
static void cmd_exec(const char *args);
static void cmd_whoami(const char *args);
static void cmd_drop(const char *args);
static void cmd_jobs(const char *args);
static void cmd_kill(const char *args);
static void cmd_topcmds(const char *args);
static void cmd_exit(const char *args);

/* Same non-POSIX naming philosophy as the kernel shell (shell.c):
 * several spellings, one canonical handler. */
struct nsh_synonym {
  const char *alias;
  const char *canonical;
};

static const struct nsh_synonym synonyms[] = {
    {"list", "ls"},   {"dir", "ls"},    {"type", "cat"},  {"read", "cat"},
    {"spawn", "run"}, {"start", "run"}, {"tasks", "ps"},  {"procs", "ps"},
    {"who", "ps"},    {"say", "echo"},  {"quit", "exit"}, {"logout", "exit"},
};

static struct nsh_command commands[] = {
    {"help", cmd_help, "", "this text", 0},
    {"echo", cmd_echo, "<text>", "print text back", 0},
    {"ps", cmd_ps, "", "list every live task", 0},
    {"ls", cmd_ls, "[path]", "list a directory (default: /)", 0},
    {"cat", cmd_cat, "<path>", "print a file's contents", 0},
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
    {"topcmds", cmd_topcmds, "", "your most-used commands this session", 0},
    {"exit", cmd_exit, "[code]", "leave the ring-3 shell", 0},
};

static const char *skip_spaces(const char *s) {
  while (*s == ' ') {
    s++;
  }
  return s;
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
  const char *path = (*args == '\0') ? "/" : args;
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

static void cmd_cat(const char *args) {
  if (*args == '\0') {
    u_print("usage: cat <path>\n");
    return;
  }
  int fd = u_open(args, O_RDONLY);
  if (fd < 0) {
    u_print("cat: no such file\n");
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
    u_print("run: couldn't spawn '");
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
    u_print("run: job table full -- see 'jobs', or wait for one to "
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
  u_print("exec: couldn't exec '");
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
    u_print("(no background jobs)\n");
  }
}

static void cmd_kill(const char *args) {
  if (*args == '\0') {
    u_print("usage: kill <pid>\n");
    return;
  }
  int pid = u_atoi(args);
  if (u_kill(pid) < 0) {
    u_print("kill: no such ring-3 task\n");
    return;
  }
  u_print("kill: signalled -- exits at its next syscall\n");
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
    u_print("drop: refused -- only uid 0 may drop, and only downward "
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

  u_print("unknown command: ");
  u_print(start);
  u_print(" (try 'help')\n");
  suggest_commands(start);
}

int main(void) {
  u_print("\nnsh -- ring-3 Nexus shell. Type 'help' for commands, 'exit' "
          "to leave.\n");

  char line[LINE_MAX];
  while (!g_should_exit) {
    reap_finished_jobs();
    u_print("nsh> ");
    u_read_line(line, sizeof(line));
    dispatch(line);
  }

  return g_exit_code;
}
