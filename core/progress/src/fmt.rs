use std::time::Duration;

/// A count with thousands separators: `31,616`.
pub fn count(n: u64) -> String {
    let digits = n.to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3);
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(c);
    }
    out
}

/// A counted noun with a plural `s`: `1 shard`, `2 shards`, `0 shards`.
pub fn noun(n: u64, word: &str) -> String {
    format!("{} {word}{}", count(n), if n == 1 { "" } else { "s" })
}

/// A time: `0.1 s`, `42.0 s`, `1m12s`, `1h02m`.
pub fn duration(d: Duration) -> String {
    let secs = d.as_secs();
    if secs < 60 {
        format!("{:.1} s", d.as_secs_f64())
    } else if secs < 3600 {
        format!("{}m{:02}s", secs / 60, secs % 60)
    } else {
        format!("{}h{:02}m", secs / 3600, secs % 3600 / 60)
    }
}

/// A size in bytes, powers of 1000: `812 bytes`, `4.2 MB`.
pub fn bytes(n: u64) -> String {
    const UNITS: [&str; 4] = ["KB", "MB", "GB", "TB"];
    if n < 1000 {
        return format!("{n} bytes");
    }
    let mut v = n as f64;
    let mut unit = "";
    for u in UNITS {
        v /= 1000.0;
        unit = u;
        if v < 1000.0 {
            break;
        }
    }
    format!("{v:.1} {unit}")
}

/// A bar of `width` cells inside brackets: `[=====>    ]`.
pub fn bar(done: u64, total: u64, width: usize) -> String {
    let cells = if total == 0 {
        width
    } else {
        usize::try_from(u128::from(done.min(total)) * width as u128 / u128::from(total))
            .unwrap_or(width)
    };
    let mut s = String::with_capacity(width + 2);
    s.push('[');
    for i in 0..width {
        s.push(if i < cells {
            '='
        } else if i == cells && done > 0 {
            '>'
        } else {
            ' '
        });
    }
    s.push(']');
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_get_separators() {
        assert_eq!(count(0), "0");
        assert_eq!(count(999), "999");
        assert_eq!(count(1000), "1,000");
        assert_eq!(count(31616), "31,616");
        assert_eq!(count(1234567), "1,234,567");
    }

    #[test]
    fn nouns_take_a_plural_s() {
        assert_eq!(noun(0, "shard"), "0 shards");
        assert_eq!(noun(1, "shard"), "1 shard");
        assert_eq!(noun(1234, "file"), "1,234 files");
    }

    #[test]
    fn durations_match_the_approved_mockup() {
        assert_eq!(duration(Duration::from_millis(100)), "0.1 s");
        assert_eq!(duration(Duration::from_millis(4200)), "4.2 s");
        assert_eq!(duration(Duration::from_secs(72)), "1m12s");
        assert_eq!(duration(Duration::from_secs(3720)), "1h02m");
    }

    #[test]
    fn sizes_use_powers_of_1000() {
        assert_eq!(bytes(812), "812 bytes");
        assert_eq!(bytes(4_200_000), "4.2 MB");
        assert_eq!(bytes(1_500), "1.5 KB");
    }

    #[test]
    fn bars_fill_in_proportion() {
        assert_eq!(bar(0, 10, 10), "[          ]");
        assert_eq!(bar(5, 10, 10), "[=====>    ]");
        assert_eq!(bar(14210, 31616, 20), "[========>           ]");
        assert_eq!(bar(10, 10, 10), "[==========]");
        assert_eq!(bar(3, 0, 4), "[====]");
    }
}
