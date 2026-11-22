//! Mini-DWM user-space compositor engine.
//!
//! This is the `dwm.exe` equivalent: a `no_std`, allocation-free process that
//! owns the window tree, redirects application surfaces (Flip/Blt), composites
//! them into a frame with a pluggable software pipeline, and presents on the
//! display's VSync through the Mini-DWM syscalls in `sys`.
//!
//! The in-kernel Novus-WM stays as the fallback desktop. This engine is a
//! separate application (`bin/dwm.rs`) launched only when CONFIG_NOVUS_DWM is
//! on; it never touches the framebuffer except through `sys::surface_present`.

pub mod tree;
pub mod surface;
pub mod compose;
pub mod present;
pub mod thumbnail;
