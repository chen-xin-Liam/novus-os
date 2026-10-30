use crate::diagnostic::SourceLocation;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Type {
    pub base: BaseType,
    pub is_unsigned: bool,
    pub is_const: bool,
    pub pointers: usize,
    pub array_len: Option<usize>,
    /// `T&` / `T&&`. A reference is stored as a pointer to the referent and is
    /// transparently dereferenced on use.
    pub is_reference: bool,
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
    /// Placeholder for `auto`; `sema` replaces it with the deduced type.
    Auto,
}

impl Type {
    pub fn new(base: BaseType) -> Self {
        Self {
            base,
            is_unsigned: false,
            is_const: false,
            pointers: 0,
            array_len: None,
            is_reference: false,
        }
    }

    pub fn int() -> Self {
        Self::new(BaseType::Int)
    }

    pub fn void() -> Self {
        Self::new(BaseType::Void)
    }

    pub fn is_void(&self) -> bool {
        self.pointers == 0
            && self.array_len.is_none()
            && !self.is_reference
            && self.base == BaseType::Void
    }

    pub fn is_integer(&self) -> bool {
        self.pointers == 0
            && self.array_len.is_none()
            && !self.is_reference
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

    pub fn is_auto(&self) -> bool {
        self.base == BaseType::Auto
    }

    pub fn is_struct(&self) -> bool {
        !self.is_reference && self.pointers == 0 && matches!(&self.base, BaseType::Struct(_))
    }

    pub fn is_aggregate(&self) -> bool {
        self.is_struct() || self.array_len.is_some()
    }

    /// The type produced when this type is used as a value: arrays decay to a
    /// pointer to their element type and references yield their referent.
    pub fn value_type(&self) -> Self {
        if self.is_reference {
            let mut referent = self.clone();
            referent.is_reference = false;
            referent
        } else {
            self.decay_array()
        }
    }

    pub fn decay_array(&self) -> Self {
        let mut ty = self.clone();
        if !ty.is_reference {
            if ty.array_len.take().is_some() {
                ty.pointers += 1;
            }
        }
        ty
    }

    pub fn referent(&self) -> Option<Self> {
        if self.is_reference {
            let mut referent = self.clone();
            referent.is_reference = false;
            Some(referent)
        } else {
            None
        }
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
    Enum(EnumDecl),
    Typedef(Typedef),
    StaticAssert(StaticAssert),
}

#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub return_type: Type,
    pub params: Vec<Parameter>,
    pub body: Option<Stmt>,
    pub is_static: bool,
    pub is_variadic: bool,
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
pub struct EnumDecl {
    pub name: Option<String>,
    /// `enum class`/`enum struct`: enumerators are only reachable as `E::name`.
    pub is_scoped: bool,
    pub underlying: Type,
    pub variants: Vec<Enumerator>,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct Enumerator {
    pub name: String,
    pub value: Option<Expr>,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct Typedef {
    pub name: String,
    pub ty: Type,
    pub location: SourceLocation,
}

#[derive(Clone, Debug)]
pub struct StaticAssert {
    pub condition: Expr,
    pub message: Option<String>,
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
    Nullptr,
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
    Cast {
        ty: Type,
        operand: Box<Expr>,
    },
    SizeofType(Type),
    SizeofExpr(Box<Expr>),
    /// Brace initializer list, e.g. `{1, 2, 3}`. Only valid as the initializer
    /// of an aggregate object.
    Aggregate(Vec<Expr>),
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
    DoWhile {
        body: Box<Stmt>,
        condition: Expr,
    },
    For {
        init: Option<Box<Stmt>>,
        condition: Option<Expr>,
        step: Option<Expr>,
        body: Box<Stmt>,
    },
    /// `for (declaration : range) body`
    RangeFor {
        var: Variable,
        range: Expr,
        body: Box<Stmt>,
    },
    Return(Option<Expr>),
    Break,
    Continue,
    StaticAssert(StaticAssert),
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
        assert!(!ty.decay_array().is_reference);
    }

    #[test]
    fn void_pointer_is_not_void_value() {
        let mut ty = Type::void();
        assert!(ty.is_void());
        ty.pointers = 1;
        assert!(!ty.is_void());
        assert!(ty.is_scalar());
    }

    #[test]
    fn reference_is_not_void_even_when_referent_is_void() {
        let mut ty = Type::void();
        ty.is_reference = true;
        assert!(!ty.is_void());
        assert_eq!(ty.value_type().base, BaseType::Void);
        assert!(!ty.value_type().is_reference);
    }
}
