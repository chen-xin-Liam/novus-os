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
 * regions. Each layer slot reserves 16 MiB (the vidmem object cap). */
const FRAME_VA: u64 = 0x0400_0000;
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
        out[i] = b'0' + (x & 0xF) as u8;
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
        loop { sys::yield_now(); }
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

    // Allocate and map the compositor's front buffer as a vidmem object.
    let frame_words = (fw as usize) * (fh as usize);
    let frame_size = (frame_words * 4) as u32;
    let front = sys::vmem_alloc(frame_size, sys::VMEM_FLAG_SCANOUT);
    if front == 0 || sys::vmem_map(front, FRAME_VA,
                                   sys::VM_PROT_WRITE | sys::VM_PROT_USER) < 0 {
        log(b"[DWM] front buffer allocation failed; idling\n");
        loop { sys::yield_now(); }
    }
    let frame = unsafe {
        core::slice::from_raw_parts_mut(FRAME_VA as *mut u32, frame_words)
    };

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

        let frame_layer = match compose_frame(&tree, frame, fw, fh, dpi, &mut maps) {
            Some(l) => l,
            None => {
                // No v2 surfaces: leave the framebuffer to the kernel WM.
                sys::yield_now();
                continue;
            }
        };

        let mut out = frame_layer;
        out.handle = front;
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

        sys::yield_now();
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    main_loop()
}
