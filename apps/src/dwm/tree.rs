//! Window tree: z-order, geometry, alpha, rounded corners and DPI.
//!
//! A fixed table of layer descriptors. The compositor walks it bottom-to-top
//! to build the frame. Everything is plain data: the kernel WM owns the real
//! window objects and event routing; this tree is the compositor's view used
//! to order and transform layers at present time.

use crate::sys::DwmLayer;

pub const MAX_LAYERS: usize = 16;

/// DPI scale is stored as a 16.16 fixed-point multiplier (1000 = 100%).
pub const DPI_SCALE_100: u32 = 1000;

#[derive(Clone, Copy)]
pub struct Layer {
    pub handle: u32,      /* vidmem object holding canonical ARGB pixels */
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
    pub alpha: u8,        /* 255 opaque .. 0 invisible */
    pub radius: u8,       /* rounded-corner radius in pixels */
    pub visible: bool,
    pub z: u8,            /* higher = closer to the viewer */
    pub flags: u32,       /* DWM_LAYER_FLIP / BLT */
}

impl Layer {
    pub fn empty() -> Self {
        Layer {
            handle: 0, x: 0, y: 0, w: 0, h: 0,
            alpha: 255, radius: 0, visible: false, z: 0, flags: 0,
        }
    }
}

pub struct Tree {
    layers: [Layer; MAX_LAYERS],
    count: usize,
}

impl Tree {
    pub fn new() -> Self {
        Tree { layers: [Layer::empty(); MAX_LAYERS], count: 0 }
    }

    /// Insert or replace a layer keyed by `key` (the WM window id).
    pub fn put(&mut self, key: u32, layer: Layer) -> bool {
        // Slots are indexed by the low bits of the key so lookup is O(1) and
        // no allocator is needed. A collision replaces the occupant, which is
        // acceptable for the bounded window count of this system.
        let slot = (key as usize) & (MAX_LAYERS - 1);
        if slot >= self.count {
            self.count = slot + 1;
        }
        self.layers[slot] = layer;
        true
    }

    pub fn remove(&mut self, key: u32) {
        let slot = (key as usize) & (MAX_LAYERS - 1);
        if slot < self.count {
            self.layers[slot] = Layer::empty();
        }
    }

    pub fn get(&self, key: u32) -> Option<Layer> {
        let slot = (key as usize) & (MAX_LAYERS - 1);
        if slot < self.count && self.layers[slot].handle != 0 {
            Some(self.layers[slot])
        } else {
            None
        }
    }

    /// Emit visible layers ordered by increasing z (bottom first), applying
    /// alpha and a DPI scale to geometry. Returns the number written.
    pub fn collect(&self, out: &mut [DwmLayer], dpi: u32) -> usize {
        let mut n = 0;
        // Simple insertion sort by z over the slot indices; MAX_LAYERS is 16.
        let mut order = [0u8; MAX_LAYERS];
        let mut m = 0usize;
        for i in 0..self.count {
            let l = self.layers[i];
            if !l.visible || l.handle == 0 || l.w == 0 || l.h == 0 {
                continue;
            }
            let mut j = m;
            while j > 0 && self.layers[order[j - 1] as usize].z > l.z {
                order[j] = order[j - 1];
                j -= 1;
            }
            order[j] = i as u8;
            m += 1;
        }
        for k in 0..m {
            if n >= out.len() {
                break;
            }
            let l = self.layers[order[k] as usize];
            let sw = ((l.w as u64 * dpi as u64) / DPI_SCALE_100 as u64) as u32;
            let sh = ((l.h as u64 * dpi as u64) / DPI_SCALE_100 as u64) as u32;
            out[n] = DwmLayer {
                handle: l.handle,
                flags: l.flags,
                x: l.x,
                y: l.y,
                w: sw,
                h: sh,
                /* Source pitch is the unscaled allocation width. A layer can
                 * be scaled for display, but the kernel reads rows from the
                 * backing object at its original pitch. */
                stride: l.w,
                pad: 0,
            };
            n += 1;
        }
        n
    }
}
