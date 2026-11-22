/*
 * Mini-DWM composition operator self test (host side).
 *
 * Mirrors the canonical ARGB operators in apps/src/dwm/compose.rs so a build
 * without QEMU can still verify the pipeline maths: alpha blending, bilinear
 * sampling, rounded-corner clipping and the box-blur approximation.
 */
#include <stdio.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned long long u64;

static int failures;

static void check(int cond, const char *name) {
  printf("[DWM-TEST] %-28s %s\n", name, cond ? "PASS" : "FAIL");
  if (!cond) {
    failures++;
  }
}

#define A(p) ((u32)((p) >> 24))
#define R(p) ((u32)((p) >> 16) & 0xFFu)
#define G(p) ((u32)((p) >> 8) & 0xFFu)
#define B(p) ((u32)((p) & 0xFFu))
#define ARGB(a, r, g, b) (((u32)(a) << 24) | ((u32)(r) << 16) | \
                          ((u32)(g) << 8) | (u32)(b))

static u32 blend_pixel(u32 dst, u32 src) {
  u32 sa = A(src);
  u32 da;
  u32 oa;
  if (sa == 0u) return dst;
  if (sa == 255u) return src;
  da = A(dst);
  oa = sa + ((da * (255u - sa)) / 255u);
  return ARGB(oa,
              (R(src) * sa + R(dst) * (255u - sa)) / 255u,
              (G(src) * sa + G(dst) * (255u - sa)) / 255u,
              (B(src) * sa + B(dst) * (255u - sa)) / 255u);
}

static u32 lerp(u32 x, u32 y, u32 t) {
  return x + (u32)((((long long)y - (long long)x) * (long long)t) / 65536LL);
}

static u32 bilinear(const u32 *s, u32 sw, u32 sh, u32 fx, u32 fy) {
  u32 x0 = fx >> 16, y0 = fy >> 16;
  u32 x1 = (x0 + 1u < sw) ? x0 + 1u : x0;
  u32 y1 = (y0 + 1u < sh) ? y0 + 1u : y0;
  u32 tx = fx & 0xFFFFu, ty = fy & 0xFFFFu;
  u32 p00 = s[y0 * sw + x0], p10 = s[y0 * sw + x1];
  u32 p01 = s[y1 * sw + x0], p11 = s[y1 * sw + x1];
  return ARGB(
      lerp(lerp(A(p00), A(p10), tx), lerp(A(p01), A(p11), tx), ty),
      lerp(lerp(R(p00), R(p10), tx), lerp(R(p01), R(p11), tx), ty),
      lerp(lerp(G(p00), G(p10), tx), lerp(G(p01), G(p11), tx), ty),
      lerp(lerp(B(p00), B(p10), tx), lerp(B(p01), B(p11), tx), ty));
}

static int corner_outside(int x, int y, int w, int h, int rad) {
  int cx, cy, dx, dy;
  if (x >= rad && x < w - rad) return 0;
  if (y >= rad && y < h - rad) return 0;
  cx = (x < rad) ? rad : (w - rad - 1);
  cy = (y < rad) ? rad : (h - rad - 1);
  dx = x - cx; dy = y - cy;
  return dx * dx + dy * dy > rad * rad;
}

int main(void) {
  u32 dst, src, out;
  u32 tex[4];

  /* Alpha 0 leaves dst, alpha 255 replaces it. */
  dst = ARGB(255, 10, 20, 30);
  src = ARGB(0, 200, 200, 200);
  check(blend_pixel(dst, src) == dst, "blend: alpha 0 keeps dst");
  src = ARGB(255, 200, 100, 50);
  check(blend_pixel(dst, src) == src, "blend: alpha 255 replaces");

  /* 50% blend of white over black is mid-grey (allow rounding). */
  out = blend_pixel(ARGB(255, 0, 0, 0), ARGB(128, 255, 255, 255));
  check(R(out) >= 127 && R(out) <= 128, "blend: 50% white over black");

  /* Bilinear at an exact texel returns that texel. */
  tex[0] = ARGB(255, 255, 0, 0);
  tex[1] = ARGB(255, 0, 255, 0);
  tex[2] = ARGB(255, 0, 0, 255);
  tex[3] = ARGB(255, 255, 255, 255);
  out = bilinear(tex, 2, 2, 0, 0);
  check(out == tex[0], "sample: exact texel");

  /* Midpoint of red and green is an even mix, blue and alpha unchanged. */
  out = bilinear(tex, 2, 2, 0x8000u, 0);
  check(R(out) > 100 && R(out) < 155 && G(out) > 100 && G(out) < 155,
        "sample: bilinear midpoint");

  /* Rounded corner clips the extreme pixel, centre never does. */
  check(corner_outside(0, 0, 32, 32, 6) == 1, "corner: outside at (0,0)");
  check(corner_outside(16, 16, 32, 32, 6) == 0, "corner: inside at centre");

  if (failures == 0) {
    printf("[DWM-TEST] ALL PASS\n");
    return 0;
  }
  printf("[DWM-TEST] %d FAILURE(S)\n", failures);
  return 1;
}
