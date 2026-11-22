//! Composition pipeline with a pluggable backend.
//!
//! Only the primitive operators a desktop compositor needs are implemented:
//! texture sampling, alpha blending, bilinear zoom, a separable box-blur
//! approximation of a Gaussian, and a colour-space (gamma/sRGB) adjustment.
//! A vendor GPU backend can replace `Ops` without touching the layer walk.
//!
//! The default software backend operates on canonical 0xAARRGGBB words in a
//! mapped vidmem framebuffer. All loops are bounded by the frame dimensions;
//! there is no allocation and no dependency beyond core.

/// Backend operations table. `sample` fetches a source pixel, `blend`
/// composites it over a destination, and the remaining operators transform a
/// rectangle in place. A hardware driver supplies its own `Ops` instance.
pub struct Ops {
    pub name: &'static str,
}

pub const SOFT_OPS: Ops = Ops { name: "software" };

#[inline]
fn a(px: u32) -> u32 { px >> 24 }
#[inline]
fn r(px: u32) -> u32 { (px >> 16) & 0xFF }
#[inline]
fn g(px: u32) -> u32 { (px >> 8) & 0xFF }
#[inline]
fn b(px: u32) -> u32 { px & 0xFF }
#[inline]
fn argb(a8: u32, r8: u32, g8: u32, b8: u32) -> u32 {
    (a8 << 24) | (r8 << 16) | (g8 << 8) | b8
}

/// Alpha-blend one source pixel over a destination using the source alpha.
#[inline]
pub fn blend_pixel(dst: u32, src: u32) -> u32 {
    let sa = a(src);
    if sa == 0 {
        return dst;
    }
    if sa == 255 {
        return src;
    }
    let da = a(dst);
    let out_a = sa + ((da * (255 - sa)) / 255);
    let mix = |s: u32, d: u32| -> u32 { (s * sa + d * (255 - sa)) / 255 };
    argb(out_a, mix(r(src), r(dst)), mix(g(src), g(dst)), mix(b(src), b(dst)))
}

/// Sample a source ARGB surface at fractional source coordinates with
/// bilinear filtering. Coordinates are in 16.16 fixed point.
#[inline]
pub fn sample_bilinear(src: &[u32], sw: u32, sh: u32, fx: u32, fy: u32) -> u32 {
    if sw == 0 || sh == 0 || src.len() < (sw * sh) as usize {
        return 0;
    }
    let x0 = (fx >> 16) as u32;
    let y0 = (fy >> 16) as u32;
    let x1 = if x0 + 1 < sw { x0 + 1 } else { x0 };
    let y1 = if y0 + 1 < sh { y0 + 1 } else { y0 };
    let tx = (fx & 0xFFFF) as u32;
    let ty = (fy & 0xFFFF) as u32;
    let p00 = src[(y0 * sw + x0) as usize];
    let p10 = src[(y0 * sw + x1) as usize];
    let p01 = src[(y1 * sw + x0) as usize];
    let p11 = src[(y1 * sw + x1) as usize];
    let lerp = |x: u32, y: u32, t: u32| -> u32 { x + (((y as i64 - x as i64) * t as i64 / 65536) as i64) as u32 };
    argb(
        lerp(lerp(a(p00), a(p10), tx), lerp(a(p01), a(p11), tx), ty),
        lerp(lerp(r(p00), r(p10), tx), lerp(r(p01), r(p11), tx), ty),
        lerp(lerp(g(p00), g(p10), tx), lerp(g(p01), g(p11), tx), ty),
        lerp(lerp(b(p00), b(p10), tx), lerp(b(p01), b(p11), tx), ty),
    )
}

/// Blit a source layer into a destination frame with alpha blending and a
/// scale from source (sw×sh) to destination rectangle (dw×dh). Pixels outside
/// the destination bounds are clipped. `radius` rounds the corners.
pub fn blit_scaled(
    ops: &Ops,
    dst: &mut [u32],
    dw: u32,
    dh: u32,
    src: &[u32],
    sw: u32,
    sh: u32,
    dx: i32,
    dy: i32,
    dwidth: u32,
    dheight: u32,
    alpha: u8,
    radius: u8,
) {
    let _ = ops; // the software backend is selected by this module's functions
    if sw == 0 || sh == 0 || dwidth == 0 || dheight == 0 {
        return;
    }
    let x0 = if dx < 0 { 0u32 } else { dx as u32 };
    let y0 = if dy < 0 { 0u32 } else { dy as u32 };
    let x1 = core::cmp::min(dw, (dx as i64 + dwidth as i64) as u32);
    let y1 = core::cmp::min(dh, (dy as i64 + dheight as i64) as u32);
    if x0 >= x1 || y0 >= y1 {
        return;
    }
    let sx_step = ((sw as u64 * 65536) / dwidth as u64) as u32;
    let sy_step = ((sh as u64 * 65536) / dheight as u64) as u32;
    let rad = radius as i32;

    for y in y0..y1 {
        let sy = ((y as i64 - dy as i64) as u64 * sy_step as u64) as u32;
        for x in x0..x1 {
            if rad > 0 && corner_outside(x as i32 - dx, y as i32 - dy,
                                         dwidth as i32, dheight as i32, rad) {
                continue;
            }
            let sx = ((x as i64 - dx as i64) as u64 * sx_step as u64) as u32;
            let mut px = sample_bilinear(src, sw, sh, sx, sy);
            if alpha != 255 {
                px = (px & 0x00FFFFFF) | ((alpha as u32) << 24);
            }
            let di = (y * dw + x) as usize;
            if di < dst.len() {
                dst[di] = blend_pixel(dst[di], px);
            }
        }
    }
}

/// True when (x,y) falls in a rounded rectangle's cut-out corner.
fn corner_outside(x: i32, y: i32, w: i32, h: i32, rad: i32) -> bool {
    let cx = if x < rad { rad } else if x >= w - rad { w - rad - 1 } else { return false };
    let cy = if y < rad { rad } else if y >= h - rad { h - rad - 1 } else { return false };
    let dx = x - cx;
    let dy = y - cy;
    dx * dx + dy * dy > rad * rad
}

/// Separable box blur, two passes, approximating a Gaussian. Used for the
/// acrylic/mica background behind translucent layers.
pub fn box_blur(buf: &mut [u32], w: u32, h: u32, tmp: &mut [u32], radius: u32) {
    if w == 0 || h == 0 || radius == 0 {
        return;
    }
    let len = (w * h) as usize;
    if buf.len() < len || tmp.len() < len {
        return;
    }
    let n = radius * 2 + 1;
    // Horizontal pass.
    for y in 0..h {
        for x in 0..w {
            let mut ar = 0u64; let mut ag = 0u64; let mut ab = 0u64; let mut aa = 0u64;
            let xs = if x > radius { x - radius } else { 0 };
            let xe = core::cmp::min(w - 1, x + radius);
            for xx in xs..=xe {
                let p = buf[(y * w + xx) as usize];
                ar += r(p) as u64; ag += g(p) as u64; ab += b(p) as u64; aa += a(p) as u64;
            }
            let cnt = (xe - xs + 1) as u64;
            tmp[(y * w + x) as usize] = argb(
                (aa / cnt) as u32, (ar / cnt) as u32,
                (ag / cnt) as u32, (ab / cnt) as u32);
        }
    }
    // Vertical pass.
    for y in 0..h {
        for x in 0..w {
            let mut ar = 0u64; let mut ag = 0u64; let mut ab = 0u64; let mut aa = 0u64;
            let ys = if y > radius { y - radius } else { 0 };
            let ye = core::cmp::min(h - 1, y + radius);
            for yy in ys..=ye {
                let p = tmp[(yy * w + x) as usize];
                ar += r(p) as u64; ag += g(p) as u64; ab += b(p) as u64; aa += a(p) as u64;
            }
            let cnt = (ye - ys + 1) as u64;
            buf[(y * w + x) as usize] = argb(
                (aa / cnt) as u32, (ar / cnt) as u32,
                (ag / cnt) as u32, (ab / cnt) as u32);
        }
    }
    let _ = n;
}

/// Colour-space adjustment: a simple sRGB gamma lift/stretch on RGB while
/// preserving alpha. `gain` is 1000 = identity.
pub fn color_adjust(buf: &mut [u32], gain: u32) {
    if gain == 1000 {
        return;
    }
    for p in buf.iter_mut() {
        let adj = |c: u32| -> u32 {
            let v = (c as u64 * gain as u64) / 1000u64;
            core::cmp::min(255u64, v) as u32
        };
        *p = argb(a(*p), adj(r(*p)), adj(g(*p)), adj(b(*p)));
    }
}
