use crate::diagnostic::{CompileError, ErrorCollector, SourceLocation};
use crate::preprocess::PreprocessedSource;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Token {
    pub kind: TokenKind,
    pub location: SourceLocation,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TokenKind {
    Identifier(String),
    Keyword(String),
    Integer(i64),
    StringLiteral(Vec<u8>),
    Character(i64),
    Punct(Punct),
    Eof,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Punct {
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Semicolon, Comma, Dot, DotStar, Arrow, ArrowStar, Scope, Ellipsis, Question, Colon,
    Plus, Minus, Star, Slash, Percent,
    PlusPlus, MinusMinus,
    Amp, Pipe, Caret, Tilde, Bang,
    AmpAmp, PipePipe,
    Equal, PlusEqual, MinusEqual, StarEqual, SlashEqual, PercentEqual,
    AmpEqual, PipeEqual, CaretEqual, ShiftLeftEqual, ShiftRightEqual,
    EqualEqual, BangEqual, Less, LessEqual, Greater, GreaterEqual,
    ShiftLeft, ShiftRight,
    Hash, HashHash,
}

#[derive(Debug)]
pub struct LexOutput {
    pub tokens: Vec<Token>,
    pub errors: Vec<CompileError>,
}

pub fn lex(source: &PreprocessedSource) -> LexOutput {
    Scanner::new(source).scan()
}

struct Scanner<'a> {
    source: &'a PreprocessedSource,
    text: &'a str,
    bytes: &'a [u8],
    offset: usize,
    tokens: Vec<Token>,
    errors: ErrorCollector,
}

impl<'a> Scanner<'a> {
    fn new(source: &'a PreprocessedSource) -> Self {
        Self {
            source,
            text: &source.text,
            bytes: source.text.as_bytes(),
            offset: 0,
            tokens: Vec::new(),
            errors: ErrorCollector::default(),
        }
    }

    fn scan(mut self) -> LexOutput {
        while self.offset < self.bytes.len() {
            if self.bytes[self.offset].is_ascii_whitespace() {
                self.offset += 1;
                continue;
            }
            let start = self.offset;
            let location = self.source.location_at(start);
            match self.bytes[self.offset] {
                b'_' | b'a'..=b'z' | b'A'..=b'Z' => self.identifier(location),
                b'0'..=b'9' => self.number(location),
                b'"' => self.quoted(location, b'"'),
                b'\'' => self.character(location),
                _ => self.punctuation(location),
            }
        }
        self.tokens.push(Token {
            kind: TokenKind::Eof,
            location: self.source.location_at(self.offset),
        });
        LexOutput {
            tokens: self.tokens,
            errors: self.errors.into_errors(),
        }
    }

    fn identifier(&mut self, location: SourceLocation) {
        let start = self.offset;
        self.offset += 1;
        while self.offset < self.bytes.len()
            && (self.bytes[self.offset] == b'_' || self.bytes[self.offset].is_ascii_alphanumeric())
        {
            self.offset += 1;
        }
        let name = self.text[start..self.offset].to_owned();
        let kind = if is_keyword(&name) {
            TokenKind::Keyword(name)
        } else {
            TokenKind::Identifier(name)
        };
        self.tokens.push(Token { kind, location });
    }

    fn number(&mut self, location: SourceLocation) {
        let start = self.offset;
        let hex = self.bytes.get(self.offset..self.offset + 2) == Some(b"0x")
            || self.bytes.get(self.offset..self.offset + 2) == Some(b"0X");
        if hex {
            self.offset += 2;
            let digits = self.offset;
            while self.offset < self.bytes.len() && self.bytes[self.offset].is_ascii_hexdigit() {
                self.offset += 1;
            }
            if self.offset == digits {
                self.errors.push(CompileError::new(location, "hexadecimal literal has no digits"));
                return;
            }
        } else {
            while self.offset < self.bytes.len() && self.bytes[self.offset].is_ascii_digit() {
                self.offset += 1;
            }
        }
        let end_digits = self.offset;
        while self.offset < self.bytes.len()
            && matches!(self.bytes[self.offset], b'u' | b'U' | b'l' | b'L')
        {
            self.offset += 1;
        }
        if self.offset < self.bytes.len()
            && (self.bytes[self.offset] == b'_' || self.bytes[self.offset].is_ascii_alphabetic())
        {
            self.offset += 1;
            while self.offset < self.bytes.len()
                && (self.bytes[self.offset] == b'_' || self.bytes[self.offset].is_ascii_alphanumeric())
            {
                self.offset += 1;
            }
            self.errors.push(CompileError::new(location, "invalid integer literal suffix"));
            return;
        }
        let digits = &self.text[start..end_digits];
        let value = if hex {
            i64::from_str_radix(&digits[2..], 16)
        } else {
            digits.parse::<i64>()
        };
        match value {
            Ok(value) => self.tokens.push(Token { kind: TokenKind::Integer(value), location }),
            Err(_) => self.errors.push(CompileError::new(location, "integer literal is out of range")),
        }
    }

    fn quoted(&mut self, location: SourceLocation, quote: u8) {
        self.offset += 1;
        let mut value = Vec::new();
        while self.offset < self.bytes.len() {
            let byte = self.bytes[self.offset];
            if byte == quote {
                self.offset += 1;
                self.tokens.push(Token { kind: TokenKind::StringLiteral(value), location });
                return;
            }
            if byte == b'\n' || byte == b'\r' {
                self.errors.push(CompileError::new(location, "newline in string literal"));
                return;
            }
            if byte == b'\\' {
                self.offset += 1;
                match self.escape(location.clone()) {
                    Some(byte) => value.push(byte),
                    None => return,
                }
            } else {
                let ch = self.text[self.offset..].chars().next().unwrap();
                let mut encoded = [0; 4];
                value.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
                self.offset += ch.len_utf8();
            }
        }
        self.errors.push(CompileError::new(location, "unterminated string literal"));
    }

    fn character(&mut self, location: SourceLocation) {
        self.offset += 1;
        let value = if self.offset >= self.bytes.len() {
            None
        } else if self.bytes[self.offset] == b'\\' {
            self.offset += 1;
            self.escape(location.clone())
        } else if self.bytes[self.offset] == b'\'' || self.bytes[self.offset] == b'\n' {
            None
        } else {
            let ch = self.text[self.offset..].chars().next().unwrap();
            let mut encoded = [0; 4];
            let bytes = ch.encode_utf8(&mut encoded).as_bytes();
            self.offset += ch.len_utf8();
            if bytes.len() == 1 { Some(bytes[0]) } else { None }
        };
        if self.bytes.get(self.offset) != Some(&b'\'') {
            self.errors.push(CompileError::new(location, "character literal must contain one byte"));
            self.recover_quote(b'\'');
            return;
        }
        self.offset += 1;
        if let Some(value) = value {
            self.tokens.push(Token { kind: TokenKind::Character(value as i64), location });
        } else {
            self.errors.push(CompileError::new(location, "invalid character literal"));
        }
    }

    fn escape(&mut self, location: SourceLocation) -> Option<u8> {
        if self.offset >= self.bytes.len() {
            self.errors.push(CompileError::new(location, "incomplete escape sequence"));
            return None;
        }
        let byte = self.bytes[self.offset];
        self.offset += 1;
        let value = match byte {
            b'\\' => b'\\', b'\'' => b'\'', b'"' => b'"', b'?' => b'?',
            b'a' => 0x07, b'b' => 0x08, b'f' => 0x0c, b'n' => b'\n',
            b'r' => b'\r', b't' => b'\t', b'v' => 0x0b,
            b'x' => {
                let start = self.offset;
                while self.offset < self.bytes.len() && self.bytes[self.offset].is_ascii_hexdigit() {
                    self.offset += 1;
                }
                if start == self.offset {
                    self.errors.push(CompileError::new(location, "hex escape requires digits"));
                    return None;
                }
                match u8::from_str_radix(&self.text[start..self.offset], 16) {
                    Ok(value) => value,
                    Err(_) => {
                        self.errors.push(CompileError::new(location, "hex escape is out of range"));
                        return None;
                    }
                }
            }
            b'0'..=b'7' => {
                let start = self.offset - 1;
                for _ in 0..2 {
                    if self.offset < self.bytes.len() && matches!(self.bytes[self.offset], b'0'..=b'7') {
                        self.offset += 1;
                    } else {
                        break;
                    }
                }
                u8::from_str_radix(&self.text[start..self.offset], 8).ok()?
            }
            _ => {
                self.errors.push(CompileError::new(location, "unknown escape sequence"));
                return None;
            }
        };
        Some(value)
    }

    fn recover_quote(&mut self, quote: u8) {
        while self.offset < self.bytes.len() && self.bytes[self.offset] != quote && self.bytes[self.offset] != b'\n' {
            self.offset += 1;
        }
        if self.bytes.get(self.offset) == Some(&quote) {
            self.offset += 1;
        }
    }

    fn punctuation(&mut self, location: SourceLocation) {
        const PUNCT: &[(&[u8], Punct)] = &[
            (b"%:%:", Punct::HashHash), (b"->*", Punct::ArrowStar), (b"...", Punct::Ellipsis),
            (b">>=", Punct::ShiftRightEqual), (b"<<=", Punct::ShiftLeftEqual),
            (b"##", Punct::HashHash), (b"::", Punct::Scope), (b".*", Punct::DotStar),
            (b"++", Punct::PlusPlus), (b"--", Punct::MinusMinus), (b"->", Punct::Arrow),
            (b"&&", Punct::AmpAmp), (b"||", Punct::PipePipe), (b"+=", Punct::PlusEqual),
            (b"-=", Punct::MinusEqual), (b"*=", Punct::StarEqual), (b"/=", Punct::SlashEqual),
            (b"%=", Punct::PercentEqual), (b"&=", Punct::AmpEqual), (b"|=", Punct::PipeEqual),
            (b"^=", Punct::CaretEqual), (b"==", Punct::EqualEqual), (b"!=", Punct::BangEqual),
            (b"<=", Punct::LessEqual), (b">=", Punct::GreaterEqual), (b"<<", Punct::ShiftLeft),
            (b">>", Punct::ShiftRight), (b"(", Punct::LParen), (b")", Punct::RParen),
            (b"<:", Punct::LBracket), (b":>", Punct::RBracket),
            (b"<%", Punct::LBrace), (b"%>", Punct::RBrace), (b"%:", Punct::Hash),
            (b"{", Punct::LBrace), (b"}", Punct::RBrace), (b"[", Punct::LBracket),
            (b"]", Punct::RBracket), (b";", Punct::Semicolon), (b",", Punct::Comma),
            (b".", Punct::Dot), (b"?", Punct::Question), (b":", Punct::Colon),
            (b"+", Punct::Plus), (b"-", Punct::Minus), (b"*", Punct::Star),
            (b"/", Punct::Slash), (b"%", Punct::Percent), (b"&", Punct::Amp),
            (b"|", Punct::Pipe), (b"^", Punct::Caret), (b"~", Punct::Tilde),
            (b"!", Punct::Bang), (b"=", Punct::Equal), (b"<", Punct::Less),
            (b">", Punct::Greater), (b"#", Punct::Hash),
        ];
        for (text, punct) in PUNCT {
            if self.bytes[self.offset..].starts_with(text) {
                self.offset += text.len();
                self.tokens.push(Token { kind: TokenKind::Punct(*punct), location });
                return;
            }
        }
        let ch = self.text[self.offset..].chars().next().unwrap();
        self.errors.push(CompileError::new(location, format!("unexpected character '{}'.", ch)));
        self.offset += ch.len_utf8();
    }
}

fn is_keyword(word: &str) -> bool {
    matches!(word,
        "void" | "bool" | "char" | "short" | "int" | "long" | "unsigned" | "signed" |
        "const" | "static" | "extern" | "struct" | "if" | "else" | "while" | "for" |
        "return" | "break" | "continue" | "true" | "false" | "class" | "namespace" |
        "template" | "typename" | "enum" | "union" | "switch" | "case" | "default" |
        "do" | "try" | "catch" | "throw" | "new" | "delete" | "using" | "virtual" |
        "public" | "private" | "protected" | "operator" | "this" | "volatile" | "inline" |
        "friend" | "typedef" | "sizeof" | "auto" | "register" | "mutable" | "goto" |
        "wchar_t" | "float" | "double" | "asm" | "typeid" | "dynamic_cast" | "static_cast" |
        "reinterpret_cast" | "const_cast" | "export" | "explicit" | "and" | "and_eq" |
        "bitand" | "bitor" | "compl" | "not" | "not_eq" | "or" | "or_eq" | "xor" | "xor_eq")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::preprocess::PreprocessedSource;
    use std::path::PathBuf;

    fn scan(text: &str) -> LexOutput {
        let locations = (0..text.len())
            .map(|i| SourceLocation::new("test.cc", 1, i + 1))
            .collect();
        lex(&PreprocessedSource { text: text.to_owned(), locations })
    }

    #[test]
    fn recognizes_all_punctuation_tokens() {
        let input = "( ) { } [ ] ; , . .* -> ->* :: ... ? : + - * / % ++ -- & | ^ ~ ! && || = += -= *= /= %= &= |= ^= <<= >>= == != < <= > >= << >> # ## <: :> <% %> %: %:%:";
        let out = scan(input);
        assert!(out.errors.is_empty(), "{:?}", out.errors);
        let puncts: Vec<_> = out.tokens.iter().filter_map(|t| match &t.kind {
            TokenKind::Punct(p) => Some(*p), _ => None,
        }).collect();
        assert_eq!(puncts.len(), 57);
    }

    #[test]
    fn recognizes_supported_and_rejected_keywords() {
        let out = scan("void bool char short int long unsigned signed const static extern struct if else while for return break continue true false class namespace");
        assert!(out.errors.is_empty());
        assert_eq!(out.tokens.iter().filter(|t| matches!(&t.kind, TokenKind::Keyword(_))).count(), 23);
    }

    #[test]
    fn parses_decimal_hex_strings_and_chars() {
        let out = scan("123 0x2a 0XFF \"hi\\n\" 'A' '\\t'");
        assert!(out.errors.is_empty(), "{:?}", out.errors);
        assert_eq!(out.tokens[0].kind, TokenKind::Integer(123));
        assert_eq!(out.tokens[1].kind, TokenKind::Integer(42));
        assert_eq!(out.tokens[2].kind, TokenKind::Integer(255));
        assert_eq!(out.tokens[3].kind, TokenKind::StringLiteral(b"hi\n".to_vec()));
        assert_eq!(out.tokens[4].kind, TokenKind::Character(65));
        assert_eq!(out.tokens[5].kind, TokenKind::Character(9));
    }

    #[test]
    fn parses_octal_and_hex_escape_sequences() {
        let out = scan("'\\0' '\\101' \"\\x41\"");
        assert!(out.errors.is_empty(), "{:?}", out.errors);
        assert_eq!(out.tokens[0].kind, TokenKind::Character(0));
        assert_eq!(out.tokens[1].kind, TokenKind::Character(65));
        assert_eq!(out.tokens[2].kind, TokenKind::StringLiteral(b"A".to_vec()));
    }

    #[test]
    fn tracks_source_locations_and_reports_bad_tokens() {
        let mut source = scan("int @;\n");
        assert_eq!(source.errors[0].location.column, 5);
        assert_eq!(source.errors[0].location.file, PathBuf::from("test.cc"));
        assert_eq!(source.errors[0].location.line, 1);
        source = scan("\"unterminated");
        assert_eq!(source.errors[0].location.column, 1);
    }
}