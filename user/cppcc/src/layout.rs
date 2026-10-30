//! Natural-alignment object layout, shared by the semantic analyzer (for
//! `sizeof`, `offsetof` folding and `static_assert`) and the code generator.
use crate::ast::*;
use anyhow::{bail, Result};
use std::collections::{HashMap, HashSet};

#[derive(Clone, Debug)]
struct StructLayout {
    fields: HashMap<String, usize>,
    size: usize,
    align: usize,
}

#[derive(Default)]
pub struct Layouts {
    definitions: HashMap<String, Vec<Variable>>,
    layouts: HashMap<String, StructLayout>,
}

impl Layouts {
    pub fn from_unit(unit: &TranslationUnit) -> Result<Self> {
        let mut layouts = Self::default();
        for item in &unit.items {
            if let TopLevel::Struct(definition) = item {
                if definition.is_definition {
                    layouts
                        .definitions
                        .insert(definition.name.clone(), definition.fields.clone());
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
        let fields = self
            .definitions
            .get(name)
            .ok_or_else(|| anyhow::anyhow!("incomplete struct '{}'", name))?
            .clone();
        let mut field_offsets = HashMap::new();
        let mut size = 0usize;
        let mut struct_align = 1usize;
        for field in &fields {
            let (field_size, field_align) = self.size_align(&field.ty, active)?;
            size = align_to(size, field_align)?;
            field_offsets.insert(field.name.clone(), size);
            size = size
                .checked_add(field_size)
                .ok_or_else(|| anyhow::anyhow!("struct size overflow"))?;
            struct_align = struct_align.max(field_align);
        }
        size = align_to(size.max(1), struct_align)?;
        active.remove(name);
        let layout = StructLayout {
            fields: field_offsets,
            size,
            align: struct_align,
        };
        self.layouts.insert(name.to_owned(), layout.clone());
        Ok(layout)
    }

    fn size_align(&mut self, ty: &Type, active: &mut HashSet<String>) -> Result<(usize, usize)> {
        if ty.is_reference {
            return Ok((8, 8));
        }
        if ty.pointers > 0 {
            return Ok((8, 8));
        }
        let (base_size, base_align) = match &ty.base {
            BaseType::Void => bail!("void has no object size"),
            BaseType::Auto => bail!("'auto' has no object size before deduction"),
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
            Ok((
                base_size
                    .checked_mul(length)
                    .ok_or_else(|| anyhow::anyhow!("array size overflow"))?,
                base_align,
            ))
        } else {
            Ok((base_size, base_align))
        }
    }

    pub fn size_of(&mut self, ty: &Type) -> Result<usize> {
        self.size_align(ty, &mut HashSet::new()).map(|pair| pair.0)
    }

    pub fn align_of(&mut self, ty: &Type) -> Result<usize> {
        self.size_align(ty, &mut HashSet::new()).map(|pair| pair.1)
    }

    pub fn size_align_known(&self, ty: &Type) -> Result<(usize, usize)> {
        if ty.is_reference || ty.pointers > 0 {
            return Ok((8, 8));
        }
        let (size, align) = match &ty.base {
            BaseType::Void => bail!("void has no object size"),
            BaseType::Auto => bail!("'auto' has no object size before deduction"),
            BaseType::Bool | BaseType::Char => (1, 1),
            BaseType::Short => (2, 2),
            BaseType::Int => (4, 4),
            BaseType::Long | BaseType::LongLong => (8, 8),
            BaseType::Struct(name) => {
                let layout = self
                    .layouts
                    .get(name)
                    .ok_or_else(|| anyhow::anyhow!("incomplete struct '{}'", name))?;
                (layout.size, layout.align)
            }
        };
        if let Some(length) = ty.array_len {
            Ok((
                size.checked_mul(length)
                    .ok_or_else(|| anyhow::anyhow!("array size overflow"))?,
                align,
            ))
        } else {
            Ok((size, align))
        }
    }

    /// Non-failing convenience for the front end.
    pub fn size_align_opt(&self, ty: &Type) -> Option<(usize, usize)> {
        self.size_align_known(ty).ok()
    }

    pub fn member_offset(&self, struct_name: &str, field: &str) -> Result<usize> {
        self.layouts
            .get(struct_name)
            .and_then(|layout| layout.fields.get(field))
            .copied()
            .ok_or_else(|| anyhow::anyhow!("struct '{}' has no member '{}'", struct_name, field))
    }

    pub fn field_order(&self, struct_name: &str) -> Option<Vec<(String, Type)>> {
        self.definitions.get(struct_name).map(|fields| {
            fields
                .iter()
                .map(|field| (field.name.clone(), field.ty.clone()))
                .collect()
        })
    }
}

pub fn align_to(value: usize, alignment: usize) -> Result<usize> {
    value
        .checked_add(alignment - 1)
        .map(|value| value & !(alignment - 1))
        .ok_or_else(|| anyhow::anyhow!("alignment overflow"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unit_of(source: &str) -> TranslationUnit {
        let locations = (0..source.len())
            .map(|_| crate::diagnostic::SourceLocation::new("layout.cc", 1, 1))
            .collect();
        let pre = crate::preprocess::PreprocessedSource {
            text: source.to_owned(),
            locations,
        };
        crate::parser::parse(crate::lexer::lex(&pre).tokens).unit
    }

    #[test]
    fn computes_scalar_array_and_struct_sizes() {
        let unit = unit_of("struct Mixed { char a; int b; char c; };");
        let layouts = Layouts::from_unit(&unit).unwrap();
        let mut ty = Type::new(BaseType::Char);
        assert_eq!(layouts.size_align_known(&ty).unwrap(), (1, 1));
        ty.array_len = Some(7);
        assert_eq!(layouts.size_align_known(&ty).unwrap(), (7, 1));
        let struct_ty = Type::new(BaseType::Struct("Mixed".into()));
        assert_eq!(layouts.size_align_known(&struct_ty).unwrap(), (12, 4));
        assert_eq!(layouts.member_offset("Mixed", "b").unwrap(), 4);
        assert_eq!(layouts.member_offset("Mixed", "c").unwrap(), 8);
    }

    #[test]
    fn rejects_void_and_recursive_structs() {
        let unit = unit_of("struct Node { struct Node next; };");
        assert!(Layouts::from_unit(&unit).is_err());
        let layouts = Layouts::default();
        assert!(layouts.size_align_known(&Type::void()).is_err());
    }
}
