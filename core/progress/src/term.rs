/// The `--color` choice.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColorChoice {
    /// Color when the output is a terminal that can show it.
    Auto,
    /// Always color.
    Always,
    /// Never color.
    Never,
}

impl ColorChoice {
    /// Parses `auto`, `always` or `never`.
    ///
    /// # Errors
    ///
    /// Any other word, with the allowed ones listed.
    pub fn parse(s: &str) -> Result<ColorChoice, String> {
        match s {
            "auto" => Ok(ColorChoice::Auto),
            "always" => Ok(ColorChoice::Always),
            "never" => Ok(ColorChoice::Never),
            other => Err(format!(
                "color must be auto, always or never, not {other:?}"
            )),
        }
    }
}

/// What the output stream and the environment allow.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Terminal {
    /// Whether redrawing lines in place can work.
    pub live: bool,
    /// Whether `auto` turns color on.
    pub auto_color: bool,
    /// The line width to keep live lines under.
    pub width: usize,
}

impl Terminal {
    /// Looks at the real environment for a stream that is a terminal or not.
    pub fn detect(is_tty: bool) -> Terminal {
        Terminal::from_vars(is_tty, cfg!(windows), |k| std::env::var(k).ok())
    }

    /// As [`Terminal::detect`], with the variables read through `var` and the
    /// platform named, so tests need no environment.
    ///
    /// `NO_COLOR` turns `auto` color off when it is set and not empty
    /// (<https://no-color.org>). `TERM=dumb` and a stream that is not a
    /// terminal turn off both color and redrawing, and so does a set `CI`.
    /// Windows consoles show escape codes only in Windows Terminal, ConEmu,
    /// ANSICON and shells that set `TERM`; elsewhere output stays plain.
    pub fn from_vars(
        is_tty: bool,
        windows: bool,
        var: impl Fn(&str) -> Option<String>,
    ) -> Terminal {
        let set = |k: &str| var(k).is_some_and(|v| !v.is_empty());
        let dumb = var("TERM").is_some_and(|t| t == "dumb");
        let ansi = !windows
            || set("WT_SESSION")
            || set("ANSICON")
            || var("ConEmuANSI").is_some_and(|v| v == "ON")
            || (set("TERM") && !dumb);
        let capable = is_tty && !dumb && ansi;
        let width = var("COLUMNS")
            .and_then(|c| c.trim().parse::<usize>().ok())
            .filter(|w| *w >= 40)
            .unwrap_or(80);
        Terminal {
            live: capable && !set("CI"),
            auto_color: capable && !set("NO_COLOR"),
            width,
        }
    }

    /// Whether color is on for `choice`.
    pub fn color(&self, choice: ColorChoice) -> bool {
        match choice {
            ColorChoice::Always => true,
            ColorChoice::Never => false,
            ColorChoice::Auto => self.auto_color,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn term(is_tty: bool, windows: bool, vars: &[(&str, &str)]) -> Terminal {
        Terminal::from_vars(is_tty, windows, |k| {
            vars.iter()
                .find(|(n, _)| *n == k)
                .map(|(_, v)| (*v).to_string())
        })
    }

    #[test]
    fn a_terminal_gets_live_lines_and_color() {
        let t = term(true, false, &[("TERM", "xterm-256color")]);
        assert!(t.live && t.color(ColorChoice::Auto));
    }

    #[test]
    fn a_pipe_gets_neither() {
        let t = term(false, false, &[("TERM", "xterm")]);
        assert!(!t.live && !t.color(ColorChoice::Auto));
    }

    #[test]
    fn term_dumb_gets_neither() {
        let t = term(true, false, &[("TERM", "dumb")]);
        assert!(!t.live && !t.color(ColorChoice::Auto));
    }

    #[test]
    fn a_non_empty_no_color_turns_auto_color_off_only() {
        let t = term(true, false, &[("NO_COLOR", "1")]);
        assert!(t.live && !t.color(ColorChoice::Auto));
        let t = term(true, false, &[("NO_COLOR", "")]);
        assert!(t.color(ColorChoice::Auto));
    }

    #[test]
    fn ci_turns_live_lines_off() {
        let t = term(true, false, &[("CI", "true")]);
        assert!(!t.live && t.color(ColorChoice::Auto));
    }

    #[test]
    fn the_flag_overrides_the_environment() {
        let t = term(false, false, &[("NO_COLOR", "1"), ("TERM", "dumb")]);
        assert!(t.color(ColorChoice::Always));
        let t = term(true, false, &[]);
        assert!(!t.color(ColorChoice::Never));
    }

    #[test]
    fn windows_needs_a_terminal_that_shows_escapes() {
        assert!(!term(true, true, &[]).live);
        assert!(term(true, true, &[("WT_SESSION", "x")]).live);
        assert!(term(true, true, &[("TERM", "xterm")]).live);
        assert!(!term(true, true, &[("TERM", "dumb")]).live);
    }

    #[test]
    fn columns_set_the_width() {
        assert_eq!(term(true, false, &[("COLUMNS", "120")]).width, 120);
        assert_eq!(term(true, false, &[("COLUMNS", "10")]).width, 80);
        assert_eq!(term(true, false, &[]).width, 80);
    }

    #[test]
    fn the_flag_parses() {
        assert_eq!(ColorChoice::parse("auto"), Ok(ColorChoice::Auto));
        assert_eq!(ColorChoice::parse("always"), Ok(ColorChoice::Always));
        assert_eq!(ColorChoice::parse("never"), Ok(ColorChoice::Never));
        assert!(ColorChoice::parse("yes").is_err());
    }
}
