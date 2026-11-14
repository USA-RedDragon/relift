//! The local socket of the dev server: a Unix domain socket where the host
//! has one in the standard library. Elsewhere binding fails, and `relift
//! run` says that hot-loading is not available on this host.

#[cfg(unix)]
mod host {
    pub use std::os::unix::net::{UnixListener as Listener, UnixStream as Stream};

    pub fn bind(path: &std::path::Path) -> std::io::Result<Listener> {
        Listener::bind(path)
    }

    pub fn connect(path: &std::path::Path) -> std::io::Result<Stream> {
        Stream::connect(path)
    }

    pub fn accept(l: &Listener) -> std::io::Result<Stream> {
        l.accept().map(|(s, _)| s)
    }
}

#[cfg(not(unix))]
mod host {
    /// No listener on this host.
    pub struct Listener;
    /// No stream on this host.
    pub type Stream = std::net::TcpStream;

    fn none() -> std::io::Error {
        std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "relift has no local socket on this host",
        )
    }

    pub fn bind(_: &std::path::Path) -> std::io::Result<Listener> {
        Err(none())
    }

    pub fn connect(_: &std::path::Path) -> std::io::Result<Stream> {
        Err(none())
    }

    pub fn accept(_: &Listener) -> std::io::Result<Stream> {
        Err(none())
    }
}

pub use host::{Listener, Stream, accept, bind, connect};
