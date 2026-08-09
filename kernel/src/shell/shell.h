#ifndef NEXUS_SHELL_H
#define NEXUS_SHELL_H

/* Task entry point (matches task_entry_t) -- an interactive line-based
 * command shell driven by the PS/2 keyboard, output via kprintf. */
void shell_task(void *arg);

#endif /* NEXUS_SHELL_H */
