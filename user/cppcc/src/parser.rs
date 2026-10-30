use crate::ast::*;
use crate::diagnostic::{CompileError, ErrorCollector, SourceLocation};
use crate::lexer::{LexOutput, Punct, Token, TokenKind};
use std::collections::HashMap;

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

#[derive(Default)]
struct DeclSpecifiers {
    is_static: bool,
    is_extern: bool,
    is_constexpr: bool,
}

struct ParameterList {
    params: Vec<Parameter>,
    is_variadic: bool,
}

struct Parser {
    tokens: Vec<Token>,
    index: usize,
    errors: ErrorCollector,
    /// Names introduced by `typedef`, `using X = T;` and `enum`, so that later
    /// declarations can use them as type specifiers.
    type_names: HashMap<String, Type>,
    anonymous_structs: usize,
}

impl Parser {
    fn new(tokens: Vec<Token>) -> Self {
        Self {
            tokens,
            index: 0,
            errors: ErrorCollector::default(),
            type_names: HashMap::new(),
            anonymous_structs: 0,
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
        self.skip_attributes();
        if self.is_kw("namespace") {
            return self.parse_namespace();
        }
        if self.is_kw("extern") && matches!(self.peek_n(1), Some(TokenKind::StringLiteral(_))) {
            self.bump();
            self.bump();
            if self.eat_punct(Punct::LBrace) {
                let mut items = Vec::new();
                while !self.at_punct(Punct::RBrace) && !self.at_eof() {
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
                self.expect_punct(Punct::RBrace, "expected '}' after extern \"C\" block")?;
                return Ok(items);
            }
            return self.parse_top_level();
        }
        if self.is_kw("using") {
            return self.parse_using();
        }
        if self.is_kw("typedef") {
            return self.parse_typedef();
        }
        if self.is_kw("enum") {
            return self.parse_enum();
        }
        if self.is_kw("static_assert") {
            let assertion = self.parse_static_assert()?;
            return Ok(vec![TopLevel::StaticAssert(assertion)]);
        }
        if self.is_kw("template") {
            return Err(self.error_here("templates are not supported by cppcc"));
        }
        if self.is_kw("class") {
            return Err(self.error_here("class types are not supported by cppcc; use a POD struct"));
        }
        if self.is_kw("union") {
            return Err(self.error_here("unions are not supported by cppcc"));
        }
        if self.is_kw("struct")
            && (self.peek_n_punct(2, Punct::LBrace) || self.peek_n_punct(2, Punct::Semicolon))
        {
            return Ok(vec![TopLevel::Struct(self.parse_struct_definition()?)]);
        }

        let specifiers = self.parse_decl_specifiers()?;
        let base_type = self.parse_type()?;
        let (name, ty, name_location) = self.parse_declarator(base_type)?;
        if self.at_punct(Punct::LParen) {
            self.bump();
            let ParameterList { params, is_variadic } = self.parse_parameters_after_lparen()?;
            let mut return_type = ty;
            if self.eat_punct(Punct::Arrow) {
                return_type = self.parse_type_identifier()?;
            }
            self.skip_function_qualifiers();
            let body = if self.at_punct(Punct::LBrace) {
                Some(self.parse_block()?)
            } else {
                self.skip_pure_or_defaulted();
                self.expect_punct(Punct::Semicolon, "expected ';' after function declaration")?;
                None
            };
            return Ok(vec![TopLevel::Function(Function {
                name,
                return_type,
                params,
                body,
                is_static: specifiers.is_static,
                is_variadic,
                location: name_location,
            })]);
        }
        if self.at_punct(Punct::Comma) {
            return Err(self.error_here("only one declarator per declaration is supported"));
        }
        let initializer = if self.eat_punct(Punct::Equal) || (specifiers.is_constexpr && self.at_punct(Punct::LBrace)) {
            Some(self.parse_initializer()?)
        } else {
            None
        };
        self.expect_punct(Punct::Semicolon, "expected ';' after global declaration")?;
        let mut ty = ty;
        if specifiers.is_constexpr {
            ty.is_const = true;
        }
        Ok(vec![TopLevel::Global(Variable {
            name,
            ty,
            initializer,
            is_static: specifiers.is_static,
            is_extern: specifiers.is_extern,
            location: name_location,
        })])
    }

    /// `namespace` blocks are flattened: cppcc has a single global namespace, so
    /// `namespace std { typedef ... }` simply injects the names globally and
    /// `std::name` resolves through the transparent `::` qualifier.
    fn parse_namespace(&mut self) -> Result<Vec<TopLevel>, CompileError> {
        self.expect_kw("namespace", "expected 'namespace'")?;
        self.eat_kw("inline");
        if matches!(self.current().kind, TokenKind::Identifier(_)) {
            self.bump();
            while self.eat_punct(Punct::Scope) {
                let _ = self.expect_identifier("expected namespace component")?;
            }
        }
        if self.eat_punct(Punct::Equal) {
            while !self.at_punct(Punct::Semicolon) && !self.at_eof() {
                self.bump();
            }
            self.expect_punct(Punct::Semicolon, "expected ';' after namespace alias")?;
            return Ok(Vec::new());
        }
        self.expect_punct(Punct::LBrace, "expected '{' after namespace name")?;
        let mut items = Vec::new();
        while !self.at_punct(Punct::RBrace) && !self.at_eof() {
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
        self.expect_punct(Punct::RBrace, "expected '}' after namespace body")?;
        Ok(items)
    }

    fn parse_using(&mut self) -> Result<Vec<TopLevel>, CompileError> {
        self.expect_kw("using", "expected 'using'")?;
        if self.eat_kw("namespace") {
            self.skip_to_semicolon("expected ';' after using-directive")?;
            return Ok(Vec::new());
        }
        // `using a::b;` / `using ::b;` imports an existing name: the global
        // namespace already contains it, so nothing needs to be recorded.
        if self.at_punct(Punct::Scope) {
            self.skip_to_semicolon("expected ';' after using-declaration")?;
            return Ok(Vec::new());
        }
        let (name, name_location) = self.expect_identifier("expected a name after 'using'")?;
        if self.eat_punct(Punct::Scope) {
            self.skip_to_semicolon("expected ';' after using-declaration")?;
            return Ok(Vec::new());
        }
        self.expect_punct(Punct::Equal, "expected '=' in alias declaration")?;
        let ty = self.parse_type_identifier()?;
        self.expect_punct(Punct::Semicolon, "expected ';' after alias declaration")?;
        self.type_names.insert(name.clone(), ty.clone());
        Ok(vec![TopLevel::Typedef(Typedef { name, ty, location: name_location })])
    }

    fn parse_typedef(&mut self) -> Result<Vec<TopLevel>, CompileError> {
        self.expect_kw("typedef", "expected 'typedef'")?;
        let base = self.parse_type()?;
        let mut items = Vec::new();
        loop {
            let (name, ty, name_location) = self.parse_declarator(base.clone())?;
            self.type_names.insert(name.clone(), ty.clone());
            items.push(TopLevel::Typedef(Typedef { name, ty, location: name_location }));
            if self.eat_punct(Punct::Comma) {
                continue;
            }
            self.expect_punct(Punct::Semicolon, "expected ';' after typedef")?;
            return Ok(items);
        }
    }

    fn parse_enum(&mut self) -> Result<Vec<TopLevel>, CompileError> {
        let location = self.current().location.clone();
        self.expect_kw("enum", "expected 'enum'")?;
        let is_scoped = self.eat_kw("class") || self.eat_kw("struct");
        let name = if matches!(self.current().kind, TokenKind::Identifier(_)) {
            Some(self.expect_identifier("expected enumerator name")?.0)
        } else {
            None
        };
        let underlying = if self.eat_punct(Punct::Colon) {
            self.parse_type_identifier()?
        } else {
            Type::int()
        };
        let mut variants = Vec::new();
        if self.eat_punct(Punct::LBrace) {
            while !self.at_punct(Punct::RBrace) && !self.at_eof() {
                let (variant_name, variant_location) = self.expect_identifier("expected enumerator")?;
                let value = if self.eat_punct(Punct::Equal) {
                    Some(self.parse_expression(1)?)
                } else {
                    None
                };
                variants.push(Enumerator {
                    name: variant_name,
                    value,
                    location: variant_location,
                });
                if !self.eat_punct(Punct::Comma) {
                    break;
                }
            }
            self.expect_punct(Punct::RBrace, "expected '}' after enumerators")?;
        }
        if self.at_punct(Punct::Comma) {
            return Err(self.error_here("only one declarator per enumeration is supported"));
        }
        self.expect_punct(Punct::Semicolon, "expected ';' after enumeration")?;
        if let Some(name) = &name {
            self.type_names.insert(name.clone(), underlying.clone());
        }
        Ok(vec![TopLevel::Enum(EnumDecl {
            name,
            is_scoped,
            underlying,
            variants,
            location,
        })])
    }

    fn parse_static_assert(&mut self) -> Result<StaticAssert, CompileError> {
        let location = self.current().location.clone();
        self.expect_kw("static_assert", "expected 'static_assert'")?;
        self.expect_punct(Punct::LParen, "expected '(' after static_assert")?;
        let condition = self.parse_expression(1)?;
        let message = if self.eat_punct(Punct::Comma) {
            match self.bump().kind {
                TokenKind::StringLiteral(bytes) => Some(String::from_utf8_lossy(&bytes).into_owned()),
                _ => return Err(self.error_here("static_assert message must be a string literal")),
            }
        } else {
            None
        };
        self.expect_punct(Punct::RParen, "expected ')' after static_assert")?;
        self.expect_punct(Punct::Semicolon, "expected ';' after static_assert")?;
        Ok(StaticAssert { condition, message, location })
    }

    fn parse_struct_definition(&mut self) -> Result<StructDecl, CompileError> {
        let location = self.current().location.clone();
        self.expect_kw("struct", "expected 'struct'")?;
        self.skip_attributes();
        let name = if matches!(self.current().kind, TokenKind::Identifier(_)) {
            self.expect_identifier("expected structure name")?.0
        } else {
            self.anonymous_structs += 1;
            format!("__cppcc_anonymous_{}", self.anonymous_structs)
        };
        let mut fields = Vec::new();
        let is_definition = self.eat_punct(Punct::LBrace);
        if is_definition {
            while !self.at_punct(Punct::RBrace) && !self.at_eof() {
                let before = self.index;
                let field_result = (|| {
                    self.skip_attributes();
                    let _ = self.parse_decl_specifiers()?;
                    let base = self.parse_type()?;
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

    fn parse_decl_specifiers(&mut self) -> Result<DeclSpecifiers, CompileError> {
        let mut specifiers = DeclSpecifiers::default();
        loop {
            let keyword = match &self.current().kind {
                TokenKind::Keyword(word) => word.clone(),
                _ => break,
            };
            match keyword.as_str() {
                "inline" | "constexpr" | "explicit" | "virtual" | "register" | "mutable"
                | "volatile" | "typename" | "thread_local" => {
                    if keyword == "constexpr" {
                        specifiers.is_constexpr = true;
                    }
                    self.bump();
                }
                "static" => {
                    specifiers.is_static = true;
                    self.bump();
                }
                "extern" => {
                    self.bump();
                    if matches!(self.current().kind, TokenKind::StringLiteral(_)) {
                        self.bump();
                    } else {
                        specifiers.is_extern = true;
                    }
                }
                _ => break,
            }
        }
        Ok(specifiers)
    }

    fn parse_type(&mut self) -> Result<Type, CompileError> {
        let location = self.current().location.clone();
        let mut ty = self.parse_type_base()?;
        let _ = location;
        // Trailing cv-qualifiers, e.g. `int const`.
        loop {
            if self.eat_kw("const") || self.eat_kw("volatile") {
                ty.is_const = true;
            } else {
                break;
            }
        }
        Ok(ty)
    }

    fn parse_type_base(&mut self) -> Result<Type, CompileError> {
        let location = self.current().location.clone();
        let mut is_const = false;
        while self.eat_kw("const") || self.eat_kw("volatile") || self.eat_kw("constexpr") || self.eat_kw("register") {
            is_const = true;
        }
        let mut is_unsigned = false;
        let is_signed = if self.eat_kw("unsigned") {
            is_unsigned = true;
            true
        } else {
            self.eat_kw("signed")
        };
        let _ = is_signed;
        let base = if self.eat_kw("void") {
            BaseType::Void
        } else if self.eat_kw("bool") {
            BaseType::Bool
        } else if self.eat_kw("char8_t") {
            is_unsigned = true;
            BaseType::Char
        } else if self.eat_kw("char16_t") {
            is_unsigned = true;
            BaseType::Short
        } else if self.eat_kw("char32_t") {
            is_unsigned = true;
            BaseType::Int
        } else if self.eat_kw("char") {
            BaseType::Char
        } else if self.eat_kw("short") {
            self.eat_kw("int");
            BaseType::Short
        } else if self.eat_kw("long") {
            if self.eat_kw("long") {
                self.eat_kw("int");
                BaseType::LongLong
            } else if self.is_kw("double") {
                return Err(self.error_here("floating point types are not supported by cppcc"));
            } else {
                self.eat_kw("int");
                BaseType::Long
            }
        } else if self.eat_kw("int") {
            BaseType::Int
        } else if self.eat_kw("float") || self.eat_kw("double") {
            return Err(CompileError::new(
                location,
                "floating point types are not supported by cppcc",
            ));
        } else if self.eat_kw("auto") {
            BaseType::Auto
        } else if self.eat_kw("struct") {
            self.skip_attributes();
            let name = self.parse_qualified_identifier("expected structure name after 'struct'")?;
            BaseType::Struct(name)
        } else if self.eat_kw("class") || self.eat_kw("union") {
            return Err(CompileError::new(
                location,
                "class and union types are not supported by cppcc; use a POD struct",
            ));
        } else if self.eat_kw("decltype") {
            return Err(CompileError::new(location, "decltype is not supported by cppcc"));
        } else if self.is_kw("wchar_t") {
            return Err(CompileError::new(location, "wchar_t is not supported by cppcc"));
        } else if matches!(self.current().kind, TokenKind::Identifier(_)) || self.at_punct(Punct::Scope) {
            let name = self.parse_qualified_identifier("expected a type name")?;
            match self.type_names.get(&name) {
                Some(ty) => {
                    let mut ty = ty.clone();
                    ty.is_const |= is_const;
                    ty.is_unsigned |= is_unsigned;
                    return Ok(ty);
                }
                None => {
                    return Err(CompileError::new(
                        location,
                        format!("unknown type name '{}'", name),
                    ))
                }
            }
        } else {
            return Err(CompileError::new(location, "expected a type specifier"));
        };
        let mut ty = Type::new(base);
        ty.is_unsigned = is_unsigned;
        ty.is_const = is_const;
        if is_unsigned && matches!(&ty.base, BaseType::Void | BaseType::Bool | BaseType::Struct(_)) {
            return Err(CompileError::new(location, "unsigned is invalid for this type"));
        }
        Ok(ty)
    }

    fn parse_type_identifier(&mut self) -> Result<Type, CompileError> {
        let mut ty = self.parse_type()?;
        loop {
            if self.eat_kw("const") {
                ty.is_const = true;
            } else if self.eat_punct(Punct::Star) {
                ty.pointers += 1;
                if self.eat_kw("const") {
                    ty.is_const = true;
                }
            } else if self.eat_punct(Punct::Amp) || self.eat_punct(Punct::AmpAmp) {
                ty.is_reference = true;
            } else {
                break;
            }
        }
        Ok(ty)
    }

    fn parse_declarator(&mut self, mut ty: Type) -> Result<(String, Type, SourceLocation), CompileError> {
        while self.eat_kw("const") || self.eat_kw("volatile") {
            ty.is_const = true;
        }
        loop {
            if self.eat_punct(Punct::Star) {
                ty.pointers += 1;
                if self.eat_kw("const") || self.eat_kw("volatile") {
                    ty.is_const = true;
                }
            } else if self.at_punct(Punct::Amp) || self.at_punct(Punct::AmpAmp) {
                self.bump();
                if ty.is_reference {
                    return Err(self.error_here("references cannot be nested"));
                }
                ty.is_reference = true;
                if self.eat_kw("const") {
                    ty.is_const = true;
                }
            } else {
                break;
            }
        }
        let (name, location) = self.expect_identifier("expected declarator name")?;
        while self.eat_punct(Punct::LBracket) {
            if ty.array_len.is_some() {
                return Err(self.error_here("multidimensional arrays are not supported"));
            }
            let length = if self.eat_punct(Punct::RBracket) {
                Some(0)
            } else {
                let value = self.parse_constant_integer("array length must be a positive integer constant")?;
                self.expect_punct(Punct::RBracket, "expected ']' after array length")?;
                Some(value)
            };
            ty.array_len = length;
        }
        Ok((name, ty, location))
    }

    fn parse_constant_integer(&mut self, message: &str) -> Result<usize, CompileError> {
        let mut value = match self.bump().kind {
            TokenKind::Integer(value) => value,
            _ => return Err(self.error_here(message)),
        };
        // Fold the simple constant forms native headers use for array bounds.
        loop {
            let operator = match &self.current().kind {
                TokenKind::Punct(Punct::Plus) => "+",
                TokenKind::Punct(Punct::Minus) => "-",
                TokenKind::Punct(Punct::Star) => "*",
                TokenKind::Punct(Punct::Slash) => "/",
                _ => break,
            };
            let next = match self.peek_n(1) {
                Some(TokenKind::Integer(next)) => *next,
                _ => break,
            };
            self.bump();
            self.bump();
            value = match operator {
                "+" => value + next,
                "-" => value - next,
                "*" => value * next,
                _ => {
                    if next == 0 {
                        return Err(self.error_here("division by zero in array length"));
                    }
                    value / next
                }
            };
        }
        if value <= 0 {
            return Err(self.error_here(message));
        }
        Ok(value as usize)
    }

    fn parse_parameters_after_lparen(&mut self) -> Result<ParameterList, CompileError> {
        let mut params = Vec::new();
        let mut is_variadic = false;
        if self.eat_punct(Punct::RParen) {
            return Ok(ParameterList { params, is_variadic });
        }
        loop {
            if self.eat_punct(Punct::Ellipsis) {
                is_variadic = true;
                self.expect_punct(Punct::RParen, "expected ')' after '...'")?;
                return Ok(ParameterList { params, is_variadic });
            }
            let _ = self.parse_decl_specifiers()?;
            let base = self.parse_type()?;
            let (name, ty, location) = if self.at_punct(Punct::Comma)
                || self.at_punct(Punct::RParen)
                || self.at_punct(Punct::Equal)
            {
                let location = self.current().location.clone();
                // `void` as the only parameter means "no parameters".
                if base.is_void() && params.is_empty() && self.at_punct(Punct::RParen) {
                    self.bump();
                    return Ok(ParameterList { params, is_variadic });
                }
                (None, base, location)
            } else {
                let (name, ty, location) = self.parse_declarator(base)?;
                (Some(name), ty, location)
            };
            if self.eat_punct(Punct::Equal) {
                // Default arguments are parsed and discarded; call arity checking
                // still uses the full list.
                let _ = self.parse_expression(1)?;
            }
            params.push(Parameter { name, ty, location });
            if self.eat_punct(Punct::Comma) {
                continue;
            }
            self.expect_punct(Punct::RParen, "expected ')' after parameters")?;
            return Ok(ParameterList { params, is_variadic });
        }
    }

    fn skip_function_qualifiers(&mut self) {
        loop {
            if self.eat_kw("noexcept") {
                if self.at_punct(Punct::LParen) {
                    let _ = self.skip_balanced_parens();
                }
            } else if self.eat_kw("override")
                || self.eat_kw("final")
                || self.eat_kw("const")
                || self.eat_kw("volatile")
                || self.eat_kw("mutable")
            {
            } else {
                break;
            }
        }
    }

    fn skip_pure_or_defaulted(&mut self) {
        if self.at_punct(Punct::Equal) {
            self.bump();
            match self.bump().kind {
                TokenKind::Integer(_) | TokenKind::Keyword(_) | TokenKind::Identifier(_) => {}
                _ => {}
            }
        }
    }

    fn skip_balanced_parens(&mut self) -> bool {
        if !self.at_punct(Punct::LParen) {
            return false;
        }
        let mut depth = 0usize;
        while !self.at_eof() {
            if self.at_punct(Punct::LParen) {
                depth += 1;
            } else if self.at_punct(Punct::RParen) {
                depth -= 1;
            }
            self.bump();
            if depth == 0 {
                return true;
            }
        }
        false
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
        self.skip_attributes();
        if self.at_punct(Punct::LBrace) {
            return self.parse_block();
        }
        if self.eat_punct(Punct::Semicolon) {
            return Ok(Stmt { kind: StmtKind::Empty, location });
        }
        if self.is_kw("using") {
            let _ = self.parse_using()?;
            return Ok(Stmt { kind: StmtKind::Empty, location });
        }
        if self.is_kw("static_assert") {
            let assertion = self.parse_static_assert()?;
            return Ok(Stmt { kind: StmtKind::StaticAssert(assertion), location });
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
        if self.eat_kw("do") {
            let body = Box::new(self.parse_statement()?);
            self.expect_kw("while", "expected 'while' after do body")?;
            self.expect_punct(Punct::LParen, "expected '(' after while")?;
            let condition = self.parse_expression(1)?;
            self.expect_punct(Punct::RParen, "expected ')' after do-while condition")?;
            self.expect_punct(Punct::Semicolon, "expected ';' after do-while")?;
            return Ok(Stmt { kind: StmtKind::DoWhile { body, condition }, location });
        }
        if self.eat_kw("for") {
            return self.parse_for(location);
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
        if self.is_kw("switch") || self.is_kw("case") || self.is_kw("default") || self.is_kw("goto") {
            let word = match &self.current().kind {
                TokenKind::Keyword(word) => word.clone(),
                _ => String::new(),
            };
            return Err(CompileError::new(location, format!("'{}' statements are not supported by cppcc", word)));
        }
        if self.starts_declaration() {
            return self.parse_local_declaration();
        }
        let expression = self.parse_expression(1)?;
        self.expect_punct(Punct::Semicolon, "expected ';' after expression")?;
        Ok(Stmt { kind: StmtKind::Expression(expression), location })
    }

    fn parse_for(&mut self, location: SourceLocation) -> Result<Stmt, CompileError> {
        self.expect_punct(Punct::LParen, "expected '(' after for")?;
        if self.eat_punct(Punct::Semicolon) {
            let condition = if self.at_punct(Punct::Semicolon) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::Semicolon, "expected ';' after for condition")?;
            let step = if self.at_punct(Punct::RParen) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::RParen, "expected ')' after for clauses")?;
            let body = Box::new(self.parse_statement()?);
            return Ok(Stmt { kind: StmtKind::For { init: None, condition, step, body }, location });
        }
        if self.starts_declaration() {
            let specifiers = self.parse_decl_specifiers()?;
            let mut base = self.parse_type()?;
            if specifiers.is_constexpr {
                base.is_const = true;
            }
            let (name, ty, name_location) = self.parse_declarator(base)?;
            if self.eat_punct(Punct::Colon) {
                let range = self.parse_expression(1)?;
                self.expect_punct(Punct::RParen, "expected ')' after range-for range")?;
                let body = Box::new(self.parse_statement()?);
                return Ok(Stmt {
                    kind: StmtKind::RangeFor {
                        var: Variable {
                            name,
                            ty,
                            initializer: None,
                            is_static: false,
                            is_extern: false,
                            location: name_location,
                        },
                        range,
                        body,
                    },
                    location,
                });
            }
            let initializer = if self.eat_punct(Punct::Equal) { Some(self.parse_initializer()?) } else { None };
            self.expect_punct(Punct::Semicolon, "expected ';' after for initializer")?;
            let init = Stmt {
                kind: StmtKind::Variable(Variable {
                    name,
                    ty,
                    initializer,
                    is_static: specifiers.is_static,
                    is_extern: false,
                    location: name_location,
                }),
                location: location.clone(),
            };
            let condition = if self.at_punct(Punct::Semicolon) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::Semicolon, "expected ';' after for condition")?;
            let step = if self.at_punct(Punct::RParen) { None } else { Some(self.parse_expression(1)?) };
            self.expect_punct(Punct::RParen, "expected ')' after for clauses")?;
            let body = Box::new(self.parse_statement()?);
            return Ok(Stmt {
                kind: StmtKind::For { init: Some(Box::new(init)), condition, step, body },
                location,
            });
        }
        let expression = self.parse_expression(1)?;
        self.expect_punct(Punct::Semicolon, "expected ';' after for initializer")?;
        let init = Stmt { kind: StmtKind::Expression(expression), location: location.clone() };
        let condition = if self.at_punct(Punct::Semicolon) { None } else { Some(self.parse_expression(1)?) };
        self.expect_punct(Punct::Semicolon, "expected ';' after for condition")?;
        let step = if self.at_punct(Punct::RParen) { None } else { Some(self.parse_expression(1)?) };
        self.expect_punct(Punct::RParen, "expected ')' after for clauses")?;
        let body = Box::new(self.parse_statement()?);
        Ok(Stmt {
            kind: StmtKind::For { init: Some(Box::new(init)), condition, step, body },
            location,
        })
    }

    fn parse_local_declaration(&mut self) -> Result<Stmt, CompileError> {
        let location = self.current().location.clone();
        let specifiers = self.parse_decl_specifiers()?;
        let mut base = self.parse_type()?;
        if specifiers.is_constexpr {
            base.is_const = true;
        }
        let (name, ty, name_location) = self.parse_declarator(base)?;
        if self.at_punct(Punct::Comma) {
            return Err(self.error_here("only one declarator per declaration is supported"));
        }
        let initializer = if self.eat_punct(Punct::Equal) || (specifiers.is_constexpr && self.at_punct(Punct::LBrace)) {
            Some(self.parse_initializer()?)
        } else {
            None
        };
        self.expect_punct(Punct::Semicolon, "expected ';' after variable declaration")?;
        Ok(Stmt { kind: StmtKind::Variable(Variable {
            name,
            ty,
            initializer,
            is_static: specifiers.is_static,
            is_extern: false,
            location: name_location,
        }), location })
    }

    fn parse_initializer(&mut self) -> Result<Expr, CompileError> {
        if self.at_punct(Punct::LBrace) {
            return self.parse_brace_initializer();
        }
        self.parse_expression(1)
    }

    fn parse_brace_initializer(&mut self) -> Result<Expr, CompileError> {
        let location = self.current().location.clone();
        self.expect_punct(Punct::LBrace, "expected '{' to start an initializer list")?;
        let mut elements = Vec::new();
        while !self.at_punct(Punct::RBrace) && !self.at_eof() {
            elements.push(self.parse_initializer()?);
            if !self.eat_punct(Punct::Comma) {
                break;
            }
        }
        self.expect_punct(Punct::RBrace, "expected '}' after initializer list")?;
        Ok(Expr { kind: ExprKind::Aggregate(elements), location, ty: None })
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
        if self.eat_kw("sizeof") {
            if self.at_punct(Punct::LParen) && self.looks_like_type(self.index + 1) {
                self.bump();
                let ty = self.parse_type_identifier()?;
                self.expect_punct(Punct::RParen, "expected ')' after sizeof type")?;
                return Ok(Expr { kind: ExprKind::SizeofType(ty), location, ty: None });
            }
            let operand = self.parse_unary()?;
            return Ok(Expr { kind: ExprKind::SizeofExpr(Box::new(operand)), location, ty: None });
        }
        if self.is_kw("static_cast") || self.is_kw("reinterpret_cast") || self.is_kw("const_cast") {
            self.bump();
            self.expect_punct(Punct::Less, "expected '<' after cast keyword")?;
            let ty = self.parse_type_identifier()?;
            self.expect_punct(Punct::Greater, "expected '>' after cast type")?;
            self.expect_punct(Punct::LParen, "expected '(' after cast type")?;
            let operand = self.parse_expression(1)?;
            self.expect_punct(Punct::RParen, "expected ')' after cast operand")?;
            return Ok(Expr { kind: ExprKind::Cast { ty, operand: Box::new(operand) }, location, ty: None });
        }
        if self.is_kw("dynamic_cast") {
            return Err(CompileError::new(location, "dynamic_cast is not supported by cppcc"));
        }
        if self.at_punct(Punct::LParen) && self.looks_like_type(self.index + 1) {
            self.bump();
            let ty = self.parse_type_identifier()?;
            self.expect_punct(Punct::RParen, "expected ')' after cast type")?;
            let operand = self.parse_unary()?;
            return Ok(Expr { kind: ExprKind::Cast { ty, operand: Box::new(operand) }, location, ty: None });
        }
        let op = match &self.current().kind {
            TokenKind::Punct(Punct::Plus) => Some("+"),
            TokenKind::Punct(Punct::Minus) => Some("-"),
            TokenKind::Punct(Punct::Bang) => Some("!"),
            TokenKind::Punct(Punct::Tilde) => Some("~"),
            TokenKind::Punct(Punct::Star) => Some("*"),
            TokenKind::Punct(Punct::Amp) => Some("&"),
            TokenKind::Punct(Punct::PlusPlus) => Some("++"),
            TokenKind::Punct(Punct::MinusMinus) => Some("--"),
            TokenKind::Keyword(word) if word == "not" => Some("!"),
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
            } else if self.at_punct(Punct::Scope) {
                return Err(self.error_here("nested-name-specifiers are only supported on declarations"));
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
            TokenKind::StringLiteral(mut value) => {
                self.bump();
                // Adjacent string literals concatenate during translation.
                while let TokenKind::StringLiteral(next) = &self.current().kind {
                    value.extend_from_slice(next);
                    self.bump();
                }
                return Ok(Expr { kind: ExprKind::String(value), location: token.location, ty: None });
            }
            TokenKind::Identifier(ref name) => {
                self.bump();
                let mut last = name.clone();
                while self.eat_punct(Punct::Scope) {
                    match self.bump().kind {
                        TokenKind::Identifier(next) => last = next,
                        _ => return Err(self.error_here("expected an identifier after '::'")),
                    }
                }
                return Ok(Expr { kind: ExprKind::Identifier(last), location: token.location, ty: None });
            }
            TokenKind::Keyword(ref word) if word == "true" => ExprKind::Boolean(true),
            TokenKind::Keyword(ref word) if word == "false" => ExprKind::Boolean(false),
            TokenKind::Keyword(ref word) if word == "nullptr" => ExprKind::Nullptr,
            TokenKind::Punct(Punct::LParen) => {
                self.bump();
                let expr = self.parse_expression(1)?;
                self.expect_punct(Punct::RParen, "expected ')' after expression")?;
                return Ok(expr);
            }
            TokenKind::Punct(Punct::LBracket) => {
                return Err(CompileError::new(
                    token.location,
                    "lambda expressions are not supported by cppcc",
                ))
            }
            TokenKind::Keyword(ref word) if matches!(word.as_str(), "new" | "delete") => {
                return Err(CompileError::new(
                    token.location,
                    format!("'{}' expressions are not supported by cppcc (no heap allocator)", word),
                ))
            }
            TokenKind::Keyword(ref word) if word == "this" => {
                return Err(CompileError::new(
                    token.location,
                    "'this' is only valid inside a class, which cppcc does not support",
                ))
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

    fn starts_declaration(&self) -> bool {
        self.starts_type_at(self.index) || self.is_kw("static") || self.is_kw("extern")
    }

    fn starts_type_at(&self, index: usize) -> bool {
        match self.tokens.get(index).map(|token| &token.kind) {
            Some(TokenKind::Keyword(word)) => matches!(
                word.as_str(),
                "const"
                    | "volatile"
                    | "constexpr"
                    | "signed"
                    | "unsigned"
                    | "void"
                    | "bool"
                    | "char"
                    | "char8_t"
                    | "char16_t"
                    | "char32_t"
                    | "short"
                    | "int"
                    | "long"
                    | "struct"
                    | "class"
                    | "union"
                    | "auto"
                    | "inline"
                    | "register"
                    | "mutable"
                    | "typename"
                    | "thread_local"
                    | "explicit"
            ),
            Some(TokenKind::Identifier(_)) | Some(TokenKind::Punct(Punct::Scope)) => {
                self.looks_like_type(index)
            }
            _ => false,
        }
    }

    /// True when the tokens starting at `index` name a type: a built-in type
    /// keyword, or a `typedef`/`using`/`enum` name (optionally `ns::`-qualified).
    fn looks_like_type(&self, index: usize) -> bool {
        let mut cursor = index;
        if matches!(self.tokens.get(cursor).map(|token| &token.kind), Some(TokenKind::Punct(Punct::Scope))) {
            cursor += 1;
        }
        let mut last: Option<&String> = None;
        loop {
            match self.tokens.get(cursor).map(|token| &token.kind) {
                Some(TokenKind::Identifier(name)) => {
                    last = Some(name);
                    cursor += 1;
                }
                _ => break,
            }
            if matches!(self.tokens.get(cursor).map(|token| &token.kind), Some(TokenKind::Punct(Punct::Scope))) {
                cursor += 1;
            } else {
                break;
            }
        }
        if let Some(TokenKind::Keyword(word)) = self.tokens.get(index).map(|token| &token.kind) {
            return matches!(
                word.as_str(),
                "const"
                    | "volatile"
                    | "constexpr"
                    | "signed"
                    | "unsigned"
                    | "void"
                    | "bool"
                    | "char"
                    | "char8_t"
                    | "char16_t"
                    | "char32_t"
                    | "short"
                    | "int"
                    | "long"
                    | "struct"
                    | "class"
                    | "union"
                    | "auto"
                    | "wchar_t"
                    | "decltype"
            );
        }
        last.map(|name| self.type_names.contains_key(name)).unwrap_or(false)
    }

    fn parse_qualified_identifier(&mut self, message: &str) -> Result<String, CompileError> {
        self.eat_punct(Punct::Scope);
        let (mut name, _) = self.expect_identifier(message)?;
        while self.eat_punct(Punct::Scope) {
            let (next, _) = self.expect_identifier("expected an identifier after '::'")?;
            name = next;
        }
        Ok(name)
    }

    fn skip_attributes(&mut self) {
        while self.at_punct(Punct::LBracket) && self.peek_n_punct(1, Punct::LBracket) {
            self.bump();
            self.bump();
            let mut depth = 1usize;
            while !self.at_eof() && depth > 0 {
                if self.at_punct(Punct::RBracket) && self.peek_n_punct(1, Punct::RBracket) {
                    self.bump();
                    self.bump();
                    depth -= 1;
                } else if self.at_punct(Punct::LBracket) && self.peek_n_punct(1, Punct::LBracket) {
                    self.bump();
                    self.bump();
                    depth += 1;
                } else {
                    self.bump();
                }
            }
        }
    }

    fn skip_to_semicolon(&mut self, message: &str) -> Result<(), CompileError> {
        while !self.at_punct(Punct::Semicolon) && !self.at_eof() {
            self.bump();
        }
        self.expect_punct(Punct::Semicolon, message)
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

    fn peek_n(&self, n: usize) -> Option<&TokenKind> {
        self.tokens.get(self.index + n).map(|token| &token.kind)
    }

    fn peek_n_punct(&self, n: usize, punct: Punct) -> bool {
        matches!(self.peek_n(n), Some(TokenKind::Punct(found)) if *found == punct)
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

    fn function_body(output: &ParseOutput, name: &str) -> Vec<Stmt> {
        for item in &output.unit.items {
            if let TopLevel::Function(function) = item {
                if function.name == name {
                    if let StmtKind::Block(body) = &function.body.as_ref().unwrap().kind {
                        return body.clone();
                    }
                }
            }
        }
        panic!("function {} not found", name);
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
        let body = function_body(&output, "f");
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

    #[test]
    fn parses_cpp11_declaration_forms() {
        let output = parse_source(
            "typedef unsigned long size_t;\n\
             using index_t = int;\n\
             enum class Color : int { Red, Green = 5, Blue };\n\
             struct Point { int x; int y; };\n\
             int main() {\n\
               auto value = 3;\n\
               const auto flag = true;\n\
               int values[3] = {1, 2, 3};\n\
               struct Point p = {1, 2};\n\
               size_t n = sizeof(int);\n\
               int total = 0;\n\
               for (auto item : values) total += item;\n\
               for (auto& item : values) total += item;\n\
               printf_like(total, n, flag, (int)3, static_cast<int>(4));\n\
               Color c = Color::Green;\n\
               return c == Color::Green ? 0 : 1;\n\
             }\n\
             static_assert(sizeof(int) == 4, \"int is 32 bits\");\n",
        );
        assert!(output.errors.is_empty(), "{:?}", output.errors);
        let items = &output.unit.items;
        assert!(items.iter().any(|item| matches!(item, TopLevel::Typedef(_))));
        assert!(items.iter().any(|item| matches!(item, TopLevel::Enum(decl) if decl.variants.len() == 3)));
        let body = function_body(&output, "main");
        let ranges = body.iter().filter(|stmt| matches!(stmt.kind, StmtKind::RangeFor { .. })).count();
        assert_eq!(ranges, 2);
    }

    #[test]
    fn parses_namespaces_and_extern_c_blocks() {
        let output = parse_source(
            "namespace std { typedef unsigned long size_t; }\n\
             extern \"C\" { int stdcall_like(int value); }\n\
             using std::size_t;\n\
             int main() { std::size_t n = 0; return (int)n; }\n",
        );
        assert!(output.errors.is_empty(), "{:?}", output.errors);
        assert!(output.unit.items.iter().any(|item| matches!(item, TopLevel::Function(function) if function.name == "stdcall_like")));
        assert!(output.unit.items.iter().any(|item| matches!(item, TopLevel::Function(function) if function.name == "main")));
    }

    #[test]
    fn rejects_unsupported_constructs_with_clear_messages() {
        let class = parse_source("class Widget { int x; };");
        assert!(class.errors[0].message.contains("class types are not supported"));
        let lambda = parse_source("int main() { auto f = [](int x) { return x; }; return 0; }");
        assert!(lambda.errors.iter().any(|error| error.message.contains("lambda")));
        let template = parse_source("template <typename T> T id(T value);");
        assert!(template.errors[0].message.contains("templates are not supported"));
    }
}
