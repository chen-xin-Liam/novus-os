//! Target-independent IR scaffolding.
//!
//! The historical backend walks the typed AST directly. This module defines
//! the representation that new backends converge on: flat functions with
//! basic blocks and explicit operands. The current `codegen` still lowers to
//! the legacy `CodeSections` object model, but the type vocabulary here is
//! public so a future lowering pass can replace it without changing the
//! stable compile API.

/// A machine-independent instruction operand.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Operand {
    /// Signed immediate, already widened to 64 bits by semantic analysis.
    Immediate(i64),
    /// Virtual register. Register allocation is target-specific.
    Virtual(u32),
    /// A symbol plus byte offset, resolved by the object linker.
    Symbol { name: String, addend: i64 },
}

/// A target-independent instruction kind.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Opcode {
    Mov,
    Add,
    Sub,
    Call,
    Ret,
    Load,
    Store,
    Branch,
    Compare,
    Other(String),
}

/// One instruction in a basic block.
#[derive(Clone, Debug)]
pub struct Instruction {
    pub opcode: Opcode,
    pub operands: Vec<Operand>,
}

/// A basic block with explicit successors.
#[derive(Clone, Debug, Default)]
pub struct BasicBlock {
    pub instructions: Vec<Instruction>,
    pub successors: Vec<usize>,
}

/// A lowered function in the target-independent IR.
#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub parameter_count: usize,
    pub blocks: Vec<BasicBlock>,
}

/// A complete IR module.
#[derive(Clone, Debug, Default)]
pub struct Module {
    pub functions: Vec<Function>,
}
