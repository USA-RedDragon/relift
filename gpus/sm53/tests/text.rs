use relift_gpu_sm53::Inst;

/// Words with random fields under each encoding's fixed bits, their code
/// offset and their text.
const CASES: &[(u64, u64, &str)] = &[
    (
        0xed4b780ca98a448e,
        0x25590,
        "not $p2 atom dec e s64 $r142 g[$r68+0x780ca] $r152",
    ),
    (
        0xed3bc7056ff0ffc6,
        0x255b8,
        "$p0 atom inc e s64 $r198 g[0xfffffffffffc7056] 0x0",
    ),
    (
        0x4bf0b58bce71f4ff,
        0x13098,
        "$p1 bfi cc 0x0 $r244 c2[0xfffffffffffff39c] $r107",
    ),
    (
        0x4bf07ffd6480ffff,
        0x130f0,
        "$p0 bfi 0x0 0x0 c31[0x5920] 0x0",
    ),
    (
        0x36f0b3b3fff71a23,
        0x1fe10,
        "bfi cc $r35 $r26 0x33fff $r103",
    ),
    (0x37f0649faff75618, 0x1fec8, "bfi $r24 $r86 -0x60501 $r201"),
    (
        0x53f03e8e82ca1990,
        0xccb8,
        "not $p2 bfi $r144 $r25 $r125 c3[0xffffffffffffa0b0]",
    ),
    (
        0x53f054a9cff708de,
        0xcd48,
        "bfi $r222 $r8 $r169 c10[0x73fc]",
    ),
    (
        0x0b4b35efdff7ff2f,
        0x2c6e8,
        "fadd32i $r47 neg abs 0x0 abs 0xb35efdff",
    ),
    (
        0x09b69bd1cff77281,
        0x2c6f0,
        "fadd32i ftz cc $r129 neg $r114 neg 0x69bd1cff",
    ),
    (
        0x4c5ab0f1280a40fa,
        0x113f0,
        "not $p2 fadd ftz rm cc $r250 $r64 neg abs c28[0x4a00]",
    ),
    (
        0x4c58907ccff0ece3,
        0x11428,
        "$p0 fadd ftz cc $r227 $r236 c31[0x33fc]",
    ),
    (
        0x3859908a11f7ffff,
        0x1dd18,
        "fadd ftz rm cc 0x0 neg 0x0 0xa11f000",
    ),
    (
        0x395d70636d6068ff,
        0x1dd50,
        "$p0 fadd ftz sat 0x0 neg abs $r104 neg 0xe36d6000",
    ),
    (
        0x0c3ce8f6eff1ffb5,
        0x2c468,
        "$p1 ffma32i ftz cc $r181 0x0 0xce8f6eff $r181",
    ),
    (
        0x0d570908e18aff51,
        0x2c470,
        "not $p2 ffma32i fmz cc $r81 neg 0x0 0x70908e18 $r81",
    ),
    (
        0x49bde134bff7e8eb,
        0x24b88,
        "ffma ftz rz sat cc $r235 $r232 neg c13[0x2ffc] $r194",
    ),
    (
        0x49c17fcbae918246,
        0x24b90,
        "$p1 ffma fmz $r70 $r130 neg c18[0xffffffffffffeba4] 0x0",
    ),
    (
        0x338fd8c55ff7a9ff,
        0x28fe8,
        "ffma rm sat cc 0x0 $r169 neg 0xc55ff000 neg $r177",
    ),
    (
        0x33cd7fec00678a27,
        0x28ff0,
        "ffma fmz rm sat $r39 $r138 neg 0xec006000 0x0",
    ),
    (
        0x519cf8958ff7ff9e,
        0x23f10,
        "ffma rz sat cc $r158 0x0 $r241 c5[0x63fc]",
    ),
    (
        0x51d1624268c7ffcd,
        0x23f18,
        "ffma fmz rp $r205 0x0 neg $r196 c16[0xffffffffffff9a30]",
    ),
    (
        0x4c61b7479ff7ff73,
        0x11038,
        "fmnmx ftz cc $r115 neg 0x0 neg c17[0xffffffffffffe7fc] not $p6",
    ),
    (
        0x4c62f64d5ef7ffff,
        0x11058,
        "fmnmx ftz cc 0x0 abs 0x0 neg abs c19[0x57bc] not $p4",
    ),
    (
        0x386226750ff7ffff,
        0x1db18,
        "fmnmx 0x0 0x0 neg abs 0x750ff000 not $p4",
    ),
    (
        0x3961e2110ee7aa5e,
        0x1db70,
        "fmnmx cc $r94 neg abs $r170 neg 0x910ee000 $p4",
    ),
    (
        0x1eb3c3ff7bd7d3bb,
        0x27388,
        "fmul32i ftz sat cc $r187 $r211 0x3c3ff7bd",
    ),
    (
        0x1ea9ab25a59784d7,
        0x27390,
        "fmul32i ftz sat $r215 $r132 0x9ab25a59",
    ),
    (
        0x4c6c9d664127ff79,
        0x10dc8,
        "fmul ftz m2 rp sat cc $r121 0x0 c25[0xffffffffffff9048]",
    ),
    (
        0x4c6993988ffa6205,
        0x10df8,
        "not $p2 fmul ftz d2 rz cc $r5 $r98 neg c6[0x23fc]",
    ),
    (
        0x386c90a332c0dfff,
        0x1d890,
        "$p0 fmul ftz rm sat cc 0x0 $r223 0x2332c000",
    ),
    (
        0x386c8b804cd09249,
        0x1d8b8,
        "$p0 fmul m4 rz sat cc $r73 $r146 0x4cd000",
    ),
    (
        0x48e021596517348b,
        0x26e88,
        "fset f or ftz $r139 abs $r52 neg c22[0x5944] $p2",
    ),
    (
        0x48d5a6c88ff1d8b3,
        0x26eb8,
        "$p1 fset bf ne or ftz cc $r179 abs $r216 c18[0x23fc] not $p5",
    ),
    (
        0x30fda1053ff78271,
        0x2a670,
        "fset bf neu or ftz cc $r113 abs $r130 neg 0x53ff000 $p2",
    ),
    (
        0x3193ab25820a8139,
        0x2a688,
        "not $p2 fset bf le or ftz cc $r57 neg $r129 0xa5820000 $p6",
    ),
    (
        0x4bbd25aa9afa26ff,
        0x1a128,
        "not $p2 fsetp neu or 0x1 0x1 abs $r38 neg c10[0xffffffffffffa6bc] not $p3",
    ),
    (
        0x4bb88425608a5dff,
        0x1a130,
        "not $p2 fsetp nan ftz and 0x1 0x1 abs $r93 neg c9[0x5820] not $p0",
    ),
    (
        0x36b3d58b2877ffc8,
        0x20bf8,
        "fsetp le ftz xor $p1 $p0 abs 0x0 neg abs 0xb287000 not $p3",
    ),
    (
        0x37b8da052c270fff,
        0x20c08,
        "fsetp nan ftz xor 0x1 0x1 neg abs $r15 neg abs 0x852c2000 $p4",
    ),
    (
        0x1c1b6c0c247a80ff,
        0x2b078,
        "not $p2 iadd32i cc 0x0 $r128 -0x493f3db9",
    ),
    (
        0x1c4affdf2ff7ffff,
        0x2b090,
        "iadd32i sat 0x0 0x0 -0x50020d01",
    ),
    (
        0x4ccf7ff43e478a2c,
        0x19c18,
        "iadd3 x $r44 neg $r138 neg c29[0xf90] neg 0x0",
    ),
    (
        0x4cc0091158b74048,
        0x19c28,
        "iadd3 $r72 $r64 c4[0x562c] $r18",
    ),
    (
        0x38cb7fa4a5674109,
        0x206f8,
        "iadd3 x $r9 neg $r65 0x24a56 neg 0x0",
    ),
    (
        0x39c544a171305515,
        0x20708,
        "$p0 iadd3 x $r21 $r85 neg -0x5e8ed $r137",
    ),
    (
        0x5ccfffac8767f2cd,
        0x17c08,
        "iadd3 rs x cc $r205 neg h0 $r242 neg h1 $r118 neg h0 0x0",
    ),
    (
        0x5cc6ffcd028124ff,
        0x17d28,
        "$p1 iadd3 ls cc 0x0 h0 $r36 neg h1 $r40 neg h1 0x0",
    ),
    (
        0x4c2090aa6897ff05,
        0x12450,
        "imnmx u32 xmed cc $r5 0x0 c10[0xffffffffffff9a24] $p1",
    ),
    (
        0x4c20849a7ffa8312,
        0x12498,
        "not $p2 imnmx u32 cc $r18 $r131 c6[0xffffffffffff9ffc] not $p1",
    ),
    (
        0x38208473c6cc8645,
        0x1f048,
        "not $p4 imnmx u32 cc $r69 $r134 0x73c6c not $p0",
    ),
    (
        0xe0da7da242b7ffe6,
        0x25a88,
        "ipa sc centroid sat $r230 a[0x224] $r43 $r251 $p4",
    ),
    (
        0xe01d339f7881ffff,
        0x25ab0,
        "$p1 ipa pass centroid sat 0x0 a[0x1f7] $r136 $r103 not $p2",
    ),
    (
        0x4c1b0dce3ffaff47,
        0x127a8,
        "not $p2 iscadd $r71 neg 0x0 neg c19[0xffffffffffff8ffc] 0x1b",
    ),
    (
        0x4c1b093550bd4dd6,
        0x128c8,
        "not $p5 iscadd $r214 neg $r77 neg c13[0x542c] 0x12",
    ),
    (
        0x381a841863d7ff28,
        0x1f118,
        "iscadd cc $r40 neg 0x0 0x1863d 0x8",
    ),
    (
        0x381887386397ffb4,
        0x1f1c8,
        "iscadd cc $r180 0x0 0x38639 0xe",
    ),
    (
        0x4b5131289b17ffe3,
        0x1ad98,
        "iset bf f or $r227 0x0 c10[0x26c4] $p2",
    ),
    (
        0x4b505a565897114c,
        0x1adb0,
        "iset bf f u32 x xor $r76 $r17 c21[0xffffffffffff9624] $p4",
    ),
    (
        0x365843f46ef1ddef,
        0x21888,
        "$p1 iset gt u32 xor $r239 $r221 0x746ef 0x1",
    ),
    (
        0x365259c5e540cb99,
        0x21890,
        "$p0 iset bf lt u32 x xor $r153 $r203 0x45e54 $p3",
    ),
    (
        0x4b652e999ea0652b,
        0x1ac48,
        "$p0 isetp eq x or $p5 $p3 $r101 c6[0x67a8] not $p5",
    ),
    (
        0x4b652c4b829d9f07,
        0x1ad28,
        "not $p5 isetp eq x or $p0 0x1 $r159 c18[0xffffffffffffe0a4] not $p0",
    ),
    (
        0x3664219eb381400f,
        0x217c8,
        "$p1 isetp eq u32 or $p1 0x1 $r64 0x1eb38 $p3",
    ),
    (
        0x366c498f29bad134,
        0x217f0,
        "not $p2 isetp ge u32 x xor $p6 $p4 $r209 0xf29b $p3",
    ),
    (
        0xef9401cb0e76e9a6,
        0x2468,
        "$p6 ld b32 $r166 c28[$r233-0x4f19]",
    ),
    (
        0xeed7eb7eb79754ff,
        0x3698,
        "ldg e cv u b128 0x0 ncg[$r84-0x481487]",
    ),
    (
        0xeed52eecd7d0ffff,
        0x36b0,
        "$p0 ldg e b64 0x0 ncg[0xffffffffffeecd7d]",
    ),
    (0xef403d9d25170334, 0x2f18, "ld cv u8 $r52 l[$r3-0x262daf]"),
    (
        0xef413924e5e7ff26,
        0x2f48,
        "ld cv s8 $r38 l[0xffffffffff924e5e]",
    ),
    (
        0xef4c0de52a41ffc9,
        0x2cb8,
        "$p1 ld b32 $r201 s[0xffffffffffde52a4]",
    ),
    (0xef4d030bbfb7539f, 0x2d48, "ld b64 $r159 s[$r83+0x30bbfb]"),
    (0x042d201003d7ff51, 0x2c968, "lop32i or $r81 0x0 0xd201003d"),
    (
        0x07d9ffed4aa7ff1a,
        0x2c970,
        "lop32i xor x cc $r26 inv 0x0 inv 0x9ffed4aa",
    ),
    (
        0x032dc079b04113cd,
        0x2b2e8,
        "$p1 lop3 lut x cc $r205 $r19 c30[0x6c10] $r128 0x2d",
    ),
    (
        0x02a4647516d7ffff,
        0x2b2f0,
        "lop3 lut 0x0 0x0 c29[0x45b4] $r200 0xa4",
    ),
    (
        0x5be2bebe5c07ff3d,
        0xc298,
        "lop3 lut nz $p2 cc $r61 0x0 $r192 $r125 0xe5",
    ),
    (
        0x5be3df67ff00ffff,
        0xc2a8,
        "$p0 lop3 lut x z $p3 cc 0x0 0x0 $r240 $r190 0x7f",
    ),
    (
        0x4c471498732069ef,
        0x11a28,
        "$p0 lop xor t 0x1 $r239 inv $r105 c6[0x1cc8]",
    ),
    (
        0x4c423da9aeb01fbe,
        0x11a38,
        "$p0 lop xor x nz $p2 $r190 inv $r31 inv c10[0x6bac]",
    ),
    (
        0x38411eb8a0ea0bfb,
        0x1e4a8,
        "not $p2 lop pass_b x t $p1 $r251 inv $r11 0x38a0e",
    ),
    (
        0x3840bf92032070ff,
        0x1e4b8,
        "$p0 lop pass_b x nz $p0 cc 0x0 inv $r112 inv 0x12032",
    ),
    (0x010aff8667f130ff, 0x1b298, "$p1 mov32i 0x0 0xaff8667f 0x3"),
    (
        0x01058d770e6030a1,
        0x1b2f8,
        "$p0 mov32i $r161 0x58d770e6 0x3",
    ),
    (
        0xebf84e6960112744,
        0x3db0,
        "$p1 red add s32 g[$r39+0x4e696] $r68",
    ),
    (
        0xebf8c9a0b18c2dbd,
        0x3dc8,
        "not $p4 red inc u32 g[$r45-0x365f5] $r189",
    ),
    (
        0xef145d5ce32740f7,
        0x31e8,
        "shfl bfly $p4 $r247 $r64 $r50 0x1757",
    ),
    (
        0xef1352f4689affc5,
        0x3208,
        "not $p2 shfl up $p3 $r197 0x0 $r137 0x14bd",
    ),
    (
        0x4c28912306f7f833,
        0x122c8,
        "shr u32 x brev cc $r51 $r248 c8[0xffffffffffffc1bc]",
    ),
    (
        0xeed8272ad6a70cb6,
        0x3448,
        "stg e u8 ncg[$r12+0x72ad6a] $r182",
    ),
    (
        0xeedba535ce87ff20,
        0x3450,
        "stg e cs s16 ncg[0x535ce8] $r32",
    ),
    (
        0xef562a0d91471716,
        0x2a18,
        "st cs b128 l[$r23-0x5f26ec] $r22",
    ),
    (
        0xef522610eabaf966,
        0x2a38,
        "not $p2 st cs u16 l[$r249+0x610eab] $r102",
    ),
    (0xef580502db275d9b, 0x2850, "st u8 s[$r93+0x502db2] $r155"),
    (0xef59033b26e7ffff, 0x2878, "st s8 s[0x33b26e] 0x0"),
    (
        0xd987750361e7ffff,
        0x29768,
        "texs nodep $r54 0x0 0x0 $r30 0x1750 tcube rga",
    ),
    (
        0xd9694e2cc981ffff,
        0x29770,
        "$p1 texs lz $r204 0x0 0x0 $r152 0x14e2 t3d rba",
    ),
    (
        0xdcfc8faf86c76982,
        0x25fb0,
        "tld ll aoffi ms cl $r130 $r105 $r108 0x8fa t1d 0xf",
    ),
    (
        0xdcfb1fb605f1dbff,
        0x25fc8,
        "$p1 tld ll cl nodep 0x0 $r219 $r95 0x11fb t1d 0xc",
    ),
    (
        0xcb7901f7eff736b8,
        0x2b590,
        "tld4 a aoffi $r184 $r54 0x0 0x101f tcube 0xf",
    ),
    (
        0xcbbfffc5affaff84,
        0x2b5a8,
        "not $p2 tld4 a ptp dc nodep $r132 0x0 0x0 0x1ffc t2d 0xb",
    ),
    (
        0xdf01ffc598a7ffb0,
        0x22278,
        "tld4s r $r89 $r176 0x0 $r138 0x1ffc",
    ),
    (
        0xdf0da3a75bead420,
        0x22288,
        "not $p2 tld4s r aoffi dc $r117 $r32 $r212 $r190 0x1a3a",
    ),
    (
        0xda5112df9f51deff,
        0x294e8,
        "$p1 tlds lz $r249 0x0 $r222 $r245 0x112d t2d rgba",
    ),
    (
        0xda41258235b0fff2,
        0x29538,
        "$p0 tlds lz $r35 $r242 0x0 $r91 0x1258 t2d rgb",
    ),
    (
        0xdf5bd148d00d0517,
        0x65b8,
        "not $p5 tmml lod ndv nodep $r23 $r5 0x1d14 array t3d 0x1",
    ),
    (
        0x4ee7ffd63ff798ff,
        0x29ee8,
        "xmad s16 s16 psl clo x cc 0x0 h1 $r152 c21[0xffffffffffff8ffc] 0x0",
    ),
    (
        0x4ea0b09c01b085bf,
        0x29ef0,
        "$p0 xmad psl cc $r191 h1 $r133 c7[0x6c] $r97",
    ),
    (
        0x36267f96800158ff,
        0x25310,
        "$p1 xmad u16 s16 psl clo 0x0 h1 $r88 0x6800 0x0",
    ),
    (
        0x363256f72ff0ff9b,
        0x25350,
        "$p0 xmad u16 s16 psl mrg cbcc x $r155 h1 0x0 0x72ff $r173",
    ),
    (
        0x5111f95b8a30ed03,
        0x24188,
        "$p0 xmad s16 u16 cc $r3 $r237 h1 $r242 c22[0xffffffffffffe28c]",
    ),
    (
        0x515dffaaf1b7427c,
        0x24190,
        "xmad s16 u16 csfu x cc $r124 $r66 h1 0x0 c10[0xffffffffffffbc6c]",
    ),
];

#[test]
fn text_matches_for_every_case() {
    for &(w, at, want) in CASES {
        let i = Inst::decode(w, at).unwrap_or_else(|| panic!("{w:#018x} unknown"));
        assert_eq!(i.to_string(), want, "{w:#018x}");
    }
}

#[test]
fn forms_without_a_spelling_print_their_fields() {
    let w = 0x5d10_0000_0007_0000 | 0x0201;
    let i = Inst::decode(w, 8).unwrap();
    let t = i.to_string();
    assert!(t.starts_with("hadd2 Dest=0x1 SrcA=0x2"), "{t}");
}

#[test]
fn bool_fields_read_as_zero_or_one() {
    let i = Inst::decode(0x5980_0000_0007_0000 | 1 << 48, 8).unwrap();
    assert_eq!(i.get("NegA"), Some(1));
    assert_eq!(i.get("NegC"), Some(0));
    assert_eq!(i.guard(), Some((7, false)));
}
