/// How a span is painted when color is on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Paint {
    /// No color.
    Plain,
    /// Bold.
    Bold,
    /// Dimmed, for times.
    Dim,
    /// Green, for finished work.
    Green,
    /// Yellow, for counts that need a look.
    Yellow,
    /// Red, for errors.
    Red,
    /// Cyan, for a running stage.
    Cyan,
}

impl Paint {
    fn code(self) -> Option<&'static str> {
        match self {
            Paint::Plain => None,
            Paint::Bold => Some("1"),
            Paint::Dim => Some("2"),
            Paint::Green => Some("32"),
            Paint::Yellow => Some("33"),
            Paint::Red => Some("31"),
            Paint::Cyan => Some("36"),
        }
    }
}

/// A line of text made of painted spans.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Line(Vec<(Paint, String)>);

impl Line {
    /// An empty line.
    pub fn new() -> Line {
        Line::default()
    }

    /// Appends unpainted text.
    #[must_use]
    pub fn text(self, text: impl Into<String>) -> Line {
        self.paint(Paint::Plain, text)
    }

    /// Appends painted text.
    #[must_use]
    pub fn paint(mut self, paint: Paint, text: impl Into<String>) -> Line {
        let text = text.into();
        if !text.is_empty() {
            self.0.push((paint, text));
        }
        self
    }

    /// Appends the spans of `other`.
    #[must_use]
    pub fn then(mut self, other: Line) -> Line {
        self.0.extend(other.0);
        self
    }

    /// The text without any paint.
    pub fn plain(&self) -> String {
        self.0.iter().map(|(_, t)| t.as_str()).collect()
    }

    /// The line as a string, with color escapes when `color` is set, cut to
    /// `width` visible characters when given.
    pub fn render(&self, color: bool, width: Option<usize>) -> String {
        let mut left = width.unwrap_or(usize::MAX);
        let mut out = String::new();
        for (paint, text) in &self.0 {
            if left == 0 {
                break;
            }
            let shown: String = text.chars().take(left).collect();
            left -= shown.chars().count();
            match paint.code().filter(|_| color) {
                Some(code) => {
                    out.push_str("\x1b[");
                    out.push_str(code);
                    out.push('m');
                    out.push_str(&shown);
                    out.push_str("\x1b[0m");
                }
                None => out.push_str(&shown),
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Line {
        Line::new()
            .paint(Paint::Cyan, "lift")
            .text("  ")
            .paint(Paint::Yellow, "3 unlifted")
    }

    #[test]
    fn color_off_has_no_escape() {
        assert_eq!(sample().render(false, None), "lift  3 unlifted");
    }

    #[test]
    fn color_on_wraps_each_painted_span() {
        assert_eq!(
            sample().render(true, None),
            "\x1b[36mlift\x1b[0m  \x1b[33m3 unlifted\x1b[0m"
        );
    }

    #[test]
    fn width_cuts_visible_characters_only() {
        assert_eq!(sample().render(false, Some(8)), "lift  3 ");
        assert_eq!(sample().render(true, Some(4)), "\x1b[36mlift\x1b[0m");
    }
}
