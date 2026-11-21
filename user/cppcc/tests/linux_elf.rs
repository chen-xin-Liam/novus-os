use cppcc::api::{compile, CompileInput, CompileOptions, Target};

fn u16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]])
}

fn u64_at(bytes: &[u8], offset: usize) -> u64 {
    let mut value = 0u64;
    for index in 0..8 {
        value |= (bytes[offset + index] as u64) << (index * 8);
    }
    value
}

#[test]
fn emits_elf64_with_interpreter_and_dynamic_library() {
    let input = CompileInput::file(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("examples")
            .join("hello.cc"),
    );
    let result = compile(&input, &CompileOptions::default(), Target::Linux).unwrap();
    let image = result.artifact.unwrap().bytes;
    assert_eq!(&image[0..4], b"\x7fELF");
    assert_eq!(image[4], 2);
    assert_eq!(u16_at(&image, 16), 2);
    assert_eq!(u16_at(&image, 18), 62);
    let phoff = u64_at(&image, 32) as usize;
    let phentsize = u16_at(&image, 54) as usize;
    let phnum = u16_at(&image, 56) as usize;
    assert_eq!(phentsize, 56);
    assert_eq!(phnum, 6);
    let mut interp = false;
    let mut dynamic = false;
    let mut gnu_stack = false;
    for index in 0..phnum {
        let header = phoff + index * phentsize;
        match u32_at(&image, header) {
            3 => interp = true,
            2 => dynamic = true,
            0x6474_e551 => gnu_stack = true,
            _ => {}
        }
    }
    assert!(interp, "missing PT_INTERP");
    assert!(dynamic, "missing PT_DYNAMIC");
    assert!(gnu_stack, "missing PT_GNU_STACK");
    let interp_offset = u64_at(&image, phoff + phentsize + 8) as usize;
    assert!(image[interp_offset..].starts_with(b"/lib64/ld-linux-x86-64.so.2"));

    let dynamic_header = (0..phnum)
        .map(|index| phoff + index * phentsize)
        .find(|&header| u32_at(&image, header) == 2)
        .unwrap();
    let dynamic_offset = u64_at(&image, dynamic_header + 8) as usize;
    let dynamic_size = u64_at(&image, dynamic_header + 32) as usize;
    let mut tags = std::collections::HashMap::new();
    for at in (dynamic_offset..dynamic_offset + dynamic_size).step_by(16) {
        tags.insert(u64_at(&image, at), u64_at(&image, at + 8));
    }
    assert_eq!(tags.get(&14), Some(&11), "missing DT_SONAME at the string it names");
    assert_eq!(tags.get(&27), Some(&7), "missing DT_PLTREL=DT_RELA");
    assert_eq!(tags.get(&2), Some(&0), "missing DT_PLTRELSZ");
    assert!(tags.contains_key(&23), "missing DT_JMPREL");
    assert!(tags.contains_key(&6), "missing DT_SYMTAB");
    assert!(tags.contains_key(&4), "missing DT_HASH");
}
