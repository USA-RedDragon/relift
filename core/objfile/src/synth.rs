//! Small ELF files built at test time: segments with the bytes a test
//! gives, and optionally a section table with a symbol table. They let
//! the readers and the guests above them be tested without any file from
//! disk.

/// One segment to write.
#[derive(Clone, Debug)]
pub struct SegmentSpec {
    /// The virtual address.
    pub addr: u64,
    /// The bytes in the file.
    pub data: Vec<u8>,
    /// The size in memory.
    pub mem_size: u64,
    /// The `PF_` flags: 1 execute, 2 write, 4 read.
    pub flags: u32,
}

/// One symbol to write.
#[derive(Clone, Debug)]
pub struct SymbolSpec {
    /// The name.
    pub name: String,
    /// The value.
    pub value: u64,
    /// The size.
    pub size: u64,
    /// The `STT_` type.
    pub kind: u8,
    /// The `STB_` binding.
    pub bind: u8,
}

/// One section to write.
#[derive(Clone, Debug)]
pub struct SectionSpec {
    /// The name.
    pub name: String,
    /// The `SHT_` type.
    pub kind: u32,
    /// The `SHF_` flags.
    pub flags: u64,
    /// The address.
    pub addr: u64,
    /// The size.
    pub size: u64,
}

/// An ELF file to write.
#[derive(Clone, Debug, Default)]
pub struct Builder {
    /// Whether the file is `ET_DYN` instead of `ET_EXEC`.
    pub shared: bool,
    /// The `e_machine` value; 0 means AArch64.
    pub machine: u16,
    /// The entry point.
    pub entry: u64,
    /// The loadable segments.
    pub segments: Vec<SegmentSpec>,
    /// The dynamic section's address and size, when there is one.
    pub dynamic: Option<(u64, u64)>,
    /// The thread-local template: address, file size, memory size and
    /// alignment.
    pub tls: Option<(u64, u64, u64, u64)>,
    /// The program interpreter's path.
    pub interpreter: Option<String>,
    /// The `.eh_frame_hdr` address and size.
    pub eh_frame_hdr: Option<(u64, u64)>,
    /// The memory size of the stack segment.
    pub stack: Option<u64>,
    /// The sections.
    pub sections: Vec<SectionSpec>,
    /// The symbol table.
    pub symbols: Vec<SymbolSpec>,
}

fn put16(v: &mut Vec<u8>, x: u16) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn put32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn put64(v: &mut Vec<u8>, x: u64) {
    v.extend_from_slice(&x.to_le_bytes());
}

type Head = (u32, u32, u64, u64, u64, u64, u64);

fn phdr(v: &mut Vec<u8>, h: &Head) {
    put32(v, h.0);
    put32(v, h.1);
    put64(v, h.2);
    put64(v, h.3);
    put64(v, h.3);
    put64(v, h.4);
    put64(v, h.5);
    put64(v, h.6);
}

fn add_name(table: &mut Vec<u8>, name: &str) -> u32 {
    let at = table.len() as u32;
    table.extend_from_slice(name.as_bytes());
    table.push(0);
    at
}

impl Builder {
    /// The file's bytes.
    pub fn build(&self) -> Vec<u8> {
        let mut heads: Vec<Head> = Vec::new();
        let phnum = self.segments.len()
            + usize::from(self.dynamic.is_some())
            + usize::from(self.tls.is_some())
            + usize::from(self.interpreter.is_some())
            + usize::from(self.eh_frame_hdr.is_some())
            + usize::from(self.stack.is_some());
        let mut body: Vec<u8> = Vec::new();
        let body_start = 64 + 56 * phnum as u64;
        for s in &self.segments {
            let offset = body_start + body.len() as u64;
            heads.push((
                1,
                s.flags,
                offset,
                s.addr,
                s.data.len() as u64,
                s.mem_size,
                0x1000,
            ));
            body.extend_from_slice(&s.data);
        }
        if let Some(i) = &self.interpreter {
            let offset = body_start + body.len() as u64;
            body.extend_from_slice(i.as_bytes());
            body.push(0);
            heads.push((3, 4, offset, 0, i.len() as u64 + 1, i.len() as u64 + 1, 1));
        }
        let placed: Vec<(u64, u64, u64)> = self
            .segments
            .iter()
            .zip(&heads)
            .map(|(s, h)| (s.addr, s.mem_size, h.2))
            .collect();
        let at = move |a: u64| {
            placed
                .iter()
                .find(|(addr, size, _)| a >= *addr && a < addr + size)
                .map_or(0, |(addr, _, off)| off + (a - addr))
        };
        if let Some((a, n)) = self.dynamic {
            heads.push((2, 6, at(a), a, n, n, 8));
        }
        if let Some((a, f, m, al)) = self.tls {
            heads.push((7, 4, at(a), a, f, m, al));
        }
        if let Some((a, n)) = self.eh_frame_hdr {
            heads.push((0x6474_e550, 4, at(a), a, n, n, 4));
        }
        if let Some(m) = self.stack {
            heads.push((0x6474_e551, 6, 0, 0, 0, m, 16));
        }
        let mut shdr: Vec<u8> = Vec::new();
        let mut shnum = 0u16;
        let mut shstrndx = 0u16;
        let mut tail: Vec<u8> = Vec::new();
        let tail_start = body_start + body.len() as u64;
        if !self.sections.is_empty() || !self.symbols.is_empty() {
            let mut names = vec![0u8];
            let mut entries: Vec<[u64; 8]> = vec![[0; 8]];
            for s in &self.sections {
                let n = add_name(&mut names, &s.name);
                entries.push([
                    u64::from(n),
                    u64::from(s.kind),
                    s.flags,
                    s.addr,
                    0,
                    s.size,
                    0,
                    0,
                ]);
            }
            if !self.symbols.is_empty() {
                let mut strs = vec![0u8];
                let mut syms = vec![0u8; 24];
                for s in &self.symbols {
                    let n = add_name(&mut strs, &s.name);
                    put32(&mut syms, n);
                    syms.push(s.kind | (s.bind << 4));
                    syms.push(0);
                    put16(&mut syms, 1);
                    put64(&mut syms, s.value);
                    put64(&mut syms, s.size);
                }
                let symtab_at = tail_start + tail.len() as u64;
                tail.extend_from_slice(&syms);
                let strtab_at = tail_start + tail.len() as u64;
                tail.extend_from_slice(&strs);
                let link = entries.len() as u64 + 1;
                let n1 = add_name(&mut names, ".symtab");
                entries.push([
                    u64::from(n1),
                    2,
                    0,
                    0,
                    symtab_at,
                    syms.len() as u64,
                    link,
                    24,
                ]);
                let n2 = add_name(&mut names, ".strtab");
                entries.push([u64::from(n2), 3, 0, 0, strtab_at, strs.len() as u64, 0, 0]);
            }
            let n = add_name(&mut names, ".shstrtab");
            let names_at = tail_start + tail.len() as u64;
            let names_len = names.len() as u64;
            tail.extend_from_slice(&names);
            shstrndx = entries.len() as u16;
            entries.push([u64::from(n), 3, 0, 0, names_at, names_len, 0, 0]);
            shnum = entries.len() as u16;
            for e in &entries {
                put32(&mut shdr, e[0] as u32);
                put32(&mut shdr, e[1] as u32);
                put64(&mut shdr, e[2]);
                put64(&mut shdr, e[3]);
                put64(&mut shdr, e[4]);
                put64(&mut shdr, e[5]);
                put32(&mut shdr, e[6] as u32);
                put32(&mut shdr, 0);
                put64(&mut shdr, 8);
                put64(&mut shdr, e[7]);
            }
        }
        let shoff = if shnum > 0 {
            tail_start + tail.len() as u64
        } else {
            0
        };
        let mut out = Vec::new();
        out.extend_from_slice(b"\x7fELF\x02\x01\x01\0\0\0\0\0\0\0\0\0");
        put16(&mut out, if self.shared { 3 } else { 2 });
        put16(&mut out, if self.machine == 0 { 183 } else { self.machine });
        put32(&mut out, 1);
        put64(&mut out, self.entry);
        put64(&mut out, 64);
        put64(&mut out, shoff);
        put32(&mut out, 0);
        put16(&mut out, 64);
        put16(&mut out, 56);
        put16(&mut out, phnum as u16);
        put16(&mut out, 64);
        put16(&mut out, shnum);
        put16(&mut out, shstrndx);
        for h in &heads {
            phdr(&mut out, h);
        }
        out.extend_from_slice(&body);
        out.extend_from_slice(&tail);
        out.extend_from_slice(&shdr);
        out
    }
}
