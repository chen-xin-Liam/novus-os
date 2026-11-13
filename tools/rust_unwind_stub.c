#include <stdint.h>

/*
 * PE/COFF host link stub for mkdisk and any other host tool that links the
 * precompiled Rust rlib.
 *
 * ELF discards the rlib's unused .xdata unwind metadata together with the
 * dead debug section, which is why --gc-sections is enough on Linux. PE keeps
 * .xdata as a live exception-handling directory entry, so the reference
 * survives and the link dies with "undefined reference to
 * rust_eh_personality". Novus never unwinds, so an empty definition satisfies
 * the reference and is never reached.
 *
 * tools.mk adds this object on Windows hosts only; ELF hosts have no .xdata
 * and would not need it.
 */
void rust_eh_personality(void)
{
}

/*
 * The symbol above has to survive --gc-sections reachability analysis from a
 * PE hand-authored unwind entry, which the linker cannot prove is unreferenced.
 * Keeping a named anchor in the same translation unit documents the intent and
 * gives the section a reason to stay.
 */
void novus_unwind_stub_anchor(void)
{
}
