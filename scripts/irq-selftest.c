#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "nd.h"

#define NOVUS_CPU_H
#define NOVUS_IRQ_H
#include "uefi.h"

#define IRQ_FIRST_VECTOR 33u
#define IRQ_LAST_VECTOR 254u
#define IRQ_SPURIOUS_VECTOR 255u
#define IRQ_MAX_LINES 256u
#define IRQ_LOW 1u
#define IRQ_LEVEL 2u
typedef void (*irq_handler_t)(void *ctx);
typedef struct {
  UINT32 gsi, vector, flags, handlers;
  UINT64 count, unhandled;
  const char *name;
} irq_info_t;

static UINT64 test_if;
static UINT32 eoi_count;
static UINT32 io_reads;
static UINT8 port_data[512];
static UINT8 port_aux[512];
static UINT32 port_head, port_tail;
static UINT8 serial_ier;
static UINT8 tx_data[4096];
static UINT32 tx_count;
static UINT32 mmio[8];
static nd_display_t display;

static UINT64 irq_save(void) {
  UINT64 saved = test_if;
  test_if = 0;
  return saved;
}
static void irq_restore(UINT64 flags) { test_if = flags; }
static UINT8 io_in8(UINT16 port) {
  io_reads++;
  if (port == 0x64u) {
    return port_tail < port_head ? (UINT8)(1u | port_aux[port_tail]) : 0u;
  }
  if (port == 0x60u) {
    return port_data[port_tail++];
  }
  if (port == 0x3FAu) {
    return port_tail < port_head ? 4u : ((serial_ier & 2u) ? 2u : 1u);
  }
  if (port == 0x3FDu) {
    return (UINT8)(0x20u | (port_tail < port_head ? 1u : 0u));
  }
  if (port == 0x3F8u) {
    return port_data[port_tail++];
  }
  return 0u;
}
static void io_out8(UINT16 port, UINT8 value) {
  if (port == 0x3F9u) {
    serial_ier = value;
  } else if (port == 0x3F8u) {
    tx_data[tx_count++] = value;
  }
}
UINT64 lapic_base(void) { return 0xFEE00000u; }
UINT32 lapic_read(UINT64 base, UINT32 reg) {
  (void)base;
  (void)reg;
  return 0;
}
void lapic_write(UINT64 base, UINT32 reg, UINT32 value) {
  (void)base;
  (void)value;
  eoi_count += reg == 0xB0u;
}
void kputs(const char *text) { fputs(text, stdout); }
void kputs_hex(UINT64 value) { printf("%llx", (unsigned long long)value); }
const nd_display_t *nd_display(void) { return &display; }

#include "../kernel/irq.c"
#include "../graphics/nd_input.c"
#include "../kernel/serial.c"
#include "../kernel/net_e1000.c"

static UINT32 checks;
#define CHECK(condition) do { \
  checks++; \
  if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    exit(1); \
  } \
} while (0)

static void counted(void *ctx) { (*(UINT32 *)ctx)++; }

static UINT32 probe_regs[0x6000u / 4u];
static UINT8 probe_pages[TX_COUNT + RX_COUNT + 2u][4096]
    __attribute__((aligned(4096)));
static UINT32 allocated, released, fail_after;

int pci_find(UINT16 vendor, UINT16 device, pci_dev_t *out) {
  (void)vendor;
  (void)device;
  memset(out, 0, sizeof(*out));
  out->irq = 20u;
  out->bar[0] = (UINT64)(UINTN)probe_regs;
  return 1;
}
void pci_enable_device(const pci_dev_t *dev) { (void)dev; }
UINT16 pci_read16(UINT8 bus, UINT8 dev, UINT8 fn, UINT8 off) {
  (void)bus; (void)dev; (void)fn; (void)off;
  return 0;
}
void pci_write32(UINT8 bus, UINT8 dev, UINT8 fn, UINT8 off, UINT32 value) {
  (void)bus; (void)dev; (void)fn; (void)off; (void)value;
}
void *pmm_alloc_page(void) {
  if (allocated >= fail_after || allocated >= TX_COUNT + RX_COUNT + 2u) {
    return NULL;
  }
  return probe_pages[allocated++];
}
void *pmm_alloc_contig(UINT32 count) { return count == 1u ? pmm_alloc_page() : NULL; }
void pmm_free_page(void *page) { released += page != NULL; }
void *phys_to_virt(UINT64 phys) { return (void *)(UINTN)phys; }
void net_register(netdev_t *dev) { (void)dev; }

static void test_irq(void) {
  UINT8 table[66];
  acpi_header_t *header = (acpi_header_t *)table;
  UINT32 first = 0, base = 0xFEC00000u, override = 19u;
  UINT16 attrs = 15u;
  UINT32 calls = 0, other = 0;
  irq_info_t info;
  int vector;

  memset(table, 0, sizeof(table));
  header->length = sizeof(table);
  table[44] = 1u;
  table[45] = 12u;
  memcpy(table + 48, &base, 4);
  memcpy(table + 52, &first, 4);
  table[56] = 2u;
  table[57] = 10u;
  table[59] = 9u;
  memcpy(table + 60, &override, 4);
  memcpy(table + 64, &attrs, 2);
  for (first = 0; first < 16u; first++) {
    g_isa_gsi[first] = first;
  }
  CHECK(parse_madt(header));
  CHECK(g_ioapic_count == 1u && g_isa_gsi[9] == 19u);
  table[57] = 0;
  CHECK(!parse_madt(header));
  CHECK(g_isa_gsi[9] == 19u);
  table[57] = 10u;
  table[64] = 2u;
  CHECK(!parse_madt(header));
  CHECK(checksum_ok("\x01\xFF", 2));
  CHECK(!checksum_ok("\x01\xFE", 2));
  CHECK(!phys_range(0xFFFFFFF0ULL, 32));

  g_ioapic[0].base = (UINT32)(UINTN)mmio;
  CHECK((UINTN)mmio <= 0xFFFFFFFFu);
  g_ioapic[0].first = 0;
  g_ioapic[0].pins = 24u;
  g_ready = 1;
  g_next_vector = IRQ_FIRST_VECTOR;
  memset(g_vector_gsi, 0xFF, sizeof(g_vector_gsi));
  test_if = 0x200u;
  vector = irq_register(9u, 0u, counted, &calls, "test-a");
  CHECK(vector == 33 && test_if == 0x200u);
  CHECK(irq_register(19u, IRQ_LOW | IRQ_LEVEL, counted, &other, "test-b") == vector);
  CHECK(irq_register(19u, 0u, counted, &other, "bad-flags") == -1);
  CHECK(irq_query(9u, &info) == 0 && info.gsi == 19u && info.handlers == 2u);
  CHECK(info.flags == (IRQ_LOW | IRQ_LEVEL));
  test_if = 0;
  irq_dispatch((UINT32)vector);
  CHECK(calls == 1u && other == 1u && eoi_count == 1u && test_if == 0u);
  irq_dispatch(IRQ_SPURIOUS_VECTOR);
  CHECK(eoi_count == 1u);
  CHECK(irq_unregister(9u, counted, &calls) == 0);
  irq_dispatch((UINT32)vector);
  CHECK(calls == 1u && other == 2u && eoi_count == 2u);
  CHECK(irq_unregister(19u, counted, &other) == 0);
  CHECK((mmio[4] & IOAPIC_MASK) != 0u);
  irq_dispatch((UINT32)vector);
  CHECK(irq_query(9u, &info) == 0 && info.count == 3u && info.unhandled == 1u);
  CHECK(irq_register(19u, IRQ_LOW | IRQ_LEVEL, counted, &calls, "again") == vector);
  CHECK(irq_register(99u, 0, counted, &calls, "invalid") == -1);
}

static void test_ps2(void) {
  nd_event_t event;
  UINT32 before;
  UINT32 index;

  memset(&display, 0, sizeof(display));
  g_mx = 100;
  g_my = 100;
  port_head = 5;
  port_tail = 0;
  port_data[0] = 0x1Eu;
  port_data[1] = 0x9Eu;
  port_data[2] = 0x08u;
  port_data[3] = 5u;
  port_data[4] = 2u;
  port_aux[2] = port_aux[3] = port_aux[4] = 0x20u;
  before = io_reads;
  nd_input_poll();
  CHECK(io_reads == before && !nd_input_pop(&event));
  ps2_irq(NULL);
  CHECK(port_tail == port_head && g_raw_head == 5u);
  CHECK(!nd_input_pop(&event));
  before = io_reads;
  nd_input_poll();
  CHECK(io_reads == before);
  CHECK(nd_input_pop(&event) && event.type == ND_EV_KEY && event.key == 'a');
  CHECK(nd_input_pop(&event) && event.type == ND_EV_MOUSE_MOVE);
  CHECK(event.x == 105 && event.y == 98 && !nd_input_pop(&event));
  g_raw_head = ND_RAW_QUEUE - 1u;
  g_raw_tail = 0;
  port_head = 1;
  port_tail = 0;
  ps2_irq(NULL);
  CHECK(g_raw_dropped == 1u && port_tail == 1u);
  for (index = 0; index < ND_EVENT_QUEUE + 8u; index++) {
    nd_input_push(&event);
  }
  CHECK((g_head + 1u) % ND_EVENT_QUEUE == g_tail);
}

static void test_serial(void) {
  port_head = 2;
  port_tail = 0;
  port_data[0] = 'R';
  port_data[1] = 'X';
  serial_ier = 5u;
  serial_irq(NULL);
  CHECK(serial_getc() == 'R' && serial_getc() == 'X' && serial_getc() == -1);
  tx_count = 0;
  g_irq_enabled = 1;
  g_tx_head = 2;
  g_tx_tail = 0;
  g_tx[0] = 'T';
  g_tx[1] = 'X';
  serial_ier = 7u;
  serial_irq(NULL);
  CHECK(tx_count == 2u && tx_data[0] == 'T' && tx_data[1] == 'X');
  CHECK(serial_ier == 5u);
  test_if = 0x200u;
  serial_putc('!');
  CHECK(tx_count == 3u && tx_data[2] == '!' && test_if == 0x200u);
  g_tx[g_tx_head] = 'A';
  g_tx_head = (g_tx_head + 1u) % SERIAL_QUEUE;
  test_if = 0;
  serial_putc('B');
  CHECK(tx_count == 5u && tx_data[3] == 'A' && tx_data[4] == 'B');
}

static void test_e1000(void) {
  static UINT32 regs[0x6000u / 4u];
  static e1000_desc_t rx[RX_COUNT];
  static UINT8 frame[BUF_SIZE];
  UINT8 out[BUF_SIZE];
  UINT32 length = 0;
  netdev_t dev;
  e1000_t *card = &g_card;

  memset(card, 0, sizeof(*card));
  memset(&dev, 0, sizeof(dev));
  dev.priv = card;
  card->base = (UINT8 *)regs;
  card->rx = rx;
  card->rx_buf[0] = frame;
  memcpy(frame, "irq-frame", 9);
  rx[0].status = 3;
  rx[0].len = 9;
  CHECK(e1000_poll(&dev, out, sizeof(out), &length) == 0);
  CHECK(rx[0].status == 3u);
  regs[E1000_ICR / 4u] = 1u << 7;
  e1000_irq(card);
  CHECK(card->irq_rx == 1u && card->irq_count == 1u && rx[0].status == 0u);
  CHECK(e1000_poll(&dev, out, sizeof(out), &length) == 1);
  CHECK(length == 9u && memcmp(out, "irq-frame", 9) == 0);
  CHECK(e1000_poll(&dev, out, sizeof(out), &length) == 0);
  rx[1].status = 3u;
  rx[1].len = BUF_SIZE + 1u;
  e1000_irq(card);
  CHECK(card->rx_dropped == 1u && rx[1].status == 0u);
  card->queue_head = RX_QUEUE - 1u;
  card->queue_tail = 0;
  rx[2].status = 3u;
  rx[2].len = 9u;
  e1000_irq(card);
  CHECK(card->rx_dropped == 2u && rx[2].status == 0u);
}

static void test_probe_failures(void) {
  UINT32 calls = 0;

  allocated = released = 0;
  fail_after = TX_COUNT + RX_COUNT + 2u;
  g_ready = 0;
  CHECK(net_e1000_probe() == -1);
  CHECK(allocated == 0u);
  g_ready = 1;
  fail_after = 3u;
  CHECK(net_e1000_probe() == -1);
  CHECK(allocated == 3u && released == 3u && g_card.base == NULL);
  allocated = released = 0;
  fail_after = TX_COUNT + RX_COUNT + 2u;
  CHECK(irq_register(20u, 0u, counted, &calls, "conflict") >= 0);
  CHECK(net_e1000_probe() == -1);
  CHECK(allocated == TX_COUNT + RX_COUNT + 2u && released == allocated);
  CHECK(irq_unregister(20u, counted, &calls) == 0);
}

int main(void) {
  test_irq();
  test_ps2();
  test_serial();
  test_e1000();
  test_probe_failures();
  printf("IRQ/PS2/COM1/E1000: %u checks PASS\n", checks);
  return 0;
}