//! Token based C++ preprocessor.
//!
//! The preprocessor lexes each file into preprocessing tokens, evaluates
//! directives line by line and expands macros on the token stream (so that
//! function-like macros, `#`/`##`, `__VA_ARGS__` and multi-line directives all
//! work the way native headers expect). The output is a flat token spelling
//! plus a byte -> `SourceLocation` map, which lets later stages keep precise
//! diagnostics.
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Flavor {
    Ident,
    Number,
    String,
    Char,
    Punct,
}

#[derive(Clone, Debug)]
struct Token {
    kind: Flavor,
    text: String,
    location: SourceLocation,
    space_before: bool,
    line_start: bool,
}

impl Token {
    fn synthetic(kind: Flavor, text: impl Into<String>, location: &SourceLocation) -> Self {
        Self {
            kind,
            text: text.into(),
            location: location.clone(),
            space_before: true,
            line_start: false,
        }
    }
}

#[derive(Clone, Debug)]
struct Macro {
    params: Option<Vec<String>>,
    variadic: bool,
    body: Vec<Token>,
}

#[derive(Default)]
pub struct Preprocessor {
    include_dirs: Vec<PathBuf>,
    system_include_dirs: Vec<PathBuf>,
    macros: HashMap<String, Macro>,
    once_files: HashSet<PathBuf>,
    include_stack: Vec<PathBuf>,
    output: String,
    locations: Vec<SourceLocation>,
    errors: ErrorCollector,
    counter: u64,
}

impl Preprocessor {
    pub fn new(include_dirs: Vec<PathBuf>) -> Self {
        Self {
            include_dirs,
            ..Self::default()
        }
    }

    /// Directories searched after `-I` for both `"..."` and `<...>` includes.
    /// This is where the bundled freestanding headers live.
    pub fn with_system_include_dirs(mut self, dirs: Vec<PathBuf>) -> Self {
        self.system_include_dirs = dirs;
        self
    }

    /// Predefine a macro as if `-Dname=value` had been passed.
    pub fn define(&mut self, name: &str, value: Option<&str>) {
        let mut errors = ErrorCollector::default();
        let source = value.unwrap_or("1");
        let tokens = tokenize(Path::new("<command line>"), source, &mut errors);
        self.macros.insert(
            name.to_owned(),
            Macro {
                params: None,
                variadic: false,
                body: tokens,
            },
        );
    }

    pub fn undefine(&mut self, name: &str) {
        self.macros.remove(name);
    }

    pub fn preprocess(mut self, input: &Path) -> Result<(PreprocessedSource, Vec<CompileError>)> {
        self.install_predefined();
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

    fn install_predefined(&mut self) {
        // `__cplusplus` advertises the C++11 language level so that native
        // headers select their modern paths instead of falling back to C89.
        for (name, value) in [
            ("__cplusplus", "201103L"),
            ("__NOVUS__", "1"),
            ("__novus__", "1"),
            ("__cppcc__", "1"),
            ("__x86_64__", "1"),
            ("__x86_64", "1"),
            ("__LP64__", "1"),
            ("__STDC_HOSTED__", "0"),
            ("__ELF__", "1"),
            ("__SIZEOF_POINTER__", "8"),
            ("__SIZEOF_LONG__", "8"),
        ] {
            let mut errors = ErrorCollector::default();
            let body = tokenize(Path::new("<built-in>"), value, &mut errors);
            self.macros.insert(
                name.to_owned(),
                Macro {
                    params: None,
                    variadic: false,
                    body,
                },
            );
        }
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
        let result = self.process_source(&path, &source);
        self.include_stack.pop();
        result
    }

    fn process_source(&mut self, path: &Path, source: &str) -> Result<()> {
        let mut lexer_errors = ErrorCollector::default();
        let tokens = tokenize(path, source, &mut lexer_errors);
        self.errors.extend(lexer_errors.into_errors());

        let mut conditionals: Vec<Conditional> = Vec::new();
        let mut index = 0usize;
        let mut last_location = SourceLocation::new(path, 1, 1);
        while index < tokens.len() {
            let start = index;
            index += 1;
            while index < tokens.len() && !tokens[index].line_start {
                index += 1;
            }
            let line = &tokens[start..index];
            last_location = line[line.len() - 1].location.clone();
            let active = conditionals.iter().all(|frame| frame.active);
            if line[0].kind == Flavor::Punct && line[0].text == "#" {
                let location = line[0].location.clone();
                self.directive(&location, &line[1..], active, &mut conditionals)?;
            } else if active {
                self.emit_line(line);
            }
        }

        if !conditionals.is_empty() {
            self.errors.push(CompileError::new(
                last_location,
                "unterminated conditional preprocessing block",
            ));
        }
        Ok(())
    }

    fn directive(
        &mut self,
        location: &SourceLocation,
        rest: &[Token],
        active: bool,
        conditionals: &mut Vec<Conditional>,
    ) -> Result<()> {
        let Some(head) = rest.first() else {
            return Ok(());
        };
        if head.kind != Flavor::Ident {
            self.errors.push(CompileError::new(
                location.clone(),
                format!("invalid preprocessing directive '#{}'", head.text),
            ));
            return Ok(());
        }
        let name = head.text.clone();
        let arguments = &rest[1..];
        match name.as_str() {
            "ifdef" | "ifndef" => {
                let defined = arguments
                    .first()
                    .filter(|token| token.kind == Flavor::Ident)
                    .map(|token| self.is_defined(&token.text))
                    .unwrap_or(false);
                if arguments.is_empty() {
                    self.errors.push(CompileError::new(
                        location.clone(),
                        format!("expected macro name after #{}", name),
                    ));
                }
                let branch = if name == "ifdef" { defined } else { !defined };
                conditionals.push(Conditional {
                    parent_active: active,
                    active: active && branch,
                    any_taken: branch,
                    saw_else: false,
                });
            }
            "if" => {
                let branch = if active {
                    self.eval_condition(location, arguments)
                } else {
                    false
                };
                conditionals.push(Conditional {
                    parent_active: active,
                    active: active && branch,
                    any_taken: branch,
                    saw_else: false,
                });
            }
            "elif" => {
                match conditionals.last_mut() {
                    Some(frame) => {
                        if frame.saw_else {
                            self.errors.push(CompileError::new(location.clone(), "#elif after #else"));
                        }
                        let parent = frame.parent_active;
                        let taken = frame.any_taken;
                        let branch = if parent && !taken {
                            self.eval_condition(location, arguments)
                        } else {
                            false
                        };
                        let frame = conditionals.last_mut().expect("conditional frame exists");
                        frame.active = parent && !taken && branch;
                        frame.any_taken = taken || branch;
                    }
                    None => self
                        .errors
                        .push(CompileError::new(location.clone(), "#elif without #if")),
                }
            }
            "else" => match conditionals.last_mut() {
                Some(frame) => {
                    if frame.saw_else {
                        self.errors.push(CompileError::new(location.clone(), "duplicate #else"));
                    }
                    frame.saw_else = true;
                    frame.active = frame.parent_active && !frame.any_taken;
                    frame.any_taken = true;
                }
                None => self
                    .errors
                    .push(CompileError::new(location.clone(), "#else without #if")),
            },
            "endif" => {
                if conditionals.pop().is_none() {
                    self.errors
                        .push(CompileError::new(location.clone(), "#endif without #if"));
                }
            }
            _ if !active => {}
            "define" => self.define_macro(location, arguments),
            "undef" => {
                if let Some(token) = arguments.first() {
                    self.macros.remove(&token.text);
                } else {
                    self.errors.push(CompileError::new(
                        location.clone(),
                        "expected macro name after #undef",
                    ));
                }
            }
            "include" | "include_next" => self.include(location, arguments, true)?,
            "error" => {
                let message = render_tokens(arguments);
                self.errors.push(CompileError::new(
                    location.clone(),
                    format!("#error {}", message.trim()),
                ));
            }
            // `#warning`, `#line`, `#ident` and unknown pragmas are accepted and
            // ignored: native headers use them for diagnostics and packing, and
            // their semantics do not affect code generation here.
            "warning" | "line" | "ident" | "sccs" => {}
            "pragma" => {
                if arguments.first().map(|token| token.text.as_str()) == Some("once") {
                    self.once_files.insert(canonical_or_absolute(&location.file)?);
                }
            }
            other => self.errors.push(CompileError::new(
                location.clone(),
                format!("unsupported preprocessing directive #{}", other),
            )),
        }
        Ok(())
    }

    fn define_macro(&mut self, location: &SourceLocation, rest: &[Token]) {
        let Some(name_token) = rest.first() else {
            self.errors.push(CompileError::new(
                location.clone(),
                "expected macro name after #define",
            ));
            return;
        };
        if name_token.kind != Flavor::Ident {
            self.errors.push(CompileError::new(
                location.clone(),
                format!("invalid macro name '{}'", name_token.text),
            ));
            return;
        }
        let name = name_token.text.clone();
        let mut params = None;
        let mut variadic = false;
        let mut body_start = 1usize;
        if rest.len() > 1 && rest[1].text == "(" && !rest[1].space_before {
            let mut list = Vec::new();
            let mut index = 2usize;
            let mut closed = false;
            while index < rest.len() {
                let token = &rest[index];
                if token.text == ")" {
                    index += 1;
                    closed = true;
                    break;
                }
                if token.text == "..." {
                    variadic = true;
                    index += 1;
                    if rest.get(index).map(|token| token.text.as_str()) == Some(")") {
                        index += 1;
                        closed = true;
                    }
                    break;
                }
                if token.kind != Flavor::Ident {
                    break;
                }
                list.push(token.text.clone());
                index += 1;
                match rest.get(index).map(|token| token.text.as_str()) {
                    Some(",") => index += 1,
                    Some(")") => {
                        index += 1;
                        closed = true;
                        break;
                    }
                    _ => break,
                }
            }
            if !closed {
                self.errors.push(CompileError::new(
                    location.clone(),
                    format!("unterminated parameter list for macro '{}'", name),
                ));
                return;
            }
            params = Some(list);
            body_start = index;
        }
        let mut body: Vec<Token> = rest[body_start.min(rest.len())..].to_vec();
        if let Some(first) = body.first_mut() {
            first.space_before = false;
        }
        self.macros.insert(
            name,
            Macro {
                params,
                variadic,
                body,
            },
        );
    }

    fn include(
        &mut self,
        location: &SourceLocation,
        rest: &[Token],
        allow_expand: bool,
    ) -> Result<()> {
        let Some(first) = rest.first() else {
            self.errors
                .push(CompileError::new(location.clone(), "malformed #include"));
            return Ok(());
        };
        let (name, angled) = if first.text == "<" {
            let mut name = String::new();
            let mut index = 1usize;
            while index < rest.len() && rest[index].text != ">" {
                name.push_str(&rest[index].text);
                index += 1;
            }
            if index >= rest.len() {
                self.errors.push(CompileError::new(
                    location.clone(),
                    "malformed #include: missing '>'",
                ));
                return Ok(());
            }
            (name, true)
        } else if first.kind == Flavor::String {
            (first.text.trim_matches('"').to_owned(), false)
        } else if allow_expand {
            let expanded = match self.expand(rest.to_vec(), &mut HashSet::new(), 0) {
                Ok(tokens) => tokens,
                Err(message) => {
                    self.errors.push(CompileError::new(location.clone(), message));
                    return Ok(());
                }
            };
            return self.include(location, &expanded, false);
        } else {
            self.errors.push(CompileError::new(
                location.clone(),
                "malformed #include: expected a header name",
            ));
            return Ok(());
        };

        match self.find_include(&location.file, &name, angled) {
            Some(path) => self.process_file(&path)?,
            None => self.errors.push(CompileError::new(
                location.clone(),
                format!(
                    "include file '{}' not found (searched {} directories)",
                    name,
                    self.include_dirs.len() + self.system_include_dirs.len() + 1
                ),
            )),
        }
        Ok(())
    }

    fn find_include(&self, current_file: &Path, name: &str, angled: bool) -> Option<PathBuf> {
        let mut candidates = Vec::with_capacity(self.include_dirs.len() + self.system_include_dirs.len() + 1);
        if !angled {
            if let Some(parent) = current_file.parent() {
                candidates.push(parent.join(name));
            }
        }
        candidates.extend(self.include_dirs.iter().map(|dir| dir.join(name)));
        candidates.extend(self.system_include_dirs.iter().map(|dir| dir.join(name)));
        candidates.into_iter().find(|candidate| candidate.is_file())
    }

    fn emit_line(&mut self, tokens: &[Token]) {
        let location = tokens[0].location.clone();
        let expanded = match self.expand(tokens.to_vec(), &mut HashSet::new(), 0) {
            Ok(tokens) => tokens,
            Err(message) => {
                self.errors.push(CompileError::new(location, message));
                return;
            }
        };
        for token in &expanded {
            if token.space_before && !self.output.chars().last().map(|c| c.is_whitespace()).unwrap_or(true) {
                self.push_char(' ', &token.location);
            }
            self.push_text(&token.text, &token.location);
        }
        if !expanded.is_empty() {
            let end = tokens[tokens.len() - 1].location.clone();
            self.push_char('\n', &end);
        }
    }

    fn push_char(&mut self, ch: char, location: &SourceLocation) {
        self.push_text(ch.encode_utf8(&mut [0u8; 4]), location);
    }

    fn push_text(&mut self, text: &str, location: &SourceLocation) {
        self.output.push_str(text);
        for _ in 0..text.len() {
            self.locations.push(location.clone());
        }
    }

    fn is_defined(&self, name: &str) -> bool {
        self.macros.contains_key(name) || matches!(name, "__LINE__" | "__FILE__" | "__COUNTER__")
    }

    fn expand(
        &mut self,
        tokens: Vec<Token>,
        disabled: &mut HashSet<String>,
        depth: usize,
    ) -> std::result::Result<Vec<Token>, String> {
        if depth > 100 {
            return Err("macro expansion depth exceeded".to_owned());
        }
        let mut out = Vec::with_capacity(tokens.len());
        let mut index = 0usize;
        while index < tokens.len() {
            let token = tokens[index].clone();
            if token.kind != Flavor::Ident {
                out.push(token);
                index += 1;
                continue;
            }
            if matches!(token.text.as_str(), "__LINE__" | "__FILE__" | "__COUNTER__") {
                out.push(self.dynamic_token(&token));
                index += 1;
                continue;
            }
            if disabled.contains(&token.text) {
                out.push(token);
                index += 1;
                continue;
            }
            let Some(definition) = self.macros.get(&token.text).cloned() else {
                out.push(token);
                index += 1;
                continue;
            };
            match definition.params {
                None => {
                    let mut body = definition.body.clone();
                    for item in &mut body {
                        item.location = token.location.clone();
                        item.line_start = false;
                    }
                    disabled.insert(token.text.clone());
                    let mut expanded = self.expand(body, disabled, depth + 1)?;
                    disabled.remove(&token.text);
                    if let Some(first) = expanded.first_mut() {
                        first.space_before = token.space_before;
                    }
                    out.extend(expanded);
                    index += 1;
                }
                Some(ref params) => {
                    if tokens.get(index + 1).map(|next| next.text.as_str()) != Some("(") {
                        out.push(token);
                        index += 1;
                        continue;
                    }
                    let Some((args, next)) = parse_arguments(&tokens, index + 1) else {
                        return Err(format!("unterminated argument list for macro '{}'", token.text));
                    };
                    let substituted = self.substitute(&definition, params, args, &token, disabled)?;
                    disabled.insert(token.text.clone());
                    let mut expanded = self.expand(substituted, disabled, depth + 1)?;
                    disabled.remove(&token.text);
                    if let Some(first) = expanded.first_mut() {
                        first.space_before = token.space_before;
                    }
                    out.extend(expanded);
                    index = next;
                }
            }
        }
        Ok(out)
    }

    fn substitute(
        &mut self,
        definition: &Macro,
        params: &[String],
        mut args: Vec<Vec<Token>>,
        use_token: &Token,
        disabled: &HashSet<String>,
    ) -> std::result::Result<Vec<Token>, String> {
        while args.len() < params.len() {
            args.push(Vec::new());
        }
        if definition.variadic && args.len() > params.len() {
            let mut joined = Vec::new();
            for (position, argument) in args[params.len()..].iter().enumerate() {
                if position > 0 {
                    joined.push(Token::synthetic(Flavor::Punct, ",", &use_token.location));
                }
                joined.extend(argument.iter().cloned());
            }
            args.truncate(params.len());
            args.push(joined);
        } else if !definition.variadic && args.len() > params.len() + 1 {
            return Err(format!(
                "macro expects {} argument(s), got {}",
                params.len(),
                args.len()
            ));
        }
        let lookup = |name: &str| -> Option<usize> {
            params
                .iter()
                .position(|param| param == name)
                .or_else(|| {
                    if definition.variadic && name == "__VA_ARGS__" {
                        Some(params.len())
                    } else {
                        None
                    }
                })
        };

        let mut produced: Vec<Token> = Vec::new();
        let mut index = 0usize;
        while index < definition.body.len() {
            let token = &definition.body[index];
            if token.kind == Flavor::Punct && token.text == "#" {
                if let Some(next) = definition.body.get(index + 1) {
                    if let Some(position) = lookup(&next.text) {
                        let raw = args.get(position).cloned().unwrap_or_default();
                        produced.push(Token::synthetic(
                            Flavor::String,
                            stringify(&raw),
                            &use_token.location,
                        ));
                        index += 2;
                        continue;
                    }
                }
            }
            let position = if token.kind == Flavor::Ident {
                lookup(&token.text)
            } else {
                None
            };
            match position {
                Some(position) => {
                    let pasted = definition
                        .body
                        .get(index + 1)
                        .map(|next| next.text == "##")
                        .unwrap_or(false)
                        || index > 0 && definition.body[index - 1].text == "##";
                    let mut replacement = args.get(position).cloned().unwrap_or_default();
                    if !pasted {
                        let mut local_disabled = disabled.clone();
                        replacement = self.expand(replacement, &mut local_disabled, 0)?;
                    }
                    for (offset, mut item) in replacement.into_iter().enumerate() {
                        if offset == 0 {
                            item.space_before = token.space_before;
                        }
                        item.location = use_token.location.clone();
                        item.line_start = false;
                        produced.push(item);
                    }
                    index += 1;
                }
                None => {
                    let mut item = token.clone();
                    item.location = use_token.location.clone();
                    item.line_start = false;
                    produced.push(item);
                    index += 1;
                }
            }
        }

        // Resolve `##` token pasting on the substituted stream.
        let mut pasted: Vec<Token> = Vec::with_capacity(produced.len());
        let mut cursor = 0usize;
        while cursor < produced.len() {
            if produced[cursor].kind == Flavor::Punct && produced[cursor].text == "##" {
                let Some(next) = produced.get(cursor + 1) else {
                    cursor += 1;
                    continue;
                };
                match pasted.pop() {
                    Some(mut previous) => {
                        previous.text.push_str(&next.text);
                        previous.kind = classify(&previous.text);
                        pasted.push(previous);
                    }
                    None => pasted.push(next.clone()),
                }
                cursor += 2;
                continue;
            }
            pasted.push(produced[cursor].clone());
            cursor += 1;
        }
        if let Some(first) = pasted.first_mut() {
            first.space_before = use_token.space_before;
        }
        Ok(pasted)
    }

    fn dynamic_token(&mut self, token: &Token) -> Token {
        let (kind, text) = match token.text.as_str() {
            "__LINE__" => (Flavor::Number, token.location.line.to_string()),
            "__FILE__" => (
                Flavor::String,
                format!(
                    "\"{}\"",
                    token.location.file.display().to_string().replace('\\', "\\\\")
                ),
            ),
            "__COUNTER__" => {
                let value = self.counter;
                self.counter += 1;
                (Flavor::Number, value.to_string())
            }
            _ => (Flavor::Ident, token.text.clone()),
        };
        Token {
            kind,
            text,
            location: token.location.clone(),
            space_before: token.space_before,
            line_start: false,
        }
    }

    fn eval_condition(&mut self, location: &SourceLocation, tokens: &[Token]) -> bool {
        let replaced = self.replace_defined(tokens);
        let expanded = match self.expand(replaced, &mut HashSet::new(), 0) {
            Ok(tokens) => tokens,
            Err(message) => {
                self.errors.push(CompileError::new(location.clone(), message));
                return false;
            }
        };
        let mut parser = ConditionParser {
            tokens: &expanded,
            index: 0,
        };
        match parser.condition() {
            Some(value) => {
                if parser.index != expanded.len() {
                    self.errors.push(CompileError::new(
                        location.clone(),
                        "trailing tokens in #if expression",
                    ));
                }
                value != 0
            }
            None => {
                self.errors.push(CompileError::new(
                    location.clone(),
                    format!("invalid #if expression '{}'", render_tokens(&expanded).trim()),
                ));
                false
            }
        }
    }

    fn replace_defined(&self, tokens: &[Token]) -> Vec<Token> {
        let mut out = Vec::with_capacity(tokens.len());
        let mut index = 0usize;
        while index < tokens.len() {
            let token = &tokens[index];
            if token.kind == Flavor::Ident && token.text == "defined" {
                let (name, next) = if tokens.get(index + 1).map(|next| next.text.as_str()) == Some("(") {
                    (
                        tokens.get(index + 2).map(|inner| inner.text.clone()),
                        index + 4,
                    )
                } else {
                    (tokens.get(index + 1).map(|inner| inner.text.clone()), index + 2)
                };
                let defined = name.map(|name| self.is_defined(&name)).unwrap_or(false);
                out.push(Token::synthetic(
                    Flavor::Number,
                    if defined { "1" } else { "0" },
                    &token.location,
                ));
                index = next;
                continue;
            }
            out.push(token.clone());
            index += 1;
        }
        out
    }
}

#[derive(Clone, Copy)]
struct Conditional {
    parent_active: bool,
    active: bool,
    any_taken: bool,
    saw_else: bool,
}

fn render_tokens(tokens: &[Token]) -> String {
    let mut text = String::new();
    for (index, token) in tokens.iter().enumerate() {
        if index > 0 && token.space_before {
            text.push(' ');
        }
        text.push_str(&token.text);
    }
    text
}

fn stringify(tokens: &[Token]) -> String {
    let mut text = String::from("\"");
    for (index, token) in tokens.iter().enumerate() {
        if index > 0 && token.space_before {
            text.push(' ');
        }
        for ch in token.text.chars() {
            if ch == '"' || ch == '\\' {
                text.push('\\');
            }
            text.push(ch);
        }
    }
    text.push('"');
    text
}

fn classify(text: &str) -> Flavor {
    let mut chars = text.chars();
    match chars.next() {
        Some(ch) if ch == '_' || ch.is_ascii_alphabetic() => {
            if chars.all(|ch| ch == '_' || ch.is_ascii_alphanumeric()) {
                Flavor::Ident
            } else {
                Flavor::Punct
            }
        }
        Some(ch) if ch.is_ascii_digit() => Flavor::Number,
        Some('"') => Flavor::String,
        Some('\'') => Flavor::Char,
        _ => Flavor::Punct,
    }
}

fn parse_arguments(tokens: &[Token], open: usize) -> Option<(Vec<Vec<Token>>, usize)> {
    if tokens.get(open).map(|token| token.text.as_str()) != Some("(") {
        return None;
    }
    let mut args: Vec<Vec<Token>> = Vec::new();
    let mut current: Vec<Token> = Vec::new();
    let mut depth = 0usize;
    let mut index = open + 1;
    while index < tokens.len() {
        let token = &tokens[index];
        match token.text.as_str() {
            "(" => {
                depth += 1;
                current.push(token.clone());
            }
            ")" => {
                if depth == 0 {
                    if !current.is_empty() || !args.is_empty() {
                        args.push(current);
                    }
                    return Some((args, index + 1));
                }
                depth -= 1;
                current.push(token.clone());
            }
            "," if depth == 0 => {
                args.push(current);
                current = Vec::new();
            }
            _ => current.push(token.clone()),
        }
        index += 1;
    }
    None
}

struct ConditionParser<'a> {
    tokens: &'a [Token],
    index: usize,
}

impl ConditionParser<'_> {
    fn peek(&self) -> Option<&str> {
        self.tokens.get(self.index).map(|token| token.text.as_str())
    }

    fn eat(&mut self, text: &str) -> bool {
        if self.peek() == Some(text) {
            self.index += 1;
            true
        } else {
            false
        }
    }

    fn condition(&mut self) -> Option<i64> {
        let value = self.binary(1)?;
        if self.eat("?") {
            let then_value = self.condition()?;
            if !self.eat(":") {
                return None;
            }
            let else_value = self.condition()?;
            return Some(if value != 0 { then_value } else { else_value });
        }
        Some(value)
    }

    fn binary(&mut self, min_precedence: u8) -> Option<i64> {
        let mut left = self.unary()?;
        loop {
            let Some(precedence) = self.peek().and_then(precedence_of) else {
                return Some(left);
            };
            if precedence < min_precedence {
                return Some(left);
            }
            let mut operator = self.peek().unwrap_or("").to_owned();
            self.index += 1;
            // Collapse the two-character logical operators, which the lexer
            // keeps as single tokens already.
            if self.peek() == Some("=") && matches!(operator.as_str(), "<" | ">") {
                operator.push('=');
                self.index += 1;
            }
            let right = self.binary(precedence + 1)?;
            left = apply(&operator, left, right)?;
        }
    }

    fn unary(&mut self) -> Option<i64> {
        for operator in ["!", "~", "-", "+"] {
            if self.eat(operator) {
                let value = self.unary()?;
                return match operator {
                    "!" => Some(if value == 0 { 1 } else { 0 }),
                    "~" => Some(!value),
                    "-" => value.checked_neg(),
                    _ => Some(value),
                };
            }
        }
        self.primary()
    }

    fn primary(&mut self) -> Option<i64> {
        if self.eat("(") {
            let value = self.condition()?;
            if !self.eat(")") {
                return None;
            }
            return Some(value);
        }
        let token = self.tokens.get(self.index)?;
        let value = match token.kind {
            Flavor::Number => parse_number(&token.text)?,
            Flavor::Char => parse_char(&token.text)?,
            Flavor::Ident => match token.text.as_str() {
                "true" => 1,
                _ => 0,
            },
            _ => return None,
        };
        self.index += 1;
        Some(value)
    }
}

fn precedence_of(operator: &str) -> Option<u8> {
    Some(match operator {
        "||" => 1,
        "&&" => 2,
        "|" => 3,
        "^" => 4,
        "&" => 5,
        "==" | "!=" => 6,
        "<" | ">" | "<=" | ">=" => 7,
        "<<" | ">>" => 8,
        "+" | "-" => 9,
        "*" | "/" | "%" => 10,
        _ => return None,
    })
}

fn apply(operator: &str, left: i64, right: i64) -> Option<i64> {
    Some(match operator {
        "||" => i64::from(left != 0 || right != 0),
        "&&" => i64::from(left != 0 && right != 0),
        "|" => left | right,
        "^" => left ^ right,
        "&" => left & right,
        "==" => i64::from(left == right),
        "!=" => i64::from(left != right),
        "<" => i64::from(left < right),
        ">" => i64::from(left > right),
        "<=" => i64::from(left <= right),
        ">=" => i64::from(left >= right),
        "<<" => left.checked_shl(u32::try_from(right).ok()?)?,
        ">>" => left.checked_shr(u32::try_from(right).ok()?)?,
        "+" => left.checked_add(right)?,
        "-" => left.checked_sub(right)?,
        "*" => left.checked_mul(right)?,
        "/" => left.checked_div(right)?,
        "%" => left.checked_rem(right)?,
        _ => return None,
    })
}

fn parse_number(text: &str) -> Option<i64> {
    let unsigned = text.ends_with('u') || text.ends_with('U');
    let trimmed = text.trim_end_matches(|ch: char| matches!(ch, 'u' | 'U' | 'l' | 'L'));
    let value = if let Some(hex) = trimmed.strip_prefix("0x").or_else(|| trimmed.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).ok().map(|value| value as i64)
    } else if let Some(binary) = trimmed.strip_prefix("0b").or_else(|| trimmed.strip_prefix("0B")) {
        u64::from_str_radix(binary, 2).ok().map(|value| value as i64)
    } else if trimmed.len() > 1 && trimmed.starts_with('0') && trimmed.bytes().all(|byte| (b'0'..=b'7').contains(&byte)) {
        i64::from_str_radix(&trimmed[1..], 8).ok()
    } else {
        trimmed.parse::<i64>().ok()
    }?;
    let _ = unsigned;
    Some(value)
}

fn parse_char(text: &str) -> Option<i64> {
    let inner = text.strip_prefix('\'')?.strip_suffix('\'')?;
    let mut chars = inner.chars();
    let value = match chars.next()? {
        '\\' => match chars.next()? {
            'n' => 10,
            't' => 9,
            'r' => 13,
            '0' => 0,
            '\\' => 92,
            '\'' => 39,
            '"' => 34,
            other => other as i64,
        },
        other => other as i64,
    };
    Some(value)
}

fn tokenize(file: &Path, source: &str, errors: &mut ErrorCollector) -> Vec<Token> {
    Tokenizer::new(file, source).run(errors)
}

struct Tokenizer<'a> {
    file: &'a Path,
    source: &'a str,
    bytes: &'a [u8],
    offset: usize,
    line: usize,
    column: usize,
    line_start: bool,
    space_before: bool,
}

impl<'a> Tokenizer<'a> {
    fn new(file: &'a Path, source: &'a str) -> Self {
        Self {
            file,
            source,
            bytes: source.as_bytes(),
            offset: 0,
            line: 1,
            column: 1,
            line_start: true,
            space_before: false,
        }
    }

    fn run(mut self, errors: &mut ErrorCollector) -> Vec<Token> {
        let mut tokens = Vec::new();
        loop {
            self.skip_trivia(errors);
            if self.offset >= self.bytes.len() {
                break;
            }
            let start = self.offset;
            let location = SourceLocation::new(self.file, self.line, self.column);
            let space_before = self.space_before;
            let line_start = self.line_start;
            self.space_before = false;
            self.line_start = false;
            let kind = self.token(errors);
            if self.offset == start {
                self.offset += 1;
                self.column += 1;
                continue;
            }
            tokens.push(Token {
                kind,
                text: self.source[start..self.offset].to_owned(),
                location,
                space_before,
                line_start,
            });
        }
        tokens
    }

    fn token(&mut self, errors: &mut ErrorCollector) -> Flavor {
        let byte = self.bytes[self.offset];
        if byte == b'_' || byte.is_ascii_alphabetic() {
            let start = self.offset;
            self.offset += 1;
            self.column += 1;
            while self.offset < self.bytes.len()
                && (self.bytes[self.offset] == b'_' || self.bytes[self.offset].is_ascii_alphanumeric())
            {
                self.offset += 1;
                self.column += 1;
            }
            let text = &self.source[start..self.offset];
            if self.offset < self.bytes.len()
                && matches!(self.bytes[self.offset], b'"' | b'\'')
                && matches!(text, "L" | "u" | "U" | "u8")
            {
                return self.quoted(errors);
            }
            return Flavor::Ident;
        }
        if byte.is_ascii_digit() || (byte == b'.' && self.bytes.get(self.offset + 1).map(|next| next.is_ascii_digit()).unwrap_or(false)) {
            return self.number();
        }
        if byte == b'"' || byte == b'\'' {
            return self.quoted(errors);
        }
        self.punct(errors)
    }

    fn number(&mut self) -> Flavor {
        self.offset += 1;
        self.column += 1;
        while self.offset < self.bytes.len() {
            let byte = self.bytes[self.offset];
            let exponent_sign = matches!(byte, b'+' | b'-')
                && matches!(self.bytes.get(self.offset - 1), Some(b'e' | b'E' | b'p' | b'P'));
            if byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'.' || exponent_sign {
                self.offset += 1;
                self.column += 1;
            } else {
                break;
            }
        }
        Flavor::Number
    }

    fn quoted(&mut self, errors: &mut ErrorCollector) -> Flavor {
        let quote = self.bytes[self.offset];
        let kind = if quote == b'"' { Flavor::String } else { Flavor::Char };
        self.offset += 1;
        self.column += 1;
        while self.offset < self.bytes.len() {
            let byte = self.bytes[self.offset];
            if byte == b'\\' {
                self.offset += 2;
                self.column += 2;
                continue;
            }
            if byte == quote {
                self.offset += 1;
                self.column += 1;
                return kind;
            }
            if byte == b'\n' {
                errors.push(CompileError::new(
                    SourceLocation::new(self.file, self.line, self.column),
                    "unterminated literal",
                ));
                return kind;
            }
            self.offset += 1;
            self.column += 1;
        }
        errors.push(CompileError::new(
            SourceLocation::new(self.file, self.line, self.column),
            "unterminated literal",
        ));
        kind
    }

    fn punct(&mut self, errors: &mut ErrorCollector) -> Flavor {
        const PUNCTUATORS: &[&str] = &[
            "%:%:", ">>=", "<<=", "->*", "...", "##", "::", "->", ".*", "++", "--", "&&", "||",
            "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=", "<<", ">>",
            "<:", ":>", "<%", "%>", "%:", "(", ")", "{", "}", "[", "]", ";", ",", ".", "?", ":",
            "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "=", "<", ">", "#",
        ];
        for punct in PUNCTUATORS {
            if self.bytes[self.offset..].starts_with(punct.as_bytes()) {
                self.offset += punct.len();
                self.column += punct.len();
                return Flavor::Punct;
            }
        }
        let ch = self.source[self.offset..].chars().next().unwrap_or('?');
        errors.push(CompileError::new(
            SourceLocation::new(self.file, self.line, self.column),
            format!("unexpected character '{}'", ch),
        ));
        self.offset += ch.len_utf8();
        self.column += 1;
        Flavor::Punct
    }

    fn skip_trivia(&mut self, errors: &mut ErrorCollector) {
        loop {
            if self.offset >= self.bytes.len() {
                return;
            }
            let byte = self.bytes[self.offset];
            if byte == b'\\' {
                let next = self.bytes.get(self.offset + 1);
                if next == Some(&b'\n') {
                    self.offset += 2;
                    self.line += 1;
                    self.column = 1;
                    continue;
                }
                if next == Some(&b'\r') && self.bytes.get(self.offset + 2) == Some(&b'\n') {
                    self.offset += 3;
                    self.line += 1;
                    self.column = 1;
                    continue;
                }
                return;
            }
            if byte == b'\n' {
                self.offset += 1;
                self.line += 1;
                self.column = 1;
                self.line_start = true;
                self.space_before = true;
                continue;
            }
            if matches!(byte, b' ' | b'\t' | b'\r' | 0x0b | 0x0c) {
                self.offset += 1;
                self.column += 1;
                self.space_before = true;
                continue;
            }
            if byte == b'/' && self.bytes.get(self.offset + 1) == Some(&b'/') {
                while self.offset < self.bytes.len() && self.bytes[self.offset] != b'\n' {
                    self.offset += 1;
                }
                self.space_before = true;
                continue;
            }
            if byte == b'/' && self.bytes.get(self.offset + 1) == Some(&b'*') {
                let start_line = self.line;
                let start_column = self.column;
                self.offset += 2;
                self.column += 2;
                let mut closed = false;
                while self.offset < self.bytes.len() {
                    if self.bytes[self.offset] == b'\n' {
                        self.offset += 1;
                        self.line += 1;
                        self.column = 1;
                    } else if self.bytes[self.offset..].starts_with(b"*/") {
                        self.offset += 2;
                        self.column += 2;
                        closed = true;
                        break;
                    } else {
                        self.offset += 1;
                        self.column += 1;
                    }
                }
                if !closed {
                    errors.push(CompileError::new(
                        SourceLocation::new(self.file, start_line, start_column),
                        "unterminated block comment",
                    ));
                }
                self.space_before = true;
                continue;
            }
            return;
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
        assert!(errors.is_empty(), "{:?}", errors);
        assert!(out.contains("int x = 42;"), "{}", out);
        assert!(out.contains("\"VALUE\""));
    }

    #[test]
    fn strips_comments_but_preserves_quoted_comment_markers() {
        let (out, errors) = run("int a; // gone\nconst char* s = \"/* kept */\"; /* x\n y */ int b;\n");
        assert!(errors.is_empty(), "{:?}", errors);
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
        assert!(errors.is_empty(), "{:?}", errors);
        assert_eq!(out.text.matches("int included;").count(), 1);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn conditional_header_guards_select_one_branch() {
        let (out, errors) = run("#define X\n#ifdef X\nint a;\n#else\nint b;\n#endif\n");
        assert!(errors.is_empty(), "{:?}", errors);
        assert!(out.contains("int a;"));
        assert!(!out.contains("int b;"));
    }

    #[test]
    fn expands_function_like_and_variadic_macros() {
        let (out, errors) = run(
            "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n\
             #define LOG(fmt, ...) puts(fmt)\n\
             #define STR(x) #x\n\
             #define CAT(a, b) a##b\n\
             int v = MAX(1, 2); int CAT(fo, o) = 3; const char* t = STR(hi there);\n\
             void f() { LOG(\"x\"); }\n",
        );
        assert!(errors.is_empty(), "{:?}", errors);
        assert!(out.contains("((1) > (2) ? (1) : (2))"), "{}", out);
        assert!(out.contains("int foo = 3;"), "{}", out);
        assert!(out.contains("\"hi there\""), "{}", out);
        assert!(out.contains("puts(\"x\")"), "{}", out);
    }

    #[test]
    fn evaluates_if_expressions_with_defined_and_predefined_macros() {
        let (out, errors) = run(
            "#if defined(__cplusplus) && __cplusplus >= 201103L\n\
             int modern;\n\
             #elif 0\n\
             int legacy;\n\
             #else\n\
             int other;\n\
             #endif\n\
             #if !defined(FEATURE) && (1 + 2 * 3 == 7)\n\
             int arithmetic;\n\
             #endif\n",
        );
        assert!(errors.is_empty(), "{:?}", errors);
        assert!(out.contains("int modern;"), "{}", out);
        assert!(out.contains("int arithmetic;"), "{}", out);
        assert!(!out.contains("int legacy;"));
        assert!(!out.contains("int other;"));
    }

    #[test]
    fn supports_line_continuations_and_line_macros() {
        let (out, errors) = run("#define LONG(a, b) \\\n  ((a) + (b))\nint v = LONG(1, 2);\nint l = __LINE__;\n");
        assert!(errors.is_empty(), "{:?}", errors);
        assert!(out.contains("((1) + (2))"), "{}", out);
        assert!(out.contains("int l = 4;"), "{}", out);
    }

    #[test]
    fn includes_angled_headers_from_system_directories() {
        let dir = temp_dir();
        let sys = dir.join("sys");
        fs::create_dir_all(&sys).unwrap();
        fs::write(sys.join("native.h"), "int native_decl;\n").unwrap();
        let main = dir.join("main.cc");
        fs::write(&main, "#include <native.h>\n").unwrap();
        let (out, errors) = Preprocessor::new(vec![sys.clone()])
            .with_system_include_dirs(vec![sys])
            .preprocess(&main)
            .unwrap();
        assert!(errors.is_empty(), "{:?}", errors);
        assert!(out.text.contains("int native_decl;"));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn reports_error_directives_and_unmatched_conditionals() {
        let (_, errors) = run("#error stop here\n#endif\n");
        assert_eq!(errors.len(), 2, "{:?}", errors);
        assert!(errors.iter().all(|e| e.location.line > 0 && e.location.column > 0));
    }
}
