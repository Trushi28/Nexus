#include "drivers/nvme.h"
#include "boot/requests.h"
#include "cpu/cpu.h"
#include "cpu/isr.h"
#include "cpu/vectors.h"
#include "debug/log.h"
#include "drivers/pci.h"
#include "klib/klib.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "time/timer.h"

/* ---- controller register layout (NVMe Base Spec, "Controller
 * Registers") -- all offsets from BAR0, little-endian. ---- */
#define NVME_REG_CAP 0x00  /* 64-bit: controller capabilities */
#define NVME_REG_VS 0x08   /* 32-bit: version (unused here) */
#define NVME_REG_CC 0x14   /* 32-bit: controller configuration */
#define NVME_REG_CSTS 0x1C /* 32-bit: controller status */
#define NVME_REG_AQA 0x24  /* 32-bit: admin queue attributes */
#define NVME_REG_ASQ 0x28  /* 64-bit: admin submission queue base */
#define NVME_REG_ACQ 0x30  /* 64-bit: admin completion queue base */
#define NVME_REG_DOORBELL_BASE 0x1000

#define NVME_CC_EN (1u << 0)
#define NVME_CC_CSS_NVM (0u << 4)
#define NVME_CC_MPS_SHIFT                                                      \
  7 /* 0 here == 2^(12+0) == 4KiB, matching PAGE_SIZE                          \
     */
#define NVME_CC_AMS_ROUND_ROBIN (0u << 11)
#define NVME_CC_SHN_NONE (0u << 14)
#define NVME_CC_IOSQES_SHIFT 16
#define NVME_CC_IOCQES_SHIFT 20

#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)

#define NVME_SQE_SIZE_LOG2 6 /* 64 bytes */
#define NVME_CQE_SIZE_LOG2 4 /* 16 bytes */
#define NVME_SQE_SIZE (1u << NVME_SQE_SIZE_LOG2)
#define NVME_CQE_SIZE (1u << NVME_CQE_SIZE_LOG2)

#define NVME_ADMIN_QUEUE_DEPTH 16
#define NVME_IO_QUEUE_DEPTH 32

#define NVME_OPC_CREATE_SQ 0x01
#define NVME_OPC_CREATE_CQ 0x05
#define NVME_OPC_IDENTIFY 0x06

#define NVME_IO_OPC_WRITE 0x01
#define NVME_IO_OPC_READ 0x02

#define NVME_IDENTIFY_CNS_NAMESPACE 0x00

/* Reset/command timeouts. A fixed conservative bound rather than
 * reading CAP.TO -- simpler, and 5s is generous for anything short of
 * genuinely broken hardware/emulation. */
#define NVME_TIMEOUT_MS 5000

struct PACKED nvme_sqe {
  uint32_t cdw0;
  uint32_t nsid;
  uint64_t reserved;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct PACKED nvme_cqe {
  uint32_t cmd_specific;
  uint32_t reserved;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status; /* bit0 = phase tag, bits1-8 = SC, bits9-11 = SCT */
};

struct PACKED nvme_identify_namespace {
  uint64_t nsze;     /* offset 0: namespace size, in logical blocks */
  uint64_t ncap;     /* offset 8 */
  uint64_t nuse;     /* offset 16 */
  uint8_t nsfeat;    /* offset 24 */
  uint8_t nlbaf;     /* offset 25: number of LBA formats - 1 */
  uint8_t flbas;     /* offset 26: bits0-3 = index of the in-use format */
  uint8_t pad1[101]; /* offsets 27-127: fields v1 doesn't need */
  struct PACKED {
    uint16_t ms;   /* metadata size */
    uint8_t lbads; /* log2(LBA data size in bytes) */
    uint8_t rp;    /* relative performance */
  } lbaf[16];      /* offsets 128-191 */
                   /* offsets 192-4095: vendor-specific / unused here */
};

struct nvme_queue {
  struct nvme_sqe *sq; /* HHDM-mapped */
  struct nvme_cqe *cq; /* HHDM-mapped */
  uint64_t sq_phys, cq_phys;
  uint16_t depth;
  uint16_t sq_tail;
  uint16_t cq_head;
  bool phase; /* expected phase-tag value on the next
                  unconsumed completion entry -- flips
                  every time the completion queue wraps */
  volatile uint32_t *sq_doorbell;
  volatile uint32_t *cq_doorbell;
  uint16_t next_cid;
  struct wait_queue irq_wq;
};

struct nvme_controller {
  struct pci_device dev;
  volatile uint8_t *regs;
  uint32_t doorbell_stride; /* bytes between consecutive doorbell registers */
  struct pci_msix msix;
  struct nvme_queue admin_q;
  struct nvme_queue io_q;
  uint32_t sector_size;
  uint64_t sector_count;
  bool ready;
};

static struct nvme_controller g_nvme;

static uint32_t reg_read32(uint32_t off) {
  return *(volatile uint32_t *)(g_nvme.regs + off);
}
static void reg_write32(uint32_t off, uint32_t val) {
  *(volatile uint32_t *)(g_nvme.regs + off) = val;
}
static uint64_t reg_read64(uint32_t off) {
  /* Two 32-bit accesses, not one 64-bit one -- keeps this correct
   * even against a controller whose BAR only guarantees well-defined
   * behaviour for dword-aligned accesses. */
  uint32_t lo = reg_read32(off);
  uint32_t hi = reg_read32(off + 4);
  return ((uint64_t)hi << 32) | lo;
}
static void reg_write64(uint32_t off, uint64_t val) {
  reg_write32(off, (uint32_t)(val & 0xFFFFFFFF));
  reg_write32(off + 4, (uint32_t)(val >> 32));
}

static bool queue_init(struct nvme_queue *q, uint16_t depth, uint16_t qid) {
  q->depth = depth;
  q->sq_tail = 0;
  q->cq_head = 0;
  q->phase = true;
  q->next_cid = 0;
  q->irq_wq.waiters_head = NULL;
  q->irq_wq.waiters_tail = NULL;
  spinlock_init(&q->irq_wq.lock);

  uint64_t sq_pages = DIV_ROUND_UP((uint64_t)depth * NVME_SQE_SIZE, PAGE_SIZE);
  uint64_t cq_pages = DIV_ROUND_UP((uint64_t)depth * NVME_CQE_SIZE, PAGE_SIZE);

  q->sq_phys = pmm_alloc_pages(sq_pages);
  q->cq_phys = pmm_alloc_pages(cq_pages);
  if (q->sq_phys == 0 || q->cq_phys == 0) {
    return false;
  }
  q->sq = (struct nvme_sqe *)phys_to_virt(q->sq_phys);
  q->cq = (struct nvme_cqe *)phys_to_virt(q->cq_phys);

  uint32_t db_off =
      NVME_REG_DOORBELL_BASE + (uint32_t)(2 * qid) * g_nvme.doorbell_stride;
  q->sq_doorbell = (volatile uint32_t *)(g_nvme.regs + db_off);
  q->cq_doorbell =
      (volatile uint32_t *)(g_nvme.regs + db_off + g_nvme.doorbell_stride);
  return true;
}

static bool submit_and_wait(struct nvme_queue *q, struct nvme_sqe *cmd) {
  uint16_t cid = q->next_cid++;
  cmd->cdw0 = (cmd->cdw0 & 0x0000FFFFu) | ((uint32_t)cid << 16);

  memcpy(&q->sq[q->sq_tail], cmd, sizeof(*cmd));
  q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);

  wait_queue_register(&q->irq_wq);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  *q->sq_doorbell = q->sq_tail;

  task_block();

  struct nvme_cqe cqe = q->cq[q->cq_head];
  bool phase_ok = (((cqe.status & 1) != 0) == q->phase);
  if (!phase_ok) {
    kprintf("[nvme] woke for a completion but the phase tag doesn't "
            "match -- queue state is out of sync\n");
    return false;
  }
  q->cq_head = (uint16_t)((q->cq_head + 1) % q->depth);
  if (q->cq_head == 0) {
    q->phase = !q->phase;
  }
  *q->cq_doorbell = q->cq_head;

  if (cqe.cid != cid) {
    kprintf("[nvme] completion CID mismatch (got %u, expected %u) -- "
            "queue out of sync\n",
            cqe.cid, cid);
    return false;
  }

  uint16_t sc = (cqe.status >> 1) & 0xFF;
  uint16_t sct = (cqe.status >> 9) & 0x7;
  if (sc != 0 || sct != 0) {
    kprintf("[nvme] command failed: SCT=%u SC=0x%x\n", sct, sc);
    return false;
  }
  return true;
}
static bool identify(uint8_t cns, uint32_t nsid,
                     void *out_page /* HHDM virt, page-aligned */) {
  struct nvme_sqe cmd = {0};
  cmd.cdw0 = NVME_OPC_IDENTIFY;
  cmd.nsid = nsid;
  cmd.prp1 = virt_to_phys_hhdm(out_page);
  cmd.cdw10 = cns;
  return submit_and_wait(&g_nvme.admin_q, &cmd);
}

static bool identify_namespace_and_fill_geometry(uint32_t nsid) {
  uint64_t page_phys = pmm_alloc_page();
  if (page_phys == 0) {
    return false;
  }
  void *page = phys_to_virt(page_phys);

  if (!identify(NVME_IDENTIFY_CNS_NAMESPACE, nsid, page)) {
    pmm_free_page(page_phys);
    return false;
  }

  struct nvme_identify_namespace *ns = (struct nvme_identify_namespace *)page;
  uint8_t fmt_idx = ns->flbas & 0xF;

  g_nvme.sector_size = 1u << ns->lbaf[fmt_idx].lbads;
  g_nvme.sector_count = ns->nsze;

  pmm_free_page(page_phys);
  return true;
}

static bool create_io_cq(uint16_t qid, uint16_t depth, uint64_t cq_phys) {
  struct nvme_sqe cmd = {0};
  cmd.cdw0 = NVME_OPC_CREATE_CQ;
  cmd.prp1 = cq_phys;
  cmd.cdw10 = ((uint32_t)(depth - 1) << 16) | qid;
  cmd.cdw11 =
      1u /* PC */ | (1u << 1) /* IEN */ | (1u << 16) /* IV = MSI-X index 1 */;
  return submit_and_wait(&g_nvme.admin_q, &cmd);
}
static bool create_io_sq(uint16_t qid, uint16_t depth, uint64_t sq_phys,
                         uint16_t cqid) {
  struct nvme_sqe cmd = {0};
  cmd.cdw0 = NVME_OPC_CREATE_SQ;
  cmd.prp1 = sq_phys;
  cmd.cdw10 = ((uint32_t)(depth - 1) << 16) | qid;
  cmd.cdw11 = ((uint32_t)cqid << 16) | 1u; /* CQID, QPRIO=0, PC=1 */
  return submit_and_wait(&g_nvme.admin_q, &cmd);
}

static bool wait_csts_rdy(bool want_ready) {
  uint64_t deadline = timer_uptime_ms() + NVME_TIMEOUT_MS;
  for (;;) {
    uint32_t csts = reg_read32(NVME_REG_CSTS);
    if (csts & NVME_CSTS_CFS) {
      kprintf("[nvme] controller reports a fatal status during reset\n");
      return false;
    }
    if (((csts & NVME_CSTS_RDY) != 0) == want_ready) {
      return true;
    }
    if (timer_uptime_ms() > deadline) {
      kprintf("[nvme] timed out waiting for CSTS.RDY=%d\n", want_ready);
      return false;
    }
    asm volatile("pause");
  }
}

static void nvme_admin_irq_handler(struct interrupt_frame *frame) {
  (void)frame;
  wait_queue_wake(&g_nvme.admin_q.irq_wq);
}

static void nvme_io_irq_handler(struct interrupt_frame *frame) {
  (void)frame;
  wait_queue_wake(&g_nvme.io_q.irq_wq);
}

bool nvme_init(void) {
  pci_scan();

  const struct pci_device *found = pci_find_by_class(0x01, 0x08, 0x02);
  if (found == NULL) {
    kprintf("[nvme] no NVMe controller found\n");
    return false;
  }
  g_nvme.dev = *found;

  if (!g_nvme.dev.bars[0].present || !g_nvme.dev.bars[0].is_mmio) {
    kprintf("[nvme] controller's BAR0 isn't a usable MMIO region\n");
    return false;
  }

  pci_enable_device(&g_nvme.dev);
  g_nvme.regs = (volatile uint8_t *)vmm_map_mmio(g_nvme.dev.bars[0].base,
                                                 g_nvme.dev.bars[0].size);
  if (!pci_msix_init(&g_nvme.dev, &g_nvme.msix)) {
    kprintf("[nvme] controller has no MSI-X capability -- unsupported\n");
    return false;
  }

  register_interrupt_handler(VEC_NVME_ADMIN, nvme_admin_irq_handler);
  register_interrupt_handler(VEC_NVME_IO, nvme_io_irq_handler);
  uint32_t dest_apic_id = g_cpus[0]->lapic_id;
  pci_msix_set_vector(&g_nvme.dev, &g_nvme.msix, 0, dest_apic_id,
                      VEC_NVME_ADMIN);
  pci_msix_set_vector(&g_nvme.dev, &g_nvme.msix, 1, dest_apic_id, VEC_NVME_IO);
  pci_msix_enable(&g_nvme.dev, &g_nvme.msix);

  uint64_t cap = reg_read64(NVME_REG_CAP);
  uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
  g_nvme.doorbell_stride = 4u << ((cap >> 32) & 0xF);

  if (!((cap >> 37) & 1)) {
    kprintf("[nvme] controller doesn't support the NVM command set\n");
    return false;
  }

  uint32_t admin_depth = MIN((uint32_t)NVME_ADMIN_QUEUE_DEPTH, mqes);
  uint32_t io_depth = MIN((uint32_t)NVME_IO_QUEUE_DEPTH, mqes);

  /* Reset: CC.EN=0, wait for CSTS.RDY to drop -- the sequence the
   * spec requires before AQA/ASQ/ACQ are allowed to be touched. */
  reg_write32(NVME_REG_CC, 0);
  if (!wait_csts_rdy(false)) {
    return false;
  }

  if (!queue_init(&g_nvme.admin_q, (uint16_t)admin_depth, 0)) {
    kprintf("[nvme] out of memory allocating the admin queue\n");
    return false;
  }

  reg_write32(NVME_REG_AQA, ((admin_depth - 1) << 16) | (admin_depth - 1));
  reg_write64(NVME_REG_ASQ, g_nvme.admin_q.sq_phys);
  reg_write64(NVME_REG_ACQ, g_nvme.admin_q.cq_phys);

  uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | (0u << NVME_CC_MPS_SHIFT) |
                NVME_CC_AMS_ROUND_ROBIN | NVME_CC_SHN_NONE |
                (NVME_SQE_SIZE_LOG2 << NVME_CC_IOSQES_SHIFT) |
                (NVME_CQE_SIZE_LOG2 << NVME_CC_IOCQES_SHIFT);
  reg_write32(NVME_REG_CC, cc);

  if (!wait_csts_rdy(true)) {
    kprintf("[nvme] controller failed to come ready after enable\n");
    return false;
  }

  if (!identify_namespace_and_fill_geometry(1)) {
    kprintf("[nvme] failed to identify namespace 1\n");
    return false;
  }

  if (!queue_init(&g_nvme.io_q, (uint16_t)io_depth, 1)) {
    kprintf("[nvme] out of memory allocating the I/O queue\n");
    return false;
  }
  if (!create_io_cq(1, (uint16_t)io_depth, g_nvme.io_q.cq_phys) ||
      !create_io_sq(1, (uint16_t)io_depth, g_nvme.io_q.sq_phys, 1)) {
    kprintf("[nvme] failed to create the I/O queue pair\n");
    return false;
  }
  /* Partial-failure cleanup above is intentionally not implemented --
   * this runs once at boot and failure just means "no disk", not
   * "retry forever"; the couple of pages a failed bring-up leaks are
   * noise against the rest of the kernel's memory. */

  g_nvme.ready = true;
  kprintf(
      "[nvme] ready: %lu sectors x %u bytes, admin depth %u, I/O depth %u\n",
      g_nvme.sector_count, g_nvme.sector_size, admin_depth, io_depth);
  return true;
}

bool nvme_available(void) { return g_nvme.ready; }
uint64_t nvme_sector_count(void) { return g_nvme.sector_count; }
uint32_t nvme_sector_size(void) { return g_nvme.sector_size; }

#define NVME_MAX_PRPLIST_ENTRIES                                               \
  (PAGE_SIZE / sizeof(uint64_t)) /* 512 pointers per list page */
/* PRP1 covers exactly one page (buf is required page-aligned); anything
 * beyond a second page goes through a single PRP-list page holding up
 * to 512 more page pointers. v1 doesn't chain multiple list pages, so
 * this is the hard ceiling on one request's size (~2MiB). */
#define NVME_MAX_TRANSFER_BYTES ((1 + NVME_MAX_PRPLIST_ENTRIES) * PAGE_SIZE)

static bool rw(uint64_t lba, uint32_t count, void *buf, bool is_write) {
  if (!g_nvme.ready) {
    return false;
  }
  if ((uint64_t)buf % PAGE_SIZE != 0) {
    kprintf("[nvme] buffer 0x%p isn't page-aligned\n", buf);
    return false;
  }

  uint64_t total_bytes = (uint64_t)count * g_nvme.sector_size;
  if (total_bytes == 0 || total_bytes > NVME_MAX_TRANSFER_BYTES) {
    kprintf("[nvme] request of %lu bytes is out of range\n", total_bytes);
    return false;
  }

  uint64_t buf_phys = virt_to_phys_hhdm(buf);
  uint64_t total_pages = DIV_ROUND_UP(total_bytes, PAGE_SIZE);

  uint64_t prp2 = 0;
  uint64_t prplist_phys = 0;

  if (total_pages == 2) {
    prp2 = buf_phys + PAGE_SIZE;
  } else if (total_pages > 2) {
    prplist_phys = pmm_alloc_page();
    if (prplist_phys == 0) {
      kprintf("[nvme] out of memory building a PRP list\n");
      return false;
    }
    uint64_t *prplist = (uint64_t *)phys_to_virt(prplist_phys);
    for (uint64_t i = 1; i < total_pages; i++) {
      prplist[i - 1] = buf_phys + i * PAGE_SIZE;
    }
    prp2 = prplist_phys;
  }

  struct nvme_sqe cmd = {0};
  cmd.cdw0 = is_write ? NVME_IO_OPC_WRITE : NVME_IO_OPC_READ;
  cmd.nsid = 1;
  cmd.prp1 = buf_phys;
  cmd.prp2 = prp2;
  cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = count - 1; /* NLB is 0-based */

  bool ok = submit_and_wait(&g_nvme.io_q, &cmd);

  if (prplist_phys != 0) {
    pmm_free_page(prplist_phys);
  }
  return ok;
}

bool nvme_read(uint64_t lba, uint32_t count, void *buf) {
  return rw(lba, count, buf, false);
}

bool nvme_write(uint64_t lba, uint32_t count, const void *buf) {
  return rw(lba, count, (void *)buf, true);
}
