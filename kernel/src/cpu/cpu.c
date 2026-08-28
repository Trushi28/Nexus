#include "cpu/cpu.h"
#include "boot/requests.h"
#include "cpu/io.h"
#include "mm/pmm.h"

struct cpu_local *g_cpus[MAX_CPUS];
uint32_t g_cpu_count = 0;

#define IA32_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)
#define IA32_GS_BASE 0xC0000101

void cpu_enable_nx(void) {
  uint64_t efer = rdmsr(IA32_EFER);
  wrmsr(IA32_EFER, efer | EFER_NXE);
}

struct cpu_local *cpu_local_create(uint32_t cpu_index, uint32_t lapic_id, uint32_t processor_id, bool is_bsp) {
  uint64_t phys = pmm_alloc_pages(2);
  struct cpu_local *cpu = (struct cpu_local *)phys_to_virt(phys);

  cpu->self = cpu;
  cpu->cpu_index = cpu_index;
  cpu->lapic_id = lapic_id;
  cpu->processor_id = processor_id;
  cpu->is_bsp = is_bsp;
  cpu->online = false;
  cpu->current_task = NULL;
  cpu->idle_task = NULL;
  cpu->pending_exit = NULL;
  cpu->parked_head = NULL;
  cpu->tick_count = 0;

  return cpu;
}

void cpu_setup_current(struct cpu_local *cpu) {
  uint64_t ist1_top = (uint64_t)&cpu->df_stack[sizeof(cpu->df_stack)];
  gdt_init(&cpu->gdt, &cpu->tss, ist1_top);
  wrmsr(IA32_GS_BASE, (uint64_t)cpu);
}
