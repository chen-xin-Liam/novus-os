//! Novus OS - Mini-DWM user-space compositor process (dwm.exe equivalent).
//!
//! Launched by the kernel when CONFIG_NOVUS_DWM is on. It owns the window
//! tree, redirects surfaces (Flip/Blt), composites layers into a frame with
//! the software pipeline, and presents on VSync through the display syscalls.
//! The in-kernel Novus-WM keeps drawing the desktop so a compositor that fails
//! to start cannot blank the screen.
//!
//! Layer updates arrive on the well-known "dwm" port as `#[repr(C)]`
//! `LayerMsg` payloads. The kernel WM forwards v2 surface/flip messages there;
//! a missing port simply leaves the compositor presenting its own backdrop.
//!
//! All frame buffers are vidmem objects mapped at fixed low-half VAs, never
//! large stack arrays: a NUC stack is capped at 4 MiB while a 1280x720 frame
//! pair alone would need ~7 MiB.

#![no_std]
#![no_main]

use novus_apps::dwm::compose::{blit_scaled, color_adjust, SOFT_OPS};
use novus_apps::dwm::present::PresentQueue;
use novus_apps::dwm::tree::{Layer, Tree, DPI_SCALE_100};
use novus_apps::{ipc, sys, sys::DwmLayer};

pub const PORT_NAME: &[u8] = b"dwm\0";

/* Messages the WM/apps send to the compositor port. */
pub const MSG_LAYER_PUT: u16 = 0x0301;
pub const MSG_LAYER_DEL: u16 = 0x0302;
pub const MSG_SET_DPI: u16 = 0x0303;
pub const MSG_SET_MODE: u16 = 0x0304;
pub const MSG_PING: u16 = 0x0305;

/* Fixed mapping windows. The NUC image sits at 0x400000, the trampoline at
 * 0x90000000 and the stack top at 0xA0000000, so these are free low-half
 * regions. Each layer slot reserves 16 MiB (the vidmem object cap).
 *
 * Two frame slots make the compositor double buffered: while the CRTC owns
 * the front object, the next frame is composed into the back object. The
 * slots are 4 MiB apart - larger than the 1280x720 frame cap (3.6 MiB) but
 * inside the same low-half region, away from the addresses the loader and
 * the kernel reserve for their own mappings. */
const FRAME_VA: u64 = 0x0400_0000;
const FRAME_B_VA: u64 = 0x0440_0000;
const LAYER_VA_BASE: u64 = 0x1000_0000;
const LAYER_VA_STRIDE: u64 = 0x0100_0000;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LayerMsg {
    pub key: u32,
    pub handle: u32,
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
    pub alpha: u8,
    pub radius: u8,
    pub z: u8,
    pub flags: u8,
    pub pad: u32,
}

fn log(s: &[u8]) { sys::console_write(s); }

fn hex32(out: &mut [u8; 8], v: u32) -> &[u8] {
    let mut x = v;
    let mut i = out.len();
    loop {
        i -= 1;
        let d = (x & 0xF) as u8;
        // B-48: digits above 9 were written as '0'+d, which lands on ':'..'?'
        // and turned every value with an A-F nibble into punctuation
        // ("primary w=400 h=2=0" instead of 0x300).
        out[i] = if d < 10 { b'0' + d } else { b'a' + (d - 10) };
        x >>= 4;
        if x == 0 { break; }
    }
    &out[i..]
}

/// Maps a source vidmem handle into a stable slot so the compositor can read
/// its pixels. A handle maps to one slot by its low bits; remapping replaces
/// the previous occupant and drops only the compositor's mapping reference.
struct LayerMaps {
    slots: [(u32, u64); 16],
}

impl LayerMaps {
    fn new() -> Self { LayerMaps { slots: [(0, 0); 16] } }

    fn ensure(&mut self, h: u32, size: u32) -> Option<u64> {
        if h == 0 { return None; }
        let i = (h as usize) & 15;
        if self.slots[i].0 == h { return Some(self.slots[i].1); }
        let va = LAYER_VA_BASE + (i as u64) * LAYER_VA_STRIDE;
        if self.slots[i].0 != 0 {
            let _ = sys::vmem_free(self.slots[i].0);
            self.slots[i] = (0, 0);
        }
        if sys::vmem_map(h, va, sys::VM_PROT_WRITE | sys::VM_PROT_USER) < 0 {
            return None;
        }
        self.slots[i] = (h, va);
        let _ = size;
        Some(va)
    }

    fn pixels(&self, va: u64, words: usize) -> &[u32] {
        unsafe { core::slice::from_raw_parts(va as *const u32, words) }
    }
}

/// Compose one frame from the tree into the mapped front buffer.
///
/// Returns `None` when there is nothing to composite. That case is critical:
/// the in-kernel Novus-WM still owns and paints the desktop, so an empty v2
/// tree must produce NO present at all. Presenting a cleared (dark) full-screen
/// frame in that state is what blanked the screen on every refresh.
fn compose_frame(
    tree: &Tree,
    frame: &mut [u32],
    fw: u32,
    fh: u32,
    dpi: u32,
    maps: &mut LayerMaps,
) -> Option<DwmLayer> {
    let mut layers = [DwmLayer::empty(); 16];
    let n = tree.collect(&mut layers, dpi);
    if n == 0 {
        return None;
    }

    // Only now, with real layers to draw, take over the framebuffer.
    for p in frame.iter_mut() { *p = 0xFF0E1116u32; }

    let words = (fw as usize) * (fh as usize);
    let mut drawn = 0usize;
    for l in layers.iter().take(n) {
        let sw = l.w;
        let sh = l.h;
        if sw == 0 || sh == 0 || (sw as usize) * (sh as usize) > words {
            continue;
        }
        if let Some(va) = maps.ensure(l.handle, sw * sh * 4) {
            let src = maps.pixels(va, (sw as usize) * (sh as usize));
            blit_scaled(
                &SOFT_OPS, frame, fw, fh, src, sw, sh,
                l.x, l.y, l.w, l.h, 255, 0,
            );
            drawn += 1;
        }
    }
    if drawn == 0 {
        return None;
    }

    // Colour-space adjustment is a no-op at unity gain; kept in the pipeline
    // so a backends/HDR path can hook it without changing this loop.
    color_adjust(frame, 1000);

    Some(DwmLayer {
        handle: 0, // filled with the front-buffer handle by the caller
        flags: sys::DWM_LAYER_BLT,
        x: 0, y: 0, w: fw, h: fh,
        stride: fw, // front buffer is packed at the frame width
        pad: 0,
    })
}

fn main_loop() -> ! {
    log(b"[DWM] compositor starting\n");

    let mut q = sys::DwmQuery {
        mode: 0,
        count: 0,
        out: [sys::DwmOutput {
            width: 0, height: 0, primary: 0, available: 0, pad: [0, 0],
        }; 4],
    };
    if sys::dwm_query(&mut q) < 0 || q.count == 0 {
        log(b"[DWM] no outputs; idling\n");
        loop { sys::sleep_ticks(1); }
    }
    let mut fw = q.out[0].width;
    let mut fh = q.out[0].height;
    if fw == 0 || fh == 0 { fw = 640; fh = 480; }
    if fw > 1280 { fw = 1280; }
    if fh > 720 { fh = 720; }

    let mut buf = [0u8; 8];
    log(b"[DWM] primary w=");
    log(hex32(&mut buf, fw));
    log(b" h=");
    log(hex32(&mut buf, fh));
    log(b"\n");

    /* Double buffering: two scanout-capable vidmem objects, each mapped at
     * its own fixed VA. The first slot is required; if the second cannot be
     * allocated the compositor still runs with a single buffer, since a
     * partially working desktop beats an idle compositor. */
    let frame_words = (fw as usize) * (fh as usize);
    let frame_size = (frame_words * 4) as u32;
    let frame_vas = [FRAME_VA, FRAME_B_VA];
    let mut handles = [0u32; 2];
    let mut buffers = [0usize; 2];
    let mut count = 0usize;

    for i in 0..2 {
        let h = sys::vmem_alloc(frame_size, sys::VMEM_FLAG_SCANOUT);
        if h == 0 || sys::vmem_map(h, frame_vas[i],
                                   sys::VM_PROT_WRITE | sys::VM_PROT_USER) < 0 {
            if i == 0 {
                log(b"[DWM] front buffer allocation failed; idling\n");
                // B-39: the front buffer is gone, so there is nothing to
                // present ever again - park instead of spinning at full
                // speed for the rest of the session.
                loop { sys::sleep_ticks(1); }
            }
            if h != 0 { sys::vmem_free(h); }
            break;
        }
        handles[i] = h;
        buffers[i] = frame_vas[i] as usize;
        count += 1;
    }
    if count == 2 {
        log(b"[DWM] double buffered: two scanout objects\n");
    } else {
        log(b"[DWM] single buffer fallback: second scanout object unavailable\n");
    }
    /* Which object the compositor writes next; the other one is the front
     * the CRTC scanned out last. */
    let mut back = 0usize;

    let port = ipc::lookup(PORT_NAME);
    let mut tree = Tree::new();
    let mut dpi = DPI_SCALE_100;
    let mut mode = PresentQueue::mode();
    let mut queue = PresentQueue::new(0, fw, fh);
    /* Keep the mapping table across frames: recreating it each frame would
     * re-map every source handle and leak a reference on each object. */
    let mut maps = LayerMaps::new();

    loop {
        if let Some(p) = port.and_then(|pt| ipc::recv(pt, false)) {
            match p.ty {
                MSG_LAYER_PUT => {
                    if let Some(m) = p.read::<LayerMsg>() {
                        tree.put(m.key, Layer {
                            handle: m.handle,
                            x: m.x, y: m.y,
                            w: if m.w == 0 { fw } else { m.w },
                            h: if m.h == 0 { fh } else { m.h },
                            alpha: m.alpha, radius: m.radius,
                            visible: true, z: m.z, flags: m.flags as u32,
                        });
                    }
                }
                MSG_LAYER_DEL => {
                    if let Some(key) = p.read::<u32>() { tree.remove(key); }
                }
                MSG_SET_DPI => {
                    if let Some(d) = p.read::<u32>() { dpi = d; }
                }
                MSG_SET_MODE => {
                    if let Some(m) = p.read::<u32>() {
                        if sys::set_mode(m) >= 0 { mode = m; }
                    }
                }
                MSG_PING => { log(b"[DWM] ping\n"); }
                _ => {}
            }
        }

        /* Compose into the back object only. The object the CRTC presented
         * last stays unchanged until the new layer list has been accepted. */
        let frame_layer = {
            let frame = unsafe {
                core::slice::from_raw_parts_mut(buffers[back] as *mut u32,
                                                frame_words)
            };
            match compose_frame(&tree, frame, fw, fh, dpi, &mut maps) {
                Some(l) => l,
                None => {
                    // No v2 surfaces: leave the framebuffer to the kernel WM.
                    // B-37: park for a tick. A yield here is a full-speed spin -
                    // on a desktop whose windows are all v1 this loop has
                    // nothing to compose, so it ran millions of iterations a
                    // second and was the task the scheduler sampled most.
                    sys::sleep_ticks(1);
                    continue;
                }
            }
        };

        let mut out = frame_layer;
        out.handle = handles[back];
        queue.stage(&[out]);

        if mode == sys::DWM_MODE_EXTEND {
            // Each output clips the same composed layer to its own geometry.
            let n = (q.count as usize).min(4);
            for i in 0..n {
                let mut qq = PresentQueue::new(i as u32,
                    q.out[i].width, q.out[i].height);
                qq.stage(&[out]);
                qq.present();
            }
        } else {
            queue.present();
        }

        /* The just-submitted object is the front buffer now; the next frame
         * is composed into its partner. With one buffer this is a no-op. */
        if count == 2 { back ^= 1; }

        // B-37: one tick per frame. Presenting in a yield loop let the
        // compositor push frames as fast as the CPU allowed (no vsync on
        // this hardware) and starved every client behind it.
        sys::sleep_ticks(1);
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    main_loop()
}
