use crate::ast::*;
use crate::diagnostic::{CompileError, ErrorCollector, SourceLocation};
use crate::lexer::{LexOutput, Punct, Token, TokenKind};

#[derive(Debug)]
pub struct ParseOutput {
    pub unit: TranslationUnit,
    pub errors: Vec<CompileError>,
}

pub fn parse(tokens: Vec<Token>) -> ParseOutput {
    Parser::new(tokens).parse_unit()
}

pub fn parse_lexed(output: LexOutput) -> ParseOutput {
    let mut parsed = parse(output.tokens);
    parsed.errors.splice(0..0, output.errors);
    parsed
}

struct Parser {
    tokens: Vec<Token>,
    index: usize,
    errors: ErrorCollector,
}

impl Parser {
    fn new(tokens: Vec<Token>) -> Self {
        Self {
            tokens,
            index: 0,
            errors: ErrorCollector::default(),
        }
    }

    fn parse_unit(mut self) -> ParseOutput {
        let mut items = Vec::new();
        while !self.at_eof() {
            let before = self.index;
            match self.parse_top_level() {
                Ok(mut parsed) => items.append(&mut parsed),
                Err(error) => {
                    self.errors.push(error);
                    self.recover_top_level();
                }
            }
            if self.index == before {
                self.bump();
            }
        }
        ParseOutput {
            unit: TranslationUnit { items },
            errors: self.errors.into_errors(),
        }
    }

    fn parse_top_level(&mut self) -> Result<Vec<TopLevel>, CompileError> {
        if self.is_kw("struct")
            && (self.peek_n_punct(2, Punct::LBrace) || self.peek_n_punct(2, Punct::Semicolon))
        {
            return Ok(vec![TopLevel::Struct(self.parse_struct_definition()?)]);
        }
        let is_static = self.eat_kw("static");
        let is_extern = if is_static { false } else { self.eat_kw("extern") };
        let (base_type, _) = self.parse_type()?;
        let (name, ty, name_location) = self.parse_declarator(base_type)?;
        if self.eat_punct(Punct::LParen) {
            let params = self.parse_parameters_after_lparen()?;
            let body = if self.at_punct(Punct::LBrace) {
                Some(self.parse_block()?)
            } else {
                self.expect_punct(Punct::Semicolon, "expected ';' after function declaration")?;
                None
            };
            return Ok(vec![TopLevel::Function(Function {
                name,
                return_type: ty,
                params,
                body,
                is_static,
                location: name_location,
            })]);
        }
        let initializer = if self.eat_punct(Punct::Equal) {
            Some(self.parse_expression(1)?)
        } else {
            None
        };
        self.expect_punct(Punct::Semicolon, "expected ';' after global declaration")?;
        Ok(vec![TopLevel::Global(Variable {
            name,
            ty,
            initializer,
            is_static,
            is_extern,
            location: name_location,
        })])
    }

    fn parse_struct_definition(&mut self) -> Result<StructDecl, CompileError> {
        let location = self.current().location.clone();
        self.expect_kw("struct", "expected 'struct'")?;
        let (name, _) = self.expect_identifier("expected structure name")?;
        let mut fields = Vec::new();
        let is_definition = self.eat_punct(Punct::LBrace);
        if is_definition {
            while !self.at_punct(Punct::RBrace) && !self.at_eof() {
                let before = self.index;
                let field_result = (|| {
                    let (base, _) = self.parse_type()?;
                    let (field_name, ty, field_location) = self.parse_declarator(base)?;
                    self.expect_punct(Punct::Semicolon, "expected ';' after structure field")?;
                    Ok(Variable {
                        name: field_name,
                        ty,
                        initializer: None,
                        is_static: false,
                        is_extern: false,
                        location: field_location,
                    })
                })();
                match field_result {
                    Ok(field) => fields.push(field),
                    Err(error) => {
                        self.errors.push(error);
                        self.recover_statement();
                    }
                }
                if self.index == before {
                    self.bump();
                }
            }
            self.expect_punct(Punct::RBrace, "expected '}' after structure fields")?;
        }
        self.expect_punct(Punct::Semicolon, "expected ';' after structure declaration")?;
        Ok(StructDecl {
            name,
            fields,
            is_definition,
            location,
        })
    }

    fn parse_type(&mut self) -> Result<(Type, SourceLocation), CompileError> {
        let location = self.current().location.clone();
        let mut is_const = false;
        while self.eat_kw("const") {
            is_const = true;
        }
        let is_unsigned = if self.eat_kw("unsigned") {
            true
        } else {
            self.eat_kw("signed");
            false
        };
        let base = if self.eat_kw("void") {
            BaseType::Void
        } else if self.eat_kw("bool") {
            BaseType::Bool
        } else if self.eat_kw("char") {
            BaseType::Char
        } else if self.eat_kw("short") {
            self.eat_kw("int");
            BaseType::Short
        } else if self.eat_kw("long") {
            if self.eat_kw("long") {
                self.eat_kw("int");
                BaseType::LongLong
            } else {
                self.eat_kw("int");
                BaseType::Long
            }
        } else if self.eat_kw("int") {
            BaseType::Int
        } else if self.eat_kw("struct") {
            let (name, _) = self.expect_identifier("expected structure name after 'struct'")?;
            BaseType::Struct(name)
        } else {
            return Err(self.error_here("expected a type specifier"));
        };
        let mut ty = Type::new(base);
        ty.is_unsigned = is_unsigned;
        ty.is_const = is_const;
        if is_unsigned && matches!(&ty.base, BaseType::Void | BaseType::Bool | BaseType::Struct(_)) {
            return Err(CompileError::new(location, "unsigned is invalid for this type"));
        }
        Ok((ty, location))
    }

    fn parse_declarator(&mut self, mut ty: Type) -> Result<(String, Type, SourceLocation), CompileError> {
        while self.eat_punct(Punct::Star) {
            ty.pointers += 1;
            if self.eat_kw("const") {
                ty.is_const = true;
            }
        }
        let (name, location) = self.expect_identifier("expected declarator name")?;
        if self.eat_punct(Punct::LBracket) {
            let length = match self.bump().kind {
                TokenKind::Integer(value) if value > 0 => value as usize,
                _ => return Err(self.error_here("array length must be a positive integer constant")),
            };
            self.expect_punct(Punct::RBracket, "expected ']' after array length")?;
            ty.array_len = Some(length);
        }
        Ok((name, ty, location))
    }

    fn parse_parameters_after_lparen(&mut self) -> Result<Vec<Parameter>, CompileError> {
        let mut params = Vec::new();
        if self.eat_punct(Punct::RParen) {
            return Ok(params);
        }
        if self.is_kw("void") && self.peek_n_punct(1, Punct::RParen) {
            self.bump();
            self.bump();
            return Ok(params);
        }
        loop {
            let (base, _) = self.parse_type()?;
            let (name, ty, location) = if self.at_punct(Punct::Comma) || self.at_punct(Punct::RParen) {
                (None, base, self.current().location.clone())
            } else {
                let (name, ty, location) = self.parse_declarator(base)?;
                (Some(name), ty, location)
            };
            params.push(Parameter { name, ty, location });
            if self.eat_punct(Punct::Comma) {
                continue;
            }
            self.expect_punct(Punct::RParen, "expected ')' after parameters")?;
            return Ok(params);
        }
    }

    fn parse_block(&mut self) -> Result<Stmt, CompileError> {
        let location = self.current().location.clone();
        self.expect_punct(Punct::LBrace, "expected '{'")?;
        let mut statements = Vec::new();
        while !self.at_punct(Punct::RBrace) && !self.at_eof() {
            let before = self.index;
            match self.parse_statement() {
                Ok(statement) => statements.push(statement),
                Err(error) => {
                    self.errors.push(error);
                    self.recover_statement();
                }
            }
            if self.index == before {
                self.bump();
            }
        }
        self.expect_punct(Punct::RBrace, "expected '}' to close block")?;
        Ok(Stmt { kind: StmtKind::Block(statements), location })
    }

    fn parse_statement(&mut self) -> Result<Stmt, CompileError> {
        let location = self.current().location.clone();
        if self.at_punct(Punct::LBrace) {
            return self.parse_block();
        }
        if self.eat_punct(Punct::Semicolon) {
            return Ok(Stmt { kind: StmtKind::Empty, location });
        }
        if self.eat_kw("if") {
            self.expect_punct(Punct::LParen, "expected '(' after if")?;
            let condition = self.parse_expression(1)?;
            self.expect_punct(Punct::RParen, "expected ')' after if condition")?;
            let then_branch = Box::new(self.parse_statement()?);
            let else_branch = if self.eat_kw("else") {
                Some(Box::new(self.parse_statement()?))
            } else {
                None
            };
            return Ok(Stmt { kind: StmtKind::If { condition, then_branch, else_branch }, location });
        }
        if self.eat_kw("while") {
            self.expect_punct(Punct::LParen, "expected '(' after while")?;
            let condition = self.parse_expression(1)?;
            self.expect_punct(Punct::RParen, "expected ')' after while condition")?;
            let body = Box::new(self.parse_statement()?);
            return Ok(Stmt { kind: StmtKind::While { condition, body }, location });
        }
        if self.eat_kw("for") {
            self.expect_punct(Punct::LParen, "expected '(' after for")?;
            let init = if self.eat_punct(Punct::Semicolon) {
                None
            } else if self.starts_type() || self.is_kw("static") {
                Some(Box::new(self.parse_local_declaration()?))
            } else {
                let expression = self.parse_expression(1)?;
                self.expect_punct(Punct::Semicolon, "expected ';' after for initializer")?;
                Some(Box::new(Stmt { kind: StmtKind::Expression(expression), location: location.clone() }))
            };
            let condition = if self.at_punct(Punct::Semicolon) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::Semicolon, "expected ';' after for condition")?;
            let step = if self.at_punct(Punct::RParen) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::RParen, "expected ')' after for clauses")?;
            let body = Box::new(self.parse_statement()?);
            return Ok(Stmt { kind: StmtKind::For { init, condition, step, body }, location });
        }
        if self.eat_kw("return") {
            let expression = if self.at_punct(Punct::Semicolon) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::Semicolon, "expected ';' after return")?;
            return Ok(Stmt { kind: StmtKind::Return(expression), location });
        }
        if self.eat_kw("break") {
            self.expect_punct(Punct::Semicolon, "expected ';' after break")?;
            return Ok(Stmt { kind: StmtKind::Break, location });
        }
        if self.eat_kw("continue") {
            self.expect_punct(Punct::Semicolon, "expected ';' after continue")?;
            return Ok(Stmt { kind: StmtKind::Continue, location });
        }
        if self.is_kw("static") || self.starts_type() {
            return self.parse_local_declaration();
        }
        let expression = self.parse_expression(1)?;
        self.expect_punct(Punct::Semicolon, "expected ';' after expression")?;
        Ok(Stmt { kind: StmtKind::Expression(expression), location })
    }

    fn parse_local_declaration(&mut self) -> Result<Stmt, CompileError> {
        let location = self.current().location.clone();
        let is_static = self.eat_kw("static");
        let (base, _) = self.parse_type()?;
        let (name, ty, name_location) = self.parse_declarator(base)?;
        let initializer = if self.eat_punct(Punct::Equal) { Some(self.parse_expression(1)?) } else { None };
        self.expect_punct(Punct::Semicolon, "expected ';' after variable declaration")?;
        Ok(Stmt { kind: StmtKind::Variable(Variable {
            name,
            ty,
            initializer,
            is_static,
            is_extern: false,
            location: name_location,
        }), location })
    }

    fn parse_expression(&mut self, min_precedence: u8) -> Result<Expr, CompileError> {
        let mut left = self.parse_unary()?;
        loop {
            if self.at_punct(Punct::Question) && min_precedence <= 2 {
                let location = left.location.clone();
                self.bump();
                let then_expr = self.parse_expression(1)?;
                self.expect_punct(Punct::Colon, "expected ':' in conditional expression")?;
                let else_expr = self.parse_expression(1)?;
                left = Expr {
                    kind: ExprKind::Ternary {
                        condition: Box::new(left),
                        then_expr: Box::new(then_expr),
                        else_expr: Box::new(else_expr),
                    },
                    location,
                    ty: None,
                };
                continue;
            }
            let Some((precedence, right_associative, op)) = self.binary_operator() else { break };
            if precedence < min_precedence {
                break;
            }
            let location = left.location.clone();
            self.bump();
            let right = self.parse_expression(if right_associative { precedence } else { precedence + 1 })?;
            left = Expr {
                kind: ExprKind::Binary { op, left: Box::new(left), right: Box::new(right) },
                location,
                ty: None,
            };
        }
        Ok(left)
    }

    fn parse_unary(&mut self) -> Result<Expr, CompileError> {
        let location = self.current().location.clone();
        let op = match &self.current().kind {
            TokenKind::Punct(Punct::Plus) => Some("+"),
            TokenKind::Punct(Punct::Minus) => Some("-"),
            TokenKind::Punct(Punct::Bang) => Some("!"),
            TokenKind::Punct(Punct::Tilde) => Some("~"),
            TokenKind::Punct(Punct::Star) => Some("*"),
            TokenKind::Punct(Punct::Amp) => Some("&"),
            TokenKind::Punct(Punct::PlusPlus) => Some("++"),
            TokenKind::Punct(Punct::MinusMinus) => Some("--"),
            _ => None,
        };
        if let Some(op) = op {
            self.bump();
            let operand = self.parse_unary()?;
            return Ok(Expr { kind: ExprKind::Unary { op: op.to_owned(), operand: Box::new(operand) }, location, ty: None });
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_primary()?;
        loop {
            let location = expr.location.clone();
            if self.eat_punct(Punct::LParen) {
                let mut args = Vec::new();
                if !self.at_punct(Punct::RParen) {
                    loop {
                        args.push(self.parse_expression(1)?);
                        if !self.eat_punct(Punct::Comma) { break; }
                    }
                }
                self.expect_punct(Punct::RParen, "expected ')' after call arguments")?;
                expr = Expr { kind: ExprKind::Call { callee: Box::new(expr), args }, location, ty: None };
            } else if self.eat_punct(Punct::LBracket) {
                let index = self.parse_expression(1)?;
                self.expect_punct(Punct::RBracket, "expected ']' after subscript")?;
                expr = Expr { kind: ExprKind::Index { base: Box::new(expr), index: Box::new(index) }, location, ty: None };
            } else if self.eat_punct(Punct::Dot) || self.eat_punct(Punct::Arrow) {
                let through_pointer = matches!(&self.previous().kind, TokenKind::Punct(Punct::Arrow));
                let (name, _) = self.expect_identifier("expected member name")?;
                expr = Expr { kind: ExprKind::Member { base: Box::new(expr), name, through_pointer }, location, ty: None };
            } else if self.eat_punct(Punct::PlusPlus) {
                expr = Expr { kind: ExprKind::Postfix { op: "++".to_owned(), operand: Box::new(expr) }, location, ty: None };
            } else if self.eat_punct(Punct::MinusMinus) {
                expr = Expr { kind: ExprKind::Postfix { op: "--".to_owned(), operand: Box::new(expr) }, location, ty: None };
            } else {
                break;
            }
        }
        Ok(expr)
    }

    fn parse_primary(&mut self) -> Result<Expr, CompileError> {
        let token = self.current().clone();
        let kind = match token.kind {
            TokenKind::Integer(value) => ExprKind::Integer(value),
            TokenKind::Character(value) => ExprKind::Character(value),
            TokenKind::StringLiteral(value) => ExprKind::String(value),
            TokenKind::Identifier(name) => ExprKind::Identifier(name),
            TokenKind::Keyword(ref word) if word == "true" => ExprKind::Boolean(true),
            TokenKind::Keyword(ref word) if word == "false" => ExprKind::Boolean(false),
            TokenKind::Punct(Punct::LParen) => {
                self.bump();
                let expr = self.parse_expression(1)?;
                self.expect_punct(Punct::RParen, "expected ')' after expression")?;
                return Ok(expr);
            }
            TokenKind::Keyword(word) => return Err(CompileError::new(token.location, format!("unsupported keyword '{}' in expression", word))),
            _ => return Err(CompileError::new(token.location, "expected expression")),
        };
        self.bump();
        Ok(Expr { kind, location: token.location, ty: None })
    }

    fn binary_operator(&self) -> Option<(u8, bool, String)> {
        let punct = match &self.current().kind { TokenKind::Punct(p) => *p, _ => return None };
        let entry = match punct {
            Punct::Equal => (1, true, "="),
            Punct::PlusEqual => (1, true, "+="), Punct::MinusEqual => (1, true, "-="),
            Punct::StarEqual => (1, true, "*="), Punct::SlashEqual => (1, true, "/="),
            Punct::PercentEqual => (1, true, "%="), Punct::AmpEqual => (1, true, "&="),
            Punct::PipeEqual => (1, true, "|="), Punct::CaretEqual => (1, true, "^="),
            Punct::ShiftLeftEqual => (1, true, "<<="), Punct::ShiftRightEqual => (1, true, ">>="),
            Punct::PipePipe => (3, false, "||"), Punct::AmpAmp => (4, false, "&&"),
            Punct::Pipe => (5, false, "|"), Punct::Caret => (6, false, "^"),
            Punct::Amp => (7, false, "&"), Punct::EqualEqual => (8, false, "=="),
            Punct::BangEqual => (8, false, "!="), Punct::Less => (9, false, "<"),
            Punct::LessEqual => (9, false, "<="), Punct::Greater => (9, false, ">"),
            Punct::GreaterEqual => (9, false, ">="), Punct::ShiftLeft => (10, false, "<<"),
            Punct::ShiftRight => (10, false, ">>"), Punct::Plus => (11, false, "+"),
            Punct::Minus => (11, false, "-"), Punct::Star => (12, false, "*"),
            Punct::Slash => (12, false, "/"), Punct::Percent => (12, false, "%"),
            _ => return None,
        };
        Some((entry.0, entry.1, entry.2.to_owned()))
    }

    fn starts_type(&self) -> bool {
        matches!(&self.current().kind, TokenKind::Keyword(word) if matches!(word.as_str(), "const" | "unsigned" | "signed" | "void" | "bool" | "char" | "short" | "int" | "long" | "struct"))
    }

    fn recover_top_level(&mut self) {
        while !self.at_eof() {
            if self.eat_punct(Punct::Semicolon) { return; }
            if self.at_punct(Punct::RBrace) { self.bump(); return; }
            self.bump();
        }
    }

    fn recover_statement(&mut self) {
        while !self.at_eof() && !self.at_punct(Punct::RBrace) {
            if self.eat_punct(Punct::Semicolon) { return; }
            self.bump();
        }
    }

    fn expect_identifier(&mut self, message: &str) -> Result<(String, SourceLocation), CompileError> {
        let token = self.current().clone();
        if let TokenKind::Identifier(name) = token.kind {
            self.bump();
            Ok((name, token.location))
        } else {
            Err(CompileError::new(token.location, message))
        }
    }

    fn expect_kw(&mut self, word: &str, message: &str) -> Result<(), CompileError> {
        if self.eat_kw(word) { Ok(()) } else { Err(self.error_here(message)) }
    }

    fn expect_punct(&mut self, punct: Punct, message: &str) -> Result<(), CompileError> {
        if self.eat_punct(punct) { Ok(()) } else { Err(self.error_here(message)) }
    }

    fn eat_kw(&mut self, word: &str) -> bool {
        if self.is_kw(word) { self.bump(); true } else { false }
    }

    fn is_kw(&self, word: &str) -> bool {
        matches!(&self.current().kind, TokenKind::Keyword(found) if found == word)
    }

    fn eat_punct(&mut self, punct: Punct) -> bool {
        if self.at_punct(punct) { self.bump(); true } else { false }
    }

    fn at_punct(&self, punct: Punct) -> bool {
        matches!(&self.current().kind, TokenKind::Punct(found) if *found == punct)
    }

    fn peek_n_punct(&self, n: usize, punct: Punct) -> bool {
        matches!(self.tokens.get(self.index + n).map(|t| &t.kind), Some(TokenKind::Punct(found)) if *found == punct)
    }

    fn at_eof(&self) -> bool {
        matches!(&self.current().kind, TokenKind::Eof)
    }

    fn current(&self) -> &Token {
        self.tokens.get(self.index).or_else(|| self.tokens.last()).expect("parser requires EOF token")
    }

    fn previous(&self) -> &Token {
        &self.tokens[self.index.saturating_sub(1)]
    }

    fn bump(&mut self) -> Token {
        let token = self.current().clone();
        if !matches!(&token.kind, TokenKind::Eof) {
            self.index += 1;
        }
        token
    }

    fn error_here(&self, message: &str) -> CompileError {
        CompileError::new(self.current().location.clone(), message)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lexer::lex;
    use crate::preprocess::PreprocessedSource;
    use std::path::PathBuf;

    fn parse_source(text: &str) -> ParseOutput {
        let locations = (0..text.len()).map(|i| SourceLocation::new("parser.cc", 1, i + 1)).collect();
        parse_lexed(lex(&PreprocessedSource { text: text.to_owned(), locations }))
    }

    #[test]
    fn parses_functions_structs_and_control_flow() {
        let output = parse_source("struct Pair { int x; int y; }; int add(int a, int b) { int s = a + b; if (s > 0) return s; else return 0; } int main() { for (int i=0; i<3; i++) { while (i < 2) break; } return add(1, 2); }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
        assert_eq!(output.unit.items.len(), 3);
        assert!(matches!(output.unit.items[0], TopLevel::Struct(_)));
        assert!(matches!(output.unit.items[1], TopLevel::Function(_)));
    }

    #[test]
    fn expression_precedence_and_ternary_are_structured() {
        let output = parse_source("int f() { return 1 + 2 * 3 < 8 ? 4 : 5; }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
        let TopLevel::Function(function) = &output.unit.items[0] else { panic!() };
        let StmtKind::Block(body) = &function.body.as_ref().unwrap().kind else { panic!() };
        let StmtKind::Return(Some(expr)) = &body[0].kind else { panic!() };
        assert!(matches!(&expr.kind, ExprKind::Ternary { .. }));
    }

    #[test]
    fn assignment_and_ternary_precedence_are_right_associative() {
        let output = parse_source("int f() { a = b ? c || d && e | f ^ g & h == i < j << k + l * m : n = 3; return 0; }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
        let TopLevel::Function(function) = &output.unit.items[0] else { panic!() };
        let StmtKind::Block(body) = &function.body.as_ref().unwrap().kind else { panic!() };
        let StmtKind::Expression(Expr { kind: ExprKind::Binary { op, right, .. }, .. }) = &body[0].kind else { panic!() };
        assert_eq!(op, "=");
        let ExprKind::Ternary { else_expr, .. } = &right.kind else { panic!() };
        assert!(matches!(&else_expr.kind, ExprKind::Binary { op, .. } if op == "="));
    }

    #[test]
    fn parser_recovers_and_reports_multiple_syntax_errors() {
        let output = parse_source("int f( { return ; } int g() { if (1 return 2; } int ok;");
        assert!(output.errors.len() >= 2);
        assert!(output.errors.iter().all(|e| e.location.line == 1 && e.location.column > 0));
    }

    #[test]
    fn reports_missing_semicolon_at_eof() {
        let output = parse_source("int value");
        assert!(!output.errors.is_empty());
        assert_eq!(output.errors[0].location.file, PathBuf::from("parser.cc"));
    }

    #[test]
    fn syntax_error_formats_real_multiline_location() {
        let text = "int f() {\n return 1\n}";
        let mut line = 1usize;
        let mut column = 1usize;
        let locations = text.bytes().map(|byte| {
            let location = SourceLocation::new("parser.cc", line, column);
            if byte == b'\n' { line += 1; column = 1; } else { column += 1; }
            location
        }).collect();
        let parsed = parse_lexed(lex(&PreprocessedSource { text: text.to_owned(), locations }));
        assert_eq!(parsed.errors[0].to_string(), "parser.cc:3:1: error: expected ';' after return");
    }
}