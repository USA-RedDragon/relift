//! A parsed cartridge image.

use crate::banks::Layout;
use crate::codemasters::CodemastersHeader;
use crate::error::Result;
use crate::header::Header;
use crate::mapper::{self, MapperDecision};
use crate::system::{System, SystemSource};

/// An image with its layout, headers, system and mapper guess.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Cartridge {
    /// The size and bank layout.
    pub layout: Layout,
    /// The `TMR SEGA` header, when one is present.
    pub header: Option<Header>,
    /// The Codemasters header, when the ROM reaches `$7FF0`; present does
    /// not mean valid, see [`CodemastersHeader::checksum_ok`].
    pub codemasters: Option<CodemastersHeader>,
    /// The system and where it came from; `None` when neither the header
    /// nor the extension says.
    pub system: Option<(System, SystemSource)>,
    /// The mapper guess.
    pub mapper: MapperDecision,
}

impl Cartridge {
    /// Parse an image. `extension` is the file extension (with or without
    /// the dot), used only when the header does not name the system.
    ///
    /// # Errors
    ///
    /// The image has no ROM bytes or is over 4 MiB.
    pub fn parse(image: &[u8], extension: Option<&str>) -> Result<Cartridge> {
        let layout = Layout::of(image.len())?;
        let rom = &image[layout.rom_start()..];
        let header = Header::find(rom);
        let codemasters = CodemastersHeader::read(rom);
        let from_header = header
            .as_ref()
            .and_then(|h| System::from_region_code(h.region_code))
            .map(|s| (s, SystemSource::Header));
        let system = from_header.or_else(|| {
            extension
                .and_then(System::from_extension)
                .map(|s| (s, SystemSource::Extension))
        });
        let mapper = mapper::detect(rom, codemasters.as_ref());
        Ok(Cartridge {
            layout,
            header,
            codemasters,
            system,
            mapper,
        })
    }
}
