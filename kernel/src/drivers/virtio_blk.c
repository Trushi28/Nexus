#include "drivers/virtio_blk.h"
#include "boot/requests.h"
#include "debug/log.h"
#include "drivers/blockdev.h"
#include "drivers/pci.h"
#include "klib/klib.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "time/timer.h"

/* ---- PCI identification (spec 5.1.4, 4.1.2) ----
 * Non-transitional (modern-only) virtio-blk-pci is vendor 0x1AF4,
 * device 0x1042 (0x1040 + block's virtio device ID, 2). Transitional
 * devices (support legacy AND modern simultaneously) present 0x1001
 * instead but still expose the full modern capability list this
 * driver uses -- VERIFY ON REAL QEMU which ID your target actually
 * presents; recent QEMU defaults to 0x1042 (`disable-legacy=on` is
 * QEMU's own current default for virtio-blk-pci), older/explicit
 * transitional configurations present 0x1001. Both are accepted here;
 * the real gate is negotiate_features() requiring VIRTIO_F_VERSION_1,
 * which is what actually determines "does modern config work at
 * all", not the PCI ID (which is only a fast way to find candidates). */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID_TRANSITIONAL 0x1001
#define VIRTIO_BLK_DEVICE_ID_MODERN 0x1042

/* ---- vendor-specific PCI capability layout (spec 4.1.4) ---- */
#define PCI_CAP_ID_VENDOR 0x09
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
/* byte offsets within one vendor capability structure */
#define VIRTIO_CAP_OFF_CFG_TYPE 3
#define VIRTIO_CAP_OFF_BAR 4
#define VIRTIO_CAP_OFF_OFFSET 8
#define VIRTIO_CAP_OFF_LENGTH 12
#define VIRTIO_CAP_OFF_NOTIFY_MULTIPLIER 16 /* NOTIFY_CFG only */

/* ---- common configuration structure (spec 4.1.4.3), MMIO-mapped ---- */
struct PACKED virtio_pci_common_cfg {
  uint32_t device_feature_select;
  uint32_t device_feature;
  uint32_t driver_feature_select;
  uint32_t driver_feature;
  uint16_t msix_config;
  uint16_t num_queues;
  uint8_t device_status;
  uint8_t config_generation;

  uint16_t queue_select;
  uint16_t queue_size;
  uint16_t queue_msix_vector;
  uint16_t queue_enable;
  uint16_t queue_notify_off;
  uint64_t queue_desc;
  uint64_t queue_avail;
  uint64_t queue_used;
};

#define VIRTIO_MSI_NO_VECTOR 0xFFFF

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_F_VERSION_1_BIT 0 /* bit 32 overall == bit 0 of the high half */

/* ---- device-specific configuration (spec 5.2.4), MMIO-mapped ----
 * Only the fields this driver actually reads are named individually;
 * the rest are only ever skipped over via `reserved_tail`, never
 * negotiated (see the file header comment). */
struct PACKED virtio_blk_config {
  uint64_t capacity; /* ALWAYS in 512-byte sectors, regardless of any
                         negotiated blk_size */
  uint8_t reserved_tail[52]; /* size_max, seg_max, geometry, blk_size,
                                 topology, writeback, discard/write-zeroes
                                 fields -- none negotiated, none read */
};

/* ---- split virtqueue layout (spec 2.7) ----
 * Three separately-allocated regions rather than one combined buffer
 * with manual offset math -- simpler, and each is already page-sized
 * or smaller for any realistic queue depth, so there's no real memory
 * cost to keeping them apart. */
struct PACKED virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2

struct PACKED virtq_avail_hdr {
  uint16_t flags;
  uint16_t idx;
};

struct PACKED virtq_used_elem {
  uint32_t id;
  uint32_t len;
};
struct PACKED virtq_used_hdr {
  uint16_t flags;
  uint16_t idx;
};

struct virtqueue {
  uint16_t size;
  uint64_t desc_phys, avail_phys, used_phys;
  struct virtq_desc *desc;             /* HHDM-mapped */
  struct virtq_avail_hdr *avail_hdr;   /* HHDM-mapped */
  uint16_t *avail_ring;                /* right after avail_hdr */
  struct virtq_used_hdr *used_hdr;     /* HHDM-mapped */
  struct virtq_used_elem *used_ring;   /* right after used_hdr */
  uint16_t last_used_idx;              /* our own consume cursor */
};

/* ---- virtio-blk request format (spec 5.2.6) ---- */
struct PACKED virtio_blk_req_header {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};
#define VIRTIO_BLK_T_IN 0  /* read */
#define VIRTIO_BLK_T_OUT 1 /* write */
#define VIRTIO_BLK_S_OK 0

#define VIRTIO_BLK_SECTOR_SIZE 512
#define VIRTIO_BLK_TIMEOUT_MS 5000 /* matches nvme.c's own conservative bound */

struct virtio_blk_dev {
  struct pci_device dev;
  struct virtqueue vq;
  volatile uint16_t *notify_addr;

  struct virtio_blk_req_header *req_hdr; /* HHDM-mapped, reused every request */
  uint64_t req_hdr_phys;
  uint8_t *req_status;
  uint64_t req_status_phys;

  uint64_t sector_count;
  bool ready;
};
static struct virtio_blk_dev g_vblk;

static const struct pci_device *find_virtio_blk_device(void) {
  uint32_t n = pci_device_count();
  for (uint32_t i = 0; i < n; i++) {
    const struct pci_device *d = pci_device_at(i);
    if (d->vendor_id == VIRTIO_PCI_VENDOR_ID &&
        (d->device_id == VIRTIO_BLK_DEVICE_ID_MODERN ||
         d->device_id == VIRTIO_BLK_DEVICE_ID_TRANSITIONAL)) {
      return d;
    }
  }
  return NULL;
}

/* Walks the PCI capability list looking for every vendor-specific
 * (id 0x09) capability, classifying each by its cfg_type byte and
 * MMIO-mapping the ones this driver needs. Unlike drivers/pci.c's own
 * pci_find_capability() (which stops at the FIRST match of a given
 * id), virtio-pci legitimately has several capabilities sharing id
 * 0x09 -- one per cfg_type -- so this has to walk the whole chain
 * itself rather than reusing that helper. Returns false if any of the
 * three required capabilities (common/notify/device config) is
 * missing, or points at a BAR this driver didn't find during
 * pci_scan(). */
static bool find_virtio_caps(struct pci_device *dev,
                             volatile struct virtio_pci_common_cfg **common_out,
                             volatile uint8_t **notify_base_out,
                             uint32_t *notify_off_multiplier_out,
                             volatile struct virtio_blk_config **device_cfg_out) {
  uint16_t status = pci_cfg_read16(dev->bus, dev->slot, dev->func, 0x06);
  if (!(status & (1u << 4))) {
    return false; /* PCI_STATUS_CAP_LIST not set -- no capability list at all */
  }

  bool have_common = false, have_notify = false, have_device = false;
  uint8_t ptr = pci_cfg_read8(dev->bus, dev->slot, dev->func, 0x34) & 0xFC;
  int guard = 0; /* same bounded-walk defensiveness as pci_find_capability() */

  while (ptr != 0 && guard++ < 64) {
    uint8_t id = pci_cfg_read8(dev->bus, dev->slot, dev->func, ptr);

    if (id == PCI_CAP_ID_VENDOR) {
      uint8_t cfg_type =
          pci_cfg_read8(dev->bus, dev->slot, dev->func,
                       (uint8_t)(ptr + VIRTIO_CAP_OFF_CFG_TYPE));
      uint8_t bar = pci_cfg_read8(dev->bus, dev->slot, dev->func,
                                  (uint8_t)(ptr + VIRTIO_CAP_OFF_BAR));
      uint32_t offset = pci_cfg_read32(dev->bus, dev->slot, dev->func,
                                       (uint8_t)(ptr + VIRTIO_CAP_OFF_OFFSET));
      uint32_t length = pci_cfg_read32(dev->bus, dev->slot, dev->func,
                                       (uint8_t)(ptr + VIRTIO_CAP_OFF_LENGTH));

      if (bar < PCI_MAX_BARS && dev->bars[bar].present) {
        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG && !have_common) {
          *common_out = (volatile struct virtio_pci_common_cfg *)vmm_map_mmio(
              dev->bars[bar].base + offset, length);
          have_common = true;
        } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && !have_notify) {
          *notify_base_out =
              (volatile uint8_t *)vmm_map_mmio(dev->bars[bar].base + offset, length);
          *notify_off_multiplier_out = pci_cfg_read32(
              dev->bus, dev->slot, dev->func,
              (uint8_t)(ptr + VIRTIO_CAP_OFF_NOTIFY_MULTIPLIER));
          have_notify = true;
        } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG && !have_device) {
          *device_cfg_out = (volatile struct virtio_blk_config *)vmm_map_mmio(
              dev->bars[bar].base + offset, length);
          have_device = true;
        }
      }
    }

    ptr = pci_cfg_read8(dev->bus, dev->slot, dev->func, (uint8_t)(ptr + 1)) & 0xFC;
  }

  return have_common && have_notify && have_device;
}

/* Negotiates the minimal feature subset: VIRTIO_F_VERSION_1 (bit 32)
 * and nothing else. A device that doesn't offer it is legacy-only --
 * unsupported by this driver, not worked around. */
static bool negotiate_features(volatile struct virtio_pci_common_cfg *common) {
  common->device_feature_select = 1; /* bits 32-63 */
  uint32_t features_hi = common->device_feature;

  if (!(features_hi & (1u << VIRTIO_F_VERSION_1_BIT))) {
    kprintf("[virtio-blk] device doesn't offer VIRTIO_F_VERSION_1 -- "
            "legacy-only device, unsupported\n");
    return false;
  }

  common->driver_feature_select = 0; /* bits 0-31: none of them */
  common->driver_feature = 0;
  common->driver_feature_select = 1; /* bits 32-63: just VERSION_1 */
  common->driver_feature = (1u << VIRTIO_F_VERSION_1_BIT);

  common->device_status = (uint8_t)(common->device_status | VIRTIO_STATUS_FEATURES_OK);
  if (!(common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
    kprintf("[virtio-blk] device rejected our feature subset\n");
    return false;
  }
  return true;
}

static bool virtqueue_alloc(struct virtqueue *vq, uint16_t size) {
  uint64_t desc_bytes = (uint64_t)size * sizeof(struct virtq_desc);
  uint64_t avail_bytes =
      sizeof(struct virtq_avail_hdr) + (uint64_t)size * sizeof(uint16_t);
  uint64_t used_bytes =
      sizeof(struct virtq_used_hdr) + (uint64_t)size * sizeof(struct virtq_used_elem);

  uint64_t desc_pages = DIV_ROUND_UP(desc_bytes, PAGE_SIZE);
  uint64_t avail_pages = DIV_ROUND_UP(avail_bytes, PAGE_SIZE);
  uint64_t used_pages = DIV_ROUND_UP(used_bytes, PAGE_SIZE);

  uint64_t desc_phys = pmm_alloc_pages(desc_pages);
  uint64_t avail_phys = pmm_alloc_pages(avail_pages);
  uint64_t used_phys = pmm_alloc_pages(used_pages);
  if (desc_phys == 0 || avail_phys == 0 || used_phys == 0) {
    return false; /* leaks whichever of the three did succeed -- boot-time
                      failure only, same tradeoff nvme.c's own init makes */
  }

  vq->size = size;
  vq->desc_phys = desc_phys;
  vq->avail_phys = avail_phys;
  vq->used_phys = used_phys;
  vq->desc = (struct virtq_desc *)phys_to_virt(desc_phys);
  vq->avail_hdr = (struct virtq_avail_hdr *)phys_to_virt(avail_phys);
  vq->avail_ring = (uint16_t *)((uint8_t *)vq->avail_hdr + sizeof(*vq->avail_hdr));
  vq->used_hdr = (struct virtq_used_hdr *)phys_to_virt(used_phys);
  vq->used_ring =
      (struct virtq_used_elem *)((uint8_t *)vq->used_hdr + sizeof(*vq->used_hdr));
  vq->last_used_idx = 0; /* pmm_alloc_pages() zeroes -- device's used.idx
                             also starts at 0, so these agree from boot */
  return true;
}

static void virtio_blk_notify(void) { *g_vblk.notify_addr = 0; /* queue index 0 */ }

/* Builds the fixed 3-descriptor chain (header -> data -> status) for
 * ONE request, submits it, and polls the used ring until it advances
 * or VIRTIO_BLK_TIMEOUT_MS elapses. No free-list, no concurrent
 * requests -- see the file header comment for why that's fine here. */
static bool submit_and_wait(uint64_t sector, uint32_t sector_count, void *buf,
                            bool is_write) {
  if (!g_vblk.ready) {
    return false;
  }

  g_vblk.req_hdr->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  g_vblk.req_hdr->reserved = 0;
  g_vblk.req_hdr->sector = sector;
  *g_vblk.req_status = 0xFF; /* sentinel; the device must overwrite this */

  struct virtq_desc *desc = g_vblk.vq.desc;

  desc[0].addr = g_vblk.req_hdr_phys;
  desc[0].len = sizeof(struct virtio_blk_req_header);
  desc[0].flags = VIRTQ_DESC_F_NEXT;
  desc[0].next = 1;

  desc[1].addr = virt_to_phys_hhdm(buf);
  desc[1].len = sector_count * VIRTIO_BLK_SECTOR_SIZE;
  desc[1].flags = (uint16_t)(VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE));
  desc[1].next = 2;

  desc[2].addr = g_vblk.req_status_phys;
  desc[2].len = 1;
  desc[2].flags = VIRTQ_DESC_F_WRITE;
  desc[2].next = 0;

  uint16_t avail_slot = (uint16_t)(g_vblk.vq.avail_hdr->idx % g_vblk.vq.size);
  g_vblk.vq.avail_ring[avail_slot] = 0; /* head descriptor index -- always 0 */

  /* The device reads the avail ring over the bus -- make sure it sees
   * the descriptor chain and ring entry above before it sees the
   * incremented idx that tells it something's there. Same ordering
   * concern nvme.c's own doorbell writes document, same fix. */
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  g_vblk.vq.avail_hdr->idx = (uint16_t)(g_vblk.vq.avail_hdr->idx + 1);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  virtio_blk_notify();

  uint64_t deadline = timer_uptime_ms() + VIRTIO_BLK_TIMEOUT_MS;
  while (g_vblk.vq.used_hdr->idx == g_vblk.vq.last_used_idx) {
    if (timer_uptime_ms() > deadline) {
      kprintf("[virtio-blk] request timed out waiting for the used ring\n");
      return false;
    }
    asm volatile("pause");
  }
  g_vblk.vq.last_used_idx++;

  if (*g_vblk.req_status != VIRTIO_BLK_S_OK) {
    kprintf("[virtio-blk] request failed, status=%u\n", *g_vblk.req_status);
    return false;
  }
  return true;
}

bool virtio_blk_available(void) { return g_vblk.ready; }
uint64_t virtio_blk_sector_count(void) { return g_vblk.sector_count; }
uint32_t virtio_blk_sector_size(void) { return VIRTIO_BLK_SECTOR_SIZE; }

bool virtio_blk_read(uint64_t lba, uint32_t count, void *buf) {
  return submit_and_wait(lba, count, buf, false);
}
bool virtio_blk_write(uint64_t lba, uint32_t count, const void *buf) {
  return submit_and_wait(lba, count, (void *)buf, true);
}

static const struct blockdev_ops virtio_blk_blockdev_ops = {
    .sector_count = virtio_blk_sector_count,
    .sector_size = virtio_blk_sector_size,
    .read = virtio_blk_read,
    .write = virtio_blk_write,
};

bool virtio_blk_init(void) {
  pci_scan(); /* safe to call again even if something else already did --
                 see drivers/nvme.c's nvme_init(), which makes the same
                 no-prior-scan-assumed choice */

  const struct pci_device *found = find_virtio_blk_device();
  if (found == NULL) {
    kprintf("[virtio-blk] no virtio-blk PCI device found\n");
    return false;
  }
  g_vblk.dev = *found;
  pci_enable_device(&g_vblk.dev);

  volatile struct virtio_pci_common_cfg *common = NULL;
  volatile uint8_t *notify_base = NULL;
  uint32_t notify_off_multiplier = 0;
  volatile struct virtio_blk_config *device_cfg = NULL;

  if (!find_virtio_caps(&g_vblk.dev, &common, &notify_base, &notify_off_multiplier,
                        &device_cfg)) {
    kprintf("[virtio-blk] missing a required PCI capability (common/notify/"
            "device config) -- not a modern virtio device this driver "
            "understands\n");
    return false;
  }

  /* Standard status-bit handshake, spec 3.1.1. */
  common->device_status = 0;
  uint64_t reset_deadline = timer_uptime_ms() + VIRTIO_BLK_TIMEOUT_MS;
  while (common->device_status != 0) {
    if (timer_uptime_ms() > reset_deadline) {
      kprintf("[virtio-blk] device did not reset in time\n");
      return false;
    }
    asm volatile("pause");
  }
  common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
  common->device_status = (uint8_t)(common->device_status | VIRTIO_STATUS_DRIVER);

  if (!negotiate_features(common)) {
    common->device_status = VIRTIO_STATUS_FAILED;
    return false;
  }

  common->queue_select = 0;
  uint16_t qsize = common->queue_size;
  if (qsize == 0 || qsize > 4096) {
    /* 4096 is just a sanity ceiling against a hostile/broken device --
       real hardware reports something like 128 or 256. We never try
       to reduce a smaller, legal value -- see virtqueue_alloc()'s
       call site for why accepting it unchanged is the safer choice. */
    kprintf("[virtio-blk] queue 0 reports an unusable size (%u)\n", qsize);
    common->device_status = VIRTIO_STATUS_FAILED;
    return false;
  }

  if (!virtqueue_alloc(&g_vblk.vq, qsize)) {
    kprintf("[virtio-blk] out of memory allocating the virtqueue\n");
    common->device_status = VIRTIO_STATUS_FAILED;
    return false;
  }

  common->queue_desc = g_vblk.vq.desc_phys;
  common->queue_avail = g_vblk.vq.avail_phys;
  common->queue_used = g_vblk.vq.used_phys;
  common->queue_msix_vector = VIRTIO_MSI_NO_VECTOR; /* polling only -- see
                                                         the file header
                                                         comment */
  common->queue_enable = 1;

  g_vblk.notify_addr = (volatile uint16_t *)(notify_base +
      (uint64_t)common->queue_notify_off * notify_off_multiplier);

  common->device_status = (uint8_t)(common->device_status | VIRTIO_STATUS_DRIVER_OK);

  g_vblk.sector_count = device_cfg->capacity;

  uint64_t hdr_phys = pmm_alloc_page();
  uint64_t status_phys = pmm_alloc_page();
  if (hdr_phys == 0 || status_phys == 0) {
    kprintf("[virtio-blk] out of memory allocating request buffers\n");
    return false;
  }
  g_vblk.req_hdr = (struct virtio_blk_req_header *)phys_to_virt(hdr_phys);
  g_vblk.req_hdr_phys = hdr_phys;
  g_vblk.req_status = (uint8_t *)phys_to_virt(status_phys);
  g_vblk.req_status_phys = status_phys;

  g_vblk.ready = true;
  kprintf("[virtio-blk] ready: %lu sectors x %u bytes (vendor:device %04x:%04x)\n",
          g_vblk.sector_count, VIRTIO_BLK_SECTOR_SIZE, g_vblk.dev.vendor_id,
          g_vblk.dev.device_id);

  blockdev_register("virtio-blk", &virtio_blk_blockdev_ops); /* logs its own
    outcome; a false return just means something else already claimed the
    slot (e.g. NVMe init'd first) -- this driver is still fully usable via
    its own virtio_blk_*() API either way */
  return true;
}
