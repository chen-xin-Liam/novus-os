//! Thumbnail service.
//!
//! Reuses a window's redirected texture: a thumbnail is just another layer
//! whose source handle is the window's front buffer, scaled to the requested
//! size. No extra pixels are copied in the common path; the CRTC's bilinear
//! scaler handles the downscale at present time.

use crate::dwm::tree::Layer;
use crate::sys;

pub struct Thumbnail {
    pub handle: u32,       /* destination object */
    pub source: u32,       /* source window's front-buffer handle */
    pub w: u32,
    pub h: u32,
    pub x: i32,
    pub y: i32,
}

impl Thumbnail {
    /// Build a thumbnail layer from a source window layer.
    pub fn from_layer(layer: &Layer, dest: u32, w: u32, h: u32, x: i32, y: i32) -> Self {
        Thumbnail {
            handle: dest,
            source: layer.front_handle_dummy(),
            w, h, x, y,
        }
    }

    /// Present the thumbnail as a DwmLayer placed at (x,y).
    pub fn as_layer(&self) -> sys::DwmLayer {
        sys::DwmLayer {
            handle: self.source,
            flags: sys::DWM_LAYER_FLIP,
            x: self.x,
            y: self.y,
            w: self.w,
            h: self.h,
            stride: self.w,
            pad: 0,
        }
    }
}

/// The Layer helper used above: the compositor tracks the front buffer in the
/// surface table, so a thumbnail references the same handle the tree emits.
impl Layer {
    pub fn front_handle_dummy(&self) -> u32 { self.handle }
}
