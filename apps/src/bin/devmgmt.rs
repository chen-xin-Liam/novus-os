//! Device Manager entry point. The window/event logic lives in
//! `novus_apps::devmgr`; this is only the no_std boot stub.

#![no_std]
#![no_main]

use novus_apps::devmgr::App;
use novus_apps::{ipc, sinfo, sys, wm};

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] devmgmt up\n");
    let wm_port = match ipc::lookup(wm::PORT_NAME) {
        Some(p) => p,
        None => sys::exit(1),
    };
    let sys_port = match ipc::lookup(sinfo::PORT_NAME) {
        Some(p) => p,
        None => sys::exit(2),
    };
    let mut app = match App::new(wm_port, sys_port) {
        Some(a) => a,
        None => sys::exit(3),
    };
    app.run();
    sys::exit(0);
}
