use std::collections::BTreeMap;
use std::io::{BufRead, Write};

/// The page size of the driver's page replies.
pub const PAGE: u64 = 4096;

/// The driver's end of the pipe: page requests go out, replies come in.
pub struct Wire {
    input: Box<dyn BufRead>,
    output: Box<dyn Write>,
}

impl Wire {
    /// A wire over a reader and a writer.
    pub fn new(input: Box<dyn BufRead>, output: Box<dyn Write>) -> Wire {
        Wire { input, output }
    }

    /// A wire over this process's standard input and output.
    pub fn stdio() -> Wire {
        Wire::new(
            Box::new(std::io::BufReader::new(std::io::stdin())),
            Box::new(std::io::stdout()),
        )
    }

    /// Writes one protocol line.
    ///
    /// # Errors
    ///
    /// The writer's.
    pub fn send(&mut self, line: &str) -> std::io::Result<()> {
        writeln!(self.output, "{line}")?;
        self.output.flush()
    }

    /// Reads one protocol line without its newline; `None` at the end.
    ///
    /// # Errors
    ///
    /// The reader's.
    pub fn recv(&mut self) -> std::io::Result<Option<String>> {
        let mut line = String::new();
        if self.input.read_line(&mut line)? == 0 {
            return Ok(None);
        }
        Ok(Some(line.trim_end().to_string()))
    }

    /// Asks the driver for the live page at `page`; `None` when it is not
    /// mapped.
    ///
    /// # Errors
    ///
    /// A broken pipe or a malformed reply.
    pub fn fetch(&mut self, page: u64) -> std::io::Result<Option<Vec<u8>>> {
        self.send(&format!("R {page:x}"))?;
        match self.recv()?.as_deref() {
            Some("OK") => {
                let mut buf = vec![0u8; PAGE as usize];
                self.input.read_exact(&mut buf)?;
                Ok(Some(buf))
            }
            Some("NO") => Ok(None),
            other => Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                format!("page reply `{other:?}`"),
            )),
        }
    }
}

/// The guest memory of a run: pages fetched from the live process on first
/// touch, kept as they were and as the run changed them.
pub struct Pager<'a> {
    wire: &'a mut Wire,
    orig: BTreeMap<u64, Vec<u8>>,
    now: BTreeMap<u64, Vec<u8>>,
    missing: Vec<u64>,
}

impl<'a> Pager<'a> {
    /// A pager that fetches over `wire`.
    pub fn new(wire: &'a mut Wire) -> Pager<'a> {
        Pager {
            wire,
            orig: BTreeMap::new(),
            now: BTreeMap::new(),
            missing: Vec::new(),
        }
    }

    fn page(&mut self, page: u64) -> Option<&mut Vec<u8>> {
        if !self.now.contains_key(&page) {
            if self.missing.contains(&page) {
                return None;
            }
            match self.wire.fetch(page) {
                Ok(Some(data)) => {
                    self.orig.insert(page, data.clone());
                    self.now.insert(page, data);
                }
                _ => {
                    self.missing.push(page);
                    return None;
                }
            }
        }
        self.now.get_mut(&page)
    }

    /// Fills `buf` from guest memory at `addr`; the address of the first
    /// byte on an unmapped page is the error.
    ///
    /// # Errors
    ///
    /// An unmapped page.
    pub fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), u64> {
        for (k, b) in buf.iter_mut().enumerate() {
            let a = addr.wrapping_add(k as u64);
            match self.page(a & !(PAGE - 1)) {
                Some(p) => *b = p[(a & (PAGE - 1)) as usize],
                None => return Err(a),
            }
        }
        Ok(())
    }

    /// Stores `data` at `addr`.
    ///
    /// # Errors
    ///
    /// An unmapped page.
    pub fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), u64> {
        for (k, b) in data.iter().enumerate() {
            let a = addr.wrapping_add(k as u64);
            match self.page(a & !(PAGE - 1)) {
                Some(p) => p[(a & (PAGE - 1)) as usize] = *b,
                None => return Err(a),
            }
        }
        Ok(())
    }

    /// Records a page an engine with its own memory ended with.
    pub fn set_page(&mut self, page: u64, data: Vec<u8>) {
        self.now.insert(page, data);
    }

    /// Fetches `page` from the live process for the engine's own memory;
    /// the page is remembered as the original.
    pub fn original(&mut self, page: u64) -> Option<Vec<u8>> {
        if let Some(p) = self.orig.get(&page) {
            return Some(p.clone());
        }
        let data = self.wire.fetch(page).ok().flatten()?;
        self.orig.insert(page, data.clone());
        Some(data)
    }

    /// Every page the run touched, with the bytes it ended with.
    pub fn touched(&self) -> impl Iterator<Item = (u64, &[u8])> {
        self.now.iter().map(|(p, d)| (*p, d.as_slice()))
    }

    /// The wire, for the protocol lines after the run.
    pub fn wire(&mut self) -> &mut Wire {
        self.wire
    }
}
