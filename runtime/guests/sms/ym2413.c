/**
 * @file ym2413.c
 * @brief The YM2413 (OPLL) FM sound unit.
 *
 * Ported from ares (component/audio/ym2413: ym2413.cpp, io.cpp,
 * operator.cpp, voice.cpp, constants.cpp; author Talarubi), with the sine
 * and exponent tables precomputed so every host gets the same values. It
 * carries this notice:
 *
 * Copyright (c) 2004-2025 ares team, Near et al
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "ym2413.h"

#include <string.h>

enum { TRIGGER, ATTACK, DECAY, SUSTAIN, RELEASE };

#define LFO_PERIOD (210u << 12)

static const int8_t level_scaling[16] = {0, 24, 32, 37, 40, 43, 45, 47, 48, 50, 51, 52, 53, 54, 55, 56};

static const uint64_t envelope_steps[17][2] = {
    {0x0101010101010101ull, 0x0101010101010101ull}, {0x0101110101011101ull, 0x0101110101011101ull},
    {0x0111011101110111ull, 0x0111011101110111ull}, {0x0111111101111111ull, 0x0111111101111111ull},
    {0x1111111111111111ull, 0x0101010101010101ull}, {0x1121112111211121ull, 0x0101110101011101ull},
    {0x1212121212121212ull, 0x0111011101110111ull}, {0x1222122212221222ull, 0x0111111101111111ull},
    {0x2222222222222222ull, 0x0101010101010101ull}, {0x2242224222422242ull, 0x0101010111110101ull},
    {0x2424242424242424ull, 0x0101111101011111ull}, {0x2444244424442444ull, 0x0101111111111111ull},
    {0x4444444444444444ull, 0x1111111111111111ull}, {0x4484448444844484ull, 0x1111111122221111ull},
    {0x4848484848484848ull, 0x1111222211112222ull}, {0x4888488848884888ull, 0x1111222222222222ull},
    {0x0000000000000000ull, 0x2222222222222222ull},
};

static const uint8_t melodic_tones[15][8] = {
    {0x71, 0x61, 0x1e, 0x17, 0xd0, 0x78, 0x00, 0x17}, {0x13, 0x41, 0x1a, 0x0d, 0xd8, 0xf7, 0x23, 0x13},
    {0x13, 0x01, 0x99, 0x00, 0xf2, 0xc4, 0x11, 0x23}, {0x31, 0x61, 0x0e, 0x07, 0xa8, 0x64, 0x70, 0x27},
    {0x32, 0x21, 0x1e, 0x06, 0xe0, 0x76, 0x00, 0x28}, {0x31, 0x22, 0x16, 0x05, 0xe0, 0x71, 0x00, 0x18},
    {0x21, 0x61, 0x1d, 0x07, 0x82, 0x81, 0x10, 0x07}, {0x23, 0x21, 0x2d, 0x14, 0xa2, 0x72, 0x00, 0x07},
    {0x61, 0x61, 0x1b, 0x06, 0x64, 0x65, 0x10, 0x17}, {0x41, 0x61, 0x0b, 0x18, 0x85, 0xf7, 0x71, 0x07},
    {0x13, 0x01, 0x83, 0x11, 0xfa, 0xe4, 0x10, 0x04}, {0x17, 0xc1, 0x24, 0x07, 0xf8, 0xf8, 0x22, 0x12},
    {0x61, 0x50, 0x0c, 0x05, 0xc2, 0xf5, 0x20, 0x42}, {0x01, 0x01, 0x55, 0x03, 0xc9, 0x95, 0x03, 0x02},
    {0x61, 0x41, 0x89, 0x03, 0xf1, 0xe4, 0x40, 0x13},
};

static const uint8_t rhythm_tones[3][8] = {
    {0x01, 0x01, 0x18, 0x0f, 0xdf, 0xf8, 0x6a, 0x6d},
    {0x01, 0x01, 0x00, 0x00, 0xc8, 0xd8, 0xa7, 0x48},
    {0x05, 0x01, 0x00, 0x00, 0xf8, 0xaa, 0x59, 0x55},
};

static const uint16_t sin_table[1024] = {
    4275, 3463, 3087, 2839, 2653, 2505, 2381, 2275, 2183, 2101, 2027, 1959, 1899, 1841, 1789, 1739,
    1693, 1651, 1609, 1571, 1535, 1499, 1465, 1435, 1403, 1375, 1345, 1319, 1293, 1267, 1243, 1219,
    1197, 1175, 1153, 1133, 1113, 1093, 1073, 1055, 1037, 1019, 1003, 985, 969, 953, 937, 923,
    907, 893, 879, 865, 851, 837, 823, 811, 799, 785, 773, 761, 751, 739, 727, 717,
    705, 695, 683, 673, 663, 653, 643, 633, 623, 615, 605, 595, 587, 579, 569, 561,
    553, 543, 535, 527, 519, 511, 503, 497, 489, 481, 473, 467, 459, 453, 445, 439,
    431, 425, 419, 411, 405, 399, 393, 387, 381, 375, 369, 363, 357, 351, 345, 339,
    335, 329, 323, 319, 313, 307, 303, 297, 293, 287, 283, 277, 273, 269, 263, 259,
    255, 251, 245, 241, 237, 233, 229, 225, 221, 217, 213, 209, 205, 201, 197, 193,
    189, 185, 183, 179, 175, 171, 167, 165, 161, 157, 155, 151, 149, 145, 141, 139,
    135, 133, 129, 127, 125, 121, 119, 115, 113, 111, 107, 105, 103, 99, 97, 95,
    93, 91, 87, 85, 83, 81, 79, 77, 75, 73, 71, 69, 67, 65, 63, 61,
    59, 57, 55, 53, 51, 49, 47, 47, 45, 43, 41, 41, 39, 37, 35, 35,
    33, 31, 31, 29, 27, 27, 25, 25, 23, 21, 21, 19, 19, 17, 17, 15,
    15, 15, 13, 13, 11, 11, 11, 9, 9, 9, 7, 7, 7, 5, 5, 5,
    5, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 5,
    5, 5, 5, 7, 7, 7, 9, 9, 9, 11, 11, 11, 13, 13, 15, 15,
    15, 17, 17, 19, 19, 21, 21, 23, 25, 25, 27, 27, 29, 31, 31, 33,
    35, 35, 37, 39, 41, 41, 43, 45, 47, 47, 49, 51, 53, 55, 57, 59,
    61, 63, 65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85, 87, 91, 93,
    95, 97, 99, 103, 105, 107, 111, 113, 115, 119, 121, 125, 127, 129, 133, 135,
    139, 141, 145, 149, 151, 155, 157, 161, 165, 167, 171, 175, 179, 183, 185, 189,
    193, 197, 201, 205, 209, 213, 217, 221, 225, 229, 233, 237, 241, 245, 251, 255,
    259, 263, 269, 273, 277, 283, 287, 293, 297, 303, 307, 313, 319, 323, 329, 335,
    339, 345, 351, 357, 363, 369, 375, 381, 387, 393, 399, 405, 411, 419, 425, 431,
    439, 445, 453, 459, 467, 473, 481, 489, 497, 503, 511, 519, 527, 535, 543, 553,
    561, 569, 579, 587, 595, 605, 615, 623, 633, 643, 653, 663, 673, 683, 695, 705,
    717, 727, 739, 751, 761, 773, 785, 799, 811, 823, 837, 851, 865, 879, 893, 907,
    923, 937, 953, 969, 985, 1003, 1019, 1037, 1055, 1073, 1093, 1113, 1133, 1153, 1175, 1197,
    1219, 1243, 1267, 1293, 1319, 1345, 1375, 1403, 1435, 1465, 1499, 1535, 1571, 1609, 1651, 1693,
    1739, 1789, 1841, 1899, 1959, 2027, 2101, 2183, 2275, 2381, 2505, 2653, 2839, 3087, 3463, 4275,
    4274, 3462, 3086, 2838, 2652, 2504, 2380, 2274, 2182, 2100, 2026, 1958, 1898, 1840, 1788, 1738,
    1692, 1650, 1608, 1570, 1534, 1498, 1464, 1434, 1402, 1374, 1344, 1318, 1292, 1266, 1242, 1218,
    1196, 1174, 1152, 1132, 1112, 1092, 1072, 1054, 1036, 1018, 1002, 984, 968, 952, 936, 922,
    906, 892, 878, 864, 850, 836, 822, 810, 798, 784, 772, 760, 750, 738, 726, 716,
    704, 694, 682, 672, 662, 652, 642, 632, 622, 614, 604, 594, 586, 578, 568, 560,
    552, 542, 534, 526, 518, 510, 502, 496, 488, 480, 472, 466, 458, 452, 444, 438,
    430, 424, 418, 410, 404, 398, 392, 386, 380, 374, 368, 362, 356, 350, 344, 338,
    334, 328, 322, 318, 312, 306, 302, 296, 292, 286, 282, 276, 272, 268, 262, 258,
    254, 250, 244, 240, 236, 232, 228, 224, 220, 216, 212, 208, 204, 200, 196, 192,
    188, 184, 182, 178, 174, 170, 166, 164, 160, 156, 154, 150, 148, 144, 140, 138,
    134, 132, 128, 126, 124, 120, 118, 114, 112, 110, 106, 104, 102, 98, 96, 94,
    92, 90, 86, 84, 82, 80, 78, 76, 74, 72, 70, 68, 66, 64, 62, 60,
    58, 56, 54, 52, 50, 48, 46, 46, 44, 42, 40, 40, 38, 36, 34, 34,
    32, 30, 30, 28, 26, 26, 24, 24, 22, 20, 20, 18, 18, 16, 16, 14,
    14, 14, 12, 12, 10, 10, 10, 8, 8, 8, 6, 6, 6, 4, 4, 4,
    4, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 4,
    4, 4, 4, 6, 6, 6, 8, 8, 8, 10, 10, 10, 12, 12, 14, 14,
    14, 16, 16, 18, 18, 20, 20, 22, 24, 24, 26, 26, 28, 30, 30, 32,
    34, 34, 36, 38, 40, 40, 42, 44, 46, 46, 48, 50, 52, 54, 56, 58,
    60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 90, 92,
    94, 96, 98, 102, 104, 106, 110, 112, 114, 118, 120, 124, 126, 128, 132, 134,
    138, 140, 144, 148, 150, 154, 156, 160, 164, 166, 170, 174, 178, 182, 184, 188,
    192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 250, 254,
    258, 262, 268, 272, 276, 282, 286, 292, 296, 302, 306, 312, 318, 322, 328, 334,
    338, 344, 350, 356, 362, 368, 374, 380, 386, 392, 398, 404, 410, 418, 424, 430,
    438, 444, 452, 458, 466, 472, 480, 488, 496, 502, 510, 518, 526, 534, 542, 552,
    560, 568, 578, 586, 594, 604, 614, 622, 632, 642, 652, 662, 672, 682, 694, 704,
    716, 726, 738, 750, 760, 772, 784, 798, 810, 822, 836, 850, 864, 878, 892, 906,
    922, 936, 952, 968, 984, 1002, 1018, 1036, 1054, 1072, 1092, 1112, 1132, 1152, 1174, 1196,
    1218, 1242, 1266, 1292, 1318, 1344, 1374, 1402, 1434, 1464, 1498, 1534, 1570, 1608, 1650, 1692,
    1738, 1788, 1840, 1898, 1958, 2026, 2100, 2182, 2274, 2380, 2504, 2652, 2838, 3086, 3462, 4274,
};

static const int16_t exp_table[512] = {
    -2043, 2042, -2038, 2037, -2032, 2031, -2027, 2026, -2021, 2020, -2016, 2015, -2011, 2010, -2005, 2004,
    -2000, 1999, -1994, 1993, -1989, 1988, -1984, 1983, -1978, 1977, -1973, 1972, -1967, 1966, -1962, 1961,
    -1957, 1956, -1952, 1951, -1946, 1945, -1941, 1940, -1936, 1935, -1931, 1930, -1925, 1924, -1920, 1919,
    -1915, 1914, -1910, 1909, -1905, 1904, -1899, 1898, -1894, 1893, -1889, 1888, -1884, 1883, -1879, 1878,
    -1874, 1873, -1869, 1868, -1864, 1863, -1859, 1858, -1854, 1853, -1849, 1848, -1844, 1843, -1839, 1838,
    -1834, 1833, -1829, 1828, -1824, 1823, -1819, 1818, -1814, 1813, -1809, 1808, -1804, 1803, -1799, 1798,
    -1795, 1794, -1790, 1789, -1785, 1784, -1780, 1779, -1775, 1774, -1770, 1769, -1766, 1765, -1761, 1760,
    -1756, 1755, -1751, 1750, -1747, 1746, -1742, 1741, -1737, 1736, -1733, 1732, -1728, 1727, -1723, 1722,
    -1718, 1717, -1714, 1713, -1709, 1708, -1705, 1704, -1700, 1699, -1695, 1694, -1691, 1690, -1686, 1685,
    -1682, 1681, -1677, 1676, -1673, 1672, -1668, 1667, -1664, 1663, -1659, 1658, -1655, 1654, -1650, 1649,
    -1646, 1645, -1641, 1640, -1637, 1636, -1632, 1631, -1628, 1627, -1624, 1623, -1619, 1618, -1615, 1614,
    -1610, 1609, -1606, 1605, -1602, 1601, -1597, 1596, -1593, 1592, -1589, 1588, -1585, 1584, -1580, 1579,
    -1576, 1575, -1572, 1571, -1567, 1566, -1563, 1562, -1559, 1558, -1555, 1554, -1551, 1550, -1546, 1545,
    -1542, 1541, -1538, 1537, -1534, 1533, -1530, 1529, -1526, 1525, -1521, 1520, -1517, 1516, -1513, 1512,
    -1509, 1508, -1505, 1504, -1501, 1500, -1497, 1496, -1493, 1492, -1489, 1488, -1485, 1484, -1481, 1480,
    -1477, 1476, -1473, 1472, -1469, 1468, -1465, 1464, -1461, 1460, -1457, 1456, -1453, 1452, -1449, 1448,
    -1445, 1444, -1441, 1440, -1437, 1436, -1434, 1433, -1430, 1429, -1426, 1425, -1422, 1421, -1418, 1417,
    -1414, 1413, -1410, 1409, -1407, 1406, -1403, 1402, -1399, 1398, -1395, 1394, -1392, 1391, -1388, 1387,
    -1384, 1383, -1380, 1379, -1377, 1376, -1373, 1372, -1369, 1368, -1365, 1364, -1362, 1361, -1358, 1357,
    -1354, 1353, -1351, 1350, -1347, 1346, -1343, 1342, -1340, 1339, -1336, 1335, -1333, 1332, -1329, 1328,
    -1325, 1324, -1322, 1321, -1318, 1317, -1315, 1314, -1311, 1310, -1308, 1307, -1304, 1303, -1301, 1300,
    -1297, 1296, -1293, 1292, -1290, 1289, -1287, 1286, -1283, 1282, -1280, 1279, -1276, 1275, -1273, 1272,
    -1269, 1268, -1266, 1265, -1262, 1261, -1259, 1258, -1256, 1255, -1252, 1251, -1249, 1248, -1245, 1244,
    -1242, 1241, -1239, 1238, -1235, 1234, -1232, 1231, -1229, 1228, -1225, 1224, -1222, 1221, -1219, 1218,
    -1215, 1214, -1212, 1211, -1209, 1208, -1206, 1205, -1202, 1201, -1199, 1198, -1196, 1195, -1193, 1192,
    -1189, 1188, -1186, 1185, -1183, 1182, -1180, 1179, -1177, 1176, -1173, 1172, -1170, 1169, -1167, 1166,
    -1164, 1163, -1161, 1160, -1158, 1157, -1155, 1154, -1151, 1150, -1148, 1147, -1145, 1144, -1142, 1141,
    -1139, 1138, -1136, 1135, -1133, 1132, -1130, 1129, -1127, 1126, -1124, 1123, -1121, 1120, -1118, 1117,
    -1115, 1114, -1112, 1111, -1109, 1108, -1106, 1105, -1103, 1102, -1100, 1099, -1097, 1096, -1094, 1093,
    -1091, 1090, -1088, 1087, -1085, 1084, -1082, 1081, -1079, 1078, -1076, 1075, -1073, 1072, -1070, 1069,
    -1067, 1066, -1065, 1064, -1062, 1061, -1059, 1058, -1056, 1055, -1053, 1052, -1050, 1049, -1047, 1046,
    -1045, 1044, -1042, 1041, -1039, 1038, -1036, 1035, -1033, 1032, -1031, 1030, -1028, 1027, -1025, 1024,
};

/** @brief An arithmetic right shift of a signed value, the same on every host. */
static int32_t asr(int32_t v, unsigned n) { return v >= 0 ? v >> n : -(int32_t)((~(uint32_t)v >> n) + 1u); }

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

static uint8_t release_rate(const sms_ym2413_op *o)
{
    switch (o->state) {
    case TRIGGER:
        return 13;
    case ATTACK:
        return o->attack;
    case DECAY:
        return o->decay;
    case SUSTAIN:
        return o->sustainable ? 0 : o->release;
    default:
        if (!o->audible)
            return 0;
        if (o->sustain_on)
            return 5;
        return o->sustainable ? o->release : 7;
    }
}

static void op_update(sms_ym2413_op *o)
{
    int32_t base = release_rate(o);
    int32_t ksr = (2 * o->block + ((o->fnumber >> 8) & 1)) >> (o->scale_rate ? 0 : 2);
    o->rate = (uint8_t)(imin(60, base ? 4 * base + ksr : 0) & 0x7F);
    o->divider = (uint8_t)(imax(o->state == ATTACK ? 2 : 0, 13 - o->rate / 4) & 15);
    o->sequence = envelope_steps[o->rate < 48 ? o->rate % 4 : o->rate - 44][o->state == ATTACK ? 0 : 1];
    int32_t ksl = level_scaling[(o->fnumber >> 5) & 15] + 8 * o->block - 56;
    int32_t level = o->total_level;
    level += imax(0, o->scale_level ? ksl : 0) >> (3 - o->scale_level);
    o->level = (uint8_t)(level & 0x7F);
    int32_t f = (o->fnumber >> 6) & 7;
    int32_t lfo[8] = {0, -f / 2, -f, -f / 2, 0, f / 2, f, f / 2};
    int32_t m = o->multiple;
    m += (m == 14) - (m == 13 || m == 11);
    m = imax(1, 2 * m);
    for (unsigned n = 0; n < 8; n++)
        o->pitch[n] = ((uint32_t)((o->fnumber + lfo[n]) * m / 2) << o->block) & 0x7FFFFu;
}

static void op_set_state(sms_ym2413_op *o, uint8_t state)
{
    o->state = state;
    op_update(o);
}

static void op_sync(sms_ym2413_op *o, bool hard, sms_ym2413_op *mod)
{
    if (o->state == TRIGGER && ((o->envelope >> 2) & 31) == 31) {
        op_set_state(o, ATTACK);
        if (o->rate >= 60) {
            op_set_state(o, DECAY);
            o->envelope = 0;
        }
        if (hard)
            o->phase = 0;
        if (mod) {
            op_set_state(mod, ATTACK);
            if (mod->rate >= 60) {
                op_set_state(mod, DECAY);
                mod->envelope = 0;
            }
            if (hard)
                mod->phase = 0;
        }
    }
    if (o->state == ATTACK && o->envelope == 0)
        op_set_state(o, DECAY);
    if (o->state == DECAY && o->envelope >= o->sustain << 3)
        op_set_state(o, SUSTAIN);
    if (mod)
        op_sync(mod, hard, NULL);
}

static void op_trigger(sms_ym2413_op *o, bool key, bool sustain)
{
    o->sustain_on = sustain;
    if (o->key_on == key)
        return;
    o->key_on = key;
    op_set_state(o, key ? TRIGGER : RELEASE);
}

static int16_t op_clock(sms_ym2413_op *o, uint32_t clock, int32_t offset, int32_t modulation)
{
    uint32_t y = sin_table[(uint32_t)(offset + modulation) & 0x3FFu];
    if (o->waveform && !(y & 1u))
        y = sin_table[0];
    uint32_t w = clock / 64 % 210 * o->tremolo;
    uint32_t lfo = (w < 105 ? w : 210 - w) >> 3;
    y = (y + ((uint32_t)o->envelope << 5)) & 0x3FFFu;
    y = (y + ((uint32_t)o->level << 6)) & 0x3FFFu;
    y = (y + (lfo << 5)) & 0x3FFFu;
    o->prior = o->output;
    int32_t out = asr(exp_table[y & 0x1FFu], (y >> 9) & 31);
    o->output = (int16_t)((int32_t)((uint32_t)out << 20) >> 20);
    o->phase = (o->phase + o->pitch[clock / 1024 % 8 * o->vibrato]) & 0x7FFFFu;
    uint32_t lsbs = o->divider ? clock & ((1u << o->divider) - 1u) : 0;
    if (o->rate && (o->state == ATTACK ? lsbs & ~3u : lsbs) == 0) {
        uint32_t msbs = (clock >> o->divider) & 15u;
        int32_t step = (int32_t)(((o->sequence << (4 * msbs)) >> 60) & 15u);
        if (o->state == ATTACK)
            step = asr(~(int32_t)o->envelope * step, 4);
        o->envelope = (uint8_t)imax(0, imin(0x7F, o->envelope + step));
    }
    return o->output;
}

static void voice_update(sms_ym2413_voice *v, const uint8_t *d)
{
    if (d) {
        v->mod.multiple = d[0] & 15;
        v->mod.scale_rate = (d[0] >> 4) & 1;
        v->mod.sustainable = (d[0] >> 5) & 1;
        v->mod.vibrato = (d[0] >> 6) & 1;
        v->mod.tremolo = (d[0] >> 7) & 1;
        v->car.multiple = d[1] & 15;
        v->car.scale_rate = (d[1] >> 4) & 1;
        v->car.sustainable = (d[1] >> 5) & 1;
        v->car.vibrato = (d[1] >> 6) & 1;
        v->car.tremolo = (d[1] >> 7) & 1;
        v->mod.total_level = d[2] & 0x3F;
        v->mod.scale_level = (uint8_t)(((d[2] >> 6) & 1) << 1 | ((d[2] >> 7) & 1));
        v->feedback = d[3] & 7;
        v->mod.waveform = (d[3] >> 3) & 1;
        v->car.waveform = (d[3] >> 4) & 1;
        v->car.scale_level = (uint8_t)(((d[3] >> 6) & 1) << 1 | ((d[3] >> 7) & 1));
        v->mod.decay = d[4] & 15;
        v->mod.attack = d[4] >> 4;
        v->car.decay = d[5] & 15;
        v->car.attack = d[5] >> 4;
        v->mod.release = d[6] & 15;
        v->mod.sustain = d[6] >> 4;
        v->car.release = d[7] & 15;
        v->car.sustain = d[7] >> 4;
    }
    v->mod.block = v->block;
    v->mod.fnumber = v->fnumber;
    if (v->mod.audible)
        v->mod.total_level = (uint8_t)((v->tone << 2) & 0x3F);
    op_update(&v->mod);
    v->car.block = v->block;
    v->car.fnumber = v->fnumber;
    v->car.total_level = (uint8_t)((v->level << 2) & 0x3F);
    op_update(&v->car);
}

static void reload(sms_ym2413_state *s, unsigned i)
{
    sms_ym2413_voice *v = &s->voice[i];
    v->mod.audible = s->rhythm && i > 6;
    v->car.audible = true;
    if (s->rhythm && i >= 6)
        voice_update(v, rhythm_tones[i - 6]);
    else if (v->tone)
        voice_update(v, melodic_tones[v->tone - 1]);
    else
        voice_update(v, s->custom);
}

void sms_ym2413_reset(sms_ym2413 *y)
{
    memset(y, 0, sizeof *y);
    sms_ym2413_state *s = &y->s;
    s->noise = 1;
    for (unsigned n = 0; n < 9; n++) {
        s->voice[n].mod.state = s->voice[n].car.state = RELEASE;
        s->voice[n].mod.envelope = s->voice[n].car.envelope = 0x7F;
        reload(s, n);
    }
    for (unsigned n = 0; n < 3; n++) {
        for (unsigned k = 0; k < 3; k++) {
            s->voice[3 * n + k].mod.slot = (uint8_t)(1 + k + 6 * n);
            s->voice[3 * n + k].car.slot = (uint8_t)(4 + k + 6 * n);
        }
    }
}

static uint32_t bit(uint32_t v, unsigned n) { return (v >> n) & 1u; }

static int32_t sample(sms_ym2413_state *s)
{
    int32_t melody = 0, rhythm = 0, mask = -8;
    for (unsigned i = 0; i < (s->rhythm ? 6u : 9u); i++) {
        sms_ym2413_voice *v = &s->voice[i];
        int32_t input = asr(v->mod.output + v->mod.prior, 8u - v->feedback);
        op_clock(&v->mod, s->clock, (int32_t)(v->mod.phase >> 9), v->feedback ? input : 0);
        op_clock(&v->car, s->clock, (int32_t)(v->car.phase >> 9), v->mod.output * 2);
        op_sync(&v->car, true, &v->mod);
        melody += mask & v->car.output;
    }
    if (s->rhythm) {
        sms_ym2413_voice *bass = &s->voice[6];
        sms_ym2413_op *hihat = &s->voice[7].mod, *snare = &s->voice[7].car;
        sms_ym2413_op *tomtom = &s->voice[8].mod, *cymbal = &s->voice[8].car;
        uint32_t noise = s->noise & 1u;
        uint32_t m = (bit(hihat->phase, 16) ^ bit(hihat->phase, 11)) | bit(hihat->phase, 12) | bit(cymbal->phase, 14) |
                     bit(cymbal->phase, 12);
        int32_t input = asr(bass->mod.output + bass->mod.prior, 8u - bass->feedback);
        op_clock(&bass->mod, s->clock, (int32_t)(bass->mod.phase >> 9), bass->feedback ? input : 0);
        op_clock(&bass->car, s->clock, (int32_t)(bass->car.phase >> 9), bass->mod.output * 2);
        op_sync(&bass->car, true, &bass->mod);
        rhythm += mask & bass->car.output;
        rhythm += mask & op_clock(hihat, s->clock, (int32_t)(m << 9 | ((m ^ noise) ? 52u : 208u)), 0);
        rhythm += mask & op_clock(tomtom, s->clock, (int32_t)(tomtom->phase >> 9), 0);
        rhythm += mask & op_clock(snare, s->clock, (int32_t)((1u << (bit(snare->phase, 17) + 8)) ^ (noise << 8)), 0);
        rhythm += mask & op_clock(cymbal, s->clock, (int32_t)(m << 9 | 256u), 0);
        op_sync(hihat, false, NULL);
        op_sync(snare, true, NULL);
        op_sync(tomtom, true, NULL);
        op_sync(cymbal, false, NULL);
    }
    s->noise = ((s->noise >> 1) ^ ((s->noise & 1u) * (1u << 22 | 1u << 8 | 1u << 7 | 1u))) & 0x7FFFFFu;
    if (++s->clock == LFO_PERIOD)
        s->clock -= LFO_PERIOD;
    return melody + 2 * rhythm;
}

void sms_ym2413_catch_up(sms_ym2413 *y, uint64_t until)
{
    while ((y->s.samples + 1) * SMS_YM2413_DIV <= until) {
        int32_t v = sample(&y->s);
        y->s.samples++;
        if (y->count == SMS_YM2413_BUF) {
            y->head = (y->head + 1) % SMS_YM2413_BUF;
            y->count--;
            y->dropped++;
        }
        y->buf[(y->head + y->count) % SMS_YM2413_BUF] = v;
        y->count++;
    }
}

void sms_ym2413_address(sms_ym2413 *y, uint64_t time, uint8_t data)
{
    sms_ym2413_catch_up(y, time);
    y->s.address = data;
}

void sms_ym2413_write(sms_ym2413 *y, uint64_t time, uint8_t data)
{
    sms_ym2413_catch_up(y, time);
    sms_ym2413_state *s = &y->s;
    uint8_t a = s->address;
    unsigned index = (a & 15u) % 9u;
    sms_ym2413_voice *v = &s->voice[index];
    if (a <= 0x07) {
        s->custom[a] = data;
        for (unsigned n = 0; n < 9; n++)
            if (s->voice[n].tone == 0)
                reload(s, n);
        return;
    }
    if (a == 0x0E) {
        bool r = (data >> 5) & 1u;
        if (s->rhythm != r) {
            s->rhythm = r;
            for (unsigned n = 0; n < 9; n++)
                reload(s, n);
        }
        if (s->rhythm) {
            op_trigger(&s->voice[7].mod, data & 1u, false);
            op_trigger(&s->voice[8].car, (data >> 1) & 1u, false);
            op_trigger(&s->voice[8].mod, (data >> 2) & 1u, false);
            op_trigger(&s->voice[7].car, (data >> 3) & 1u, false);
            op_trigger(&s->voice[6].car, (data >> 4) & 1u, false);
        }
        return;
    }
    if (a >= 0x10 && a <= 0x1F) {
        v->fnumber = (uint16_t)((v->fnumber & 0x100u) | data);
        voice_update(v, NULL);
        return;
    }
    if (a >= 0x20 && a <= 0x2F) {
        v->fnumber = (uint16_t)((v->fnumber & 0xFFu) | (data & 1u) << 8);
        v->block = (data >> 1) & 7u;
        voice_update(v, NULL);
        op_trigger(&v->car, (data >> 4) & 1u, (data >> 5) & 1u);
        return;
    }
    if (a >= 0x30 && a <= 0x3F) {
        v->level = data & 15u;
        v->tone = data >> 4;
        if (s->rhythm && index >= 6)
            voice_update(v, NULL);
        else if (v->tone)
            voice_update(v, melodic_tones[v->tone - 1]);
        else
            voice_update(v, s->custom);
    }
}

size_t sms_ym2413_drain(sms_ym2413 *y, int32_t *out, size_t max)
{
    size_t n = 0;
    while (n < max && y->count) {
        out[n++] = y->buf[y->head];
        y->head = (y->head + 1) % SMS_YM2413_BUF;
        y->count--;
    }
    return n;
}

double sms_ym2413_level(int32_t sample) { return sample / 32768.0 * 4.0; }
