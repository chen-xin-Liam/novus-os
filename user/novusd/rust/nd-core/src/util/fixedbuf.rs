pub struct FixedBuf<const N: usize> {
    data: [u8; N],
    len: usize,
}

impl<const N: usize> Default for FixedBuf<N> {
    fn default() -> Self {
        Self {
            data: [0u8; N],
            len: 0,
        }
    }
}

impl<const N: usize> FixedBuf<N> {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn clear(&mut self) {
        self.len = 0;
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.data[..self.len]
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        &mut self.data[..self.len]
    }

    pub fn capacity(&self) -> usize {
        N
    }

    pub fn push(&mut self, byte: u8) -> bool {
        if self.len >= N {
            return false;
        }
        self.data[self.len] = byte;
        self.len += 1;
        true
    }

    pub fn extend_from_slice(&mut self, bytes: &[u8]) -> bool {
        if self.len + bytes.len() > N {
            return false;
        }
        self.data[self.len..self.len + bytes.len()].copy_from_slice(bytes);
        self.len += bytes.len();
        true
    }

    pub fn push_u16_le(&mut self, value: u16) -> bool {
        self.extend_from_slice(&value.to_le_bytes())
    }

    pub fn push_u32_le(&mut self, value: u32) -> bool {
        self.extend_from_slice(&value.to_le_bytes())
    }

    pub fn push_u64_le(&mut self, value: u64) -> bool {
        self.extend_from_slice(&value.to_le_bytes())
    }
}
