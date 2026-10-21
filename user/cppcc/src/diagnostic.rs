use std::fmt;
use std::path::PathBuf;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SourceLocation {
    pub file: PathBuf,
    pub line: usize,
    pub column: usize,
}

impl SourceLocation {
    pub fn new(file: impl Into<PathBuf>, line: usize, column: usize) -> Self {
        Self {
            file: file.into(),
            line,
            column,
        }
    }

    pub fn synthetic() -> Self {
        Self::new("<command line>", 1, 1)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CompileError {
    pub location: SourceLocation,
    pub message: String,
}

impl CompileError {
    pub fn new(location: SourceLocation, message: impl Into<String>) -> Self {
        Self {
            location,
            message: message.into(),
        }
    }
}

impl fmt::Display for CompileError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}:{}:{}: error: {}",
            self.location.file.display(),
            self.location.line,
            self.location.column,
            self.message
        )
    }
}

impl std::error::Error for CompileError {}

#[derive(Default, Debug)]
pub struct ErrorCollector {
    errors: Vec<CompileError>,
}

impl ErrorCollector {
    pub fn push(&mut self, error: CompileError) {
        self.errors.push(error);
    }

    pub fn extend(&mut self, errors: impl IntoIterator<Item = CompileError>) {
        self.errors.extend(errors);
    }

    pub fn has_errors(&self) -> bool {
        !self.errors.is_empty()
    }

    pub fn errors(&self) -> &[CompileError] {
        &self.errors
    }

    pub fn into_errors(self) -> Vec<CompileError> {
        self.errors
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn diagnostic_has_stable_location_format() {
        let error = CompileError::new(SourceLocation::new("a.cc", 3, 7), "bad token");
        assert_eq!(error.to_string(), "a.cc:3:7: error: bad token");
    }

    #[test]
    fn collector_keeps_multiple_errors() {
        let mut errors = ErrorCollector::default();
        errors.push(CompileError::new(SourceLocation::synthetic(), "one"));
        errors.push(CompileError::new(SourceLocation::synthetic(), "two"));
        assert_eq!(errors.errors().len(), 2);
    }
}