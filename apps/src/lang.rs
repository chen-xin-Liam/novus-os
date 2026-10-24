//! UI language for the built-in applications (M11).
//!
//! The kernel owns the switch (`SYSINFO_REQ_LANG`); every app keeps a
//! local copy and polls the version about once a second, the same rhythm
//! the theme uses. Strings are inline pairs - `l("磁盘", "Disk")` - so a
//! translation is reviewed where it is drawn and a missing one is visible
//! as Chinese, never as an id.

use core::sync::atomic::{AtomicU32, Ordering};

use crate::sinfo;

static LANG: AtomicU32 = AtomicU32::new(0);
static VERSION: AtomicU32 = AtomicU32::new(1);
static POLL: AtomicU32 = AtomicU32::new(0);

pub const ZH: u32 = 0;
pub const EN: u32 = 1;

pub fn l(zh: &'static str, en: &'static str) -> &'static str {
    if LANG.load(Ordering::Relaxed) == EN { en } else { zh }
}

/// Translate a Chinese source string through the generated table. The
/// kernel renders the same translation, so layout maths and pixels agree.
pub fn tr<'a>(s: &'a str) -> &'a str {
    if LANG.load(Ordering::Relaxed) == EN {
        crate::lang_dict::lookup(s).unwrap_or(s)
    } else {
        s
    }
}

pub fn is_en() -> bool {
    LANG.load(Ordering::Relaxed) == EN
}

pub fn get() -> u32 {
    LANG.load(Ordering::Relaxed)
}

pub fn set(lang: u32) {
    if lang <= EN && lang != LANG.load(Ordering::Relaxed) {
        LANG.store(lang, Ordering::Relaxed);
        VERSION.fetch_add(1, Ordering::Relaxed);
    }
}

pub fn version() -> u32 {
    VERSION.load(Ordering::Relaxed)
}

/// Adopt the kernel state before the first frame.
pub fn init(sys_port: u64) {
    if let Some(v) = sinfo::lang(sys_port) {
        set(v.lang);
    }
}

/// Cheap poll: returns true when the language changed.
pub fn poll(sys_port: u64) -> bool {
    if POLL.fetch_add(1, Ordering::Relaxed) % 60 != 0 {
        return false;
    }
    let Some(v) = sinfo::lang(sys_port) else { return false };
    if v.version == VERSION.load(Ordering::Relaxed) {
        return false;
    }
    set(v.lang);
    true
}
