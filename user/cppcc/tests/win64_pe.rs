use cppcc::api::{compile, CompileInput, CompileOptions, Target};

fn u16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]])
}

#[test]
fn emits_pe32_plus_headers_and_import_directory() {
    let input = CompileInput::file(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("examples")
            .join("hello.cc"),
    );
    let result = compile(&input, &CompileOptions::default(), Target::Win64).unwrap();
    let image = result.artifact.unwrap().bytes;
    assert_eq!(&image[0..2], b"MZ");
    assert_eq!(u32_at(&image, 0x3C), 0x80);
    assert_eq!(&image[0x80..0x84], b"PE\0\0");
    assert_eq!(u16_at(&image, 0x84), 0x8664);
    assert_eq!(u16_at(&image, 0x94), 240);
    assert_eq!(u16_at(&image, 0x96), 0x0022);
    assert_eq!(u16_at(&image, 0x98), 0x020B);
    assert_eq!(u32_at(&image, 0xB8), 0x1000);
    assert_eq!(image[0x188..0x18D], *b".text");
    assert_eq!(image[0x1B0..0x1B6], *b".rdata");
    let directories = 0x98 + 112;
    let import_rva = u32_at(&image, directories + 8);
    let import_size = u32_at(&image, directories + 12);
    assert_ne!(import_rva, 0);
    assert!(import_size >= 20);
}
