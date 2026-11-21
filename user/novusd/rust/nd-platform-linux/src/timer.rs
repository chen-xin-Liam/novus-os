use std::time::{Duration, Instant};

pub struct Timer {
    interval: Duration,
    last: Instant,
}

impl Timer {
    pub fn new(interval_ms: u32) -> Self {
        Self {
            interval: Duration::from_millis(interval_ms as u64),
            last: Instant::now(),
        }
    }

    pub fn due(&mut self) -> bool {
        if self.last.elapsed() >= self.interval {
            self.last = Instant::now();
            true
        } else {
            false
        }
    }

    pub fn reset(&mut self) {
        self.last = Instant::now();
    }
}
