#include <stdio.h>
#include <stdlib.h>
#include "../graphics/nvgui.c"
#include "../kernel/shared_mem.h"
#include "../fs/novusfs/novusfs.h"
#include "../graphics/nd_font_uni.h"
#include "../kernel/rtc.h"
#define g_ready wp_ready
#include "../graphics/nd_wallpaper.c"
#undef g_ready

#define REQUIRE(c) do { \
  if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } \
} while (0)

static shared_mem_t handoff;
static int failures;
static nd_event_t events[128];
static UINT32 head, tail;
static int font_present;
static void *font_buffer;

void *kmalloc(UINT64 size) { return calloc(1, (size_t)size); }
void kfree(void *ptr) { free(ptr); }
void *phys_to_virt(UINT64 phys) { return (void *)(UINTN)phys; }
shared_mem_t *shared_mem_get(void) { return &handoff; }
void kputs(const char *text) {
  fputs(text, stdout);
  if (text[0] == '[' && text[1] == 'N' && text[2] == 'D' &&
      text[3] == ']' && text[5] == 'F') failures++;
}
void kputs_hex(UINT64 value) { printf("0x%llx", value); }
int rtc_now(UINT8 *hour, UINT8 *minute) {
  *hour = 12; *minute = 0;
  return 1;
}
void nd_input_push(const nd_event_t *ev) {
  REQUIRE(head - tail < 128u);
  events[head++ % 128u] = *ev;
}
int nd_input_pop(nd_event_t *ev) {
  if (tail == head) return 0;
  *ev = events[tail++ % 128u];
  return 1;
}

UINT32 novusfs_lookup_path(novusfs_fs_t *fs, UINT32 ino, const char *path) {
  (void)fs; (void)ino; (void)path;
  return font_present ? 2u : 0u;
}
int novusfs_stat(novusfs_fs_t *fs, UINT32 ino, UINT64 *size, UINT16 *mode) {
  UINT32 i, count = 0;
  (void)fs; (void)ino;
  for (i = 0; i < 32; i++) {
    UINT8 bits = g_uni_valid[(g_uni_slot_count - 1u) * 32u + i];
    while (bits != 0) { count += bits & 1u; bits >>= 1; }
  }
  *size = ((UINT64)g_uni_base[g_uni_slot_count - 1] + count) * ND_UNI_GLYPH_BYTES;
  *mode = 0;
  return 0;
}
INT64 novusfs_read(novusfs_fs_t *fs, UINT32 ino, UINT64 off,
                   void *dst, UINT64 length) {
  (void)fs; (void)ino; (void)off;
  font_buffer = dst;
  memset(dst, 0xAA, length);
  return (INT64)length;
}

static void check_font_without_live_root(void) {
  novusfs_fs_t fs = {0};
  UINT32 wide;
  nd_font_set_data(NULL);
  REQUIRE(handoff.live_root_base == 0);
  REQUIRE(nd_font_load_from(&fs) == -1);
  REQUIRE(nd_glyph_find(0x4E2D, &wide) != NULL && wide == 16);
  REQUIRE(nd_glyph_find(0x10FFFF, &wide) == NULL);
  REQUIRE(nd_text_width("AB", 1) == 12);
  font_present = 1;
  /* The installed filesystem's font can load without dereferencing any
   * live-image physical addresses, even when none was staged. */
  REQUIRE(nd_font_load_from(&fs) == 0);
  REQUIRE(nd_glyph_find(0x4E2D, &wide) != NULL);
  nd_font_set_data(NULL);
  kfree(font_buffer);
}

static void check_metrics(void) {
  const char mixed[] = "A\xE4\xB8\xAD";
  nd_surface_t s;
  UINT32 expected[32 * 16];
  REQUIRE(nd_text_width(NULL, 1) == 0);
  REQUIRE(nd_text_width_n(NULL, 0, 1) == 0);
  REQUIRE(nd_text_width("AB", 1) == 12);
  REQUIRE(nd_text_width("AB", 2) == 24);
  REQUIRE(nd_text_width_n(mixed, 3, 1) == 6);
  REQUIRE(nd_text_width_n(mixed, 4, 1) == 22);
  REQUIRE(nd_text_height(mixed, 2) == 32);
  REQUIRE(nd_surface_create(&s, 32, 16) == 0);
  nd_text(&s, 0, 0, "A", ND_RGB(255, 255, 255), 1);
  memcpy(expected, s.px, sizeof(expected));
  nd_clear(&s, 0);
  nd_text_limit(&s, 0, 0, "AB", 11, ND_RGB(255, 255, 255), 1);
  REQUIRE(memcmp(expected, s.px, sizeof(expected)) == 0);
  nd_text_limit(&s, 0, 0, "AB", 12, ND_RGB(255, 255, 255), 1);
  REQUIRE(memcmp(expected, s.px, sizeof(expected)) != 0);
  nd_surface_destroy(&s);
}

static void check_icons(nd_surface_t *s) {
  UINT32 kind;
  for (kind = NG_ICON_FILES; kind <= NG_ICON_VOLUME; kind++) {
    UINT32 ink = 0;
    INT32 x, y;
    if (kind != NG_ICON_TERM && kind != NG_ICON_DEVICES &&
        kind != NG_ICON_FILES && kind != NG_ICON_VOLUME) continue;
    nd_clear(s, ND_RGB(0, 0, 0));
    icon_glyph(s, 32, 32, 24, (UINT8)kind, ND_RGB(255, 255, 255));
    for (y = 20; y < 44; y++)
      for (x = 20; x < 44; x++)
        if (nd_get_pixel(s, x, y) != ND_RGB(0, 0, 0)) ink++;
    REQUIRE(ink > 40 && ink < 300);
    REQUIRE(nd_get_pixel(s, 32, 32) == ND_RGB(0, 0, 0));
  }
}

static void check_placeholder_bounds(nd_surface_t *s) {
  char text[8] = "";
  ng_ctx_t *ctx = ng_init(s);
  ng_widget_t *edit;
  const nd_color_t background = ND_RGB(10, 20, 30);
  INT32 x, y;
  REQUIRE(ctx != NULL);
  edit = ng_textbox(ng_create(ctx->root, NG_TEXTBOX, 20, 20, 200, 34),
                    text, sizeof(text), "\xE6\x90\x9C\xE7\xB4\xA2");
  ng_layout();
  nd_clear(s, background);
  draw_textbox(edit);
  for (y = 54; y < 68; y++)
    for (x = 20; x < 220; x++)
      REQUIRE(nd_get_pixel(s, x, y) == background);
  ng_shutdown();
}

static void check_repaints(nd_surface_t *s) {
  UINT32 *reference = malloc((size_t)s->w * s->h * 4);
  ng_ctx_t *ctx = ng_init(s);
  ng_widget_t *win;
  INT32 y;

  REQUIRE(reference && ctx);
  win = ng_create(ctx->root, NG_WINDOW, 303, 242, 420, 250);
  ng_window(win, "AB", 1);
  ng_render_all();
  memcpy(reference, s->px, (UINT64)s->w * s->h * 4);
  nd_clear(s, ND_RGB(255, 255, 255));
  /* Scanline damage must reproduce a full frame, including shadow-only
   * rows and rounded edges, without any untouched white/black bands. */
  for (y = 0; y < s->h; y++) ng_render(nd_rect(0, y, s->w, 1));
  for (y = 0; y < s->w * s->h; y++) {
    if (reference[y] != s->px[y]) {
      fprintf(stderr, "repaint mismatch (%d,%d): %08x != %08x\n",
              y % s->w, y / s->w, reference[y], s->px[y]);
      break;
    }
  }
  REQUIRE(memcmp(reference, s->px, (UINT64)s->w * s->h * 4) == 0);
  {
    nd_event_t ev = {0};
    nd_rect_t damage;
    ev.type = ND_EV_MOUSE_DOWN;
    ev.button = ND_BUTTON_LEFT;
    ev.x = 315; ev.y = 250;
    ng_dispatch(&ev);
    ev.type = ND_EV_MOUSE_MOVE;
    ev.x += 20; ev.y += 15;
    ng_dispatch(&ev);
    ev.type = ND_EV_MOUSE_UP;
    ng_dispatch(&ev);
    REQUIRE(win->rect.x == 323 && win->rect.y == 257);
    REQUIRE(ng_take_damage(&damage));
    ng_render(damage);
    memcpy(reference, s->px, (UINT64)s->w * s->h * 4);
    ng_render_all();
    REQUIRE(memcmp(reference, s->px, (UINT64)s->w * s->h * 4) == 0);
  }
  ng_show(win, 0);
  {
    nd_rect_t damage;
    REQUIRE(ng_take_damage(&damage));
    ng_render(damage);
  }
  memcpy(reference, s->px, (UINT64)s->w * s->h * 4);
  ng_render_all();
  REQUIRE(memcmp(reference, s->px, (UINT64)s->w * s->h * 4) == 0);
  ng_shutdown();
  free(reference);
}

static void check_wallpaper(void) {
  nd_surface_t src, dst;
  UINT32 x, y;
  REQUIRE(nd_surface_create(&src, 8, 4) == 0);
  REQUIRE(nd_surface_create(&dst, 4, 4) == 0);
  for (y = 0; y < 4; y++)
    for (x = 0; x < 8; x++)
      src.px[y * 8 + x] = ND_RGB(x * 30, y * 60, 10);
  wp_cover(&dst, &src);
  for (y = 0; y < 4; y++)
    for (x = 0; x < 4; x++)
      REQUIRE(dst.px[y * 4 + x] == src.px[y * 8 + x + 2]);
  g_img = dst;
  wp_ready = 1;
  {
    UINT32 average = nd_wallpaper_luma(nd_rect(0, 0, 4, 4), 0);
    REQUIRE(average > 50 && average < 150);
    REQUIRE(nd_wallpaper_luma(nd_rect(-10, -10, 2, 2),
                             ND_RGB(255, 255, 255)) == 255);
  }
  nd_clear(&src, ND_RGB(0, 0, 0));
  nd_surface_set_clip(&src, nd_rect(1, 1, 2, 2));
  {
    nd_rect_t area = nd_rect(0, 0, 4, 4);
    nd_wallpaper_paint(&src, &area, 0, 0);
  }
  REQUIRE(src.px[9] == dst.px[5]);
  REQUIRE(src.px[0] == ND_RGB(0, 0, 0));
  wp_ready = 0;
  memset(&g_img, 0, sizeof(g_img));
  nd_surface_destroy(&src);
  nd_surface_destroy(&dst);
  REQUIRE(nd_surface_create(&src, 1, 1) == 0);
  REQUIRE(nd_surface_create(&dst, 3, 5) == 0);
  src.px[0] = ND_RGB(12, 34, 56);
  wp_cover(&dst, &src);
  for (x = 0; x < 15; x++) REQUIRE(dst.px[x] == src.px[0]);
  nd_surface_destroy(&src);
  nd_surface_destroy(&dst);
}

static void put_le32(UINT8 *p, UINT32 value) {
  UINT32 i;
  for (i = 0; i < 4; i++) p[i] = (UINT8)(value >> (i * 8));
}

static void check_staged_wallpaper(const UINT8 *jpeg, UINT32 length) {
  UINT32 offset = WPKG_HEADER_SIZE + 4 * WPKG_ENTRY_SIZE;
  UINT32 total = offset + length;
  UINT8 *package = calloc(1, total);
  const UINT8 *data;
  UINT32 size;
  UINT32 *previous;
  nd_surface_t surface;
  nd_rect_t area = nd_rect(0, 0, 1024, 768);

  REQUIRE(package != NULL);
  memcpy(package, "NWPK", 4);
  put_le32(package + WPKG_OFF_VERSION, WPKG_VERSION);
  put_le32(package + WPKG_OFF_COUNT, 4);
  put_le32(package + WPKG_OFF_TOTAL, total);
  put_le32(package + WPKG_HEADER_SIZE + WPKG_EOFF_OFFSET, offset);
  put_le32(package + WPKG_HEADER_SIZE + WPKG_EOFF_SIZE, length);
  memcpy(package + offset, jpeg, length);
  handoff.wpkg_img_base = (UINT64)(UINTN)package;
  handoff.wpkg_img_size = total;
  REQUIRE(nd_wallpaper_init(area.w, area.h) == 0);
  REQUIRE(nd_wallpaper_ready());
  REQUIRE(nd_surface_create(&surface, area.w, area.h) == 0);
  nd_wallpaper_paint(&surface, &area, 0, 0);
  REQUIRE(memcmp(surface.px, g_img.px, (size_t)area.w * area.h * 4) == 0);
  check_repaints(&surface);
  previous = g_img.px;
  REQUIRE(nd_wallpaper_set(1) == -1);
  REQUIRE(nd_wallpaper_current() == 0);
  REQUIRE(nd_wallpaper_set(1) == -1);
  REQUIRE(g_img.px == previous && nd_wallpaper_ready());
  put_le32(package + WPKG_HEADER_SIZE + WPKG_ENTRY_SIZE + WPKG_EOFF_OFFSET, offset);
  put_le32(package + WPKG_HEADER_SIZE + WPKG_ENTRY_SIZE + WPKG_EOFF_SIZE, length);
  REQUIRE(nd_wallpaper_set(1) == 0);
  REQUIRE(g_img.px != previous);
  put_le32(package + WPKG_OFF_TOTAL, WPKG_HEADER_SIZE);
  /* The active and previous photo are usable even if decoding is now
   * impossible: switching back must use the cached scaled surface. */
  REQUIRE(nd_wallpaper_set(0) == 0);
  REQUIRE(g_img.px == previous);
  REQUIRE(wp_locate(0, &data, &size) == -1);
  REQUIRE(data == NULL && size == 0);
  put_le32(package + WPKG_OFF_TOTAL, total);
  for (size = 2; size < 4; size++) {
    UINT8 *entry = package + WPKG_HEADER_SIZE + size * WPKG_ENTRY_SIZE;
    put_le32(entry + WPKG_EOFF_OFFSET, offset);
    put_le32(entry + WPKG_EOFF_SIZE, length);
    REQUIRE(nd_wallpaper_set(size) == 0);
  }
  for (size = 0; size < WP_CACHE_SIZE; size++) {
    REQUIRE(g_cache[size].image.px != NULL);
    REQUIRE(g_cache[size].index != 1);
  }
  REQUIRE(nd_wallpaper_set(0) == 0);
  REQUIRE(g_img.px == previous);
  nd_surface_destroy(&surface);
  handoff.wpkg_img_base = handoff.wpkg_img_size = 0;
  REQUIRE(nd_wallpaper_init(800, 600) == -1);
  REQUIRE(!nd_wallpaper_ready());
  REQUIRE(nd_wallpaper_luma(area, ND_RGB(0, 0, 0)) == 0);
  for (size = 0; size < WP_CACHE_SIZE; size++) REQUIRE(g_cache[size].image.px == NULL);
  free(package);
  puts("[TEST] staged package, photo repaint and failed selection rollback PASS");
}

int main(int argc, char **argv) {
  nd_surface_t surface;
  nd_selftest();
  REQUIRE(failures == 0);
  REQUIRE(ng_ctx_get() == NULL);
  REQUIRE(head == tail);
  check_metrics();
  check_font_without_live_root();
  REQUIRE(nd_surface_create(&surface, 1024, 768) == 0);
  check_icons(&surface);
  check_placeholder_bounds(&surface);
  check_repaints(&surface);
  check_wallpaper();
  nd_surface_destroy(&surface);
  if (argc == 3) {
    FILE *input = fopen(argv[1], "rb");
    FILE *output;
    long length;
    UINT8 *bytes;
    REQUIRE(input != NULL);
    REQUIRE(fseek(input, 0, SEEK_END) == 0);
    length = ftell(input);
    REQUIRE(length > 0);
    rewind(input);
    bytes = malloc((size_t)length);
    REQUIRE(bytes != NULL);
    REQUIRE(fread(bytes, 1, (size_t)length, input) == (size_t)length);
    fclose(input);
    REQUIRE(nd_jpeg_decode(bytes, (UINT32)length, 1, &surface) == 0);
    output = fopen(argv[2], "wb");
    REQUIRE(output != NULL);
    REQUIRE(fwrite(surface.px, 4, (size_t)surface.w * surface.h, output) ==
            (size_t)surface.w * surface.h);
    fclose(output);
    check_staged_wallpaper(bytes, (UINT32)length);
    free(bytes);
    nd_surface_destroy(&surface);
  }
  puts("[TEST] display metrics, icons, cleanup and scanline repaint PASS");
  return 0;
}
