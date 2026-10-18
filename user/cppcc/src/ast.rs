use crate::diagnostic::SourceLocation;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Type {
    pub base: BaseType,
    pub is_unsigned: bool,
    pub is_const: bool,
    pub pointers: usize,
    pub array_len: Option<usize>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum BaseType {
    Void,
    Bool,
    Char,
    Short,
    Int,
    Long,
    LongLong,
    Struct(String),
}

impl Type {
    pub fn new(base: BaseType) -> Self {
        Self {
            base,
            is_unsigned: false,
            is_const: false,
            pointers: 0,
            array_len: None,
        }
    }

    pub fn int() -> Self {
        Self::new(BaseType::Int)
    }

    pub fn void() -> Self {
        Self::new(BaseType::Void)
    }

    pub fn is_void(&self) -> bool {
        self.pointers == 0 && self.array_len.is_none() && self.base == BaseType::Void
    }

    pub fn is_integer(&self) -> bool {
        self.pointers == 0
            && self.array_len.is_none()
            && matches!(
                &self.base,
                BaseType::Bool
                    | BaseType::Char
                    | BaseType::Short
                    | BaseType::Int
                    | BaseType::Long
                    | BaseType::LongLong
            )
    }

    pub fn is_scalar(&self) -> bool {
        self.is_integer() || self.pointers > 0
    }

    pub fn decay_array(&self) -> Self {
        let mut ty = self.clone();
        if ty.array_len.take().is_some() {
            ty.pointers += 1;
        }
        ty
    }
}

#[derive(Clone, Debug)]
pub struct TranslationUnit {
    pub items: Vec<TopLevel>,
}

#[derive(Clone, Debug)]
pub enum TopLevel {
    Function(Function),
    Global(Variable),
    Struct(StructDecl),
}

#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub return_type: Type,
    pub params: Vec<Parameter>,
    pub body: Option<Stmt>,
    pub is_static: bool,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct Parameter {
    pub name: Option<String>,
    pub ty: Type,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct Variable {
    pub name: String,
    pub ty: Type,
    pub initializer: Option<Expr>,
    pub is_static: bool,
    pub is_extern: bool,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct StructDecl {
    pub name: String,
    pub fields: Vec<Variable>,
    pub is_definition: bool,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct Expr {
    pub kind: ExprKind,
    pub location: SourceLocation,
    pub ty: Option<Type>,
}

#[derive(Clone, Debug)]
pub enum ExprKind {
    Integer(i64),
    String(Vec<u8>),
    Character(i64),
    Boolean(bool),
    Identifier(String),
    Unary {
        op: String,
        operand: Box<Expr>,
    },
    Binary {
        op: String,
        left: Box<Expr>,
        right: Box<Expr>,
    },
    Ternary {
        condition: Box<Expr>,
        then_expr: Box<Expr>,
        else_expr: Box<Expr>,
    },
    Call {
        callee: Box<Expr>,
        args: Vec<Expr>,
    },
    Index {
        base: Box<Expr>,
        index: Box<Expr>,
    },
    Member {
        base: Box<Expr>,
        name: String,
        through_pointer: bool,
    },
    Postfix {
        op: String,
        operand: Box<Expr>,
    },
}

#[derive(Clone, Debug)]
pub struct Stmt {
    pub kind: StmtKind,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub enum StmtKind {
    Block(Vec<Stmt>),
    Empty,
    Expression(Expr),
    Variable(Variable),
    If {
        condition: Expr,
        then_branch: Box<Stmt>,
        else_branch: Option<Box<Stmt>>,
    },
    While {
        condition: Expr,
        body: Box<Stmt>,
    },
    For {
        init: Option<Box<Stmt>>,
        condition: Option<Expr>,
        step: Option<Expr>,
        body: Box<Stmt>,
    },
    Return(Option<Expr>),
    Break,
    Continue,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn array_type_decays_to_pointer() {
        let mut ty = Type::new(BaseType::Int);
        ty.array_len = Some(4);
        assert_eq!(ty.decay_array().pointers, 1);
        assert_eq!(ty.decay_array().array_len, None);
    }

    #[test]
    fn void_pointer_is_not_void_value() {
        let mut ty = Type::void();
        assert!(ty.is_void());
        ty.pointers = 1;
        assert!(!ty.is_void());
        assert!(ty.is_scalar());
    }
}