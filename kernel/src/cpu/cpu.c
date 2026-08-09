#include "cpu/cpu.h"
#include "cpu/io.h"
#include "mm/pmm.h"
#include "boot/requests.h"

struct cpu_local *g_cpus[MAX_CPUS];
uint32_t g_cpu_count = 0;

#define IA32_EFER    0xC0000080
#define EFER_NXE     (1ULL << 11)
#define IA32_GS_BASE 0xC0000101

/*
 * Must run on *every* CPU before any of our page tables (which mark
 * .rodata/.data/.bss and the whole direct map NX) are made active via
 * CR3. Per the Intel/AMD manuals: if EFER.NXE=0, bit 63 of a
 * paging-structure entry is a *reserved* bit, not an ignored one --
 * using an entry with it set faults on every access (read, write, or
 * execute), not just execution attempts. So this has to happen strictly
 * before vmm_init()'s CR3 switch on the BSP, and strictly before an AP
 * loads the shared kernel pagemap.
 */
void cpu_enable_nx(void) {
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_NXE);
}

struct cpu_local *cpu_local_create(uint32_t cpu_index, uint32_t lapic_id,
                                    uint32_t processor_id, bool is_bsp) {
    /* sizeof(struct cpu_local) comfortably fits two pages; allocate
     * directly from the PMM since this runs before the heap exists. */
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
    cpu->tick_count = 0;

    return cpu;
}

void cpu_setup_current(struct cpu_local *cpu) {
    uint64_t ist1_top = (uint64_t)&cpu->df_stack[sizeof(cpu->df_stack)];
    gdt_init(&cpu->gdt, &cpu->tss, ist1_top);
    wrmsr(IA32_GS_BASE, (uint64_t)cpu);
}
