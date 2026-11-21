use nd_platform_api::RawEvent;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    Allow,
    Deny,
    Quarantine,
}

pub const EV_PROCESS: u32 = 1;
pub const EV_FILE: u32 = 2;
pub const EV_SYSCALL: u32 = 3;
pub const SENSITIVE_OBJECT: u64 = 0x4E44_5359_5346_4C00; // "NDSYSFL\0"

#[derive(Default)]
pub struct BehaviorState {
    pub risk: u32,
    pub process_events: u32,
    pub file_events: u32,
    pub syscall_events: u32,
    pub denials: u32,
}

impl BehaviorState {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn on_event(&mut self, ev: &RawEvent) -> Decision {
        match ev.kind {
            EV_PROCESS => {
                self.process_events += 1;
                self.risk += 10;
                if self.risk >= 60 {
                    self.denials += 1;
                    Decision::Deny
                } else {
                    Decision::Allow
                }
            }
            EV_FILE => {
                self.file_events += 1;
                self.risk += 5;
                if ev.object_id == SENSITIVE_OBJECT {
                    self.denials += 1;
                    Decision::Quarantine
                } else if self.risk >= 90 {
                    self.denials += 1;
                    Decision::Deny
                } else {
                    Decision::Allow
                }
            }
            EV_SYSCALL => {
                self.syscall_events += 1;
                self.risk += 15;
                if self.risk >= 80 {
                    self.denials += 1;
                    Decision::Deny
                } else {
                    Decision::Allow
                }
            }
            _ => Decision::Allow,
        }
    }

    pub fn decay(&mut self) {
        self.risk = self.risk.saturating_sub(1);
    }
}
