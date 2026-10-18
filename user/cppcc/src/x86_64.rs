//! AST is already type-checked and expressions carry resolved types.
//! This single-target backend walks it directly; a second mutable IR would
//! duplicate scope/type state before there is another backend to reuse it.
use crate::ast::*;
use crate::diagnostic::CompileError;
use crate::nucgen::{CodeSections, NucRelocation, NucSymbol, SegmentKind};
use anyhow::{bail, Result};
use std::collections::{HashMap, HashSet};

const RELOC_32: u32 = 1;
const RELOC_PC32: u32 = 2;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
enum Reg {
    Rax = 0,
    Rcx = 1,
    Rdx = 2,
    Rbx = 3,
    Rsp = 4,
    Rbp = 5,
    Rsi = 6,
    Rdi = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
}

const ARG_REGS: [Reg; 6] = [Reg::Rdi, Reg::Rsi, Reg::Rdx, Reg::Rcx, Reg::R8, Reg::R9];

#[derive(Clone, Debug)]
struct StructLayout {
    fields: HashMap<String, usize>,
    size: usize,
    align: usize,
}

struct Layouts {
    definitions: HashMap<String, Vec<Variable>>,
    layouts: HashMap<String, StructLayout>,
}

impl Layouts {
    fn from_unit(unit: &TranslationUnit) -> Result<Self> {
        let mut layouts = Self {
            definitions: HashMap::new(),
            layouts: HashMap::new(),
        };
        for item in &unit.items {
            if let TopLevel::Struct(definition) = item {
                if definition.is_definition {
                    layouts.definitions.insert(definition.name.clone(), definition.fields.clone());
                }
            }
        }
        let names: Vec<String> = layouts.definitions.keys().cloned().collect();
        for name in names {
            layouts.layout_struct(&name, &mut HashSet::new())?;
        }
        Ok(layouts)
    }

    fn layout_struct(&mut self, name: &str, active: &mut HashSet<String>) -> Result<StructLayout> {
        if let Some(layout) = self.layouts.get(name) {
            return Ok(layout.clone());
        }
        if !active.insert(name.to_owned()) {
            bail!("recursive by-value struct '{}' has no finite size", name);
        }
        let fields = self.definitions.get(name)
            .ok_or_else(|| anyhow::anyhow!("incomplete struct '{}'", name))?.clone();
        let mut field_offsets = HashMap::new();
        let mut size = 0usize;
        let mut struct_align = 1usize;
        for field in &fields {
            let (field_size, field_align) = self.size_align(&field.ty, active)?;
            size = align_to(size, field_align)?;
            field_offsets.insert(field.name.clone(), size);
            size = size.checked_add(field_size).ok_or_else(|| anyhow::anyhow!("struct size overflow"))?;
            struct_align = struct_align.max(field_align);
        }
        size = align_to(size.max(1), struct_align)?;
        active.remove(name);
        let layout = StructLayout { fields: field_offsets, size, align: struct_align };
        self.layouts.insert(name.to_owned(), layout.clone());
        Ok(layout)
    }

    fn size_align(&mut self, ty: &Type, active: &mut HashSet<String>) -> Result<(usize, usize)> {
        if ty.pointers > 0 {
            return Ok((8, 8));
        }
        let (base_size, base_align) = match &ty.base {
            BaseType::Void => bail!("void has no object size"),
            BaseType::Bool | BaseType::Char => (1, 1),
            BaseType::Short => (2, 2),
            BaseType::Int => (4, 4),
            BaseType::Long | BaseType::LongLong => (8, 8),
            BaseType::Struct(name) => {
                let layout = self.layout_struct(name, active)?;
                (layout.size, layout.align)
            }
        };
        if let Some(length) = ty.array_len {
            Ok((base_size.checked_mul(length).ok_or_else(|| anyhow::anyhow!("array size overflow"))?, base_align))
        } else {
            Ok((base_size, base_align))
        }
    }

    fn size_of(&mut self, ty: &Type) -> Result<usize> {
        self.size_align(ty, &mut HashSet::new()).map(|pair| pair.0)
    }

    fn align_of(&mut self, ty: &Type) -> Result<usize> {
        self.size_align(ty, &mut HashSet::new()).map(|pair| pair.1)
    }

    fn size_align_known(&self, ty: &Type) -> Result<(usize, usize)> {
        if ty.pointers > 0 { return Ok((8, 8)); }
        let (size, align) = match &ty.base {
            BaseType::Void => bail!("void has no object size"),
            BaseType::Bool | BaseType::Char => (1, 1),
            BaseType::Short => (2, 2),
            BaseType::Int => (4, 4),
            BaseType::Long | BaseType::LongLong => (8, 8),
            BaseType::Struct(name) => {
                let layout = self.layouts.get(name).ok_or_else(|| anyhow::anyhow!("incomplete struct '{}'", name))?;
                (layout.size, layout.align)
            }
        };
        if let Some(length) = ty.array_len {
            Ok((size.checked_mul(length).ok_or_else(|| anyhow::anyhow!("array size overflow"))?, align))
        } else {
            Ok((size, align))
        }
    }

    fn member_offset(&self, struct_name: &str, field: &str) -> Result<usize> {
        self.layouts.get(struct_name)
            .and_then(|layout| layout.fields.get(field))
            .copied()
            .ok_or_else(|| anyhow::anyhow!("struct '{}' has no member '{}'", struct_name, field))
    }
}

#[derive(Clone, Debug)]
struct GlobalObject {
    segment: SegmentKind,
    offset: usize,
    ty: Type,
    size: usize,
}

struct Generator<'a> {
    unit: &'a TranslationUnit,
    layouts: Layouts,
    sections: CodeSections,
    globals: HashMap<String, GlobalObject>,
    strings: HashMap<Vec<u8>, String>,
    string_order: Vec<Vec<u8>>,
}

pub(crate) fn generate(unit: &TranslationUnit) -> Result<CodeSections> {
    let mut generator = Generator {
        unit,
        layouts: Layouts::from_unit(unit)?,
        sections: CodeSections::default(),
        globals: HashMap::new(),
        strings: HashMap::new(),
        string_order: Vec::new(),
    };
    generator.collect_strings();
    generator.add_string_objects();
    generator.prepare_globals()?;
    generator.emit_program()?;
    Ok(generator.sections)
}

impl Generator<'_> {
    fn collect_strings(&mut self) {
        for item in &self.unit.items {
            match item {
                TopLevel::Global(variable) => {
                    if let Some(initializer) = &variable.initializer { self.visit_expr_strings(initializer); }
                }
                TopLevel::Function(function) => {
                    if let Some(body) = &function.body { self.visit_stmt_strings(body); }
                }
                TopLevel::Struct(_) => {}
            }
        }
    }

    fn visit_stmt_strings(&mut self, stmt: &Stmt) {
        match &stmt.kind {
            StmtKind::Block(items) => for item in items { self.visit_stmt_strings(item); },
            StmtKind::Expression(expr) => self.visit_expr_strings(expr),
            StmtKind::Variable(variable) => if let Some(expr) = &variable.initializer { self.visit_expr_strings(expr); },
            StmtKind::If { condition, then_branch, else_branch } => {
                self.visit_expr_strings(condition);
                self.visit_stmt_strings(then_branch);
                if let Some(branch) = else_branch { self.visit_stmt_strings(branch); }
            }
            StmtKind::While { condition, body } => { self.visit_expr_strings(condition); self.visit_stmt_strings(body); }
            StmtKind::For { init, condition, step, body } => {
                if let Some(init) = init { self.visit_stmt_strings(init); }
                if let Some(condition) = condition { self.visit_expr_strings(condition); }
                if let Some(step) = step { self.visit_expr_strings(step); }
                self.visit_stmt_strings(body);
            }
            StmtKind::Return(expr) => if let Some(expr) = expr { self.visit_expr_strings(expr); },
            StmtKind::Empty | StmtKind::Break | StmtKind::Continue => {}
        }
    }

    fn visit_expr_strings(&mut self, expr: &Expr) {
        match &expr.kind {
            ExprKind::String(bytes) => {
                if !self.strings.contains_key(bytes) {
                    let name = format!(".L.str.{}", self.string_order.len());
                    self.strings.insert(bytes.clone(), name);
                    self.string_order.push(bytes.clone());
                }
            }
            ExprKind::Unary { operand, .. } | ExprKind::Postfix { operand, .. } => self.visit_expr_strings(operand),
            ExprKind::Binary { left, right, .. } => { self.visit_expr_strings(left); self.visit_expr_strings(right); }
            ExprKind::Ternary { condition, then_expr, else_expr } => {
                self.visit_expr_strings(condition); self.visit_expr_strings(then_expr); self.visit_expr_strings(else_expr);
            }
            ExprKind::Call { callee, args } => {
                self.visit_expr_strings(callee);
                for arg in args { self.visit_expr_strings(arg); }
            }
            ExprKind::Index { base, index } => { self.visit_expr_strings(base); self.visit_expr_strings(index); }
            ExprKind::Member { base, .. } => self.visit_expr_strings(base),
            ExprKind::Integer(_) | ExprKind::Character(_) | ExprKind::Boolean(_) | ExprKind::Identifier(_) => {}
        }
    }

    fn add_string_objects(&mut self) {
        for bytes in &self.string_order {
            let offset = self.sections.rodata.len();
            self.sections.rodata.extend_from_slice(bytes);
            self.sections.rodata.push(0);
            self.sections.symbols.push(NucSymbol {
                name: self.strings[bytes].clone(),
                sym_type: 2,
                bind: 0,
                segment: SegmentKind::Rodata,
                value: offset as u64,
                size: (bytes.len() + 1) as u64,
            });
        }
    }

    fn prepare_globals(&mut self) -> Result<()> {
        let globals: Vec<Variable> = self.unit.items.iter().filter_map(|item| match item {
            TopLevel::Global(variable) if !variable.is_extern || variable.initializer.is_some() => Some(variable.clone()),
            _ => None,
        }).collect();
        for variable in &globals {
            if self.globals.contains_key(&variable.name) { continue; }
            let size = self.layouts.size_of(&variable.ty)?;
            let align = self.layouts.align_of(&variable.ty)?;
            let segment = if variable.ty.is_const && variable.initializer.is_some() {
                SegmentKind::Rodata
            } else if variable.initializer.is_some() {
                SegmentKind::Data
            } else {
                SegmentKind::Bss
            };
            let section_len = match segment {
                SegmentKind::Rodata => &mut self.sections.rodata,
                SegmentKind::Data => &mut self.sections.data,
                SegmentKind::Bss => {
                    self.sections.bss_size = align_to(self.sections.bss_size as usize, align)? as u64;
                    let offset = self.sections.bss_size as usize;
                    self.sections.bss_size = self.sections.bss_size.checked_add(size as u64).ok_or_else(|| anyhow::anyhow!("BSS size overflow"))?;
                    self.globals.insert(variable.name.clone(), GlobalObject { segment, offset, ty: variable.ty.clone(), size });
                    continue;
                }
                SegmentKind::Text => unreachable!(),
            };
            let offset = align_to(section_len.len(), align)?;
            section_len.resize(offset.checked_add(size).ok_or_else(|| anyhow::anyhow!("global data size overflow"))?, 0);
            self.globals.insert(variable.name.clone(), GlobalObject { segment, offset, ty: variable.ty.clone(), size });
        }

        for variable in &globals {
            let object = self.globals[&variable.name].clone();
            if let Some(initializer) = &variable.initializer {
                self.emit_global_initializer(variable, &object, initializer)?;
            }
            self.sections.symbols.push(NucSymbol {
                name: variable.name.clone(),
                sym_type: 2,
                bind: if variable.is_static { 0 } else { 1 },
                segment: object.segment,
                value: object.offset as u64,
                size: object.size as u64,
            });
        }
        Ok(())
    }

    fn emit_global_initializer(&mut self, variable: &Variable, object: &GlobalObject, expr: &Expr) -> Result<()> {
        if object.ty.pointers > 0 {
            let target = match &expr.kind {
                ExprKind::String(bytes) => self.strings.get(bytes).cloned(),
                ExprKind::Unary { op, operand } if op == "&" => match &operand.kind {
                    ExprKind::Identifier(name) => Some(name.clone()),
                    _ => None,
                },
                _ => None,
            }.ok_or_else(|| codegen_error(&expr.location, "global pointer initializer must name a string or global object"))?;
            if object.size != 8 {
                bail!("{}", codegen_error(&variable.location, "pointer object must be 64 bits"));
            }
            self.sections.relocations.push(NucRelocation {
                place_segment: object.segment,
                place_offset: object.offset as u64,
                kind: RELOC_32,
                symbol: target,
            });
            return Ok(());
        }
        let value = const_eval(expr).ok_or_else(|| codegen_error(&expr.location, "global initializer is not a supported constant"))?;
        if object.size > 8 || object.ty.array_len.is_some() || matches!(&object.ty.base, BaseType::Struct(_)) {
            bail!("{}", codegen_error(&variable.location, "aggregate initialization is not implemented"));
        }
        let target = match object.segment {
            SegmentKind::Rodata => &mut self.sections.rodata,
            SegmentKind::Data => &mut self.sections.data,
            SegmentKind::Bss => bail!("initialized global was placed in BSS"),
            SegmentKind::Text => unreachable!(),
        };
        let value_bytes = (value as u64).to_le_bytes();
        target[object.offset..object.offset + object.size].copy_from_slice(&value_bytes[..object.size]);
        Ok(())
    }

    fn emit_program(&mut self) -> Result<()> {
        let main = self.unit.items.iter().find_map(|item| match item {
            TopLevel::Function(function) if function.name == "main" && function.body.is_some() => Some(function),
            _ => None,
        }).ok_or_else(|| anyhow::anyhow!("entry requires a defined main function"))?;
        if main.return_type.base != BaseType::Int || main.return_type.pointers != 0 || main.return_type.is_unsigned || !main.params.is_empty() {
            bail!("{}", codegen_error(&main.location, "entry main must have signature int main()"));
        }
        if self.unit.items.iter().any(|item| matches!(item, TopLevel::Function(function) if function.name == "_start" && function.body.is_some())) {
            bail!("user-defined _start conflicts with the generated NUC runtime entry");
        }

        self.sections.text.extend_from_slice(&[
            0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
            0xE8, 0, 0, 0, 0,             // call main
            0x48, 0x83, 0xC4, 0x08,       // add rsp, 8
            0x48, 0x89, 0xC7,             // mov rdi, rax
            0xB8, 0x3C, 0, 0, 0,          // mov eax, SYS_EXIT
            0x0F, 0x05,                   // syscall
            0xF4,                         // hlt if SYS_EXIT unexpectedly returns
        ]);
        self.sections.symbols.push(NucSymbol {
            name: "_start".into(), sym_type: 1, bind: 0,
            segment: SegmentKind::Text, value: 0, size: self.sections.text.len() as u64,
        });
        self.sections.relocations.push(NucRelocation {
            place_segment: SegmentKind::Text, place_offset: 5,
            kind: RELOC_PC32, symbol: "main".into(),
        });

        for item in &self.unit.items {
            let TopLevel::Function(function) = item else { continue };
            let Some(body) = &function.body else { continue };
            if is_builtin(&function.name) { continue; }
            if function.name == "_start" { continue; }
            if function.params.len() > ARG_REGS.len() {
                bail!("{}", codegen_error(&function.location, "functions with more than six parameters are not implemented"));
            }
            let start = self.sections.text.len();
            let generated = FunctionGenerator::new(self, function)?.generate(body)?;
            self.sections.text.extend_from_slice(&generated.code);
            for mut relocation in generated.relocations {
                relocation.place_offset += start as u64;
                self.sections.relocations.push(relocation);
            }
            self.sections.symbols.push(NucSymbol {
                name: function.name.clone(),
                sym_type: 1,
                bind: if function.is_static { 0 } else { 1 },
                segment: SegmentKind::Text,
                value: start as u64,
                size: generated.code.len() as u64,
            });
        }
        Ok(())
    }
}

fn codegen_error(location: &crate::diagnostic::SourceLocation, message: &str) -> CompileError {
    CompileError::new(location.clone(), message)
}

fn align_to(value: usize, alignment: usize) -> Result<usize> {
    value.checked_add(alignment - 1)
        .map(|value| value & !(alignment - 1))
        .ok_or_else(|| anyhow::anyhow!("alignment overflow"))
}

fn bool_value(value: bool) -> i64 {
    if value { 1 } else { 0 }
}

fn const_eval(expr: &Expr) -> Option<i64> {
    match &expr.kind {
        ExprKind::Integer(value) | ExprKind::Character(value) => Some(*value),
        ExprKind::Boolean(value) => Some(bool_value(*value)),
        ExprKind::Unary { op, operand } => {
            let value = const_eval(operand)?;
            match op.as_str() { "+" => Some(value), "-" => value.checked_neg(), "~" => Some(!value), "!" => Some(bool_value(value == 0)), _ => None }
        }
        ExprKind::Binary { op, left, right } => {
            let left = const_eval(left)?;
            let right = const_eval(right)?;
            match op.as_str() {
                "+" => left.checked_add(right), "-" => left.checked_sub(right), "*" => left.checked_mul(right),
                "/" => left.checked_div(right), "%" => left.checked_rem(right),
                "<<" => u32::try_from(right).ok().and_then(|shift| left.checked_shl(shift)),
                ">>" => u32::try_from(right).ok().and_then(|shift| left.checked_shr(shift)),
                "&" => Some(left & right), "|" => Some(left | right), "^" => Some(left ^ right),
                "==" => Some(bool_value(left == right)), "!=" => Some(bool_value(left != right)),
                "<" => Some(bool_value(left < right)), "<=" => Some(bool_value(left <= right)),
                ">" => Some(bool_value(left > right)), ">=" => Some(bool_value(left >= right)),
                "&&" => Some(bool_value(left != 0 && right != 0)), "||" => Some(bool_value(left != 0 || right != 0)),
                _ => None,
            }
        }
        ExprKind::Ternary { condition, then_expr, else_expr } => {
            if const_eval(condition)? != 0 { const_eval(then_expr) } else { const_eval(else_expr) }
        }
        _ => None,
    }
}

struct FunctionOutput {
    code: Vec<u8>,
    relocations: Vec<NucRelocation>,
}

#[derive(Clone)]
struct LocalBinding {
    offset: i32,
    ty: Type,
}

#[derive(Clone, Copy)]
struct LoopLabels {
    break_label: usize,
    continue_label: usize,
}

struct BranchFixup {
    offset: usize,
    label: usize,
}

struct FunctionGenerator<'g, 'u> {
    generator: &'g Generator<'u>,
    function: Function,
    code: Vec<u8>,
    relocations: Vec<NucRelocation>,
    locals: Vec<HashMap<String, LocalBinding>>,
    local_cursor: usize,
    frame_size: usize,
    stack_depth: usize,
    next_label: usize,
    labels: HashMap<usize, usize>,
    branch_fixups: Vec<BranchFixup>,
    loops: Vec<LoopLabels>,
    epilogue: usize,
}

impl<'g, 'u> FunctionGenerator<'g, 'u> {
    fn new(generator: &'g Generator<'u>, function: &Function) -> Result<Self> {
        let body = function.body.as_ref().ok_or_else(|| anyhow::anyhow!("function body is missing"))?;
        if function.params.len() > ARG_REGS.len() {
            bail!("{}", codegen_error(&function.location, "functions with more than six parameters are not implemented"));
        }
        let mut local_bytes = 0usize;
        for param in &function.params {
            let ty = param.ty.decay_array();
            local_bytes = local_bytes.checked_add(align_to(generator.layouts.size_align_known(&ty)?.0.max(1), 8)?).ok_or_else(|| anyhow::anyhow!("stack frame size overflow"))?;
        }
        count_local_bytes(body, &generator.layouts, &mut local_bytes)?;
        let frame_size = align_to(local_bytes, 16)?;
        Ok(Self {
            generator,
            function: function.clone(),
            code: Vec::new(),
            relocations: Vec::new(),
            locals: vec![HashMap::new()],
            local_cursor: 0,
            frame_size,
            stack_depth: 0,
            next_label: 0,
            labels: HashMap::new(),
            branch_fixups: Vec::new(),
            loops: Vec::new(),
            epilogue: 0,
        })
    }

    fn generate(mut self, body: &Stmt) -> Result<FunctionOutput> {
        self.epilogue = self.new_label();
        self.emit_u8(0x55); // push rbp
        self.emit_bytes(&[0x48, 0x89, 0xE5]); // mov rbp, rsp
        if self.frame_size != 0 {
            self.adjust_rsp(false, self.frame_size)?;
        }
        let params = self.function.params.clone();
        for (index, param) in params.iter().enumerate() {
            let ty = param.ty.decay_array();
            let name = param.name.as_ref().ok_or_else(|| anyhow::anyhow!("{}", codegen_error(&param.location, "function definition parameter requires a name")))?;
            let offset = self.allocate_local(name, ty.clone())?;
            self.emit_mov_rr(Reg::Rax, ARG_REGS[index], true);
            self.emit_store(Reg::Rax, Reg::Rbp, -offset, &ty)?;
        }
        self.emit_stmt(body, true)?;
        self.emit_mov_imm64(Reg::Rax, 0);
        self.place_label(self.epilogue);
        if self.stack_depth != 0 {
            bail!("unbalanced temporary stack in function '{}'", self.function.name);
        }
        self.emit_bytes(&[0x48, 0x89, 0xEC, 0x5D, 0xC3]); // mov rsp, rbp; pop rbp; ret
        self.resolve_branches()?;
        Ok(FunctionOutput { code: self.code, relocations: self.relocations })
    }

    fn emit_stmt(&mut self, stmt: &Stmt, is_function_body: bool) -> Result<()> {
        match &stmt.kind {
            StmtKind::Block(statements) => {
                if !is_function_body { self.locals.push(HashMap::new()); }
                for statement in statements { self.emit_stmt(statement, false)?; }
                if !is_function_body { self.locals.pop(); }
            }
            StmtKind::Empty => {}
            StmtKind::Expression(expr) => self.emit_expr(expr)?,
            StmtKind::Variable(variable) => {
                if variable.is_static {
                    bail!("{}", codegen_error(&variable.location, "static local variables are not implemented"));
                }
                if variable.ty.is_void() || matches!(&variable.ty.base, BaseType::Struct(_)) && variable.ty.pointers == 0 && variable.ty.array_len.is_none() {
                    if matches!(&variable.ty.base, BaseType::Struct(_)) && variable.ty.pointers == 0 && variable.ty.array_len.is_none() {
                        self.allocate_local(&variable.name, variable.ty.clone())?;
                    } else {
                        bail!("{}", codegen_error(&variable.location, "void local object is invalid"));
                    }
                } else {
                    self.allocate_local(&variable.name, variable.ty.clone())?;
                }
                if let Some(initializer) = &variable.initializer {
                    if variable.ty.array_len.is_some() || matches!(&variable.ty.base, BaseType::Struct(_)) && variable.ty.pointers == 0 {
                        bail!("{}", codegen_error(&variable.location, "aggregate local initialization is not implemented"));
                    }
                    self.emit_expr(initializer)?;
                    let local = self.lookup_local(&variable.name).ok_or_else(|| anyhow::anyhow!("local variable allocation failed"))?;
                    self.emit_store(Reg::Rax, Reg::Rbp, -local.offset, &local.ty)?;
                }
            }
            StmtKind::If { condition, then_branch, else_branch } => {
                let else_label = self.new_label();
                let end_label = self.new_label();
                self.emit_expr(condition)?;
                self.emit_test_rax();
                self.emit_jcc(0x84, else_label);
                self.emit_stmt(then_branch, false)?;
                if let Some(else_branch) = else_branch {
                    self.emit_jmp(end_label);
                    self.place_label(else_label);
                    self.emit_stmt(else_branch, false)?;
                    self.place_label(end_label);
                } else {
                    self.place_label(else_label);
                }
            }
            StmtKind::While { condition, body } => {
                let condition_label = self.new_label();
                let end_label = self.new_label();
                self.place_label(condition_label);
                self.emit_expr(condition)?;
                self.emit_test_rax();
                self.emit_jcc(0x84, end_label);
                self.loops.push(LoopLabels { break_label: end_label, continue_label: condition_label });
                self.emit_stmt(body, false)?;
                self.loops.pop();
                self.emit_jmp(condition_label);
                self.place_label(end_label);
            }
            StmtKind::For { init, condition, step, body } => {
                self.locals.push(HashMap::new());
                if let Some(init) = init { self.emit_stmt(init, false)?; }
                let condition_label = self.new_label();
                let step_label = self.new_label();
                let end_label = self.new_label();
                self.place_label(condition_label);
                if let Some(condition) = condition {
                    self.emit_expr(condition)?;
                    self.emit_test_rax();
                    self.emit_jcc(0x84, end_label);
                }
                self.loops.push(LoopLabels { break_label: end_label, continue_label: step_label });
                self.emit_stmt(body, false)?;
                self.loops.pop();
                self.place_label(step_label);
                if let Some(step) = step { self.emit_expr(step)?; }
                self.emit_jmp(condition_label);
                self.place_label(end_label);
                self.locals.pop();
            }
            StmtKind::Return(expr) => {
                if let Some(expr) = expr { self.emit_expr(expr)?; }
                else { self.emit_mov_imm64(Reg::Rax, 0); }
                self.emit_jmp(self.epilogue);
            }
            StmtKind::Break => {
                let target = self.loops.last().ok_or_else(|| anyhow::anyhow!("break outside loop"))?.break_label;
                self.emit_jmp(target);
            }
            StmtKind::Continue => {
                let target = self.loops.last().ok_or_else(|| anyhow::anyhow!("continue outside loop"))?.continue_label;
                self.emit_jmp(target);
            }
        }
        Ok(())
    }

    fn emit_expr(&mut self, expr: &Expr) -> Result<()> {
        let ty = expr.ty.as_ref().ok_or_else(|| anyhow::anyhow!("{}", codegen_error(&expr.location, "expression has no semantic type")))?;
        match &expr.kind {
            ExprKind::Integer(value) | ExprKind::Character(value) => self.emit_mov_imm64(Reg::Rax, *value as u64),
            ExprKind::Boolean(value) => self.emit_mov_imm64(Reg::Rax, if *value { 1 } else { 0 }),
            ExprKind::String(bytes) => {
                let name = self.generator.strings.get(bytes).ok_or_else(|| anyhow::anyhow!("string literal was not collected"))?.clone();
                self.emit_abs32_symbol(&name);
            }
            ExprKind::Identifier(name) => {
                if ty.array_len.is_some() {
                    self.emit_lvalue_address(expr)?;
                } else if let Some(local) = self.lookup_local(name) {
                    self.emit_load(Reg::Rax, Reg::Rbp, -local.offset, &local.ty)?;
                } else if let Some(global) = self.generator.globals.get(name).cloned() {
                    self.emit_abs32_symbol(name);
                    self.emit_load(Reg::Rax, Reg::Rax, 0, &global.ty)?;
                } else {
                    bail!("{}", codegen_error(&expr.location, "identifier is not an emitted object"));
                }
            }
            ExprKind::Unary { op, operand } => match op.as_str() {
                "&" => self.emit_lvalue_address(operand)?,
                "*" => { self.emit_expr(operand)?; self.emit_load(Reg::Rax, Reg::Rax, 0, ty)?; }
                "+" => self.emit_expr(operand)?,
                "-" => { self.emit_expr(operand)?; self.emit_bytes(&[0x48, 0xF7, 0xD8]); }
                "~" => { self.emit_expr(operand)?; self.emit_bytes(&[0x48, 0xF7, 0xD0]); }
                "!" => {
                    self.emit_expr(operand)?;
                    self.emit_test_rax();
                    self.emit_setcc(0x94);
                }
                "++" | "--" => self.emit_increment(operand, op == "++", false)?,
                _ => bail!("{}", codegen_error(&expr.location, "unsupported unary operator")),
            },
            ExprKind::Postfix { op, operand } => self.emit_increment(operand, op == "++", true)?,
            ExprKind::Binary { op, left, right } => self.emit_binary(expr, op, left, right)?,
            ExprKind::Ternary { condition, then_expr, else_expr } => {
                let else_label = self.new_label();
                let end_label = self.new_label();
                self.emit_expr(condition)?;
                self.emit_test_rax();
                self.emit_jcc(0x84, else_label);
                self.emit_expr(then_expr)?;
                self.emit_jmp(end_label);
                self.place_label(else_label);
                self.emit_expr(else_expr)?;
                self.place_label(end_label);
            }
            ExprKind::Call { callee, args } => {
                let ExprKind::Identifier(name) = &callee.kind else {
                    bail!("{}", codegen_error(&expr.location, "indirect function calls are not implemented"));
                };
                self.emit_call(name, args, &expr.location)?;
            }
            ExprKind::Index { .. } | ExprKind::Member { .. } => {
                self.emit_lvalue_address(expr)?;
                if ty.array_len.is_some() { return Ok(()); }
                self.emit_load(Reg::Rax, Reg::Rax, 0, ty)?;
            }
        }
        Ok(())
    }

    fn emit_binary(&mut self, expr: &Expr, op: &str, left: &Expr, right: &Expr) -> Result<()> {
        if matches!(op, "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=") {
            return self.emit_assignment(op, left, right);
        }
        if op == "&&" || op == "||" {
            return self.emit_logical(op, left, right);
        }
        let left_ty = left.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing left expression type"))?;
        let right_ty = right.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing right expression type"))?;
        if matches!(op, "+" | "-") && (left_ty.pointers > 0 || right_ty.pointers > 0) {
            return self.emit_pointer_arithmetic(op, left, right, left_ty, right_ty);
        }
        self.emit_expr(left)?;
        self.push(Reg::Rax);
        self.emit_expr(right)?;
        self.emit_mov_rr(Reg::Rcx, Reg::Rax, true);
        self.pop(Reg::Rax);
        match op {
            "+" => self.emit_bytes(&[0x48, 0x01, 0xC8]),
            "-" => self.emit_bytes(&[0x48, 0x29, 0xC8]),
            "*" => self.emit_bytes(&[0x48, 0x0F, 0xAF, 0xC1]),
            "/" | "%" => {
                if left_ty.is_unsigned {
                    self.emit_bytes(&[0x31, 0xD2, 0x48, 0xF7, 0xF1]);
                } else {
                    self.emit_bytes(&[0x48, 0x99, 0x48, 0xF7, 0xF9]);
                }
                if op == "%" { self.emit_mov_rr(Reg::Rax, Reg::Rdx, true); }
            }
            "&" => self.emit_bytes(&[0x48, 0x21, 0xC8]),
            "|" => self.emit_bytes(&[0x48, 0x09, 0xC8]),
            "^" => self.emit_bytes(&[0x48, 0x31, 0xC8]),
            "<<" => self.emit_bytes(&[0x48, 0xD3, 0xE0]),
            ">>" => {
                if left_ty.is_unsigned { self.emit_bytes(&[0x48, 0xD3, 0xE8]); }
                else { self.emit_bytes(&[0x48, 0xD3, 0xF8]); }
            }
            "==" | "!=" | "<" | "<=" | ">" | ">=" => {
                self.emit_bytes(&[0x48, 0x39, 0xC8]);
                self.emit_setcc(condition_code(op, left_ty.is_unsigned));
            }
            _ => bail!("{}", codegen_error(&expr.location, "unsupported binary operator")),
        }
        Ok(())
    }

    fn emit_assignment(&mut self, op: &str, left: &Expr, right: &Expr) -> Result<()> {
        let left_ty = left.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing assignment target type"))?;
        self.emit_lvalue_address(left)?;
        self.push(Reg::Rax);
        if op == "=" {
            self.emit_expr(right)?;
        } else {
            self.emit_mov_rr(Reg::R11, Reg::Rax, true);
            self.emit_load(Reg::Rax, Reg::R11, 0, left_ty)?;
            self.push(Reg::Rax);
            self.emit_expr(right)?;
            if matches!(op, "+=" | "-=") && left_ty.pointers > 0 {
                let element_size = self.pointee_size(left_ty)?;
                if element_size > 1 { self.emit_scale_rax(element_size)?; }
            }
            self.emit_mov_rr(Reg::Rcx, Reg::Rax, true);
            self.pop(Reg::Rax);
            self.emit_compound_op(op, left_ty)?;
        }
        self.pop(Reg::R11);
        self.emit_store(Reg::Rax, Reg::R11, 0, left_ty)
    }

    fn emit_compound_op(&mut self, op: &str, left_ty: &Type) -> Result<()> {
        match op {
            "+=" => self.emit_bytes(&[0x48, 0x01, 0xC8]),
            "-=" => self.emit_bytes(&[0x48, 0x29, 0xC8]),
            "*=" => self.emit_bytes(&[0x48, 0x0F, 0xAF, 0xC1]),
            "/=" | "%=" => {
                if left_ty.is_unsigned { self.emit_bytes(&[0x31, 0xD2, 0x48, 0xF7, 0xF1]); }
                else { self.emit_bytes(&[0x48, 0x99, 0x48, 0xF7, 0xF9]); }
                if op == "%=" { self.emit_mov_rr(Reg::Rax, Reg::Rdx, true); }
            }
            "&=" => self.emit_bytes(&[0x48, 0x21, 0xC8]),
            "|=" => self.emit_bytes(&[0x48, 0x09, 0xC8]),
            "^=" => self.emit_bytes(&[0x48, 0x31, 0xC8]),
            "<<=" => self.emit_bytes(&[0x48, 0xD3, 0xE0]),
            ">>=" => {
                if left_ty.is_unsigned { self.emit_bytes(&[0x48, 0xD3, 0xE8]); }
                else { self.emit_bytes(&[0x48, 0xD3, 0xF8]); }
            }
            _ => bail!("unsupported compound assignment"),
        }
        Ok(())
    }

    fn emit_pointer_arithmetic(&mut self, op: &str, left: &Expr, right: &Expr, left_ty: &Type, right_ty: &Type) -> Result<()> {
        if left_ty.pointers > 0 && right_ty.pointers > 0 {
            self.emit_expr(left)?;
            self.push(Reg::Rax);
            self.emit_expr(right)?;
            self.emit_mov_rr(Reg::Rcx, Reg::Rax, true);
            self.pop(Reg::Rax);
            self.emit_bytes(&[0x48, 0x29, 0xC8]);
            let element_size = self.pointee_size(left_ty)?;
            if element_size > 1 {
                self.emit_mov_imm64(Reg::Rcx, element_size as u64);
                self.emit_bytes(&[0x48, 0x99, 0x48, 0xF7, 0xF9]);
            }
            return Ok(());
        }
        if left_ty.pointers > 0 {
            self.emit_expr(left)?;
            self.push(Reg::Rax);
            self.emit_expr(right)?;
            let element_size = self.pointee_size(left_ty)?;
            if element_size > 1 { self.emit_scale_rax(element_size)?; }
            self.emit_mov_rr(Reg::Rcx, Reg::Rax, true);
            self.pop(Reg::Rax);
            self.emit_bytes(if op == "+" { &[0x48, 0x01, 0xC8] } else { &[0x48, 0x29, 0xC8] });
        } else if right_ty.pointers > 0 && op == "+" {
            self.emit_expr(left)?;
            let element_size = self.pointee_size(right_ty)?;
            if element_size > 1 { self.emit_scale_rax(element_size)?; }
            self.push(Reg::Rax);
            self.emit_expr(right)?;
            self.emit_mov_rr(Reg::Rcx, Reg::Rax, true);
            self.pop(Reg::Rax);
            self.emit_bytes(&[0x48, 0x01, 0xC8]);
        } else {
            bail!("invalid pointer arithmetic");
        }
        Ok(())
    }

    fn emit_logical(&mut self, op: &str, left: &Expr, right: &Expr) -> Result<()> {
        let short_label = self.new_label();
        let end_label = self.new_label();
        self.emit_expr(left)?;
        self.emit_test_rax();
        if op == "&&" { self.emit_jcc(0x84, short_label); }
        else { self.emit_jcc(0x85, short_label); }
        self.emit_expr(right)?;
        self.emit_test_rax();
        if op == "&&" {
            self.emit_mov_imm32(Reg::Rax, 1);
            self.emit_jcc(0x85, end_label);
            self.place_label(short_label);
            self.emit_bytes(&[0x31, 0xC0]);
        } else {
            self.emit_mov_imm32(Reg::Rax, 0);
            self.emit_jcc(0x84, end_label);
            self.place_label(short_label);
            self.emit_mov_imm32(Reg::Rax, 1);
        }
        self.place_label(end_label);
        Ok(())
    }

    fn emit_call(&mut self, name: &str, args: &[Expr], location: &crate::diagnostic::SourceLocation) -> Result<()> {
        match name {
            "novus_write" => {
                if args.len() != 2 { bail!("{}", codegen_error(location, "novus_write expects two arguments")); }
                self.emit_expr(&args[1])?;
                self.push(Reg::Rax);
                self.emit_expr(&args[0])?;
                self.emit_mov_rr(Reg::Rsi, Reg::Rax, true);
                self.pop(Reg::Rdx);
                self.emit_mov_imm32(Reg::Rdi, 1);
                self.emit_mov_imm32(Reg::Rax, 2);
                self.emit_bytes(&[0x0F, 0x05]);
                return Ok(());
            }
            "novus_yield" => {
                if !args.is_empty() { bail!("{}", codegen_error(location, "novus_yield takes no arguments")); }
                self.emit_mov_imm32(Reg::Rax, 1);
                self.emit_bytes(&[0x0F, 0x05]);
                return Ok(());
            }
            "novus_exit" => {
                if args.len() != 1 { bail!("{}", codegen_error(location, "novus_exit expects one argument")); }
                self.emit_expr(&args[0])?;
                self.emit_mov_rr(Reg::Rdi, Reg::Rax, true);
                self.emit_mov_imm32(Reg::Rax, 60);
                self.emit_bytes(&[0x0F, 0x05, 0xF4]);
                return Ok(());
            }
            _ => {}
        }
        if args.len() > ARG_REGS.len() { bail!("{}", codegen_error(location, "calls with more than six arguments are not implemented")); }
        let argument_bytes = args.len() * 8;
        let padding = (16 - ((self.stack_depth + argument_bytes) & 15)) & 15;
        if padding != 0 { self.adjust_rsp(false, padding)?; self.stack_depth += padding; }
        for argument in args.iter().rev() {
            self.emit_expr(argument)?;
            self.push(Reg::Rax);
        }
        for (index, register) in ARG_REGS.iter().take(args.len()).enumerate() {
            self.emit_load(*register, Reg::Rsp, (index * 8) as i32, &Type::new(BaseType::Long))?;
        }
        let displacement = self.code.len() + 1;
        self.emit_bytes(&[0xE8, 0, 0, 0, 0]);
        self.relocations.push(NucRelocation {
            place_segment: SegmentKind::Text,
            place_offset: displacement as u64,
            kind: RELOC_PC32,
            symbol: name.to_owned(),
        });
        let cleanup = argument_bytes + padding;
        if cleanup != 0 { self.adjust_rsp(true, cleanup)?; self.stack_depth -= cleanup; }
        Ok(())
    }

    fn emit_increment(&mut self, operand: &Expr, increment: bool, postfix: bool) -> Result<()> {
        let ty = operand.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing increment operand type"))?;
        self.emit_lvalue_address(operand)?;
        self.push(Reg::Rax);
        self.emit_load(Reg::Rax, Reg::Rax, 0, ty)?;
        if postfix { self.push(Reg::Rax); }
        let amount = if ty.pointers > 0 { self.pointee_size(ty)? } else { 1 };
        self.emit_add_imm(Reg::Rax, if increment { amount as i32 } else { -(amount as i32) });
        if postfix {
            self.emit_mov_rr(Reg::Rdx, Reg::Rax, true);
            self.pop(Reg::Rcx);
            self.pop(Reg::R11);
            self.emit_mov_rr(Reg::Rax, Reg::Rdx, true);
            self.emit_store(Reg::Rax, Reg::R11, 0, ty)?;
            self.emit_mov_rr(Reg::Rax, Reg::Rcx, true);
        } else {
            self.pop(Reg::R11);
            self.emit_store(Reg::Rax, Reg::R11, 0, ty)?;
        }
        Ok(())
    }

    fn emit_lvalue_address(&mut self, expr: &Expr) -> Result<()> {
        match &expr.kind {
            ExprKind::Identifier(name) => {
                if let Some(local) = self.lookup_local(name) {
                    self.emit_lea(Reg::Rax, Reg::Rbp, -local.offset);
                } else if self.generator.globals.contains_key(name) {
                    self.emit_abs32_symbol(name);
                } else {
                    bail!("{}", codegen_error(&expr.location, "lvalue has no storage"));
                }
            }
            ExprKind::Unary { op, operand } if op == "*" => self.emit_expr(operand)?,
            ExprKind::Index { base, index } => {
                let base_type = base.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing subscript base type"))?;
                if base_type.array_len.is_some() {
                    self.emit_lvalue_address(base)?;
                } else {
                    self.emit_expr(base)?;
                }
                self.push(Reg::Rax);
                self.emit_expr(index)?;
                let element_size = self.generator.layouts.size_align_known(expr.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing subscript result type"))?)?.0;
                if element_size > 1 { self.emit_scale_rax(element_size)?; }
                self.emit_mov_rr(Reg::Rcx, Reg::Rax, true);
                self.pop(Reg::Rax);
                self.emit_bytes(&[0x48, 0x01, 0xC8]);
            }
            ExprKind::Member { base, name, through_pointer } => {
                let base_type = base.ty.as_ref().ok_or_else(|| anyhow::anyhow!("missing member base type"))?;
                let struct_name = match &base_type.base {
                    BaseType::Struct(name) => name,
                    _ => bail!("{}", codegen_error(&expr.location, "member base is not a struct")),
                };
                if *through_pointer { self.emit_expr(base)?; }
                else { self.emit_lvalue_address(base)?; }
                let offset = self.generator.layouts.member_offset(struct_name, name)?;
                if offset != 0 { self.emit_add_imm(Reg::Rax, offset as i32); }
            }
            _ => bail!("{}", codegen_error(&expr.location, "expression is not an addressable lvalue")),
        }
        Ok(())
    }

    fn pointee_size(&self, pointer: &Type) -> Result<usize> {
        let mut pointee = pointer.clone();
        pointee.pointers = pointee.pointers.saturating_sub(1);
        pointee.array_len = None;
        self.generator.layouts.size_align_known(&pointee).map(|pair| pair.0)
    }

    fn emit_scale_rax(&mut self, factor: usize) -> Result<()> {
        let factor = i32::try_from(factor).map_err(|_| anyhow::anyhow!("scale factor too large"))?;
        self.emit_bytes(&[0x48, 0x69, 0xC0]);
        self.emit_bytes(&factor.to_le_bytes());
        Ok(())
    }

    fn lookup_local(&self, name: &str) -> Option<LocalBinding> {
        self.locals.iter().rev().find_map(|scope| scope.get(name).cloned())
    }

    fn allocate_local(&mut self, name: &str, ty: Type) -> Result<i32> {
        let size = align_to(self.generator.layouts.size_align_known(&ty)?.0.max(1), 8)?;
        self.local_cursor = self.local_cursor.checked_add(size).ok_or_else(|| anyhow::anyhow!("local frame size overflow"))?;
        if self.local_cursor > self.frame_size { bail!("local allocation exceeded precomputed stack frame"); }
        let offset = i32::try_from(self.local_cursor).map_err(|_| anyhow::anyhow!("local frame exceeds 2 GiB"))?;
        self.locals.last_mut().expect("function has a local scope").insert(name.to_owned(), LocalBinding { offset, ty });
        Ok(offset)
    }

    fn emit_load(&mut self, target: Reg, base: Reg, displacement: i32, ty: &Type) -> Result<()> {
        if ty.array_len.is_some() || matches!(&ty.base, BaseType::Struct(_)) && ty.pointers == 0 {
            bail!("aggregate values cannot be loaded into a scalar register");
        }
        let size = self.generator.layouts.size_align_known(ty)?.0;
        let signed = !ty.is_unsigned;
        let width = match size { 1 => 1, 2 => 2, 4 => 4, 8 => 8, _ => bail!("unsupported scalar width {}", size) };
        self.emit_memory_instruction(target, base, displacement, width, signed, false);
        Ok(())
    }

    fn emit_store(&mut self, source: Reg, base: Reg, displacement: i32, ty: &Type) -> Result<()> {
        if ty.array_len.is_some() || matches!(&ty.base, BaseType::Struct(_)) && ty.pointers == 0 {
            bail!("aggregate values cannot be stored from a scalar register");
        }
        let size = self.generator.layouts.size_align_known(ty)?.0;
        let width = match size { 1 => 1, 2 => 2, 4 => 4, 8 => 8, _ => bail!("unsupported scalar width {}", size) };
        self.emit_memory_instruction(source, base, displacement, width, false, true);
        Ok(())
    }

    fn emit_memory_instruction(&mut self, reg: Reg, base: Reg, displacement: i32, width: usize, signed: bool, store: bool) {
        let reg_low = reg as u8 & 7;
        let base_low = base as u8 & 7;
        let rex_w = if store { width == 8 } else { width == 8 || (width < 8 && signed) };
        let needs_rex = rex_w || reg as u8 >= 8 || base as u8 >= 8 || (store && width == 1 && reg as u8 >= 4);
        if width == 2 { self.emit_u8(0x66); }
        if needs_rex {
            let mut rex = 0x40;
            if rex_w { rex |= 0x08; }
            if reg as u8 >= 8 { rex |= 0x04; }
            if base as u8 >= 8 { rex |= 0x01; }
            self.emit_u8(rex);
        }
        if store {
            self.emit_u8(if width == 1 { 0x88 } else { 0x89 });
            self.emit_u8(0x80 | (reg_low << 3) | base_low);
        } else if width == 1 {
            self.emit_bytes(&[0x0F, if signed { 0xBE } else { 0xB6 }]);
            self.emit_u8(0x80 | (reg_low << 3) | base_low);
        } else if width == 2 {
            self.emit_bytes(&[0x0F, if signed { 0xBF } else { 0xB7 }]);
            self.emit_u8(0x80 | (reg_low << 3) | base_low);
        } else if width == 4 && signed {
            self.emit_u8(0x63);
            self.emit_u8(0x80 | (reg_low << 3) | base_low);
        } else {
            self.emit_u8(0x8B);
            self.emit_u8(0x80 | (reg_low << 3) | base_low);
        }
        if base_low == 4 { self.emit_u8(0x24); }
        self.emit_bytes(&displacement.to_le_bytes());
    }

    fn emit_lea(&mut self, target: Reg, base: Reg, displacement: i32) {
        self.emit_rex(true, target, base);
        self.emit_u8(0x8D);
        self.emit_u8(0x80 | ((target as u8 & 7) << 3) | (base as u8 & 7));
        if base as u8 & 7 == 4 { self.emit_u8(0x24); }
        self.emit_bytes(&displacement.to_le_bytes());
    }

    fn emit_abs32_symbol(&mut self, symbol: &str) {
        self.emit_u8(0xB8); // mov eax, imm32; zero-extends the address
        let place = self.code.len();
        self.emit_u32(0);
        self.relocations.push(NucRelocation {
            place_segment: SegmentKind::Text,
            place_offset: place as u64,
            kind: RELOC_32,
            symbol: symbol.to_owned(),
        });
    }

    fn emit_mov_imm64(&mut self, register: Reg, value: u64) {
        if register as u8 >= 8 { self.emit_u8(0x49); }
        else { self.emit_u8(0x48); }
        self.emit_u8(0xB8 + (register as u8 & 7));
        self.emit_bytes(&value.to_le_bytes());
    }

    fn emit_mov_imm32(&mut self, register: Reg, value: u32) {
        if register as u8 >= 8 { self.emit_u8(0x41); }
        self.emit_u8(0xB8 + (register as u8 & 7));
        self.emit_u32(value);
    }

    fn emit_mov_rr(&mut self, destination: Reg, source: Reg, wide: bool) {
        self.emit_rex(wide, source, destination);
        self.emit_u8(0x89);
        self.emit_u8(0xC0 | ((source as u8 & 7) << 3) | (destination as u8 & 7));
    }

    fn emit_rex(&mut self, wide: bool, reg: Reg, rm: Reg) {
        let mut rex = 0x40;
        if wide { rex |= 0x08; }
        if reg as u8 >= 8 { rex |= 0x04; }
        if rm as u8 >= 8 { rex |= 0x01; }
        if rex != 0x40 { self.emit_u8(rex); }
    }

    fn emit_add_imm(&mut self, register: Reg, value: i32) {
        self.emit_rex(true, Reg::Rax, register);
        self.emit_u8(0x81);
        let group = if value < 0 { 5 } else { 0 };
        self.emit_u8(0xC0 | (group << 3) | (register as u8 & 7));
        self.emit_bytes(&value.unsigned_abs().to_le_bytes());
    }

    fn emit_test_rax(&mut self) { self.emit_bytes(&[0x48, 0x85, 0xC0]); }

    fn emit_setcc(&mut self, condition: u8) {
        self.emit_bytes(&[0x0F, condition, 0xC0]); // setcc al
        self.emit_bytes(&[0x0F, 0xB6, 0xC0]); // movzx eax, al
    }

    fn push(&mut self, register: Reg) {
        if register as u8 >= 8 { self.emit_u8(0x41); }
        self.emit_u8(0x50 + (register as u8 & 7));
        self.stack_depth += 8;
    }

    fn pop(&mut self, register: Reg) {
        if register as u8 >= 8 { self.emit_u8(0x41); }
        self.emit_u8(0x58 + (register as u8 & 7));
        self.stack_depth -= 8;
    }

    fn adjust_rsp(&mut self, add: bool, amount: usize) -> Result<()> {
        let amount = i32::try_from(amount).map_err(|_| anyhow::anyhow!("stack adjustment exceeds 2 GiB"))?;
        if amount <= 127 {
            self.emit_bytes(&[0x48, 0x83, if add { 0xC4 } else { 0xEC }, amount as u8]);
        } else {
            self.emit_bytes(&[0x48, 0x81, if add { 0xC4 } else { 0xEC }]);
            self.emit_bytes(&amount.to_le_bytes());
        }
        Ok(())
    }

    fn new_label(&mut self) -> usize {
        let label = self.next_label;
        self.next_label += 1;
        label
    }

    fn place_label(&mut self, label: usize) { self.labels.insert(label, self.code.len()); }

    fn emit_jmp(&mut self, label: usize) {
        self.emit_u8(0xE9);
        let offset = self.code.len();
        self.emit_u32(0);
        self.branch_fixups.push(BranchFixup { offset, label });
    }

    fn emit_jcc(&mut self, condition: u8, label: usize) {
        self.emit_bytes(&[0x0F, condition]);
        let offset = self.code.len();
        self.emit_u32(0);
        self.branch_fixups.push(BranchFixup { offset, label });
    }

    fn resolve_branches(&mut self) -> Result<()> {
        for fixup in &self.branch_fixups {
            let target = *self.labels.get(&fixup.label).ok_or_else(|| anyhow::anyhow!("unbound local code label"))?;
            let displacement = i32::try_from(target as i64 - (fixup.offset + 4) as i64)
                .map_err(|_| anyhow::anyhow!("local branch exceeds rel32 range"))?;
            self.code[fixup.offset..fixup.offset + 4].copy_from_slice(&displacement.to_le_bytes());
        }
        Ok(())
    }

    fn emit_bytes(&mut self, bytes: &[u8]) { self.code.extend_from_slice(bytes); }
    fn emit_u8(&mut self, byte: u8) { self.code.push(byte); }
    fn emit_u32(&mut self, value: u32) { self.code.extend_from_slice(&value.to_le_bytes()); }
}

fn count_local_bytes(stmt: &Stmt, layouts: &Layouts, total: &mut usize) -> Result<()> {
    match &stmt.kind {
        StmtKind::Block(statements) => for statement in statements { count_local_bytes(statement, layouts, total)?; },
        StmtKind::Variable(variable) => {
            let size = align_to(layouts.size_align_known(&variable.ty)?.0.max(1), 8)?;
            *total = total.checked_add(size).ok_or_else(|| anyhow::anyhow!("stack frame size overflow"))?;
        }
        StmtKind::If { then_branch, else_branch, .. } => {
            count_local_bytes(then_branch, layouts, total)?;
            if let Some(branch) = else_branch { count_local_bytes(branch, layouts, total)?; }
        }
        StmtKind::While { body, .. } => count_local_bytes(body, layouts, total)?,
        StmtKind::For { init, body, .. } => {
            if let Some(init) = init { count_local_bytes(init, layouts, total)?; }
            count_local_bytes(body, layouts, total)?;
        }
        StmtKind::Empty | StmtKind::Expression(_) | StmtKind::Return(_) | StmtKind::Break | StmtKind::Continue => {}
    }
    Ok(())
}

fn condition_code(op: &str, unsigned: bool) -> u8 {
    match (op, unsigned) {
        ("==", _) => 0x94, ("!=", _) => 0x95,
        ("<", false) => 0x9C, ("<=", false) => 0x9E, (">", false) => 0x9F, (">=", false) => 0x9D,
        ("<", true) => 0x92, ("<=", true) => 0x96, (">", true) => 0x97, (">=", true) => 0x93,
        _ => 0x94,
    }
}

fn is_builtin(name: &str) -> bool {
    matches!(name, "novus_write" | "novus_yield" | "novus_exit")
}
