use crate::diagnostic::{CompileError, ErrorCollector, SourceLocation};
use anyhow::{Context, Result};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct PreprocessedSource {
    pub text: String,
    pub(crate) locations: Vec<SourceLocation>,
}

impl PreprocessedSource {
    pub fn location_at(&self, byte_offset: usize) -> SourceLocation {
        self.locations
            .get(byte_offset)
            .cloned()
            .or_else(|| self.locations.last().cloned())
            .unwrap_or_else(SourceLocation::synthetic)
    }
}

#[derive(Default)]
pub struct Preprocessor {
    include_dirs: Vec<PathBuf>,
    macros: HashMap<String, String>,
    once_files: HashSet<PathBuf>,
    include_stack: Vec<PathBuf>,
    output: String,
    locations: Vec<SourceLocation>,
    errors: ErrorCollector,
}

impl Preprocessor {
    pub fn new(include_dirs: Vec<PathBuf>) -> Self {
        Self {
            include_dirs,
            ..Self::default()
        }
    }

    pub fn preprocess(mut self, input: &Path) -> Result<(PreprocessedSource, Vec<CompileError>)> {
        let path = canonical_or_absolute(input)?;
        self.process_file(&path)?;
        Ok((
            PreprocessedSource {
                text: self.output,
                locations: self.locations,
            },
            self.errors.into_errors(),
        ))
    }

    fn process_file(&mut self, path: &Path) -> Result<()> {
        let path = canonical_or_absolute(path)?;
        if self.once_files.contains(&path) {
            return Ok(());
        }
        if self.include_stack.contains(&path) {
            self.errors.push(CompileError::new(
                SourceLocation::new(&path, 1, 1),
                "recursive #include detected",
            ));
            return Ok(());
        }
        let source = fs::read_to_string(&path)
            .with_context(|| format!("failed to read source file {}", path.display()))?;
        self.include_stack.push(path.clone());
        let result = self.process_text(&path, &source);
        self.include_stack.pop();
        result?;
        Ok(())
    }

    fn process_text(&mut self, path: &Path, source: &str) -> Result<()> {
        let mut block_comment = false;
        let mut conditionals: Vec<Conditional> = Vec::new();

        for (line_index, raw_line) in source.lines().enumerate() {
            let line_number = line_index + 1;
            let clean = strip_comments(raw_line, &mut block_comment);
            let trimmed = clean.trim_start();
            let active = conditionals.iter().all(|c| c.active);
            if let Some(directive) = trimmed.strip_prefix('#') {
                self.directive(
                    path,
                    line_number,
                    directive.trim_start(),
                    active,
                    &mut conditionals,
                )?;
            } else if active {
                self.emit_expanded_line(path, line_number, &clean);
            }
        }

        if !conditionals.is_empty() {
            self.errors.push(CompileError::new(
                SourceLocation::new(path, source.lines().count().max(1), 1),
                "unterminated conditional preprocessing block",
            ));
        }
        if block_comment {
            self.errors.push(CompileError::new(
                SourceLocation::new(path, source.lines().count().max(1), 1),
                "unterminated block comment",
            ));
        }
        Ok(())
    }

    fn directive(
        &mut self,
        current_file: &Path,
        line: usize,
        directive: &str,
        currently_active: bool,
        conditionals: &mut Vec<Conditional>,
    ) -> Result<()> {
        let location = |column| SourceLocation::new(current_file, line, column);
        let (name, rest) = split_word(directive);
        match name {
            "ifdef" | "ifndef" => {
                let macro_name = rest.trim();
                if !is_identifier(macro_name) {
                    self.errors.push(CompileError::new(
                        location(1),
                        format!("expected macro name after #{}", name),
                    ));
                    conditionals.push(Conditional::inactive());
                } else {
                    let defined = self.macros.contains_key(macro_name);
                    let branch = if name == "ifdef" { defined } else { !defined };
                    conditionals.push(Conditional {
                        parent_active: currently_active,
                        active: currently_active && branch,
                        branch_taken: branch,
                        saw_else: false,
                    });
                }
            }
            "else" => {
                if let Some(frame) = conditionals.last_mut() {
                    if frame.saw_else {
                        self.errors.push(CompileError::new(location(1), "duplicate #else"));
                    }
                    frame.saw_else = true;
                    frame.active = frame.parent_active && !frame.branch_taken;
                    frame.branch_taken = true;
                } else {
                    self.errors.push(CompileError::new(location(1), "#else without #ifdef/#ifndef"));
                }
            }
            "endif" => {
                if conditionals.pop().is_none() {
                    self.errors.push(CompileError::new(location(1), "#endif without #ifdef/#ifndef"));
                }
            }
            _ if !currently_active => {}
            "define" => {
                let (macro_name, replacement) = split_word(rest.trim_start());
                if !is_identifier(macro_name) {
                    self.errors.push(CompileError::new(location(1), "invalid object-like macro name"));
                } else if replacement.starts_with('(') {
                    self.errors.push(CompileError::new(location(1), "function-like macros are not supported"));
                } else {
                    self.macros.insert(macro_name.to_owned(), replacement.trim().to_owned());
                }
            }
            "undef" => {
                let macro_name = rest.trim();
                if !is_identifier(macro_name) {
                    self.errors.push(CompileError::new(location(1), "expected macro name after #undef"));
                } else {
                    self.macros.remove(macro_name);
                }
            }
            "include" => {
                match parse_include(rest.trim()) {
                    Some((name, angled)) => {
                        if let Some(include) = self.find_include(current_file, &name, angled) {
                            self.process_file(&include)?;
                        } else {
                            self.errors.push(CompileError::new(
                                location(1),
                                format!("include file '{}' not found", name),
                            ));
                        }
                    }
                    None => self.errors.push(CompileError::new(location(1), "malformed #include")),
                }
            }
            "pragma" if rest.trim() == "once" => {
                self.once_files.insert(canonical_or_absolute(current_file)?);
            }
            "pragma" => self.errors.push(CompileError::new(location(1), "unsupported #pragma")),
            "" => {}
            other => self.errors.push(CompileError::new(
                location(1),
                format!("unsupported preprocessing directive #{}", other),
            )),
        }
        Ok(())
    }

    fn find_include(&self, current_file: &Path, name: &str, _angled: bool) -> Option<PathBuf> {
        let mut candidates = Vec::with_capacity(self.include_dirs.len() + 1);
        if let Some(parent) = current_file.parent() {
            candidates.push(parent.join(name));
        }
        candidates.extend(self.include_dirs.iter().map(|dir| dir.join(name)));
        candidates.into_iter().find(|candidate| candidate.is_file())
    }

    fn emit_expanded_line(&mut self, path: &Path, line: usize, text: &str) {
        let expanded = expand_macros(text, &self.macros, 0);
        match expanded {
            Ok(expanded) => {
                let mut col = 1usize;
                for ch in expanded.chars() {
                    self.output.push(ch);
                    for _ in 0..ch.len_utf8() {
                        self.locations.push(SourceLocation::new(path, line, col));
                    }
                    col += 1;
                }
                self.output.push('\n');
                self.locations.push(SourceLocation::new(path, line, text.len() + 1));
            }
            Err(message) => self.errors.push(CompileError::new(
                SourceLocation::new(path, line, 1),
                message,
            )),
        }
    }
}

#[derive(Clone, Copy)]
struct Conditional {
    parent_active: bool,
    active: bool,
    branch_taken: bool,
    saw_else: bool,
}

impl Conditional {
    fn inactive() -> Self {
        Self {
            parent_active: false,
            active: false,
            branch_taken: false,
            saw_else: false,
        }
    }
}

fn canonical_or_absolute(path: &Path) -> Result<PathBuf> {
    if path.exists() {
        fs::canonicalize(path).with_context(|| format!("cannot resolve {}", path.display()))
    } else if path.is_absolute() {
        Ok(path.to_path_buf())
    } else {
        Ok(std::env::current_dir()?.join(path))
    }
}

fn split_word(text: &str) -> (&str, &str) {
    let end = text.find(char::is_whitespace).unwrap_or(text.len());
    (&text[..end], &text[end..])
}

fn is_identifier(text: &str) -> bool {
    let mut chars = text.chars();
    matches!(chars.next(), Some(c) if c == '_' || c.is_ascii_alphabetic())
        && chars.all(|c| c == '_' || c.is_ascii_alphanumeric())
}

fn parse_include(text: &str) -> Option<(String, bool)> {
    let (close, angled) = match text.as_bytes().first()? {
        b'"' => ('"', false),
        b'<' => ('>', true),
        _ => return None,
    };
    let end = text[1..].find(close)? + 1;
    if text[end + 1..].trim().is_empty() {
        Some((text[1..end].to_owned(), angled))
    } else {
        None
    }
}

fn expand_macros(text: &str, macros: &HashMap<String, String>, depth: usize) -> std::result::Result<String, String> {
    if depth > 64 {
        return Err("macro expansion depth exceeded".to_owned());
    }
    let bytes = text.as_bytes();
    let mut output = String::new();
    let mut i = 0usize;
    while i < bytes.len() {
        let byte = bytes[i];
        if byte == b'\'' || byte == b'"' {
            let quote = byte;
            let start = i;
            i += 1;
            while i < bytes.len() {
                if bytes[i] == b'\\' {
                    i = (i + 2).min(bytes.len());
                } else if bytes[i] == quote {
                    i += 1;
                    break;
                } else {
                    i += 1;
                }
            }
            output.push_str(&text[start..i]);
        } else if byte == b'_' || byte.is_ascii_alphabetic() {
            let start = i;
            i += 1;
            while i < bytes.len() && (bytes[i] == b'_' || bytes[i].is_ascii_alphanumeric()) {
                i += 1;
            }
            let ident = &text[start..i];
            if let Some(replacement) = macros.get(ident) {
                output.push_str(&expand_macros(replacement, macros, depth + 1)?);
            } else {
                output.push_str(ident);
            }
        } else {
            let ch = text[i..].chars().next().expect("valid UTF-8 boundary");
            output.push(ch);
            i += ch.len_utf8();
        }
    }
    Ok(output)
}

fn strip_comments(line: &str, in_block: &mut bool) -> String {
    let bytes = line.as_bytes();
    let mut output = String::with_capacity(line.len());
    let mut i = 0usize;
    let mut quote = None;
    while i < bytes.len() {
        if *in_block {
            if bytes.get(i..i + 2) == Some(b"*/") {
                *in_block = false;
                output.push_str("  ");
                i += 2;
            } else {
                let ch = line[i..].chars().next().unwrap();
                output.extend(std::iter::repeat(' ').take(ch.len_utf8()));
                i += ch.len_utf8();
            }
        } else if let Some(q) = quote {
            let ch = line[i..].chars().next().unwrap();
            output.push(ch);
            i += ch.len_utf8();
            if ch == '\\' && i < bytes.len() {
                let escaped = line[i..].chars().next().unwrap();
                output.push(escaped);
                i += escaped.len_utf8();
            } else if ch == q {
                quote = None;
            }
        } else if bytes[i] == b'\'' || bytes[i] == b'"' {
            quote = Some(bytes[i] as char);
            output.push(bytes[i] as char);
            i += 1;
        } else if bytes.get(i..i + 2) == Some(b"//") {
            output.extend(std::iter::repeat(' ').take(bytes.len() - i));
            break;
        } else if bytes.get(i..i + 2) == Some(b"/*") {
            *in_block = true;
            output.push_str("  ");
            i += 2;
        } else {
            let ch = line[i..].chars().next().unwrap();
            output.push(ch);
            i += ch.len_utf8();
        }
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_dir() -> PathBuf {
        let id = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!("cppcc-{}", id));
        fs::create_dir_all(&path).unwrap();
        path
    }

    fn run(source: &str) -> (String, Vec<CompileError>) {
        let dir = temp_dir();
        let file = dir.join("main.cc");
        fs::write(&file, source).unwrap();
        let (out, errors) = Preprocessor::new(vec![]).preprocess(&file).unwrap();
        fs::remove_dir_all(dir).unwrap();
        (out.text, errors)
    }

    #[test]
    fn expands_object_macros_without_touching_literals() {
        let (out, errors) = run("#define VALUE 42\nint x = VALUE; const char* s = \"VALUE\";\n");
        assert!(errors.is_empty());
        assert!(out.contains("int x = 42;"));
        assert!(out.contains("\"VALUE\""));
    }

    #[test]
    fn strips_comments_but_preserves_quoted_comment_markers() {
        let (out, errors) = run("int a; // gone\nconst char* s = \"/* kept */\"; /* x\n y */ int b;\n");
        assert!(errors.is_empty());
        assert!(!out.contains("gone"));
        assert!(out.contains("\"/* kept */\""));
        assert!(out.contains("int b;"));
    }

    #[test]
    fn includes_local_file_once_with_pragma_once() {
        let dir = temp_dir();
        fs::write(dir.join("once.h"), "#pragma once\nint included;\n").unwrap();
        let main = dir.join("main.cc");
        fs::write(&main, "#include \"once.h\"\n#include \"once.h\"\nint main();\n").unwrap();
        let (out, errors) = Preprocessor::new(vec![]).preprocess(&main).unwrap();
        assert!(errors.is_empty());
        assert_eq!(out.text.matches("int included;").count(), 1);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn conditional_header_guards_select_one_branch() {
        let (out, errors) = run("#define X\n#ifdef X\nint a;\n#else\nint b;\n#endif\n");
        assert!(errors.is_empty());
        assert!(out.contains("int a;"));
        assert!(!out.contains("int b;"));
    }

    #[test]
    fn rejects_function_macros_and_unmatched_directives() {
        let (_, errors) = run("#define F(x) x\n#endif\n");
        assert_eq!(errors.len(), 2);
        assert!(errors.iter().all(|e| e.location.line > 0 && e.location.column > 0));
    }
}