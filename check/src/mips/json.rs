//! A small JSON reader for the oracle's answers.

/// A JSON value.
#[derive(Clone, Debug)]
pub enum J {
    S(String),
    N(f64),
    A(Vec<J>),
    O(Vec<(String, J)>),
    B,
}
/// Parses the value at `i`.
pub fn jparse(s: &[u8], i: &mut usize) -> J {
    while s[*i].is_ascii_whitespace() {
        *i += 1;
    }
    match s[*i] {
        b'{' => {
            *i += 1;
            let mut o = Vec::new();
            loop {
                while s[*i].is_ascii_whitespace() || s[*i] == b',' {
                    *i += 1;
                }
                if s[*i] == b'}' {
                    *i += 1;
                    return J::O(o);
                }
                let J::S(k) = jparse(s, i) else { panic!("key") };
                while s[*i] != b':' {
                    *i += 1;
                }
                *i += 1;
                o.push((k, jparse(s, i)));
            }
        }
        b'[' => {
            *i += 1;
            let mut a = Vec::new();
            loop {
                while s[*i].is_ascii_whitespace() || s[*i] == b',' {
                    *i += 1;
                }
                if s[*i] == b']' {
                    *i += 1;
                    return J::A(a);
                }
                a.push(jparse(s, i));
            }
        }
        b'"' => {
            *i += 1;
            let st = *i;
            while s[*i] != b'"' {
                *i += 1;
            }
            *i += 1;
            J::S(String::from_utf8_lossy(&s[st..*i - 1]).into())
        }
        b't' | b'f' | b'n' => {
            while s[*i].is_ascii_alphabetic() {
                *i += 1;
            }
            J::B
        }
        _ => {
            let st = *i;
            while matches!(s[*i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                *i += 1;
            }
            J::N(
                std::str::from_utf8(&s[st..*i])
                    .unwrap()
                    .parse()
                    .unwrap_or(0.0),
            )
        }
    }
}
impl J {
    /// The member `k` of an object.
    pub fn get(&self, k: &str) -> Option<&J> {
        match self {
            J::O(o) => o.iter().find(|(n, _)| n == k).map(|(_, v)| v),
            _ => None,
        }
    }
    /// The number: hex text or a JSON number.
    pub fn u(&self) -> u64 {
        match self {
            J::S(s) => u64::from_str_radix(s.trim_start_matches("0x"), 16).unwrap_or(0),
            J::N(n) => *n as u64,
            _ => 0,
        }
    }
}
