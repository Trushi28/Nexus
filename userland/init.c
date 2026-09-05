#include "syscall.h"
#include "ulib.h"

/*
 * /bin/init -- Nexus's userland PID-1 equivalent, replacing loomd.
 *
 * Reads service definitions straight out of GraphFS via the ordinary
 * classic-VFS syscalls (open/read/readdir) -- no privileged access,
 * no new syscalls. A service is a directory of plain files:
 *
 *   /services/nsh/path      -> "/bin/nsh"
 *   /services/nsh/uid       -> "0"           (optional, default 0)
 *   /services/nsh/respawn   -> "always"       (optional, default "once")
 *   /services/nsh/needs/    -> outgoing edges, one per dependency,
 *                               EACH EDGE NAMED AFTER THE DEPENDENCY
 *                               ITSELF (e.g. glink services/nsh/needs
 *                               hello services/hello) -- a plain
 *                               readdir of that directory already
 *                               gives back the dependency's name with
 *                               no extra read required.
 *
 * Dependency ordering is launch-order only, not a hard gate -- same
 * posture the old kernel-side Loom used and documented: a dependency
 * that fails to start doesn't block its dependents forever, it just
 * means they're attempted after it and may fail for their own
 * reasons. A real readiness gate needs "is this service actually up",
 * not just "does the process exist", which is a bigger feature than
 * this minimal init needs yet.
 */

#define MAX_SERVICES 32
#define MAX_NEEDS 8
#define NAME_MAX 64
#define PATH_MAX 128

/* Same crash-loop budget the old kernel-side Loom used
 * (LOOM_MAX_RAPID_RESTARTS / LOOM_RESPAWN_COOLDOWN_MS) -- a
 * respawn=always service that dies instantly every time must
 * eventually stop being retried automatically. */
#define RESPAWN_COOLDOWN_MS 3000
#define MAX_RAPID_RESTARTS 5

#define TASK_STATE_DEAD 5 /* mirrors sched/sched.h's enum task_state */

struct service {
  bool used;
  bool running;
  bool ever_started;
  bool faulted;
  bool respawn_always;
  int pid;
  unsigned uid;
  unsigned restart_count;
  unsigned last_launch_ms;
  char name[NAME_MAX];
  char path[PATH_MAX];
  char needs[MAX_NEEDS][NAME_MAX];
  unsigned needs_count;
};

static struct service services[MAX_SERVICES];

static struct service *find_service(const char *name) {
  for (int i = 0; i < MAX_SERVICES; i++) {
    if (services[i].used && u_strcmp(services[i].name, name) == 0) {
      return &services[i];
    }
  }
  return NULL;
}

/* Builds "/services/<svc_name>/<field>" by hand -- ulib has no
 * snprintf, and this whole file deliberately avoids needing one. */
static void build_path(const char *svc_name, const char *field, char *out,
                       size_t out_max) {
  size_t n = 0;
  const char *prefix = "/services/";
  while (prefix[n] != '\0' && n + 1 < out_max) {
    out[n] = prefix[n];
    n++;
  }
  size_t i = 0;
  while (svc_name[i] != '\0' && n + 1 < out_max) {
    out[n++] = svc_name[i++];
  }
  if (field[0] != '\0' && n + 1 < out_max) {
    out[n++] = '/';
  }
  i = 0;
  while (field[i] != '\0' && n + 1 < out_max) {
    out[n++] = field[i++];
  }
  out[n] = '\0';
}

static bool read_field(const char *svc_name, const char *field, char *out,
                       size_t out_max) {
  char path[PATH_MAX];
  build_path(svc_name, field, path, sizeof(path));

  int fd = u_open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }
  int got = u_read(fd, out, out_max - 1);
  u_close(fd);
  if (got < 0) {
    got = 0;
  }
  out[got] = '\0';
  while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r')) {
    out[--got] = '\0';
  }
  return true;
}

static void read_needs(struct service *svc) {
  char dir[PATH_MAX];
  build_path(svc->name, "needs", dir, sizeof(dir));

  svc->needs_count = 0;
  char name[NAME_MAX];
  for (unsigned idx = 0; svc->needs_count < MAX_NEEDS &&
                        u_readdir(dir, idx, name, sizeof(name));
      idx++) {
    u_strncpy(svc->needs[svc->needs_count], name, NAME_MAX - 1);
    svc->needs[svc->needs_count][NAME_MAX - 1] = '\0';
    svc->needs_count++;
  }
}

/* Discovers any service under "/services" not yet tracked, and
 * refreshes every tracked service's fields -- same "re-scan on every
 * poll tick" posture the old loom_reload() used, so a service
 * gtouch'd/gwrite'n mid-session is picked up without a restart. */
static void scan_services(void) {
  char name[NAME_MAX];
  for (unsigned i = 0; u_readdir("/services", i, name, sizeof(name)); i++) {
    struct service *svc = find_service(name);
    if (svc == NULL) {
      int slot = -1;
      for (int j = 0; j < MAX_SERVICES; j++) {
        if (!services[j].used) {
          slot = j;
          break;
        }
      }
      if (slot < 0) {
        continue; /* table full -- silently ignore, same as the old Loom */
      }
      svc = &services[slot];
      svc->used = true;
      u_strncpy(svc->name, name, sizeof(svc->name) - 1);
      svc->name[sizeof(svc->name) - 1] = '\0';
    }

    char buf[PATH_MAX];
    if (read_field(name, "path", buf, sizeof(buf))) {
      u_strncpy(svc->path, buf, sizeof(svc->path) - 1);
      svc->path[sizeof(svc->path) - 1] = '\0';
    }
    if (read_field(name, "uid", buf, sizeof(buf))) {
      svc->uid = (unsigned)u_atoi(buf);
    }
    if (read_field(name, "respawn", buf, sizeof(buf))) {
      svc->respawn_always = (u_strcmp(buf, "always") == 0);
    }
    read_needs(svc);
  }
}

static bool needs_satisfied(struct service *svc) {
  for (unsigned i = 0; i < svc->needs_count; i++) {
    struct service *dep = find_service(svc->needs[i]);
    /* Not tracked, or tracked but never launched yet -- see this
     * file's header comment on why this doesn't hard-block. */
    if (dep != NULL && !dep->running && !dep->ever_started) {
      return false;
    }
  }
  return true;
}

static void start_service(struct service *svc) {
  if (svc->path[0] == '\0') {
    return; /* gtouch'd but not gwrite'n yet -- silently pending */
  }

  int pid = u_spawn(svc->path);
  svc->last_launch_ms = u_uptime_ms();

  if (pid < 0) {
    svc->faulted = true;
    u_print("[init] '");
    u_print(svc->name);
    u_print("' failed to spawn (");
    u_print(svc->path);
    u_print(")\n");
    return;
  }

  svc->pid = pid;
  svc->running = true;
  svc->ever_started = true;
  svc->faulted = false;

  char buf[16];
  u_itoa(pid, buf);
  u_print("[init] '");
  u_print(svc->name);
  u_print("' started, pid ");
  u_print(buf);
  u_print("\n");
}

static void start_ready_services(void) {
  for (int i = 0; i < MAX_SERVICES; i++) {
    struct service *svc = &services[i];
    if (!svc->used || svc->running || svc->faulted) {
      continue;
    }
    if (svc->ever_started && !svc->respawn_always) {
      continue; /* respawn=once, already ran */
    }
    if (!needs_satisfied(svc)) {
      continue;
    }
    start_service(svc);
  }
}

static void handle_exit(struct service *svc) {
  svc->running = false;
  svc->pid = 0;

  if (!svc->respawn_always) {
    u_print("[init] '");
    u_print(svc->name);
    u_print("' finished\n");
    return;
  }

  unsigned now = u_uptime_ms();
  if (now - svc->last_launch_ms < RESPAWN_COOLDOWN_MS) {
    svc->restart_count++;
  } else {
    svc->restart_count = 1;
  }

  if (svc->restart_count > MAX_RAPID_RESTARTS) {
    svc->faulted = true;
    u_print("[init] '");
    u_print(svc->name);
    u_print("' crash-looped -- giving up on it for this session\n");
  }
  /* else: left running=false, not faulted -- start_ready_services()
   * naturally respawns it on the next pass. */
}

/* Non-blocking pass over every tracked, running service, reaping any
 * that have died. Deliberately does NOT use u_wait_any(), which
 * blocks until *something* dies -- that would starve scan_services()
 * from ever running again while nothing is crashing, which would
 * silently break picking up newly gtouch'd/gwrite'n services. */
static void reap_dead(void) {
  for (int i = 0; i < MAX_SERVICES; i++) {
    struct service *svc = &services[i];
    if (!svc->used || !svc->running) {
      continue;
    }

    nx_task_info_t info;
    if (!u_find_task(svc->pid, &info)) {
      u_wait(svc->pid); /* gone from the task table -- reap anyway */
      handle_exit(svc);
      continue;
    }
    if (info.state != TASK_STATE_DEAD) {
      continue;
    }
    u_wait(svc->pid);
    handle_exit(svc);
  }
}

int main(void) {
  u_print("[init] starting -- reading service definitions from /services\n");

  scan_services();
  start_ready_services();

  for (;;) {
    reap_dead();
    scan_services();
    start_ready_services();
    u_sleep_ms(500);
  }

  return 0;
}
