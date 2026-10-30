use crate::ast::*;
use crate::diagnostic::{CompileError, ErrorCollector, SourceLocation};
use crate::layout::Layouts;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SymbolKind {
    Variable(Type),
    Function {
        return_type: Type,
        params: Vec<Type>,
        defined: bool,
        is_variadic: bool,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Symbol {
    pub name: String,
    pub kind: SymbolKind,
}

#[derive(Default, Debug)]
pub struct SymbolTable {
    scopes: Vec<HashMap<String, Symbol>>,
}

impl SymbolTable {
    pub fn new() -> Self {
        Self { scopes: vec![HashMap::new()] }
    }

    pub fn enter_scope(&mut self) {
        self.scopes.push(HashMap::new());
    }

    pub fn exit_scope(&mut self) {
        if self.scopes.len() > 1 {
            self.scopes.pop();
        }
    }

    pub fn define(&mut self, symbol: Symbol) -> Result<(), Symbol> {
        let scope = self.scopes.last_mut().expect("symbol table always has global scope");
        if scope.contains_key(&symbol.name) {
            Err(symbol)
        } else {
            scope.insert(symbol.name.clone(), symbol);
            Ok(())
        }
    }

    pub fn lookup(&self, name: &str) -> Option<&Symbol> {
        self.scopes.iter().rev().find_map(|scope| scope.get(name))
    }

    pub fn lookup_current(&self, name: &str) -> Option<&Symbol> {
        self.scopes.last().and_then(|scope| scope.get(name))
    }

    fn update_global(&mut self, name: &str, kind: SymbolKind) {
        if let Some(symbol) = self.scopes[0].get_mut(name) {
            symbol.kind = kind;
        }
    }
}

#[derive(Debug)]
pub struct SemaOutput {
    pub errors: Vec<CompileError>,
    pub symbols: SymbolTable,
}

pub fn analyze(unit: &mut TranslationUnit) -> SemaOutput {
    Analyzer::new().analyze(unit)
}

#[derive(Clone, Debug)]
struct StructInfo {
    fields: HashMap<String, Type>,
    order: Vec<(String, Type)>,
}

struct Analyzer {
    symbols: SymbolTable,
    structs: HashMap<String, StructInfo>,
    struct_definitions: HashSet<String>,
    enum_constants: HashMap<String, i64>,
    layouts: Layouts,
    errors: ErrorCollector,
    loop_depth: usize,
    unresolved_globals: HashMap<String, SourceLocation>,
    defined_globals: HashSet<String>,
}

fn builtin_function(name: &str, return_type: Type, params: Vec<Type>) -> Symbol {
    Symbol {
        name: name.to_owned(),
        kind: SymbolKind::Function { return_type, params, defined: true, is_variadic: false },
    }
}

impl Analyzer {
    fn new() -> Self {
        let mut symbols = SymbolTable::new();
        let mut write_string = Type::new(BaseType::Char);
        write_string.is_const = true;
        write_string.pointers = 1;
        let mut write_length = Type::new(BaseType::Long);
        write_length.is_unsigned = true;
        let _ = symbols.define(builtin_function(
            "novus_write",
            Type::new(BaseType::Long),
            vec![write_string.clone(), write_length],
        ));
        let _ = symbols.define(builtin_function("novus_yield", Type::void(), Vec::new()));
        let _ = symbols.define(builtin_function("novus_exit", Type::void(), vec![Type::int()]));
        // Text helpers provided by the backend so that <stdio.h> works without
        // a runtime library.
        let _ = symbols.define(builtin_function("puts", Type::int(), vec![write_string]));
        let _ = symbols.define(builtin_function("putchar", Type::int(), vec![Type::int()]));
        let _ = symbols.define(builtin_function("exit", Type::void(), vec![Type::int()]));
        let _ = symbols.define(builtin_function("abort", Type::void(), Vec::new()));
        Self {
            symbols,
            structs: HashMap::new(),
            struct_definitions: HashSet::new(),
            enum_constants: HashMap::new(),
            layouts: Layouts::default(),
            errors: ErrorCollector::default(),
            loop_depth: 0,
            unresolved_globals: HashMap::new(),
            defined_globals: HashSet::new(),
        }
    }

    fn analyze(mut self, unit: &mut TranslationUnit) -> SemaOutput {
        self.layouts = Layouts::from_unit(unit).unwrap_or_default();
        self.collect_structs(unit);
        self.collect_enums(unit);
        self.collect_globals(unit);
        for (name, location) in &self.unresolved_globals {
            self.errors.push(CompileError::new(
                location.clone(),
                format!("extern global '{}' has no definition in this translation unit", name),
            ));
        }
        self.check_static_asserts(unit);
        self.check_global_initializers(unit);
        self.check_function_bodies(unit);
        SemaOutput {
            errors: self.errors.into_errors(),
            symbols: self.symbols,
        }
    }

    fn collect_structs(&mut self, unit: &TranslationUnit) {
        for item in &unit.items {
            if let TopLevel::Struct(decl) = item {
                if !decl.is_definition {
                    self.structs.entry(decl.name.clone()).or_insert_with(|| StructInfo {
                        fields: HashMap::new(),
                        order: Vec::new(),
                    });
                    continue;
                }
                if !self.struct_definitions.insert(decl.name.clone()) {
                    self.errors.push(CompileError::new(decl.location.clone(), format!("redefinition of struct '{}'", decl.name)));
                    continue;
                }
                let mut fields = HashMap::new();
                let mut order = Vec::new();
                for field in &decl.fields {
                    if field.ty.is_void() {
                        self.errors.push(CompileError::new(field.location.clone(), "structure field cannot have type void"));
                    }
                    if field.ty.is_reference {
                        self.errors.push(CompileError::new(field.location.clone(), "structure fields cannot be references"));
                    }
                    if fields.insert(field.name.clone(), field.ty.clone()).is_some() {
                        self.errors.push(CompileError::new(field.location.clone(), format!("duplicate structure field '{}'", field.name)));
                    }
                    order.push((field.name.clone(), field.ty.clone()));
                }
                self.structs.insert(decl.name.clone(), StructInfo { fields, order });
            }
        }
    }

    fn collect_enums(&mut self, unit: &mut TranslationUnit) {
        for index in 0..unit.items.len() {
            let TopLevel::Enum(decl) = &mut unit.items[index] else { continue };
            if decl.variants.is_empty() {
                continue;
            }
            let mut next_value: i64 = 0;
            for variant in &mut decl.variants {
                let mut value = next_value;
                if let Some(expr) = &mut variant.value {
                    let _ = self.check_expr(expr);
                    match self.const_eval(expr) {
                        Some(evaluated) => value = evaluated,
                        None => self.errors.push(CompileError::new(
                            expr.location.clone(),
                            "enumerator value must be an integer constant expression",
                        )),
                    }
                }
                next_value = value.wrapping_add(1);
                if self
                    .enum_constants
                    .insert(variant.name.clone(), value)
                    .is_some()
                {
                    self.errors.push(CompileError::new(
                        variant.location.clone(),
                        format!("redefinition of enumerator '{}'", variant.name),
                    ));
                }
            }
        }
    }

    fn collect_globals(&mut self, unit: &TranslationUnit) {
        for item in &unit.items {
            match item {
                TopLevel::Global(variable) => {
                    if variable.ty.is_reference {
                        self.errors.push(CompileError::new(
                            variable.location.clone(),
                            "global reference variables are not supported",
                        ));
                    }
                    if !variable.ty.is_auto() {
                        self.validate_object_type(&variable.ty, &variable.location);
                    }
                    let symbol = Symbol { name: variable.name.clone(), kind: SymbolKind::Variable(variable.ty.clone()) };
                    let is_definition = !variable.is_extern || variable.initializer.is_some();
                    if let Some(existing) = self.symbols.lookup_current(&variable.name).cloned() {
                        match existing.kind {
                            SymbolKind::Variable(existing_type) if same_declared_type(&existing_type, &variable.ty) => {
                                if is_definition && !self.defined_globals.insert(variable.name.clone()) {
                                    self.errors.push(CompileError::new(variable.location.clone(), format!("redefinition of '{}'", variable.name)));
                                } else if is_definition {
                                    self.unresolved_globals.remove(&variable.name);
                                }
                            }
                            SymbolKind::Variable(_) => self.errors.push(CompileError::new(variable.location.clone(), format!("conflicting types for global '{}'", variable.name))),
                            _ => self.errors.push(CompileError::new(variable.location.clone(), format!("'{}' declared as both function and variable", variable.name))),
                        }
                    } else {
                        let _ = self.symbols.define(symbol);
                        if is_definition {
                            self.defined_globals.insert(variable.name.clone());
                        } else {
                            self.unresolved_globals.insert(variable.name.clone(), variable.location.clone());
                        }
                    }
                }
                TopLevel::Function(function) => {
                    if function.return_type.is_reference {
                        self.errors.push(CompileError::new(
                            function.location.clone(),
                            "functions returning references are not supported",
                        ));
                    }
                    if !function.return_type.is_void() && !function.return_type.is_auto() {
                        self.validate_object_type(&function.return_type, &function.location);
                    }
                    if function.return_type.is_auto() {
                        self.errors.push(CompileError::new(
                            function.location.clone(),
                            "deduced return types are not supported; write the return type explicitly",
                        ));
                    }
                    for param in &function.params {
                        self.validate_parameter_type(&param.ty, &param.location);
                    }
                    if let Some(existing) = self.symbols.lookup_current(&function.name).cloned() {
                        match existing.kind {
                            SymbolKind::Function { return_type, params, defined, .. } => {
                                let signature_matches = same_declared_type(&return_type, &function.return_type)
                                    && params.len() == function.params.len()
                                    && params.iter().zip(&function.params).all(|(left, right)| same_parameter_type(left, &right.ty));
                                if !signature_matches {
                                    self.errors.push(CompileError::new(function.location.clone(), format!("conflicting declaration of function '{}'", function.name)));
                                }
                                if defined && function.body.is_some() {
                                    self.errors.push(CompileError::new(function.location.clone(), format!("redefinition of function '{}'", function.name)));
                                }
                                if function.body.is_some() {
                                    if let Some(symbol) = self.symbols.scopes[0].get_mut(&function.name) {
                                        symbol.kind = SymbolKind::Function {
                                            return_type: function.return_type.clone(),
                                            params: function.params.iter().map(|p| p.ty.clone()).collect(),
                                            defined: true,
                                            is_variadic: function.is_variadic,
                                        };
                                    }
                                }
                            }
                            _ => self.errors.push(CompileError::new(function.location.clone(), format!("'{}' declared as both function and variable", function.name))),
                        }
                    } else {
                        let _ = self.symbols.define(Symbol {
                            name: function.name.clone(),
                            kind: SymbolKind::Function {
                                return_type: function.return_type.clone(),
                                params: function.params.iter().map(|p| p.ty.clone()).collect(),
                                defined: function.body.is_some(),
                                is_variadic: function.is_variadic,
                            },
                        });
                    }
                }
                TopLevel::Struct(_) | TopLevel::Enum(_) | TopLevel::Typedef(_) | TopLevel::StaticAssert(_) => {}
            }
        }
    }

    fn check_static_asserts(&mut self, unit: &mut TranslationUnit) {
        for index in 0..unit.items.len() {
            if let TopLevel::StaticAssert(assertion) = &mut unit.items[index] {
                let _ = self.check_expr(&mut assertion.condition);
                let condition = assertion.condition.clone();
                let message = assertion.message.clone();
                let location = assertion.location.clone();
                self.check_static_assert(&condition, message.as_deref(), &location);
            }
        }
    }

    fn check_static_assert(&mut self, condition: &Expr, message: Option<&str>, location: &SourceLocation) {
        match self.const_eval(condition) {
            Some(0) => {
                let detail = message.map(|text| format!(": {}", text)).unwrap_or_default();
                self.errors.push(CompileError::new(
                    location.clone(),
                    format!("static assertion failed{}", detail),
                ));
            }
            Some(_) => {}
            None => self.errors.push(CompileError::new(
                location.clone(),
                "static_assert condition must be a constant expression",
            )),
        }
    }

    fn check_global_initializers(&mut self, unit: &mut TranslationUnit) {
        for index in 0..unit.items.len() {
            let TopLevel::Global(variable) = &mut unit.items[index] else { continue };
            if variable.is_extern && variable.initializer.is_none() {
                continue;
            }
            let name = variable.name.clone();
            let location = variable.location.clone();
            match &mut variable.initializer {
                Some(initializer) => {
                    self.check_initializer(&mut variable.ty, initializer, &name);
                    if variable.ty.is_auto() {
                        self.errors.push(CompileError::new(location, "'auto' cannot be deduced from this initializer"));
                        continue;
                    }
                    self.symbols.update_global(&name, SymbolKind::Variable(variable.ty.clone()));
                    if !self.is_constant_aggregate(initializer) {
                        self.errors.push(CompileError::new(initializer.location.clone(), "global initializer must be a constant expression"));
                    }
                }
                None => {
                    if variable.ty.is_auto() {
                        self.errors.push(CompileError::new(location, "'auto' requires an initializer"));
                    } else if variable.ty.array_len == Some(0) {
                        self.errors.push(CompileError::new(location, "array size cannot be deduced without an initializer"));
                    }
                }
            }
        }
    }

    fn check_function_bodies(&mut self, unit: &mut TranslationUnit) {
        for item in &mut unit.items {
            let TopLevel::Function(function) = item else { continue };
            let Some(body) = &mut function.body else { continue };
            self.symbols.enter_scope();
            for param in &function.params {
                let Some(name) = &param.name else {
                    self.errors.push(CompileError::new(param.location.clone(), "function definition parameter requires a name"));
                    continue;
                };
                if self.symbols.define(Symbol { name: name.clone(), kind: SymbolKind::Variable(param.ty.clone()) }).is_err() {
                    self.errors.push(CompileError::new(param.location.clone(), format!("duplicate parameter '{}'", name)));
                }
            }
            let return_type = function.return_type.clone();
            self.check_stmt(body, &return_type, true);
            if !return_type.is_void()
                && function.name != "main"
                && !stmt_returns(body)
            {
                self.errors.push(CompileError::new(function.location.clone(), format!("non-void function '{}' may not return a value", function.name)));
            }
            self.symbols.exit_scope();
        }
    }

    fn validate_object_type(&mut self, ty: &Type, location: &SourceLocation) {
        if ty.is_void() {
            self.errors.push(CompileError::new(location.clone(), "object cannot have type void"));
        }
        if ty.is_auto() {
            return;
        }
        if ty.is_reference {
            return;
        }
        if let BaseType::Struct(name) = &ty.base {
            if !self.structs.contains_key(name) {
                self.errors.push(CompileError::new(location.clone(), format!("use of incomplete struct '{}'", name)));
            }
        }
        if ty.array_len == Some(0) {
            self.errors.push(CompileError::new(location.clone(), "array length must be positive"));
        }
    }

    fn validate_parameter_type(&mut self, ty: &Type, location: &SourceLocation) {
        if ty.is_void() {
            self.errors.push(CompileError::new(location.clone(), "parameter cannot have type void"));
        }
        if ty.is_reference {
            if ty.referent().map(|referent| referent.is_void()).unwrap_or(false) {
                self.errors.push(CompileError::new(location.clone(), "parameter cannot be a reference to void"));
            }
            return;
        }
        if let BaseType::Struct(name) = &ty.base {
            if !self.structs.contains_key(name) {
                self.errors.push(CompileError::new(location.clone(), format!("use of incomplete struct '{}'", name)));
            }
        }
    }

    fn check_stmt(&mut self, stmt: &mut Stmt, return_type: &Type, is_function_body: bool) {
        match &mut stmt.kind {
            StmtKind::Block(statements) => {
                if !is_function_body { self.symbols.enter_scope(); }
                for statement in statements {
                    self.check_stmt(statement, return_type, false);
                }
                if !is_function_body { self.symbols.exit_scope(); }
            }
            StmtKind::Empty => {}
            StmtKind::StaticAssert(assertion) => {
                let _ = self.check_expr(&mut assertion.condition);
                let condition = assertion.condition.clone();
                let message = assertion.message.clone();
                let location = assertion.location.clone();
                self.check_static_assert(&condition, message.as_deref(), &location);
            }
            StmtKind::Break | StmtKind::Continue => {
                if self.loop_depth == 0 {
                    self.errors.push(CompileError::new(stmt.location.clone(), "break/continue outside a loop"));
                }
            }
            StmtKind::Expression(expr) => { self.check_expr(expr); }
            StmtKind::Variable(variable) => {
                self.check_local_variable(variable);
            }
            StmtKind::If { condition, then_branch, else_branch } => {
                let condition_type = self.check_expr(condition);
                self.require_condition(&condition_type, &condition.location);
                self.check_stmt(then_branch, return_type, false);
                if let Some(branch) = else_branch { self.check_stmt(branch, return_type, false); }
            }
            StmtKind::While { condition, body } => {
                let condition_type = self.check_expr(condition);
                self.require_condition(&condition_type, &condition.location);
                self.loop_depth += 1;
                self.check_stmt(body, return_type, false);
                self.loop_depth -= 1;
            }
            StmtKind::DoWhile { body, condition } => {
                self.loop_depth += 1;
                self.check_stmt(body, return_type, false);
                self.loop_depth -= 1;
                let condition_type = self.check_expr(condition);
                self.require_condition(&condition_type, &condition.location);
            }
            StmtKind::For { init, condition, step, body } => {
                self.symbols.enter_scope();
                if let Some(init) = init { self.check_stmt(init, return_type, false); }
                if let Some(condition) = condition {
                    let ty = self.check_expr(condition);
                    self.require_condition(&ty, &condition.location);
                }
                if let Some(step) = step { self.check_expr(step); }
                self.loop_depth += 1;
                self.check_stmt(body, return_type, false);
                self.loop_depth -= 1;
                self.symbols.exit_scope();
            }
            StmtKind::RangeFor { var, range, body } => {
                self.check_range_for(var, range, body, return_type);
            }
            StmtKind::Return(expr) => match (return_type.is_void(), expr) {
                (true, Some(expr)) => {
                    self.check_expr(expr);
                    self.errors.push(CompileError::new(expr.location.clone(), "void function cannot return a value"));
                }
                (false, None) => self.errors.push(CompileError::new(stmt.location.clone(), "non-void function must return a value")),
                (false, Some(expr)) => {
                    let actual = self.check_expr(expr);
                    if !assignable_expr(return_type, &actual, expr) {
                        self.errors.push(CompileError::new(expr.location.clone(), format!("return type {} does not match {}", describe_type(&actual), describe_type(return_type))));
                    }
                }
                (true, None) => {}
            },
        }
    }

    fn check_local_variable(&mut self, variable: &mut Variable) {
        if let Some(initializer) = &mut variable.initializer {
            self.check_initializer(&mut variable.ty, initializer, &variable.name);
        } else if variable.ty.is_auto() {
            self.errors.push(CompileError::new(
                variable.location.clone(),
                "'auto' requires an initializer",
            ));
            return;
        } else if variable.ty.array_len == Some(0) {
            self.errors.push(CompileError::new(
                variable.location.clone(),
                "array size cannot be deduced without an initializer",
            ));
        }
        if variable.ty.is_auto() {
            return;
        }
        self.validate_object_type(&variable.ty, &variable.location);
        if self.symbols.define(Symbol { name: variable.name.clone(), kind: SymbolKind::Variable(variable.ty.clone()) }).is_err() {
            self.errors.push(CompileError::new(variable.location.clone(), format!("redefinition of '{}' in this scope", variable.name)));
        }
    }

    fn check_range_for(&mut self, var: &mut Variable, range: &mut Expr, body: &mut Stmt, return_type: &Type) {
        let range_type = self.check_expr(range);
        let mut element = if range_type.array_len.is_some() && range_type.pointers == 0 {
            let mut element = range_type.clone();
            element.array_len = None;
            element
        } else {
            self.errors.push(CompileError::new(
                range.location.clone(),
                "range-based for requires a fixed-size array",
            ));
            Type::int()
        };
        element.is_const = false;
        if var.ty.is_auto() {
            let mut deduced = element.clone();
            deduced.is_const = var.ty.is_const;
            deduced.is_reference = var.ty.is_reference;
            var.ty = deduced;
        } else {
            let target = if var.ty.is_reference {
                var.ty.referent().unwrap_or_else(|| var.ty.clone())
            } else {
                var.ty.clone()
            };
            if !assignable(&target, &element) {
                self.errors.push(CompileError::new(
                    var.location.clone(),
                    format!("loop variable of type {} cannot hold {}", describe_type(&var.ty), describe_type(&element)),
                ));
            }
        }
        self.symbols.enter_scope();
        let _ = self.symbols.define(Symbol { name: var.name.clone(), kind: SymbolKind::Variable(var.ty.clone()) });
        self.loop_depth += 1;
        self.check_stmt(body, return_type, false);
        self.loop_depth -= 1;
        self.symbols.exit_scope();
    }

    /// Type-checks `expr` against `ty`, deducing `auto` and verifying brace
    /// initializer lists element by element.
    fn check_initializer(&mut self, ty: &mut Type, expr: &mut Expr, name: &str) {
        if ty.is_auto() {
            let actual = self.check_expr(expr);
            let mut deduced = if ty.is_reference { actual.clone() } else { actual.value_type() };
            deduced.is_const = ty.is_const || (ty.is_reference && actual.is_const);
            deduced.is_reference = ty.is_reference;
            if !ty.is_reference {
                deduced.array_len = None;
            }
            *ty = deduced;
            if ty.is_reference && !is_lvalue(expr) {
                self.errors.push(CompileError::new(
                    expr.location.clone(),
                    "a reference must bind to an lvalue",
                ));
            }
            expr.ty = Some(ty.value_type());
            return;
        }
        if ty.is_reference {
            let referent = ty.referent().unwrap_or_else(|| ty.clone());
            if !is_lvalue(expr) {
                self.errors.push(CompileError::new(
                    expr.location.clone(),
                    "a reference must bind to an lvalue",
                ));
            }
            let actual = self.check_expr(expr);
            if !assignable(&referent, &actual) {
                self.errors.push(CompileError::new(
                    expr.location.clone(),
                    format!("cannot bind {} to {}", describe_type(&actual), describe_type(ty)),
                ));
            }
            expr.ty = Some(actual);
            return;
        }
        if ty.is_aggregate() {
            self.check_aggregate_initializer(ty, expr, name);
            expr.ty = Some(ty.clone());
            return;
        }
        let actual = self.check_expr(expr);
        if !assignable_expr(ty, &actual, expr) {
            self.errors.push(CompileError::new(
                expr.location.clone(),
                format!("cannot initialize '{}' of type {} with {}", name, describe_type(ty), describe_type(&actual)),
            ));
        }
        expr.ty = Some(actual);
    }

    fn check_aggregate_initializer(&mut self, ty: &mut Type, expr: &mut Expr, name: &str) {
        if let ExprKind::Aggregate(elements) = &mut expr.kind {
            if ty.array_len.is_some() {
                let mut element_type = ty.clone();
                element_type.array_len = None;
                let capacity = ty.array_len.unwrap_or(0);
                if capacity == 0 {
                    ty.array_len = Some(elements.len());
                } else if elements.len() > capacity {
                    self.errors.push(CompileError::new(
                        expr.location.clone(),
                        format!("too many initializers for '{}' ({} of {})", name, elements.len(), capacity),
                    ));
                }
                for element in elements.iter_mut() {
                    let mut declared = element_type.clone();
                    self.check_initializer(&mut declared, element, name);
                }
                expr.ty = Some(ty.clone());
                return;
            }
            if let BaseType::Struct(struct_name) = &ty.base {
                let Some(fields) = self.structs.get(struct_name).map(|info| info.order.clone()) else {
                    self.errors.push(CompileError::new(expr.location.clone(), format!("use of incomplete struct '{}'", struct_name)));
                    return;
                };
                if elements.len() > fields.len() {
                    self.errors.push(CompileError::new(
                        expr.location.clone(),
                        format!("too many initializers for '{}' ({})", name, fields.len()),
                    ));
                }
                for (element, (field_name, field_type)) in elements.iter_mut().zip(fields.iter()) {
                    let mut declared = field_type.clone();
                    self.check_initializer(&mut declared, element, field_name);
                }
                expr.ty = Some(ty.clone());
                return;
            }
            // A braced initializer for a scalar is a single-element list.
            if elements.len() == 1 {
                let mut element = elements.remove(0);
                self.check_initializer(ty, &mut element, name);
                *elements = vec![element];
                expr.ty = Some(ty.clone());
                return;
            }
            self.errors.push(CompileError::new(
                expr.location.clone(),
                format!("too many initializers for scalar '{}'", name),
            ));
            return;
        }
        if ty.array_len.is_some() {
            // `char text[] = "abc";` and `char text[4] = "abc";`
            if let ExprKind::String(bytes) = &expr.kind {
                if ty.pointers == 0 {
                    let needed = bytes.len() + 1;
                    match ty.array_len {
                        Some(0) => ty.array_len = Some(needed),
                        Some(length) if length < bytes.len() => {
                            self.errors.push(CompileError::new(
                                expr.location.clone(),
                                format!("initializer string is too long for '{}'", name),
                            ));
                        }
                        _ => {}
                    }
                    expr.ty = Some(ty.clone());
                    return;
                }
            }
            self.errors.push(CompileError::new(
                expr.location.clone(),
                format!("cannot initialize array '{}' with a scalar expression", name),
            ));
            return;
        }
        self.errors.push(CompileError::new(
            expr.location.clone(),
            format!("cannot initialize aggregate '{}' with a scalar expression", name),
        ));
    }

    fn check_expr(&mut self, expr: &mut Expr) -> Type {
        let ty = match &mut expr.kind {
            ExprKind::Integer(_) | ExprKind::Character(_) => Type::int(),
            ExprKind::Boolean(_) => Type::new(BaseType::Bool),
            ExprKind::Nullptr => {
                let mut ty = Type::new(BaseType::Void);
                ty.pointers = 1;
                ty
            }
            ExprKind::String(_) => {
                let mut ty = Type::new(BaseType::Char);
                ty.is_const = true;
                ty.pointers = 1;
                ty
            }
            ExprKind::Identifier(name) => {
                if let Some(value) = self.enum_constants.get(name).copied() {
                    expr.kind = ExprKind::Integer(value);
                    Type::int()
                } else {
                    match self.symbols.lookup(name).map(|symbol| symbol.kind.clone()) {
                        Some(SymbolKind::Variable(ty)) => ty.value_type(),
                        Some(SymbolKind::Function { .. }) => {
                            self.errors.push(CompileError::new(expr.location.clone(), format!("function '{}' requires a call", name)));
                            Type::int()
                        }
                        None => {
                            self.errors.push(CompileError::new(expr.location.clone(), format!("use of undeclared identifier '{}'", name)));
                            Type::int()
                        }
                    }
                }
            }
            ExprKind::Aggregate(_) => {
                self.errors.push(CompileError::new(
                    expr.location.clone(),
                    "braced initializer list is only allowed in an initialization",
                ));
                Type::int()
            }
            ExprKind::Cast { ty: target, operand } => {
                let actual = self.check_expr(operand);
                let target = target.clone();
                if target.is_void() {
                    target
                } else if target.is_scalar() && actual.is_scalar() {
                    target
                } else if actual.is_aggregate() || target.is_aggregate() {
                    self.errors.push(CompileError::new(
                        expr.location.clone(),
                        "cannot cast a struct or array value",
                    ));
                    target
                } else {
                    target
                }
            }
            ExprKind::SizeofType(ty) => {
                let target = ty.clone();
                if self.layouts.size_align_opt(&target).is_none() {
                    self.errors.push(CompileError::new(
                        expr.location.clone(),
                        format!("sizeof {} is not a valid object size", describe_type(&target)),
                    ));
                }
                unsigned_long()
            }
            ExprKind::SizeofExpr(operand) => {
                let target = self.check_expr(operand);
                if target.is_void() {
                    self.errors.push(CompileError::new(
                        expr.location.clone(),
                        "sizeof void is not a valid object size",
                    ));
                }
                unsigned_long()
            }
            ExprKind::Unary { op, operand } => {
                let operand_type = self.check_expr(operand);
                match op.as_str() {
                    "&" => {
                        if !is_lvalue(operand) {
                            self.errors.push(CompileError::new(expr.location.clone(), "address-of requires an lvalue"));
                        }
                        let mut result = operand_type;
                        result.pointers += 1;
                        result.array_len = None;
                        result.is_reference = false;
                        result
                    }
                    "*" => {
                        if operand_type.pointers == 0 {
                            self.errors.push(CompileError::new(expr.location.clone(), "cannot dereference a non-pointer"));
                            Type::int()
                        } else if operand_type.pointers == 1 && operand_type.base == BaseType::Void {
                            self.errors.push(CompileError::new(expr.location.clone(), "cannot dereference a void pointer"));
                            Type::int()
                        } else {
                            let mut result = operand_type;
                            result.pointers -= 1;
                            result
                        }
                    }
                    "!" => {
                        if !operand_type.is_scalar() {
                            self.errors.push(CompileError::new(expr.location.clone(), "logical not requires a scalar operand"));
                        }
                        Type::new(BaseType::Bool)
                    }
                    "++" | "--" => {
                        if !is_lvalue(operand) || operand_type.is_const || !(operand_type.is_integer() || operand_type.pointers > 0) {
                            self.errors.push(CompileError::new(expr.location.clone(), "increment/decrement requires a modifiable scalar lvalue"));
                        }
                        operand_type
                    }
                    _ => {
                        if !operand_type.is_integer() {
                            self.errors.push(CompileError::new(expr.location.clone(), format!("unary '{}' requires an integer operand", op)));
                        }
                        operand_type
                    }
                }
            }
            ExprKind::Postfix { op: _, operand } => {
                let ty = self.check_expr(operand);
                if !is_lvalue(operand) || ty.is_const || !(ty.is_integer() || ty.pointers > 0) {
                    self.errors.push(CompileError::new(expr.location.clone(), "postfix increment/decrement requires a modifiable scalar lvalue"));
                }
                ty
            }
            ExprKind::Binary { op, left, right } => {
                let left_type = self.check_expr(left);
                let right_type = self.check_expr(right);
                self.check_binary(op, &left_type, &right_type, left, right, &expr.location)
            }
            ExprKind::Ternary { condition, then_expr, else_expr } => {
                let condition_type = self.check_expr(condition);
                self.require_condition(&condition_type, &condition.location);
                let then_type = self.check_expr(then_expr);
                let else_type = self.check_expr(else_expr);
                if assignable_expr(&then_type, &else_type, else_expr) { then_type }
                else if assignable_expr(&else_type, &then_type, then_expr) { else_type }
                else {
                    self.errors.push(CompileError::new(expr.location.clone(), "conditional expression branches have incompatible types"));
                    Type::int()
                }
            }
            ExprKind::Call { callee, args } => {
                if let ExprKind::Identifier(name) = &callee.kind {
                    let name = name.clone();
                    match self.symbols.lookup(&name).map(|symbol| symbol.kind.clone()) {
                        Some(SymbolKind::Function { return_type, params, defined, is_variadic }) => {
                            if !defined {
                                self.errors.push(CompileError::new(expr.location.clone(), format!("call to function '{}' which has no definition", name)));
                            }
                            if is_variadic && args.len() > params.len() {
                                self.errors.push(CompileError::new(
                                    expr.location.clone(),
                                    format!("variadic calls to '{}' are not supported by cppcc", name),
                                ));
                            } else if !is_variadic && args.len() != params.len() {
                                self.errors.push(CompileError::new(expr.location.clone(), format!("function '{}' expects {} argument(s), got {}", name, params.len(), args.len())));
                            }
                            for (index, arg) in args.iter_mut().enumerate() {
                                let actual = self.check_expr(arg);
                                if let Some(expected) = params.get(index) {
                                    if expected.is_reference && !is_lvalue(arg) {
                                        self.errors.push(CompileError::new(arg.location.clone(), format!("argument {} to '{}' must be an lvalue", index + 1, name)));
                                    }
                                    let target = expected.value_type();
                                    if !assignable_expr(&target, &actual, arg) {
                                        self.errors.push(CompileError::new(arg.location.clone(), format!("argument {} to '{}' has type {}, expected {}", index + 1, name, describe_type(&actual), describe_type(&target))));
                                    }
                                }
                            }
                            return_type
                        }
                        Some(SymbolKind::Variable(_)) => {
                            self.errors.push(CompileError::new(expr.location.clone(), format!("'{}' is not a function", name)));
                            for arg in args { self.check_expr(arg); }
                            Type::int()
                        }
                        None => {
                            self.errors.push(CompileError::new(expr.location.clone(), format!("call to undeclared function '{}'", name)));
                            for arg in args { self.check_expr(arg); }
                            Type::int()
                        }
                    }
                } else {
                    self.errors.push(CompileError::new(expr.location.clone(), "only direct function calls are supported"));
                    self.check_expr(callee);
                    for arg in args { self.check_expr(arg); }
                    Type::int()
                }
            }
            ExprKind::Index { base, index } => {
                let base_type = self.check_expr(base).decay_array();
                let index_type = self.check_expr(index);
                if !index_type.is_integer() {
                    self.errors.push(CompileError::new(index.location.clone(), "array index must be an integer"));
                }
                if base_type.pointers == 0 {
                    self.errors.push(CompileError::new(base.location.clone(), "subscripted value is not an array or pointer"));
                    Type::int()
                } else {
                    let mut result = base_type;
                    result.pointers -= 1;
                    result
                }
            }
            ExprKind::Member { base, name, through_pointer } => {
                let mut base_type = self.check_expr(base);
                if *through_pointer {
                    if base_type.pointers == 0 {
                        self.errors.push(CompileError::new(base.location.clone(), "'->' requires a pointer to struct"));
                    } else {
                        base_type.pointers -= 1;
                    }
                }
                match base_type.base {
                    BaseType::Struct(ref struct_name) if base_type.pointers == 0 => {
                        let field_type = self.structs.get(struct_name).and_then(|info| info.fields.get(name)).cloned();
                        if let Some(field_type) = field_type {
                            field_type.value_type()
                        } else {
                            self.errors.push(CompileError::new(expr.location.clone(), format!("struct '{}' has no member '{}'", struct_name, name)));
                            Type::int()
                        }
                    }
                    _ => {
                        self.errors.push(CompileError::new(expr.location.clone(), "member access requires a struct value"));
                        Type::int()
                    }
                }
            }
        };
        expr.ty = Some(ty.clone());
        ty
    }

    fn check_binary(&mut self, op: &str, left_ty: &Type, right_ty: &Type, left: &Expr, right: &Expr, location: &SourceLocation) -> Type {
        let assignment = matches!(op, "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=");
        if assignment {
            if !is_lvalue(left) || left_ty.is_const || left_ty.array_len.is_some() {
                self.errors.push(CompileError::new(location.clone(), "assignment requires a modifiable lvalue"));
            }
            if op == "=" {
                if !assignable_expr(left_ty, right_ty, right) {
                    self.errors.push(CompileError::new(location.clone(), format!("cannot assign {} to {}", describe_type(right_ty), describe_type(left_ty))));
                }
            } else if matches!(op, "+=" | "-=") && left_ty.pointers > 0 && right_ty.is_integer() {
                return left_ty.clone();
            } else if !left_ty.is_integer() || !right_ty.is_integer() {
                self.errors.push(CompileError::new(location.clone(), "compound assignment requires integer operands"));
            }
            return left_ty.clone();
        }
        match op {
            "&&" | "||" => {
                self.require_condition(left_ty, &left.location);
                self.require_condition(right_ty, &right.location);
                Type::new(BaseType::Bool)
            }
            "==" | "!=" | "<" | "<=" | ">" | ">=" => {
                let pointer_compare =
                    (left_ty.pointers > 0 && is_null_constant(right)) || (right_ty.pointers > 0 && is_null_constant(left));
                if !pointer_compare
                    && !(left_ty.is_integer() && right_ty.is_integer())
                    && !type_compatible(left_ty, right_ty)
                {
                    self.errors.push(CompileError::new(location.clone(), "comparison operands have incompatible types"));
                }
                Type::new(BaseType::Bool)
            }
            "+" if left_ty.pointers > 0 && right_ty.is_integer() => left_ty.clone(),
            "+" if right_ty.pointers > 0 && left_ty.is_integer() => right_ty.clone(),
            "-" if left_ty.pointers > 0 && right_ty.is_integer() => left_ty.clone(),
            "-" if left_ty.pointers > 0 && right_ty.pointers > 0 && type_compatible(left_ty, right_ty) => Type::new(BaseType::Long),
            "+" | "-" | "*" | "/" | "%" | "<<" | ">>" | "|" | "^" | "&" => {
                if !left_ty.is_integer() || !right_ty.is_integer() {
                    self.errors.push(CompileError::new(location.clone(), format!("operator '{}' requires integer operands", op)));
                }
                usual_arithmetic_type(left_ty, right_ty)
            }
            _ => Type::int(),
        }
    }

    fn require_condition(&mut self, ty: &Type, location: &SourceLocation) {
        if !ty.is_scalar() {
            self.errors.push(CompileError::new(location.clone(), "condition must have scalar type"));
        }
    }

    /// Folds integer constant expressions used by enumerators and
    /// `static_assert`. Returns `None` when the expression needs run time.
    fn const_eval(&self, expr: &Expr) -> Option<i64> {
        match &expr.kind {
            ExprKind::Integer(value) | ExprKind::Character(value) => Some(*value),
            ExprKind::Boolean(value) => Some(if *value { 1 } else { 0 }),
            ExprKind::Nullptr => Some(0),
            ExprKind::Cast { ty, operand } => {
                let value = self.const_eval(operand)?;
                Some(cast_constant(ty, value))
            }
            ExprKind::SizeofType(ty) => self.layouts.size_align_opt(ty).map(|pair| pair.0 as i64),
            ExprKind::SizeofExpr(operand) => {
                let ty = operand.ty.as_ref()?;
                self.layouts.size_align_opt(ty).map(|pair| pair.0 as i64)
            }
            ExprKind::Unary { op, operand } => {
                let value = self.const_eval(operand)?;
                match op.as_str() {
                    "+" => Some(value),
                    "-" => value.checked_neg(),
                    "~" => Some(!value),
                    "!" => Some(if value == 0 { 1 } else { 0 }),
                    _ => None,
                }
            }
            ExprKind::Binary { op, left, right } => {
                let left = self.const_eval(left)?;
                let right = self.const_eval(right)?;
                fold_binary(op, left, right)
            }
            ExprKind::Ternary { condition, then_expr, else_expr } => {
                if self.const_eval(condition)? != 0 {
                    self.const_eval(then_expr)
                } else {
                    self.const_eval(else_expr)
                }
            }
            _ => None,
        }
    }

    fn is_constant_aggregate(&self, expr: &Expr) -> bool {
        match &expr.kind {
            ExprKind::Aggregate(elements) => elements.iter().all(|element| self.is_constant_aggregate(element)),
            ExprKind::String(_) => true,
            ExprKind::SizeofType(ty) => self.layouts.size_align_opt(ty).is_some(),
            ExprKind::SizeofExpr(operand) => operand
                .ty
                .as_ref()
                .and_then(|ty| self.layouts.size_align_opt(ty))
                .is_some(),
            _ => is_constant_expr(expr),
        }
    }
}

fn unsigned_long() -> Type {
    let mut ty = Type::new(BaseType::Long);
    ty.is_unsigned = true;
    ty
}

fn fold_binary(op: &str, left: i64, right: i64) -> Option<i64> {
    match op {
        "+" => left.checked_add(right),
        "-" => left.checked_sub(right),
        "*" => left.checked_mul(right),
        "/" => left.checked_div(right),
        "%" => left.checked_rem(right),
        "<<" => u32::try_from(right).ok().and_then(|shift| left.checked_shl(shift)),
        ">>" => u32::try_from(right).ok().and_then(|shift| left.checked_shr(shift)),
        "&" => Some(left & right),
        "|" => Some(left | right),
        "^" => Some(left ^ right),
        "==" => Some(i64::from(left == right)),
        "!=" => Some(i64::from(left != right)),
        "<" => Some(i64::from(left < right)),
        "<=" => Some(i64::from(left <= right)),
        ">" => Some(i64::from(left > right)),
        ">=" => Some(i64::from(left >= right)),
        "&&" => Some(i64::from(left != 0 && right != 0)),
        "||" => Some(i64::from(left != 0 || right != 0)),
        _ => None,
    }
}

fn cast_constant(ty: &Type, value: i64) -> i64 {
    if ty.is_reference || ty.pointers > 0 {
        return value;
    }
    match ty.base {
        BaseType::Bool => i64::from(value != 0),
        BaseType::Char => {
            if ty.is_unsigned { (value as u8) as i64 } else { (value as i8) as i64 }
        }
        BaseType::Short => {
            if ty.is_unsigned { (value as u16) as i64 } else { (value as i16) as i64 }
        }
        BaseType::Int => {
            if ty.is_unsigned { (value as u32) as i64 } else { (value as i32) as i64 }
        }
        _ => value,
    }
}

fn type_compatible(expected: &Type, actual: &Type) -> bool {
    let expected = expected.value_type();
    let actual = actual.value_type();
    if expected.is_integer() && actual.is_integer() { return true; }
    if expected.pointers > 0 && actual.pointers > 0 {
        return expected.pointers == actual.pointers
            && (expected.base == actual.base || expected.base == BaseType::Void || actual.base == BaseType::Void)
            && (!actual.is_const || expected.is_const);
    }
    expected.base == actual.base && expected.pointers == actual.pointers && expected.array_len == actual.array_len
}

fn same_declared_type(left: &Type, right: &Type) -> bool {
    left.base == right.base
        && left.is_unsigned == right.is_unsigned
        && left.pointers == right.pointers
        && left.is_const == right.is_const
        && left.array_len == right.array_len
        && left.is_reference == right.is_reference
}

fn same_parameter_type(left: &Type, right: &Type) -> bool {
    if left.is_reference || right.is_reference {
        return left.is_reference == right.is_reference
            && same_declared_type(
                &left.clone().referent().unwrap_or_else(|| left.clone()),
                &right.clone().referent().unwrap_or_else(|| right.clone()),
            );
    }
    same_declared_type(&left.value_type(), &right.value_type())
}

fn assignable(expected: &Type, actual: &Type) -> bool {
    if expected.is_void() || actual.is_void() { return false; }
    if expected.is_reference {
        let referent = expected.referent().unwrap_or_else(|| expected.clone());
        return type_compatible(&referent, actual);
    }
    type_compatible(expected, actual)
}

fn assignable_expr(expected: &Type, actual: &Type, expr: &Expr) -> bool {
    if is_null_constant(expr) && (expected.pointers > 0 || expected.is_reference) {
        return true;
    }
    assignable(expected, actual)
}

/// Null pointer constants: `nullptr`, literal `0`, and `(T)0` casts of them.
fn is_null_constant(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::Nullptr => true,
        ExprKind::Integer(0) => true,
        ExprKind::Cast { operand, .. } => is_null_constant(operand),
        ExprKind::Unary { op, operand } if op == "+" => is_null_constant(operand),
        _ => false,
    }
}

fn usual_arithmetic_type(left: &Type, right: &Type) -> Type {
    let rank = |ty: &Type| match &ty.base {
        BaseType::LongLong => 5,
        BaseType::Long => 4,
        BaseType::Int => 3,
        BaseType::Short => 2,
        BaseType::Char => 1,
        BaseType::Bool => 0,
        _ => 0,
    };
    let mut result = if rank(left) >= rank(right) { left.clone() } else { right.clone() };
    result.is_const = false;
    result.array_len = None;
    result.is_reference = false;
    result
}

fn describe_type(ty: &Type) -> String {
    let base = match &ty.base {
        BaseType::Void => "void".to_owned(),
        BaseType::Bool => "bool".to_owned(),
        BaseType::Char => "char".to_owned(),
        BaseType::Short => "short".to_owned(),
        BaseType::Int => "int".to_owned(),
        BaseType::Long => "long".to_owned(),
        BaseType::LongLong => "long long".to_owned(),
        BaseType::Struct(name) => format!("struct {}", name),
        BaseType::Auto => "auto".to_owned(),
    };
    let suffix = if ty.is_reference {
        "&".to_owned()
    } else {
        "*".repeat(ty.pointers)
    };
    format!("{}{}{}", if ty.is_unsigned { "unsigned " } else { "" }, base, suffix)
}

fn is_lvalue(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::Identifier(_) | ExprKind::Index { .. } | ExprKind::Member { .. } => true,
        ExprKind::Unary { op, .. } => op == "*",
        _ => false,
    }
}

fn is_constant_expr(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::Integer(_) | ExprKind::Character(_) | ExprKind::Boolean(_) | ExprKind::String(_) => true,
        ExprKind::Nullptr => true,
        ExprKind::Unary { operand, .. } => is_constant_expr(operand),
        ExprKind::Binary { left, right, .. } => is_constant_expr(left) && is_constant_expr(right),
        ExprKind::Ternary { condition, then_expr, else_expr } => is_constant_expr(condition) && is_constant_expr(then_expr) && is_constant_expr(else_expr),
        ExprKind::Cast { operand, .. } => is_constant_expr(operand),
        ExprKind::SizeofType(_) | ExprKind::SizeofExpr(_) => true,
        ExprKind::Aggregate(elements) => elements.iter().all(is_constant_expr),
        _ => false,
    }
}

fn stmt_returns(stmt: &Stmt) -> bool {
    match &stmt.kind {
        StmtKind::Return(_) => true,
        StmtKind::Block(statements) => statements.iter().any(stmt_returns),
        StmtKind::If { then_branch, else_branch: Some(else_branch), .. } => stmt_returns(then_branch) && stmt_returns(else_branch),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn analyze_source(text: &str) -> SemaOutput {
        let locations = (0..text.len()).map(|i| SourceLocation::new("sema.cc", 1, i + 1)).collect();
        let pre = crate::preprocess::PreprocessedSource { text: text.to_owned(), locations };
        let lexed = crate::lexer::lex(&pre);
        let mut parsed = crate::parser::parse(lexed.tokens);
        let mut output = crate::sema::analyze(&mut parsed.unit);
        output.errors.splice(0..0, parsed.errors);
        output
    }

    fn assert_ok(text: &str) {
        let output = analyze_source(text);
        assert!(output.errors.is_empty(), "{:?}", output.errors);
    }

    #[test]
    fn symbol_table_has_nested_scope_lookup_and_shadowing() {
        let mut table = SymbolTable::new();
        table.define(Symbol { name: "x".into(), kind: SymbolKind::Variable(Type::int()) }).unwrap();
        table.enter_scope();
        let inner = Type::new(BaseType::Char);
        table.define(Symbol { name: "x".into(), kind: SymbolKind::Variable(inner.clone()) }).unwrap();
        assert_eq!(table.lookup("x").unwrap().kind, SymbolKind::Variable(inner));
        table.exit_scope();
        assert_eq!(table.lookup("x").unwrap().kind, SymbolKind::Variable(Type::int()));
    }

    #[test]
    fn reports_undeclared_identifiers_and_void_objects() {
        let output = analyze_source("void bad; int f() { return missing; }");
        assert!(output.errors.iter().any(|e| e.message.contains("type void")));
        assert!(output.errors.iter().any(|e| e.message.contains("undeclared identifier 'missing'")));
    }

    #[test]
    fn checks_function_signatures_calls_and_returns() {
        let output = analyze_source("int add(int a, int b); int add(int a, char b) { return a + b; } int main() { return add(1); }");
        assert!(output.errors.iter().any(|e| e.message.contains("conflicting declaration")));
        assert!(output.errors.iter().any(|e| e.message.contains("expects 2 argument(s)")));
    }

    #[test]
    fn validates_scope_and_return_types() {
        let output = analyze_source("void f() { return 1; } int g() { int x; { int y; } return y; }");
        assert!(output.errors.iter().any(|e| e.message.contains("void function cannot return a value")));
        assert!(output.errors.iter().any(|e| e.message.contains("undeclared identifier 'y'")));
    }

    #[test]
    fn accepts_struct_member_and_valid_control_flow() {
        let output = analyze_source("struct Pair { int x; }; int main() { struct Pair p; p.x = 2; if (p.x) return p.x; else return 0; }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
    }

    #[test]
    fn recognizes_builtin_syscall_functions() {
        assert_ok("int main() { novus_write(\"ok\", 2); novus_yield(); novus_exit(0); return 0; }");
    }

    #[test]
    fn locations_are_present_on_all_semantic_errors() {
        let output = analyze_source("int f() { return nope; }");
        assert!(!output.errors.is_empty());
        assert!(output.errors.iter().all(|e| e.location.line > 0 && e.location.column > 0));
    }

    #[test]
    fn rejects_undefined_extern_globals_but_accepts_later_definition() {
        let missing = analyze_source("extern int external_value; int main() { return external_value; }");
        assert!(missing.errors.iter().any(|e| e.message.contains("has no definition")));
        let defined = analyze_source("extern int external_value; int external_value; int main() { return external_value; }");
        assert!(defined.errors.is_empty(), "{:?}", defined.errors);
    }

    #[test]
    fn deduces_auto_variables_and_references() {
        assert_ok("int main() { auto value = 3; auto* pointer = &value; auto& alias = value; alias = 4; return *pointer + value; }");
        let output = analyze_source("int main() { auto value; return 0; }");
        assert!(output.errors.iter().any(|e| e.message.contains("requires an initializer")));
    }

    #[test]
    fn resolves_enum_constants_and_scoped_enums() {
        assert_ok("enum class Color : int { Red, Green = 4, Blue }; int main() { Color c = Color::Blue; return c == Color::Green ? 0 : 1; }");
        let output = analyze_source("enum E { A = 1 + 2 }; int main() { return A; }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
    }

    #[test]
    fn evaluates_static_assert_and_sizeof() {
        assert_ok("static_assert(sizeof(int) == 4, \"int\"); static_assert(sizeof(long) == 8, \"long\"); int main() { return 0; }");
        let output = analyze_source("static_assert(sizeof(int) == 8, \"wrong\"); int main() { return 0; }");
        assert!(output.errors.iter().any(|e| e.message.contains("static assertion failed: wrong")));
    }

    #[test]
    fn checks_aggregate_initializers() {
        assert_ok(
            "struct Point { int x; int y; }; \
             int main() { int values[3] = {1, 2, 3}; struct Point p = {4, 5}; char text[] = \"hi\"; \
             return values[2] + p.x + text[0]; }",
        );
        let output = analyze_source("struct Point { int x; int y; }; int main() { struct Point p = {4}; return p.x; }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
        let too_many = analyze_source("int main() { int values[2] = {1, 2, 3}; return 0; }");
        assert!(too_many.errors.iter().any(|e| e.message.contains("too many initializers")));
    }

    #[test]
    fn range_for_requires_fixed_size_arrays() {
        assert_ok("int main() { int values[3] = {1, 2, 3}; int total = 0; for (auto value : values) total += value; return total; }");
        let output = analyze_source("int main() { int* pointer = 0; int total = 0; for (auto value : pointer) total += value; return total; }");
        assert!(output.errors.iter().any(|e| e.message.contains("requires a fixed-size array")));
    }

    #[test]
    fn reference_parameters_require_lvalues() {
        assert_ok("int read(const int& value) { return value; } int main() { int x = 3; return read(x); }");
        let output = analyze_source("int read(const int& value) { return value; } int main() { return read(3); }");
        assert!(output.errors.iter().any(|e| e.message.contains("must be an lvalue")));
    }

    #[test]
    fn null_pointer_constants_convert_to_pointers() {
        assert_ok(
            "int main() { const char* a = nullptr; char* b = 0; const char* c = \"x\"; \
             if (a == nullptr) return 1; if (b != 0) return 2; return c != 0 ? 0 : 3; }",
        );
    }
}
