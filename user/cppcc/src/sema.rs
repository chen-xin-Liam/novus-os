use crate::ast::*;
use crate::diagnostic::{CompileError, ErrorCollector};
use std::collections::{HashMap, HashSet};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SymbolKind {
    Variable(Type),
    Function { return_type: Type, params: Vec<Type>, defined: bool },
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
}

#[derive(Debug)]
pub struct SemaOutput {
    pub errors: Vec<CompileError>,
    pub symbols: SymbolTable,
}

pub fn analyze(unit: &mut TranslationUnit) -> SemaOutput {
    Analyzer::new().analyze(unit)
}

struct Analyzer {
    symbols: SymbolTable,
    structs: HashMap<String, HashMap<String, Type>>,
    struct_definitions: HashSet<String>,
    errors: ErrorCollector,
    loop_depth: usize,
    unresolved_globals: HashMap<String, crate::diagnostic::SourceLocation>,
    defined_globals: HashSet<String>,
}

impl Analyzer {
    fn new() -> Self {
        let mut symbols = SymbolTable::new();
        let mut write_string = Type::new(BaseType::Char);
        write_string.is_const = true;
        write_string.pointers = 1;
        let mut write_length = Type::new(BaseType::Long);
        write_length.is_unsigned = true;
        let _ = symbols.define(Symbol {
            name: "novus_write".to_owned(),
            kind: SymbolKind::Function {
                return_type: Type::new(BaseType::Long),
                params: vec![write_string, write_length],
                defined: true,
            },
        });
        let _ = symbols.define(Symbol {
            name: "novus_yield".to_owned(),
            kind: SymbolKind::Function {
                return_type: Type::void(),
                params: Vec::new(),
                defined: true,
            },
        });
        let _ = symbols.define(Symbol {
            name: "novus_exit".to_owned(),
            kind: SymbolKind::Function {
                return_type: Type::void(),
                params: vec![Type::int()],
                defined: true,
            },
        });
        Self {
            symbols,
            structs: HashMap::new(),
            struct_definitions: HashSet::new(),
            errors: ErrorCollector::default(),
            loop_depth: 0,
            unresolved_globals: HashMap::new(),
            defined_globals: HashSet::new(),
        }
    }

    fn analyze(mut self, unit: &mut TranslationUnit) -> SemaOutput {
        self.collect_structs(unit);
        self.collect_globals(unit);
        for (name, location) in &self.unresolved_globals {
            self.errors.push(CompileError::new(
                location.clone(),
                format!("extern global '{}' has no definition in this translation unit", name),
            ));
        }
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
                    self.structs.entry(decl.name.clone()).or_default();
                    continue;
                }
                if !self.struct_definitions.insert(decl.name.clone()) {
                    self.errors.push(CompileError::new(decl.location.clone(), format!("redefinition of struct '{}'", decl.name)));
                    continue;
                }
                let mut fields = HashMap::new();
                for field in &decl.fields {
                    if field.ty.is_void() {
                        self.errors.push(CompileError::new(field.location.clone(), "structure field cannot have type void"));
                    }
                    if fields.insert(field.name.clone(), field.ty.clone()).is_some() {
                        self.errors.push(CompileError::new(field.location.clone(), format!("duplicate structure field '{}'", field.name)));
                    }
                }
                self.structs.insert(decl.name.clone(), fields);
            }
        }
    }

    fn collect_globals(&mut self, unit: &TranslationUnit) {
        for item in &unit.items {
            match item {
                TopLevel::Global(variable) => {
                    self.validate_object_type(&variable.ty, &variable.location);
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
                    if !function.return_type.is_void() {
                        self.validate_object_type(&function.return_type, &function.location);
                    }
                    for param in &function.params {
                        self.validate_parameter_type(&param.ty, &param.location);
                    }
                    if let Some(existing) = self.symbols.lookup_current(&function.name).cloned() {
                        match existing.kind {
                            SymbolKind::Function { return_type, params, defined } => {
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
                            },
                        });
                    }
                }
                TopLevel::Struct(_) => {}
            }
        }
    }

    fn check_global_initializers(&mut self, unit: &mut TranslationUnit) {
        for item in &mut unit.items {
            if let TopLevel::Global(variable) = item {
                if let Some(initializer) = &mut variable.initializer {
                    let ty = self.check_expr(initializer);
                    if !assignable(&variable.ty, &ty) {
                        self.errors.push(CompileError::new(initializer.location.clone(), format!("cannot initialize '{}' of type {} with {}", variable.name, describe_type(&variable.ty), describe_type(&ty))));
                    }
                    if !is_constant_expr(initializer) {
                        self.errors.push(CompileError::new(initializer.location.clone(), "global initializer must be a constant expression"));
                    }
                } else if variable.is_extern {
                    continue;
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
            self.check_stmt(body, &function.return_type, true);
            if !function.return_type.is_void()
                && function.name != "main"
                && !stmt_returns(body)
            {
                self.errors.push(CompileError::new(function.location.clone(), format!("non-void function '{}' may not return a value", function.name)));
            }
            self.symbols.exit_scope();
        }
    }

    fn validate_object_type(&mut self, ty: &Type, location: &crate::diagnostic::SourceLocation) {
        if ty.is_void() {
            self.errors.push(CompileError::new(location.clone(), "object cannot have type void"));
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

    fn validate_parameter_type(&mut self, ty: &Type, location: &crate::diagnostic::SourceLocation) {
        if ty.is_void() {
            self.errors.push(CompileError::new(location.clone(), "parameter cannot have type void"));
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
            StmtKind::Break | StmtKind::Continue => {
                if self.loop_depth == 0 {
                    self.errors.push(CompileError::new(stmt.location.clone(), "break/continue outside a loop"));
                }
            }
            StmtKind::Expression(expr) => { self.check_expr(expr); }
            StmtKind::Variable(variable) => {
                self.validate_object_type(&variable.ty, &variable.location);
                if self.symbols.define(Symbol { name: variable.name.clone(), kind: SymbolKind::Variable(variable.ty.clone()) }).is_err() {
                    self.errors.push(CompileError::new(variable.location.clone(), format!("redefinition of '{}' in this scope", variable.name)));
                }
                if let Some(initializer) = &mut variable.initializer {
                    let ty = self.check_expr(initializer);
                    if !assignable(&variable.ty, &ty) {
                        self.errors.push(CompileError::new(initializer.location.clone(), format!("cannot initialize '{}' of type {} with {}", variable.name, describe_type(&variable.ty), describe_type(&ty))));
                    }
                }
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
            StmtKind::Return(expr) => match (return_type.is_void(), expr) {
                (true, Some(expr)) => {
                    self.check_expr(expr);
                    self.errors.push(CompileError::new(expr.location.clone(), "void function cannot return a value"));
                }
                (false, None) => self.errors.push(CompileError::new(stmt.location.clone(), "non-void function must return a value")),
                (false, Some(expr)) => {
                    let actual = self.check_expr(expr);
                    if !assignable(return_type, &actual) {
                        self.errors.push(CompileError::new(expr.location.clone(), format!("return type {} does not match {}", describe_type(&actual), describe_type(return_type))));
                    }
                }
                (true, None) => {}
            },
        }
    }

    fn check_expr(&mut self, expr: &mut Expr) -> Type {
        let ty = match &mut expr.kind {
            ExprKind::Integer(_) | ExprKind::Character(_) => Type::int(),
            ExprKind::Boolean(_) => Type::new(BaseType::Bool),
            ExprKind::String(_) => {
                let mut ty = Type::new(BaseType::Char);
                ty.is_const = true;
                ty.pointers = 1;
                ty
            }
            ExprKind::Identifier(name) => match self.symbols.lookup(name).map(|s| s.kind.clone()) {
                Some(SymbolKind::Variable(ty)) => ty,
                Some(SymbolKind::Function { .. }) => {
                    self.errors.push(CompileError::new(expr.location.clone(), format!("function '{}' requires a call", name)));
                    Type::int()
                }
                None => {
                    self.errors.push(CompileError::new(expr.location.clone(), format!("use of undeclared identifier '{}'", name)));
                    Type::int()
                }
            },
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
                if assignable(&then_type, &else_type) { then_type }
                else if assignable(&else_type, &then_type) { else_type }
                else {
                    self.errors.push(CompileError::new(expr.location.clone(), "conditional expression branches have incompatible types"));
                    Type::int()
                }
            }
            ExprKind::Call { callee, args } => {
                if let ExprKind::Identifier(name) = &callee.kind {
                    match self.symbols.lookup(name).map(|s| s.kind.clone()) {
                        Some(SymbolKind::Function { return_type, params, defined }) => {
                            if !defined {
                                self.errors.push(CompileError::new(expr.location.clone(), format!("call to function '{}' which has no definition", name)));
                            }
                            if args.len() != params.len() {
                                self.errors.push(CompileError::new(expr.location.clone(), format!("function '{}' expects {} argument(s), got {}", name, params.len(), args.len())));
                            }
                            for (index, arg) in args.iter_mut().enumerate() {
                                let actual = self.check_expr(arg);
                                if let Some(expected) = params.get(index) {
                                    if !assignable(expected, &actual) {
                                        self.errors.push(CompileError::new(arg.location.clone(), format!("argument {} to '{}' has type {}, expected {}", index + 1, name, describe_type(&actual), describe_type(expected))));
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
                        let field_type = self.structs.get(struct_name).and_then(|fields| fields.get(name)).cloned();
                        if let Some(field_type) = field_type {
                            field_type
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

    fn check_binary(&mut self, op: &str, left_ty: &Type, right_ty: &Type, left: &Expr, right: &Expr, location: &crate::diagnostic::SourceLocation) -> Type {
        let assignment = matches!(op, "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=");
        if assignment {
            if !is_lvalue(left) || left_ty.is_const || left_ty.array_len.is_some() {
                self.errors.push(CompileError::new(location.clone(), "assignment requires a modifiable lvalue"));
            }
            if op == "=" {
                if !assignable(left_ty, right_ty) {
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
                if !(left_ty.is_integer() && right_ty.is_integer()) && !type_compatible(left_ty, right_ty) {
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

    fn require_condition(&mut self, ty: &Type, location: &crate::diagnostic::SourceLocation) {
        if !ty.is_scalar() {
            self.errors.push(CompileError::new(location.clone(), "condition must have scalar type"));
        }
    }
}

fn type_compatible(expected: &Type, actual: &Type) -> bool {
    let expected = expected.decay_array();
    let actual = actual.decay_array();
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
}

fn same_parameter_type(left: &Type, right: &Type) -> bool {
    let left = left.decay_array();
    let right = right.decay_array();
    same_declared_type(&left, &right)
}

fn assignable(expected: &Type, actual: &Type) -> bool {
    if expected.is_void() || actual.is_void() { return false; }
    type_compatible(expected, actual)
}

fn usual_arithmetic_type(left: &Type, right: &Type) -> Type {
    let rank = |ty: &Type| match &ty.base { BaseType::LongLong => 5, BaseType::Long => 4, BaseType::Int => 3, BaseType::Short => 2, BaseType::Char => 1, BaseType::Bool => 0, _ => 0 };
    let mut result = if rank(left) >= rank(right) { left.clone() } else { right.clone() };
    result.is_const = false;
    result.array_len = None;
    result
}

fn describe_type(ty: &Type) -> String {
    let base = match &ty.base {
        BaseType::Void => "void".to_owned(), BaseType::Bool => "bool".to_owned(),
        BaseType::Char => "char".to_owned(), BaseType::Short => "short".to_owned(),
        BaseType::Int => "int".to_owned(), BaseType::Long => "long".to_owned(),
        BaseType::LongLong => "long long".to_owned(), BaseType::Struct(name) => format!("struct {}", name),
    };
    format!("{}{}{}", if ty.is_unsigned { "unsigned " } else { "" }, base, "*".repeat(ty.pointers))
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
        ExprKind::Unary { operand, .. } => is_constant_expr(operand),
        ExprKind::Binary { left, right, .. } => is_constant_expr(left) && is_constant_expr(right),
        ExprKind::Ternary { condition, then_expr, else_expr } => is_constant_expr(condition) && is_constant_expr(then_expr) && is_constant_expr(else_expr),
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
    use crate::diagnostic::SourceLocation;

    fn loc() -> crate::diagnostic::SourceLocation { SourceLocation::new("sema.cc", 1, 1) }

    fn analyze_source(text: &str) -> SemaOutput {
        let locations = (0..text.len()).map(|i| SourceLocation::new("sema.cc", 1, i + 1)).collect();
        let pre = crate::preprocess::PreprocessedSource { text: text.to_owned(), locations };
        let lexed = crate::lexer::lex(&pre);
        let mut parsed = crate::parser::parse(lexed.tokens);
        let mut output = crate::sema::analyze(&mut parsed.unit);
        output.errors.splice(0..0, parsed.errors);
        output
    }

    #[test]
    fn symbol_table_has_nested_scope_lookup_and_shadowing() {
        let mut table = SymbolTable::new();
        table.define(Symbol { name: "x".into(), kind: SymbolKind::Variable(Type::int()) }).unwrap();
        table.enter_scope();
        let mut inner = Type::new(BaseType::Char);
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
        let output = analyze_source("int main() { novus_write(\"ok\", 2); novus_yield(); novus_exit(0); return 0; }");
        assert!(output.errors.is_empty(), "{:?}", output.errors);
    }

    #[test]
    fn locations_are_present_on_all_semantic_errors() {
        let output = analyze_source("int f() { return nope; }");
        assert!(!output.errors.is_empty());
        assert!(output.errors.iter().all(|e| e.location.line > 0 && e.location.column > 0));
        let _ = loc();
    }

    #[test]
    fn rejects_undefined_extern_globals_but_accepts_later_definition() {
        let missing = analyze_source("extern int external_value; int main() { return external_value; }");
        assert!(missing.errors.iter().any(|e| e.message.contains("has no definition")));
        let defined = analyze_source("extern int external_value; int external_value; int main() { return external_value; }");
        assert!(defined.errors.is_empty(), "{:?}", defined.errors);
    }
}