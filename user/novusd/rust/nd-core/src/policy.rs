#[derive(Clone, Copy)]
pub struct Policy {
    pub realtime: bool,
    pub scheduled: bool,
    pub heuristic_threshold: u32,
    pub fuzzy_threshold: u32,
    pub auto_quarantine: bool,
}

impl Default for Policy {
    fn default() -> Self {
        Self {
            realtime: true,
            scheduled: true,
            heuristic_threshold: 70,
            fuzzy_threshold: 80,
            auto_quarantine: true,
        }
    }
}

impl Policy {
    pub fn from_bytes(bytes: &[u8]) -> Self {
        let mut p = Self::default();
        if !bytes.is_empty() {
            p.realtime = bytes[0] != 0;
        }
        if bytes.len() >= 2 {
            p.scheduled = bytes[1] != 0;
        }
        if bytes.len() >= 6 {
            p.heuristic_threshold = u32::from_le_bytes([bytes[2], bytes[3], bytes[4], bytes[5]]);
        }
        if bytes.len() >= 10 {
            p.fuzzy_threshold = u32::from_le_bytes([bytes[6], bytes[7], bytes[8], bytes[9]]);
        }
        if bytes.len() >= 11 {
            p.auto_quarantine = bytes[10] != 0;
        }
        p
    }
}
