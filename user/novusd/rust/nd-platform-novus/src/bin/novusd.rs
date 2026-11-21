#![no_std]
#![no_main]

// The panic handler is supplied by `novus-apps` (linked through the syscall
// facade); defining another one here would be a duplicate symbol.

#[no_mangle]
pub extern "C" fn _start() -> ! {
    nd_platform_novus::app::nd_novus_app_start()
}
