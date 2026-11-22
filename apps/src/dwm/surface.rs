//! Redirected surfaces: the Flip and Blt paths.
//!
//! Flip is the modern, zero-copy path: an app renders into its own vidmem
//! object and hands the handle to the compositor, which maps it read-only.
//! Blt is the compatibility path: legacy content is copied into a
//! compositor-managed object. Both end up as a canonical ARGB layer.
//!
//! The compositor itself does not need to copy pixels for Flip - the kernel
//! CRTC blits the submitted layer handles at present time - so this module
//! tracks surface bookkeeping and provides the Blt copy helper for the rare
//! path that does need a software copy.

use crate::sys;

#[derive(Clone, Copy, PartialEq)]
pub enum Path {
    Flip,
    Blt,
}

pub struct RedirectedSurface {
    pub handle: u32,
    pub path: Path,
    pub mapped_va: u64,   /* 0 when not mapped into the compositor */
    pub w: u32,
    pub h: u32,
    pub front: u32,       /* current front-buffer handle */
    pub back: u32,        /* back buffer for double-buffered Flip */
}

impl RedirectedSurface {
    pub fn empty() -> Self {
        RedirectedSurface {
            handle: 0, path: Path::Flip, mapped_va: 0,
            w: 0, h: 0, front: 0, back: 0,
        }
    }

    /// Adopt an app-owned object for the Flip path. `va` is where the object
    /// has been mapped into the compositor's address space (may be 0 if the
    /// compositor only forwards the handle to the kernel).
    pub fn adopt_flip(handle: u32, w: u32, h: u32, va: u64) -> Self {
        RedirectedSurface {
            handle, path: Path::Flip, mapped_va: va,
            w, h, front: handle, back: 0,
        }
    }

    /// Create a compositor-owned object for the Blt path. The kernel charges
    /// allocation to the calling pid (the compositor), so `owner` is only
    /// kept for API symmetry with the Flip grant path.
    pub fn create_blt(_owner: u64, w: u32, h: u32) -> Option<Self> {
        let size = (w as u64) * (h as u64) * 4u64;
        if size == 0 || size > u32::MAX as u64 {
            return None;
        }
        let handle = sys::vmem_alloc(size as u32, sys::VMEM_FLAG_SCANOUT);
        if handle == 0 {
            return None;
        }
        Some(RedirectedSurface {
            handle, path: Path::Blt, mapped_va: 0,
            w, h, front: handle, back: 0,
        })
    }

    /// Present time: the handle the kernel should scan out.
    pub fn front_handle(&self) -> u32 {
        if self.front != 0 { self.front } else { self.handle }
    }

    /// Swap front and back buffers for a double-buffered Flip chain.
    pub fn swap(&mut self) {
        if self.back != 0 {
            let t = self.front;
            self.front = self.back;
            self.back = t;
        }
    }

    /// Release this process's references on the object(s).
    pub fn release(&self, pid: u64) {
        if self.handle != 0 {
            let _ = sys::vmem_free(self.handle);
        }
        let _ = pid;
    }
}
