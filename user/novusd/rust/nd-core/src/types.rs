pub const VERDICT_CLEAN: i32 = 0;
pub const VERDICT_SUSPICIOUS: i32 = 1;
pub const VERDICT_MALICIOUS: i32 = 2;

pub const ERR_INVAL: i32 = -1;
pub const ERR_NOMEM: i32 = -2;
pub const ERR_NOTFOUND: i32 = -3;
pub const ERR_VERSION: i32 = -4;
pub const ERR_CRC: i32 = -5;
pub const ERR_STATE: i32 = -6;
pub const ERR_CAP: i32 = -7;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    Clean,
    Suspicious,
    Malicious,
}

impl Verdict {
    pub fn from_i32(v: i32) -> Self {
        match v {
            VERDICT_MALICIOUS => Verdict::Malicious,
            VERDICT_SUSPICIOUS => Verdict::Suspicious,
            _ => Verdict::Clean,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ScanResult {
    pub verdict: i32,
    pub rule_id: i32,
    pub offset: u64,
    pub confidence: u32,
    pub reserved: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Detection {
    pub verdict: Verdict,
    pub confidence: u32,
    pub rule_id: i32,
    pub heuristic_score: u32,
    pub fuzzy_match: bool,
    pub object_id: u64,
}

impl Default for Detection {
    fn default() -> Self {
        Self {
            verdict: Verdict::Clean,
            confidence: 0,
            rule_id: 0,
            heuristic_score: 0,
            fuzzy_match: false,
            object_id: 0,
        }
    }
}
