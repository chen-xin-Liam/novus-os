use crate::types::ERR_NOMEM;

#[derive(Clone, Copy)]
pub struct Task {
    pub kind: u8, // 0 = realtime, 1 = on-demand, 2 = scheduled
    pub due: u64,
    pub path: [u8; 256],
    pub path_len: usize,
}

impl Default for Task {
    fn default() -> Self {
        Self {
            kind: 2,
            due: 0,
            path: [0u8; 256],
            path_len: 0,
        }
    }
}

pub struct TaskQueue {
    slots: [Task; 64],
    len: usize,
}

impl Default for TaskQueue {
    fn default() -> Self {
        Self::new()
    }
}

impl TaskQueue {
    pub fn new() -> Self {
        Self {
            slots: [Task::default(); 64],
            len: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.len
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn push(&mut self, task: Task) -> Result<(), i32> {
        if self.len >= self.slots.len() {
            return Err(ERR_NOMEM);
        }
        self.slots[self.len] = task;
        self.len += 1;
        Ok(())
    }

    /// Highest priority first (lower `kind` wins), then earliest deadline.
    pub fn pop(&mut self) -> Option<Task> {
        if self.len == 0 {
            return None;
        }
        let mut best = 0usize;
        for i in 1..self.len {
            let a = &self.slots[i];
            let b = &self.slots[best];
            if a.kind < b.kind || (a.kind == b.kind && a.due < b.due) {
                best = i;
            }
        }
        let out = self.slots[best];
        self.len -= 1;
        if best != self.len {
            self.slots[best] = self.slots[self.len];
        }
        Some(out)
    }
}
