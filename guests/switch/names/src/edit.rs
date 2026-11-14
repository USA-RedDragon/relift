//! Text edits on a mangled name and the substitution renumbering they need.
//!
//! An edit replaces a byte range with new text. Each candidate span of the
//! input maps to a span of the output: positions before, after or between
//! edits shift, and a position at an edit maps to the point the edit names
//! for spans that start or end there. After the edits, the output is parsed
//! again with references as opaque terminals, and every reference is
//! rewritten to the index of the output candidate whose span equals the
//! mapped span of the candidate it named.

use crate::parse::{Parsed, Span, parse};

/// One replacement of `start..end` by `text`.
#[derive(Debug, Clone)]
pub(crate) struct Edit {
    pub start: usize,
    pub end: usize,
    pub text: String,
    /// The input position that, as the start of a span, maps to
    /// `start_off` bytes into `text`.
    pub anchor: usize,
    pub start_off: usize,
    /// Where in `text` a span that ends at `end` ends.
    pub end_off: usize,
}

impl Edit {
    /// Inserts `text` at `at`: spans that start there start `start_off` bytes
    /// into it, spans that end there end `end_off` bytes into it.
    pub fn insert(at: usize, text: String, start_off: usize, end_off: usize) -> Edit {
        Edit {
            start: at,
            end: at,
            text,
            anchor: at,
            start_off,
            end_off,
        }
    }

    /// Replaces `start..end`: spans that start at `anchor` start at
    /// `start_off`, and spans that end at `end` end at `end_off`.
    pub fn replace(
        start: usize,
        end: usize,
        anchor: usize,
        text: String,
        start_off: usize,
        end_off: usize,
    ) -> Edit {
        Edit {
            start,
            end,
            text,
            anchor,
            start_off,
            end_off,
        }
    }
}

/// A set of edits plus explicit span overrides.
#[derive(Debug, Default)]
pub(crate) struct Rewrite {
    edits: Vec<Edit>,
    pairs: Vec<(Span, Span)>,
}

impl Rewrite {
    /// Adds an edit. A second insertion at the same point goes after the
    /// first: spans that end there keep the first one's rule, spans that
    /// start there take the second one's.
    pub fn push(&mut self, e: Edit) {
        let same = self
            .edits
            .iter_mut()
            .find(|a| a.start == a.end && e.start == e.end && a.start == e.start);
        match same {
            Some(a) => {
                a.start_off = a.text.len() + e.start_off;
                a.text.push_str(&e.text);
            }
            None => self.edits.push(e),
        }
    }

    /// The edits, sorted, if none overlap.
    pub fn finish(&mut self) -> Option<()> {
        self.edits.sort_by_key(|e| (e.start, e.end));
        for w in self.edits.windows(2) {
            if w[1].start < w[0].end {
                return None;
            }
        }
        Some(())
    }

    /// Where the text of the edit that starts at `start` begins in the
    /// output.
    pub fn out_start(&self, start: usize) -> Option<usize> {
        let mut delta: isize = 0;
        for e in &self.edits {
            if e.start == start {
                return Some(shift(e.start, delta));
            }
            delta += e.text.len() as isize - (e.end - e.start) as isize;
        }
        None
    }

    /// Where the text of the edit that starts at `start` ends in the output.
    pub fn out_end(&self, start: usize) -> Option<usize> {
        let e = self.edits.iter().find(|e| e.start == start)?;
        Some(self.out_start(start)? + e.text.len())
    }

    /// Maps the input span `from` to the output span `to`, overriding the
    /// position rules.
    pub fn pair(&mut self, from: Span, to: Span) {
        self.pairs.push((from, to));
    }

    pub fn apply(&self, input: &str) -> String {
        let mut out = String::with_capacity(input.len() + 64);
        let mut at = 0;
        for e in &self.edits {
            out.push_str(&input[at..e.start]);
            out.push_str(&e.text);
            at = e.end;
        }
        out.push_str(&input[at..]);
        out
    }

    fn map_pos(&self, p: usize, as_start: bool) -> Option<usize> {
        let mut delta: isize = 0;
        for e in &self.edits {
            let out_s = shift(e.start, delta);
            if as_start && p == e.anchor {
                return Some(out_s + e.start_off);
            }
            if !as_start && p == e.end {
                return Some(out_s + e.end_off);
            }
            if p < e.start || (p == e.start && !as_start) {
                return Some(shift(p, delta));
            }
            if p < e.end || (p == e.start && e.start == e.end) {
                return None;
            }
            delta += e.text.len() as isize - (e.end - e.start) as isize;
        }
        Some(shift(p, delta))
    }

    pub fn map_span(&self, s: Span) -> Option<Span> {
        if let Some((_, to)) = self.pairs.iter().find(|(from, _)| *from == s) {
            return Some(*to);
        }
        Some(Span {
            start: self.map_pos(s.start, true)?,
            end: self.map_pos(s.end, false)?,
        })
    }
}

fn shift(p: usize, delta: isize) -> usize {
    (p as isize + delta) as usize
}

/// The text of a reference to candidate `index`.
pub(crate) fn ref_text(index: usize) -> String {
    if index == 0 {
        return "S_".to_string();
    }
    let mut n = index - 1;
    let mut digits = Vec::new();
    loop {
        let d = (n % 36) as u8;
        digits.push(if d < 10 { b'0' + d } else { b'A' + d - 10 });
        n /= 36;
        if n == 0 {
            break;
        }
    }
    digits.reverse();
    format!("S{}_", String::from_utf8_lossy(&digits))
}

/// Applies `rw` to `input` (parsed strictly as `orig`), renumbers every
/// reference, and checks that the result parses strictly.
pub(crate) fn rewrite(input: &str, orig: &Parsed, rw: &Rewrite) -> Option<String> {
    let mid = rw.apply(input);
    let mid_parsed = parse(mid.as_bytes(), false).ok()?;
    if mid_parsed.refs.len() != orig.refs.len() {
        return None;
    }
    let mut out = String::with_capacity(mid.len());
    let mut at = 0;
    for (r, m) in orig.refs.iter().zip(&mid_parsed.refs) {
        let start = rw.map_pos(r.span.start, true)?;
        if start != m.span.start {
            return None;
        }
        let target = *orig.subs.get(r.index)?;
        let k = orig.subs[..r.index]
            .iter()
            .filter(|s| **s == target)
            .count();
        let mapped = rw.map_span(target)?;
        let j = mid_parsed
            .subs
            .iter()
            .enumerate()
            .filter(|(_, s)| **s == mapped)
            .nth(k)?
            .0;
        let new = ref_text(j);
        out.push_str(&mid[at..m.span.start]);
        out.push_str(&new);
        at = m.span.end;
    }
    out.push_str(&mid[at..]);
    parse(out.as_bytes(), true).ok()?;
    Some(out)
}
