//! A small JSON reader for the test files: objects, arrays, numbers,
//! strings, true, false and null.

/// A JSON value.
#[derive(Clone, Debug)]
pub enum J {
    /// A number (the tests only have integers).
    N(u64),
    /// A string.
    S(String),
    /// An array.
    A(Vec<J>),
    /// An object.
    O(Vec<(String, J)>),
    /// null, true or false.
    Null,
}

impl J {
    /// The field `k` of an object.
    pub fn get(&self, k: &str) -> Option<&J> {
        match self {
            J::O(o) => o.iter().find(|(n, _)| n == k).map(|(_, v)| v),
            _ => None,
        }
    }

    /// Element `i` of an array (null when absent).
    pub fn idx(&self, i: usize) -> &J {
        match self {
            J::A(a) => a.get(i).unwrap_or(&J::Null),
            _ => &J::Null,
        }
    }

    /// The number (0 for anything else).
    pub fn num(&self) -> u64 {
        match self {
            J::N(n) => *n,
            _ => 0,
        }
    }

    /// The string ("" for anything else).
    pub fn str(&self) -> &str {
        match self {
            J::S(s) => s,
            _ => "",
        }
    }
}

/// Parses `s`.
pub fn parse(s: &[u8]) -> J {
    let mut i = 0;
    value(s, &mut i)
}

fn ws(s: &[u8], i: &mut usize) {
    while *i < s.len() && s[*i].is_ascii_whitespace() {
        *i += 1;
    }
}

fn value(s: &[u8], i: &mut usize) -> J {
    ws(s, i);
    match s[*i] {
        b'{' => {
            *i += 1;
            let mut o = Vec::new();
            loop {
                ws(s, i);
                if s[*i] == b'}' {
                    *i += 1;
                    return J::O(o);
                }
                if s[*i] == b',' {
                    *i += 1;
                    continue;
                }
                let J::S(k) = value(s, i) else {
                    panic!("object key")
                };
                ws(s, i);
                assert_eq!(s[*i], b':');
                *i += 1;
                let v = value(s, i);
                o.push((k, v));
            }
        }
        b'[' => {
            *i += 1;
            let mut a = Vec::new();
            loop {
                ws(s, i);
                if s[*i] == b']' {
                    *i += 1;
                    return J::A(a);
                }
                if s[*i] == b',' {
                    *i += 1;
                    continue;
                }
                a.push(value(s, i));
            }
        }
        b'"' => {
            *i += 1;
            let st = *i;
            while s[*i] != b'"' {
                if s[*i] == b'\\' {
                    *i += 1;
                }
                *i += 1;
            }
            let v = String::from_utf8_lossy(&s[st..*i]).into_owned();
            *i += 1;
            J::S(v)
        }
        b'n' | b't' | b'f' => {
            while *i < s.len() && s[*i].is_ascii_alphabetic() {
                *i += 1;
            }
            J::Null
        }
        _ => {
            let mut n: u64 = 0;
            while *i < s.len() && s[*i].is_ascii_digit() {
                n = n * 10 + u64::from(s[*i] - b'0');
                *i += 1;
            }
            J::N(n)
        }
    }
}
