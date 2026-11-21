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

    let idata_section = 0x188 + 4 * 40;
    let idata_rva = u32_at(&image, idata_section + 12);
    let idata_offset = u32_at(&image, idata_section + 20) as usize;
    let mut iat_names = Vec::new();
    for index in 0..3usize {
        let hint_rva = u32_at(
            &image,
            idata_offset + 40 + 32 + index * 8,
        );
        let name_offset = idata_offset + (hint_rva - idata_rva) as usize + 2;
        let mut end = name_offset;
        while image[end] != 0 {
            end += 1;
        }
        iat_names.push(std::str::from_utf8(&image[name_offset..end]).unwrap().to_string());
    }
    // The order follows the first use in the program, not a fixed list; what
    // matters is that every supported import got a slot and no other name did.
    iat_names.sort();
    assert_eq!(iat_names, ["ExitProcess", "GetStdHandle", "WriteFile"]);
}

#[test]
fn win32_api_table_and_shared_time_id_are_consistent() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..");
    let win32_c = std::fs::read_to_string(root.join("kernel/win32.c")).unwrap().replace("\r\n", "\n");
    let win32_h = std::fs::read_to_string(root.join("kernel/win32.h")).unwrap().replace("\r\n", "\n");

    let enum_start = win32_c.find("enum {\n").unwrap();
    let enum_end = win32_c[enum_start..].find("typedef struct").unwrap() + enum_start;
    let enum_block = &win32_c[enum_start..enum_end];
    let api_ids = enum_block
        .lines()
        .filter(|line| line.trim_start().starts_with("WIN32_API_"))
        .count();
    assert_eq!(api_ids, 143);
    assert!(win32_h.contains("#define WIN32_API_COUNT 143u"));

    let table_start = win32_c.find("static const win32_api_t g_api[]").unwrap();
    let table_end = win32_c[table_start..].find("\n};").unwrap() + table_start;
    let table = &win32_c[table_start..table_end];
    let api_rows = table
        .lines()
        .filter(|line| line.trim_start().starts_with("{\""))
        .count();
    assert_eq!(api_rows, 144);
    assert!(table.contains(r#"{"kernel32.dll", "GetSystemTime", WIN32_API_GET_SYSTEM_TIME}"#));
    assert!(table.contains(r#"{"kernel32.dll", "GetLocalTime", WIN32_API_GET_SYSTEM_TIME}"#));
    assert!(table.contains(r#"{"kernel32.dll", "FindResourceA", WIN32_API_FIND_RESOURCE}"#));
    assert!(table.contains(r#"{"kernel32.dll", "LoadStringA", WIN32_API_LOAD_STRING}"#));
}
