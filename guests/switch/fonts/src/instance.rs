//! Subsetting a variable TrueType font and instancing it into a static one.
//!
//! skera subsets the variable font to the characters wanted, keeping its
//! variation tables; each kept glyph is then drawn by skrifa at the weight
//! wanted (gvar and avar applied, composites flattened) and written back as
//! a simple glyph with write-fonts, with its advance from HVAR. The layout
//! and vertical tables, whose values vary too, are dropped: the console's
//! font users read outlines and horizontal metrics.

use read_fonts::collections::IntSet;
use read_fonts::tables::glyf::CurvePoint;
use read_fonts::types::{GlyphId, NameId, Tag};
use read_fonts::{FontRef, TableProvider};
use skrifa::MetadataProvider;
use skrifa::instance::Size;
use skrifa::outline::{DrawSettings, OutlinePen};
use write_fonts::FontBuilder;
use write_fonts::from_obj::ToOwnedTable;
use write_fonts::tables::glyf::{Bbox, Contour, GlyfLocaBuilder, Glyph, SimpleGlyph};
use write_fonts::tables::hmtx::{Hmtx, LongMetric};
use write_fonts::tables::loca::LocaFormat;
use write_fonts::types::UfWord;

/// Why a font could not be made.
#[derive(Debug)]
pub enum Error {
    /// The source is not a font relift can read, or lacks a table.
    Read(String),
    /// The subsetter refused the font.
    Subset(String),
    /// A glyph could not be drawn or written.
    Glyph(u32, String),
    /// A table could not be written.
    Write(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Read(e) => write!(f, "the source font cannot be read: {e}"),
            Error::Subset(e) => write!(f, "the subsetter refused the font: {e}"),
            Error::Glyph(g, e) => write!(f, "glyph {g}: {e}"),
            Error::Write(e) => write!(f, "the font cannot be written: {e}"),
        }
    }
}

impl std::error::Error for Error {}

const DROPPED: [&[u8; 4]; 11] = [
    b"GSUB", b"GPOS", b"GDEF", b"BASE", b"vhea", b"vmtx", b"VVAR", b"VORG", b"STAT", b"DSIG",
    b"MVAR",
];
const COPIED: [&[u8; 4]; 3] = [b"cmap", b"name", b"post"];

fn read<T>(r: Result<T, read_fonts::ReadError>) -> Result<T, Error> {
    r.map_err(|e| Error::Read(e.to_string()))
}

fn write<T>(r: Result<T, write_fonts::BuilderError>) -> Result<T, Error> {
    r.map_err(|e| Error::Write(e.to_string()))
}

/// The contours a glyph draws, in font units rounded to integers.
#[derive(Default)]
struct Pen {
    contours: Vec<Vec<CurvePoint>>,
    cubic: bool,
}

fn unit(v: f32) -> i16 {
    v.round().clamp(f32::from(i16::MIN), f32::from(i16::MAX)) as i16
}

impl Pen {
    fn push(&mut self, p: CurvePoint) {
        if let Some(c) = self.contours.last_mut() {
            c.push(p);
        }
    }
}

impl OutlinePen for Pen {
    fn move_to(&mut self, x: f32, y: f32) {
        self.contours
            .push(vec![CurvePoint::on_curve(unit(x), unit(y))]);
    }

    fn line_to(&mut self, x: f32, y: f32) {
        self.push(CurvePoint::on_curve(unit(x), unit(y)));
    }

    fn quad_to(&mut self, cx: f32, cy: f32, x: f32, y: f32) {
        self.push(CurvePoint::off_curve(unit(cx), unit(cy)));
        self.push(CurvePoint::on_curve(unit(x), unit(y)));
    }

    fn curve_to(&mut self, _: f32, _: f32, _: f32, _: f32, _: f32, _: f32) {
        self.cubic = true;
    }

    fn close(&mut self) {
        if let Some(c) = self.contours.last_mut()
            && c.len() > 1
            && c.first().map(|p| (p.x, p.y)) == c.last().map(|p| (p.x, p.y))
        {
            c.pop();
        }
    }
}

/// `contour` without the on-curve points TrueType implies: those exactly
/// halfway between two off-curve neighbors.
fn implied_points_dropped(contour: Vec<CurvePoint>) -> Vec<CurvePoint> {
    let n = contour.len();
    (0..n)
        .filter(|&i| {
            let (a, p, b) = (contour[(i + n - 1) % n], contour[i], contour[(i + 1) % n]);
            !(p.on_curve
                && !a.on_curve
                && !b.on_curve
                && i32::from(a.x) + i32::from(b.x) == 2 * i32::from(p.x)
                && i32::from(a.y) + i32::from(b.y) == 2 * i32::from(p.y))
        })
        .map(|i| contour[i])
        .collect()
}

/// A static TrueType font of `variable`'s glyphs for `code_points` at
/// weight `wght`.
///
/// # Errors
///
/// The source is not a TrueType variable font with a weight axis, the
/// subsetter refuses it, or a glyph has cubic outlines or cannot be written.
pub fn instance(variable: &[u8], code_points: &[u32], wght: f32) -> Result<Vec<u8>, Error> {
    let font = read(FontRef::new(variable))?;
    if !font.axes().iter().any(|a| a.tag() == Tag::new(b"wght")) {
        return Err(Error::Read("the font has no weight axis".into()));
    }
    let unicodes: IntSet<u32> = code_points.iter().copied().collect();
    let dropped: IntSet<Tag> = DROPPED.iter().map(|t| Tag::new(t)).collect();
    let names: IntSet<NameId> = (0..=6u16).map(NameId::new).collect();
    let languages: IntSet<u16> = [0x409u16].into_iter().collect();
    let plan = skera::Plan::new(
        &IntSet::empty(),
        &unicodes,
        &font,
        skera::SubsetFlags::default(),
        &dropped,
        &IntSet::empty(),
        &IntSet::empty(),
        &names,
        &languages,
    );
    let subset = skera::subset_font(&font, &plan).map_err(|e| Error::Subset(e.to_string()))?;
    static_font(&subset, wght)
}

fn static_font(subset: &[u8], wght: f32) -> Result<Vec<u8>, Error> {
    let font = read(FontRef::new(subset))?;
    let location = font.axes().location([("wght", wght)]);
    let outlines = font.outline_glyphs();
    let metrics = font.glyph_metrics(Size::unscaled(), &location);
    let count = read(font.maxp())?.num_glyphs();
    let mut glyphs = GlyfLocaBuilder::new();
    let mut hmtx = Vec::with_capacity(usize::from(count));
    let mut bounds: Option<Bbox> = None;
    let (mut max_points, mut max_contours) = (0u16, 0u16);
    for g in 0..u32::from(count) {
        let gid = GlyphId::new(g);
        let mut pen = Pen::default();
        if let Some(outline) = outlines.get(gid) {
            outline
                .draw(
                    DrawSettings::unhinted(Size::unscaled(), &location),
                    &mut pen,
                )
                .map_err(|e| Error::Glyph(g, e.to_string()))?;
        }
        if pen.cubic {
            return Err(Error::Glyph(g, "cubic outlines are not TrueType".into()));
        }
        let advance = metrics.advance_width(gid).unwrap_or(0.0).round();
        let advance = advance.clamp(0.0, f32::from(u16::MAX)) as u16;
        let contours: Vec<Contour> = pen
            .contours
            .into_iter()
            .map(implied_points_dropped)
            .filter(|c| !c.is_empty())
            .map(Contour::from)
            .collect();
        if contours.is_empty() {
            glyphs
                .add_glyph(&Glyph::Empty)
                .map_err(|e| Error::Glyph(g, e.to_string()))?;
            hmtx.push(LongMetric::new(advance, 0));
            continue;
        }
        let points: usize = contours.iter().map(Contour::len).sum();
        max_points = max_points.max(u16::try_from(points).unwrap_or(u16::MAX));
        max_contours = max_contours.max(u16::try_from(contours.len()).unwrap_or(u16::MAX));
        let mut simple = SimpleGlyph {
            bbox: Bbox::default(),
            contours,
            instructions: Vec::new(),
            overlaps: false,
        };
        simple.recompute_bounding_box();
        bounds = Some(bounds.map_or(simple.bbox, |b| b.union(simple.bbox)));
        hmtx.push(LongMetric::new(advance, simple.bbox.x_min));
        glyphs
            .add_glyph(&simple)
            .map_err(|e| Error::Glyph(g, e.to_string()))?;
    }
    let (glyf, loca, format) = glyphs.build();
    let mut out = FontBuilder::new();
    write(out.add_table(&glyf))?;
    write(out.add_table(&loca))?;
    let mut head: write_fonts::tables::head::Head = read(font.head())?.to_owned_table();
    head.index_to_loc_format = i16::from(format != LocaFormat::Short);
    let b = bounds.unwrap_or_default();
    (head.x_min, head.y_min, head.x_max, head.y_max) = (b.x_min, b.y_min, b.x_max, b.y_max);
    write(out.add_table(&head))?;
    let mut hhea: write_fonts::tables::hhea::Hhea = read(font.hhea())?.to_owned_table();
    hhea.number_of_h_metrics = count;
    hhea.advance_width_max = UfWord::new(hmtx.iter().map(|m| m.advance).max().unwrap_or(0));
    write(out.add_table(&hhea))?;
    write(out.add_table(&Hmtx::new(hmtx, Vec::new())))?;
    let mut maxp: write_fonts::tables::maxp::Maxp = read(font.maxp())?.to_owned_table();
    maxp.max_points = Some(max_points);
    maxp.max_contours = Some(max_contours);
    maxp.max_composite_points = Some(0);
    maxp.max_composite_contours = Some(0);
    maxp.max_component_elements = Some(0);
    maxp.max_component_depth = Some(0);
    maxp.max_zones.get_or_insert(1);
    for f in [
        &mut maxp.max_twilight_points,
        &mut maxp.max_storage,
        &mut maxp.max_function_defs,
        &mut maxp.max_instruction_defs,
        &mut maxp.max_stack_elements,
        &mut maxp.max_size_of_instructions,
    ] {
        f.get_or_insert(0);
    }
    write(out.add_table(&maxp))?;
    let mut os2: write_fonts::tables::os2::Os2 = read(font.os2())?.to_owned_table();
    os2.us_weight_class = wght.round().clamp(1.0, 1000.0) as u16;
    write(out.add_table(&os2))?;
    for t in COPIED {
        let tag = Tag::new(t);
        let data = font
            .table_data(tag)
            .ok_or_else(|| Error::Read(format!("no {tag} table")))?;
        out.add_raw(tag, data.as_bytes().to_vec());
    }
    Ok(out.build())
}

#[cfg(test)]
mod tests {
    use super::*;
    use read_fonts::tables::glyf::Glyph as ReadGlyph;
    use write_fonts::OffsetMarker;
    use write_fonts::tables::cmap::Cmap;
    use write_fonts::tables::fvar::{AxisInstanceArrays, Fvar, VariationAxisRecord};
    use write_fonts::tables::gvar::{GlyphDelta, GlyphDeltas, GlyphVariations, Gvar, Tent};
    use write_fonts::tables::head::Head;
    use write_fonts::tables::hhea::Hhea;
    use write_fonts::tables::maxp::Maxp;
    use write_fonts::tables::name::{Name, NameRecord};
    use write_fonts::tables::os2::Os2;
    use write_fonts::tables::post::Post;
    use write_fonts::types::{F2Dot14, FWord, Fixed};

    fn square(x0: i16, y0: i16, x1: i16, y1: i16) -> SimpleGlyph {
        let c = Contour::from(vec![
            CurvePoint::on_curve(x0, y0),
            CurvePoint::on_curve(x0, y1),
            CurvePoint::on_curve(x1, y1),
            CurvePoint::on_curve(x1, y0),
        ]);
        let mut g = SimpleGlyph {
            bbox: Bbox::default(),
            contours: vec![c],
            instructions: Vec::new(),
            overlaps: false,
        };
        g.recompute_bounding_box();
        g
    }

    /// A weight-axis font (100 to 900, default 100) whose `A` moves 80 units
    /// right and widens its advance by 100 at the heaviest weight; `B` is
    /// there to be dropped.
    fn variable_font() -> Vec<u8> {
        let mut glyphs = GlyfLocaBuilder::new();
        glyphs.add_glyph(&Glyph::Empty).unwrap();
        glyphs.add_glyph(&square(0, 0, 500, 700)).unwrap();
        glyphs.add_glyph(&square(10, 10, 20, 20)).unwrap();
        let (glyf, loca, format) = glyphs.build();
        let head = Head {
            units_per_em: 1000,
            index_to_loc_format: i16::from(format != LocaFormat::Short),
            ..Default::default()
        };
        let hhea = Hhea::new(
            FWord::new(880),
            FWord::new(-120),
            FWord::new(0),
            UfWord::new(600),
            FWord::new(0),
            FWord::new(0),
            FWord::new(500),
            1,
            0,
            0,
            3,
        );
        let hmtx = Hmtx::new(
            vec![
                LongMetric::new(500, 0),
                LongMetric::new(600, 0),
                LongMetric::new(600, 10),
            ],
            Vec::new(),
        );
        let cmap = Cmap::from_mappings([('A', GlyphId::new(1)), ('B', GlyphId::new(2))]).unwrap();
        let name = Name::new(vec![NameRecord::new(
            3,
            1,
            0x409,
            NameId::new(1),
            OffsetMarker::new("Test".to_string()),
        )]);
        let axis = VariationAxisRecord::new(
            Tag::new(b"wght"),
            Fixed::from_f64(100.0),
            Fixed::from_f64(100.0),
            Fixed::from_f64(900.0),
            0,
            NameId::new(1),
        );
        let fvar = Fvar::new(AxisInstanceArrays::new(vec![axis], Vec::new()));
        let mut deltas: Vec<GlyphDelta> = (0..4).map(|_| GlyphDelta::required(80, 0)).collect();
        deltas.extend([
            GlyphDelta::required(0, 0),
            GlyphDelta::required(100, 0),
            GlyphDelta::required(0, 0),
            GlyphDelta::required(0, 0),
        ]);
        let gvar = Gvar::new(
            vec![
                GlyphVariations::new(GlyphId::new(0), Vec::new()),
                GlyphVariations::new(
                    GlyphId::new(1),
                    vec![GlyphDeltas::new(
                        vec![Tent::new(F2Dot14::from_f32(1.0), None)],
                        deltas,
                    )],
                ),
                GlyphVariations::new(GlyphId::new(2), Vec::new()),
            ],
            1,
        )
        .unwrap();
        let os2 = Os2 {
            us_weight_class: 100,
            ..Default::default()
        };
        let mut b = FontBuilder::new();
        b.add_table(&glyf).unwrap();
        b.add_table(&loca).unwrap();
        b.add_table(&head).unwrap();
        b.add_table(&hhea).unwrap();
        b.add_table(&hmtx).unwrap();
        b.add_table(&Maxp::new(3)).unwrap();
        b.add_table(&cmap).unwrap();
        b.add_table(&name).unwrap();
        b.add_table(&os2).unwrap();
        b.add_table(&Post::default()).unwrap();
        b.add_table(&fvar).unwrap();
        b.add_table(&gvar).unwrap();
        b.build()
    }

    #[test]
    fn instances_the_wanted_glyphs_at_the_weight() {
        let out = instance(&variable_font(), &[u32::from('A')], 400.0).unwrap();
        let font = FontRef::new(&out).unwrap();
        for t in [b"fvar", b"gvar", b"HVAR", b"avar"] {
            assert!(font.table_data(Tag::new(t)).is_none());
        }
        assert_eq!(font.maxp().unwrap().num_glyphs(), 2);
        let gid = font.charmap().map('A').unwrap();
        assert!(font.charmap().map('B').is_none());
        assert_eq!(font.os2().unwrap().us_weight_class(), 400);
        assert_eq!(font.hmtx().unwrap().advance(gid), Some(638));
        let glyf = font.glyf().unwrap();
        let loca = font.loca(None).unwrap();
        let Some(ReadGlyph::Simple(g)) = loca.get_glyf(gid, &glyf).unwrap() else {
            panic!("A is not a simple glyph");
        };
        let xs: Vec<i16> = g.points().map(|p| p.x).collect();
        assert_eq!(xs, vec![30, 30, 530, 530]);
        assert_eq!((g.x_min(), g.x_max(), g.y_max()), (30, 530, 700));
    }

    #[test]
    fn refuses_a_font_without_a_weight_axis() {
        assert!(matches!(
            instance(b"not a font", &[0x41], 400.0),
            Err(Error::Read(_))
        ));
    }
}
