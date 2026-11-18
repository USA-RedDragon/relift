use relift_gpu_nv::Sched;

/// One 128-bit SM86 instruction word.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Word(pub u128);

impl Word {
    pub fn from_bytes(b: &[u8; 16]) -> Word {
        Word(u128::from_le_bytes(*b))
    }

    pub fn get(self, lo: u32, len: u32) -> u32 {
        ((self.0 >> lo) & ((1u128 << len) - 1)) as u32
    }

    pub fn get64(self, lo: u32, len: u32) -> u64 {
        ((self.0 >> lo) & ((1u128 << len) - 1)) as u64
    }

    pub fn bit(self, n: u32) -> bool {
        (self.0 >> n) & 1 == 1
    }

    pub fn opcode(self) -> u32 {
        self.get(0, 12)
    }

    pub fn sched(self) -> Sched {
        Sched {
            stall: self.get(105, 4) as u8,
            yield_: self.bit(109),
            wr_bar: self.get(110, 3) as u8,
            rd_bar: self.get(113, 3) as u8,
            wait_mask: self.get(116, 6) as u8,
            reuse: self.get(122, 4) as u8,
        }
    }
}
