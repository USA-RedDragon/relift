//! Which console a cartridge is for.

/// The console family of an image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum System {
    /// Sega Master System (and Mark III).
    Sms,
    /// Game Gear.
    Gg,
    /// SG-1000 and SC-3000.
    Sg1000,
}

/// Where the system came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SystemSource {
    /// The region code of the `TMR SEGA` header.
    Header,
    /// The file extension the caller passed in.
    Extension,
}

impl System {
    /// A short lowercase name, stable for tool output.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            System::Sms => "sms",
            System::Gg => "gg",
            System::Sg1000 => "sg1000",
        }
    }

    /// The system a header region code `3..=7` names.
    #[must_use]
    pub fn from_region_code(code: u8) -> Option<System> {
        match code {
            3 | 4 => Some(System::Sms),
            5..=7 => Some(System::Gg),
            _ => None,
        }
    }

    /// The system a file extension names (`sms`, `gg`, `sg`, `sc`, any case,
    /// with or without the dot).
    #[must_use]
    pub fn from_extension(ext: &str) -> Option<System> {
        let e = ext.strip_prefix('.').unwrap_or(ext);
        if e.eq_ignore_ascii_case("sms") {
            Some(System::Sms)
        } else if e.eq_ignore_ascii_case("gg") {
            Some(System::Gg)
        } else if e.eq_ignore_ascii_case("sg") || e.eq_ignore_ascii_case("sc") {
            Some(System::Sg1000)
        } else {
            None
        }
    }
}

impl SystemSource {
    /// A short lowercase name, stable for tool output.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            SystemSource::Header => "header",
            SystemSource::Extension => "extension",
        }
    }
}
