module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

export module avionix.entity.unicode;

// All Unicode processing in Avionix goes through this module:
//
//   UTF-8 bytes ──decode_utf8──► code points ──grapheme segmentation──►
//   grapheme clusters ──grapheme_width──► terminal cells
//
// Segmentation follows UAX #29 extended grapheme cluster rules GB3-GB13,
// except GB9a (SpacingMark) and GB9b (Prepend), which are not implemented.
// Width follows East Asian Width (W and F are two cells) with emoji
// presentation treated as wide. The property tables below are a compact
// subset of Unicode 15.1 covering the common scripts and emoji ranges; they
// are sorted, non-overlapping, and searched with binary search.

namespace avionix::detail {

struct code_point_range {
  char32_t first;
  char32_t last;
};

constexpr bool in_table(std::span<const code_point_range> table, char32_t cp) noexcept {
  // First range whose `last` is not below cp; cp is in the table only if
  // that range also starts at or before it.
  const auto candidate =
      std::ranges::lower_bound(table, cp, {}, &code_point_range::last);
  return candidate != table.end() && candidate->first <= cp;
}

// Grapheme_Extend plus emoji modifiers (UAX #29 treats both as Extend).
inline constexpr std::array extend_table = std::to_array<code_point_range>({
    {0x0300, 0x036F},   {0x0483, 0x0489},   {0x0591, 0x05BD},   {0x05BF, 0x05BF},
    {0x05C1, 0x05C2},   {0x05C4, 0x05C5},   {0x05C7, 0x05C7},   {0x0610, 0x061A},
    {0x064B, 0x065F},   {0x0670, 0x0670},   {0x06D6, 0x06DC},   {0x06DF, 0x06E4},
    {0x06E7, 0x06E8},   {0x06EA, 0x06ED},   {0x0711, 0x0711},   {0x0730, 0x074A},
    {0x07A6, 0x07B0},   {0x07EB, 0x07F3},   {0x0816, 0x0819},   {0x081B, 0x0823},
    {0x0825, 0x0827},   {0x0829, 0x082D},   {0x0859, 0x085B},   {0x08D3, 0x08E1},
    {0x08E3, 0x0902},   {0x093A, 0x093A},   {0x093C, 0x093C},   {0x0941, 0x0948},
    {0x094D, 0x094D},   {0x0951, 0x0957},   {0x0962, 0x0963},   {0x0981, 0x0981},
    {0x09BC, 0x09BC},   {0x09BE, 0x09BE},   {0x09C1, 0x09C4},   {0x09CD, 0x09CD},
    {0x09D7, 0x09D7},   {0x09E2, 0x09E3},   {0x0A01, 0x0A02},   {0x0A3C, 0x0A3C},
    {0x0A41, 0x0A42},   {0x0A47, 0x0A48},   {0x0A4B, 0x0A4D},   {0x0A51, 0x0A51},
    {0x0A70, 0x0A71},   {0x0A75, 0x0A75},   {0x0A81, 0x0A82},   {0x0ABC, 0x0ABC},
    {0x0AC1, 0x0AC5},   {0x0AC7, 0x0AC8},   {0x0ACD, 0x0ACD},   {0x0AE2, 0x0AE3},
    {0x0B01, 0x0B01},   {0x0B3C, 0x0B3C},   {0x0B3E, 0x0B3F},   {0x0B41, 0x0B44},
    {0x0B4D, 0x0B4D},   {0x0B56, 0x0B57},   {0x0B62, 0x0B63},   {0x0B82, 0x0B82},
    {0x0BBE, 0x0BBE},   {0x0BC0, 0x0BC0},   {0x0BCD, 0x0BCD},   {0x0BD7, 0x0BD7},
    {0x0C00, 0x0C00},   {0x0C3E, 0x0C40},   {0x0C46, 0x0C48},   {0x0C4A, 0x0C4D},
    {0x0C55, 0x0C56},   {0x0C62, 0x0C63},   {0x0C81, 0x0C81},   {0x0CBC, 0x0CBC},
    {0x0CBF, 0x0CBF},   {0x0CC2, 0x0CC2},   {0x0CC6, 0x0CC6},   {0x0CCC, 0x0CCD},
    {0x0CD5, 0x0CD6},   {0x0CE2, 0x0CE3},   {0x0D00, 0x0D01},   {0x0D3B, 0x0D3C},
    {0x0D3E, 0x0D3E},   {0x0D41, 0x0D44},   {0x0D4D, 0x0D4D},   {0x0D57, 0x0D57},
    {0x0D62, 0x0D63},   {0x0DCA, 0x0DCA},   {0x0DCF, 0x0DCF},   {0x0DD2, 0x0DD4},
    {0x0DD6, 0x0DD6},   {0x0DDF, 0x0DDF},   {0x0E31, 0x0E31},   {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E},   {0x0EB1, 0x0EB1},   {0x0EB4, 0x0EBC},   {0x0EC8, 0x0ECD},
    {0x0F18, 0x0F19},   {0x0F35, 0x0F35},   {0x0F37, 0x0F37},   {0x0F39, 0x0F39},
    {0x0F71, 0x0F7E},   {0x0F80, 0x0F84},   {0x0F86, 0x0F87},   {0x0F8D, 0x0FBC},
    {0x0FC6, 0x0FC6},   {0x102D, 0x1030},   {0x1032, 0x1037},   {0x1039, 0x103A},
    {0x103D, 0x103E},   {0x1058, 0x1059},   {0x105E, 0x1060},   {0x1071, 0x1074},
    {0x1082, 0x1082},   {0x1085, 0x1086},   {0x108D, 0x108D},   {0x109D, 0x109D},
    {0x135D, 0x135F},   {0x1712, 0x1714},   {0x1732, 0x1734},   {0x1752, 0x1753},
    {0x1772, 0x1773},   {0x17B4, 0x17B5},   {0x17B7, 0x17BD},   {0x17C6, 0x17C6},
    {0x17C9, 0x17D3},   {0x17DD, 0x17DD},   {0x180B, 0x180D},   {0x1885, 0x1886},
    {0x18A9, 0x18A9},   {0x1920, 0x1922},   {0x1927, 0x1928},   {0x1932, 0x1932},
    {0x1939, 0x193B},   {0x1A17, 0x1A18},   {0x1A1B, 0x1A1B},   {0x1A56, 0x1A56},
    {0x1A58, 0x1A60},   {0x1A62, 0x1A62},   {0x1A65, 0x1A6C},   {0x1A73, 0x1A7F},
    {0x1AB0, 0x1AFF},   {0x1B00, 0x1B03},   {0x1B34, 0x1B3A},   {0x1B3C, 0x1B3C},
    {0x1B42, 0x1B42},   {0x1B6B, 0x1B73},   {0x1B80, 0x1B81},   {0x1BA2, 0x1BA5},
    {0x1BA8, 0x1BAD},   {0x1BE6, 0x1BE6},   {0x1BE8, 0x1BE9},   {0x1BED, 0x1BED},
    {0x1BEF, 0x1BF1},   {0x1C2C, 0x1C33},   {0x1C36, 0x1C37},   {0x1CD0, 0x1CD2},
    {0x1CD4, 0x1CE0},   {0x1CE2, 0x1CE8},   {0x1CED, 0x1CED},   {0x1CF4, 0x1CF4},
    {0x1CF8, 0x1CF9},   {0x1DC0, 0x1DFF},   {0x200C, 0x200C},   {0x20D0, 0x20F0},
    {0x2CEF, 0x2CF1},   {0x2D7F, 0x2D7F},   {0x2DE0, 0x2DFF},   {0x302A, 0x302F},
    {0x3099, 0x309A},   {0xA66F, 0xA672},   {0xA674, 0xA67D},   {0xA69E, 0xA69F},
    {0xA6F0, 0xA6F1},   {0xA802, 0xA802},   {0xA806, 0xA806},   {0xA80B, 0xA80B},
    {0xA825, 0xA826},   {0xA8C4, 0xA8C5},   {0xA8E0, 0xA8F1},   {0xA8FF, 0xA8FF},
    {0xA926, 0xA92D},   {0xA947, 0xA951},   {0xA980, 0xA982},   {0xA9B3, 0xA9B3},
    {0xA9B6, 0xA9B9},   {0xA9BC, 0xA9BD},   {0xA9E5, 0xA9E5},   {0xAA29, 0xAA2E},
    {0xAA31, 0xAA32},   {0xAA35, 0xAA36},   {0xAA43, 0xAA43},   {0xAA4C, 0xAA4C},
    {0xAA7C, 0xAA7C},   {0xAAB0, 0xAAB0},   {0xAAB2, 0xAAB4},   {0xAAB7, 0xAAB8},
    {0xAABE, 0xAABF},   {0xAAC1, 0xAAC1},   {0xAAEC, 0xAAED},   {0xAAF6, 0xAAF6},
    {0xABE5, 0xABE5},   {0xABE8, 0xABE8},   {0xABED, 0xABED},   {0xFB1E, 0xFB1E},
    {0xFE00, 0xFE0F},   {0xFE20, 0xFE2F},   {0xFF9E, 0xFF9F},   {0x101FD, 0x101FD},
    {0x102E0, 0x102E0}, {0x10376, 0x1037A}, {0x10A01, 0x10A0F}, {0x10A38, 0x10A3F},
    {0x10AE5, 0x10AE6}, {0x10D24, 0x10D27}, {0x10F46, 0x10F50}, {0x11001, 0x11001},
    {0x11038, 0x11046}, {0x1107F, 0x11081}, {0x110B3, 0x110B6}, {0x110B9, 0x110BA},
    {0x11100, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11134}, {0x11173, 0x11173},
    {0x11180, 0x11181}, {0x111B6, 0x111BE}, {0x1D165, 0x1D165}, {0x1D167, 0x1D169},
    {0x1D16E, 0x1D172}, {0x1D17B, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD},
    {0x1D242, 0x1D244}, {0x1E000, 0x1E02A}, {0x1E130, 0x1E136}, {0x1E2EC, 0x1E2EF},
    {0x1E8D0, 0x1E8D6}, {0x1E944, 0x1E94A}, {0x1F3FB, 0x1F3FF}, {0xE0020, 0xE007F},
    {0xE0100, 0xE01EF},
});

// Extended_Pictographic, coarsened to blocks. Used for GB11 (emoji ZWJ
// sequences) and for VS16 emoji presentation.
inline constexpr std::array pictographic_table = std::to_array<code_point_range>({
    {0x00A9, 0x00A9},   {0x00AE, 0x00AE},   {0x203C, 0x203C},   {0x2049, 0x2049},
    {0x2122, 0x2122},   {0x2139, 0x2139},   {0x2194, 0x2199},   {0x21A9, 0x21AA},
    {0x231A, 0x231B},   {0x2328, 0x2328},   {0x2388, 0x2388},   {0x23CF, 0x23CF},
    {0x23E9, 0x23F3},   {0x23F8, 0x23FA},   {0x24C2, 0x24C2},   {0x25AA, 0x25AB},
    {0x25B6, 0x25B6},   {0x25C0, 0x25C0},   {0x25FB, 0x25FE},   {0x2600, 0x27BF},
    {0x2934, 0x2935},   {0x2B05, 0x2B07},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},   {0x3030, 0x3030},   {0x303D, 0x303D},   {0x3297, 0x3297},
    {0x3299, 0x3299},   {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F},
    {0x1F16C, 0x1F171}, {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F1AD, 0x1F1E5}, {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F}, {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D},
    {0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D5, 0x1F7FF},
    {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F},
    {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF},
    {0x1FC00, 0x1FFFD},
});

// East Asian Wide/Fullwidth plus default emoji presentation.
inline constexpr std::array wide_table = std::to_array<code_point_range>({
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
    {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
    {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B},
    {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
});

// Zero-width code points that are not Extend: ZWSP, ZWJ, word joiner and
// invisible operators, BOM/ZWNBSP, and Hangul medial/final jamo which only
// occur after a leading jamo.
inline constexpr std::array zero_width_table = std::to_array<code_point_range>({
    {0x1160, 0x11FF},
    {0x200B, 0x200B},
    {0x200D, 0x200D},
    {0x2060, 0x2064},
    {0xD7B0, 0xD7FF},
    {0xFEFF, 0xFEFF},
});

}  // namespace avionix::detail

export namespace avionix {

inline constexpr char32_t replacement_character = U'�';

struct decoded_code_point {
  char32_t value{};
  std::uint8_t length{};  // bytes consumed, 1-4; 0 only for empty input
  bool valid{};

  friend constexpr bool operator==(const decoded_code_point&,
                                   const decoded_code_point&) noexcept = default;
};

// Decodes one code point starting at `text[0]`. Invalid, overlong,
// surrogate, and truncated sequences decode as U+FFFD consuming the maximal
// invalid prefix (at least one byte), matching the WHATWG "maximal subpart"
// practice so decoding always makes progress.
[[nodiscard]] constexpr decoded_code_point decode_utf8(std::string_view text) noexcept {
  if (text.empty()) {
    return {replacement_character, 0, false};
  }
  const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
  const std::uint8_t lead = byte(0);
  if (lead < 0x80) {
    return {char32_t{lead}, 1, true};
  }

  std::uint8_t length = 0;
  char32_t value = 0;
  std::uint8_t lower = 0x80;
  std::uint8_t upper = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    value = lead & 0x1FU;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    value = lead & 0x0FU;
    if (lead == 0xE0) lower = 0xA0;  // overlong
    if (lead == 0xED) upper = 0x9F;  // surrogates
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    value = lead & 0x07U;
    if (lead == 0xF0) lower = 0x90;  // overlong
    if (lead == 0xF4) upper = 0x8F;  // > U+10FFFF
  } else {
    return {replacement_character, 1, false};
  }

  for (std::uint8_t i = 1; i < length; ++i) {
    if (i >= text.size()) {
      return {replacement_character, i, false};
    }
    const std::uint8_t continuation = byte(i);
    const std::uint8_t lo = i == 1 ? lower : std::uint8_t{0x80};
    const std::uint8_t hi = i == 1 ? upper : std::uint8_t{0xBF};
    if (continuation < lo || continuation > hi) {
      return {replacement_character, i, false};
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  return {value, length, true};
}

// Encodes a code point as UTF-8. Returns the number of bytes written (1-4).
// Surrogates and out-of-range values encode U+FFFD.
constexpr std::size_t encode_utf8(char32_t cp, std::span<char, 4> out) noexcept {
  if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    cp = replacement_character;
  }
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0U | (cp >> 6U));
    out[1] = static_cast<char>(0x80U | (cp & 0x3FU));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0U | (cp >> 12U));
    out[1] = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    out[2] = static_cast<char>(0x80U | (cp & 0x3FU));
    return 3;
  }
  out[0] = static_cast<char>(0xF0U | (cp >> 18U));
  out[1] = static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
  out[2] = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
  out[3] = static_cast<char>(0x80U | (cp & 0x3FU));
  return 4;
}

// C0 controls, DEL, and C1 controls. These must never reach the terminal
// as cell content because they move the cursor or start escape sequences.
[[nodiscard]] constexpr bool is_control(char32_t cp) noexcept {
  return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
}

[[nodiscard]] constexpr bool is_extend(char32_t cp) noexcept {
  return cp >= 0x0300 && detail::in_table(detail::extend_table, cp);
}

[[nodiscard]] constexpr bool is_extended_pictographic(char32_t cp) noexcept {
  return cp >= 0x00A9 && detail::in_table(detail::pictographic_table, cp);
}

[[nodiscard]] constexpr bool is_regional_indicator(char32_t cp) noexcept {
  return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

// Display width of a single code point in terminal cells: 0, 1, or 2.
// Controls report 0; callers that render must filter them separately.
[[nodiscard]] constexpr std::uint8_t code_point_width(char32_t cp) noexcept {
  if (cp < 0x7F) {
    return cp >= 0x20 ? 1 : 0;
  }
  if (is_control(cp)) {
    return 0;
  }
  if (cp < 0x0300) {
    return 1;
  }
  if (is_extend(cp) || detail::in_table(detail::zero_width_table, cp)) {
    return 0;
  }
  if (detail::in_table(detail::wide_table, cp)) {
    return 2;
  }
  if (is_regional_indicator(cp)) {
    return 1;  // a lone regional indicator; pairs are handled per grapheme
  }
  return 1;
}

namespace detail {

enum class break_class : std::uint8_t {
  other,
  cr,
  lf,
  control,
  extend,
  zwj,
  regional_indicator,
  hangul_l,
  hangul_v,
  hangul_t,
  hangul_lv,
  hangul_lvt,
  pictographic,
};

constexpr break_class classify(char32_t cp) noexcept {
  if (cp == U'\r') return break_class::cr;
  if (cp == U'\n') return break_class::lf;
  if (cp == 0x200D) return break_class::zwj;
  if (is_control(cp) || cp == 0x2028 || cp == 0x2029) return break_class::control;
  if (cp < 0x0300)
    return cp == 0x00A9 || cp == 0x00AE ? break_class::pictographic
                                        : break_class::other;
  if (is_extend(cp)) return break_class::extend;
  if (is_regional_indicator(cp)) return break_class::regional_indicator;
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0xA960 && cp <= 0xA97C))
    return break_class::hangul_l;
  if ((cp >= 0x1160 && cp <= 0x11A7) || (cp >= 0xD7B0 && cp <= 0xD7C6))
    return break_class::hangul_v;
  if ((cp >= 0x11A8 && cp <= 0x11FF) || (cp >= 0xD7CB && cp <= 0xD7FB))
    return break_class::hangul_t;
  if (cp >= 0xAC00 && cp <= 0xD7A3)
    return (cp - 0xAC00) % 28 == 0 ? break_class::hangul_lv : break_class::hangul_lvt;
  if (is_extended_pictographic(cp)) return break_class::pictographic;
  return break_class::other;
}

}  // namespace detail

// Returns the byte offset one past the end of the grapheme cluster that
// starts at `offset`. Returns text.size() at the end of input.
[[nodiscard]] constexpr std::size_t next_grapheme_boundary(
    std::string_view text, std::size_t offset) noexcept {
  using detail::break_class;
  if (offset >= text.size()) {
    return text.size();
  }

  // Printable ASCII followed by ASCII (or the end) is always a single
  // cluster. This covers most terminal text without table lookups.
  const auto lead = static_cast<std::uint8_t>(text[offset]);
  if (lead >= 0x20 && lead < 0x7F &&
      (offset + 1 == text.size() ||
       static_cast<std::uint8_t>(text[offset + 1]) < 0x80)) {
    return offset + 1;
  }

  auto first = decode_utf8(text.substr(offset));
  std::size_t end = offset + first.length;
  break_class previous = detail::classify(first.value);

  if (previous == break_class::control || previous == break_class::lf) {
    return end;  // GB4
  }
  if (previous == break_class::cr) {
    if (end < text.size() && text[end] == '\n') {
      return end + 1;  // GB3
    }
    return end;  // GB4
  }

  bool pictographic_run = previous == break_class::pictographic;  // for GB11
  int regional_count = previous == break_class::regional_indicator ? 1 : 0;

  while (end < text.size()) {
    const auto next = decode_utf8(text.substr(end));
    const break_class current = detail::classify(next.value);

    bool join = false;
    switch (current) {
      case break_class::control:
      case break_class::cr:
      case break_class::lf:
        join = false;  // GB5
        break;
      case break_class::extend:
      case break_class::zwj:
        join = true;  // GB9
        break;
      default:
        if (previous == break_class::hangul_l &&
            (current == break_class::hangul_l || current == break_class::hangul_v ||
             current == break_class::hangul_lv || current == break_class::hangul_lvt)) {
          join = true;  // GB6
        } else if ((previous == break_class::hangul_lv ||
                    previous == break_class::hangul_v) &&
                   (current == break_class::hangul_v ||
                    current == break_class::hangul_t)) {
          join = true;  // GB7
        } else if ((previous == break_class::hangul_lvt ||
                    previous == break_class::hangul_t) &&
                   current == break_class::hangul_t) {
          join = true;  // GB8
        } else if (previous == break_class::zwj && pictographic_run &&
                   current == break_class::pictographic) {
          join = true;  // GB11
        } else if (previous == break_class::regional_indicator &&
                   current == break_class::regional_indicator &&
                   regional_count % 2 == 1) {
          join = true;  // GB12/GB13
        }
        break;
    }
    if (!join) {
      break;
    }

    if (current == break_class::pictographic) {
      pictographic_run = true;
    } else if (current != break_class::extend && current != break_class::zwj) {
      pictographic_run = false;
    }
    if (current == break_class::regional_indicator) {
      ++regional_count;
    }
    previous = current;
    end += next.length;
  }
  return end;
}

// Display width of one grapheme cluster in cells: 0, 1, or 2.
//
// Width comes from the first code point, with two adjustments: a regional
// indicator pair (a flag) is two cells, and VS16 (U+FE0F) requests emoji
// presentation, which terminals render two cells wide.
[[nodiscard]] constexpr std::uint8_t grapheme_width(std::string_view cluster) noexcept {
  if (cluster.empty()) {
    return 0;
  }
  const auto first = decode_utf8(cluster);
  if (first.length == cluster.size()) {
    return code_point_width(first.value);
  }
  if (is_regional_indicator(first.value)) {
    const auto second = decode_utf8(cluster.substr(first.length));
    if (is_regional_indicator(second.value)) {
      return 2;
    }
  }
  std::uint8_t width = code_point_width(first.value);
  if (width == 1 && is_extended_pictographic(first.value)) {
    for (std::size_t i = first.length; i < cluster.size();) {
      const auto cp = decode_utf8(cluster.substr(i));
      if (cp.value == 0xFE0F) {
        width = 2;
        break;
      }
      i += cp.length;
    }
  }
  return width;
}

struct grapheme {
  std::string_view text;
  std::uint8_t width{};

  friend constexpr bool operator==(const grapheme&, const grapheme&) noexcept = default;
};

// Forward range of grapheme clusters over borrowed UTF-8 text. The view and
// its elements refer into the original string and must not outlive it.
class grapheme_view {
 public:
  class iterator {
   public:
    using value_type = grapheme;
    using difference_type = std::ptrdiff_t;

    constexpr iterator() noexcept = default;
    constexpr iterator(std::string_view text, std::size_t offset) noexcept
        : text_{text}, offset_{offset}, next_{next_grapheme_boundary(text, offset)} {}

    [[nodiscard]] constexpr grapheme operator*() const noexcept {
      const auto cluster = text_.substr(offset_, next_ - offset_);
      return {cluster, grapheme_width(cluster)};
    }

    constexpr iterator& operator++() noexcept {
      offset_ = next_;
      next_ = next_grapheme_boundary(text_, offset_);
      return *this;
    }

    constexpr iterator operator++(int) noexcept {
      iterator copy = *this;
      ++*this;
      return copy;
    }

    [[nodiscard]] constexpr std::size_t offset() const noexcept { return offset_; }

    friend constexpr bool operator==(const iterator& a, const iterator& b) noexcept {
      return a.offset_ == b.offset_;
    }

   private:
    std::string_view text_{};
    std::size_t offset_{};
    std::size_t next_{};
  };

  constexpr explicit grapheme_view(std::string_view text) noexcept : text_{text} {}

  [[nodiscard]] constexpr iterator begin() const noexcept { return {text_, 0}; }
  [[nodiscard]] constexpr iterator end() const noexcept {
    return {text_, text_.size()};
  }

 private:
  std::string_view text_;
};

[[nodiscard]] constexpr grapheme_view graphemes(std::string_view text) noexcept {
  return grapheme_view{text};
}

// Total display width of UTF-8 text in cells. Controls contribute 0. Never
// use std::string::size() for this.
[[nodiscard]] constexpr std::size_t display_width(std::string_view text) noexcept {
  std::size_t width = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto lead = static_cast<std::uint8_t>(text[offset]);
    if (lead < 0x80 && (offset + 1 == text.size() ||
                        static_cast<std::uint8_t>(text[offset + 1]) < 0x80)) {
      width += lead >= 0x20 && lead < 0x7F ? 1 : 0;
      ++offset;
      continue;
    }
    const std::size_t next = next_grapheme_boundary(text, offset);
    width += grapheme_width(text.substr(offset, next - offset));
    offset = next;
  }
  return width;
}

// Longest prefix of `text` that fits in `max_width` cells without splitting
// a grapheme cluster. Returns the byte length of that prefix.
[[nodiscard]] constexpr std::size_t prefix_fitting_width(
    std::string_view text, std::size_t max_width) noexcept {
  std::size_t width = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t next = next_grapheme_boundary(text, offset);
    const std::size_t w = grapheme_width(text.substr(offset, next - offset));
    if (width + w > max_width) {
      break;
    }
    width += w;
    offset = next;
  }
  return offset;
}

}  // namespace avionix
