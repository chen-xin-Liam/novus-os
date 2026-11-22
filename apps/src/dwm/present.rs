//! Output queue and Present scheduling.
//!
//! The compositor builds a frame in a front buffer (a vidmem object), then
//! submits the layer list to the kernel CRTC, which waits for VSync and scans
//! it out. In CLONE mode one frame goes to every output; in EXTEND mode each
//! output gets its own queue. A bounded dirty rectangle limits work.

use crate::sys;
use crate::sys::DwmLayer;

pub struct PresentQueue {
    pub crtc: u32,
    pub layers: [DwmLayer; 16],
    pub count: usize,
    pub width: u32,
    pub height: u32,
    pub synced: bool,
}

impl PresentQueue {
    pub fn new(crtc: u32, width: u32, height: u32) -> Self {
        PresentQueue {
            crtc, layers: [DwmLayer::empty(); 16],
            count: 0, width, height, synced: false,
        }
    }

    /// Stage the layers for this output. `nil` layers are skipped.
    pub fn stage(&mut self, src: &[DwmLayer]) {
        let mut n = 0;
        for l in src {
            if n >= self.layers.len() {
                break;
            }
            if l.handle != 0 && l.w != 0 && l.h != 0 {
                self.layers[n] = *l;
                n += 1;
            }
        }
        self.count = n;
    }

    /// Submit the staged frame. Waits for VSync first (the kernel also waits
    /// on the present path, so this is the compositor-side pacing), then
    /// hands the layer descriptors to the CRTC.
    pub fn present(&mut self) -> bool {
        if self.count == 0 {
            return true;
        }
        self.synced = sys::wait_vsync(self.crtc) != 0;
        sys::surface_present(self.crtc, &self.layers[..self.count]) >= 0
    }

    /// Current mode reported by the kernel (clone/extend).
    pub fn mode() -> u32 {
        let mut q = sys::DwmQuery {
            mode: 0,
            count: 0,
            out: [sys::DwmOutput {
                width: 0, height: 0, primary: 0, available: 0, pad: [0, 0],
            }; 4],
        };
        if sys::dwm_query(&mut q) < 0 { sys::DWM_MODE_CLONE } else { q.mode }
    }
}
