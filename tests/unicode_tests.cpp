// UTF-8 decoding, grapheme segmentation, and display width.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

import avionix.entity.unicode;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

static_assert(display_width("hello") == 5);
static_assert(decode_utf8("\xE2\x82\xAC").value == U'€');

std::vector<std::string_view> clusters(std::string_view text) {
  std::vector<std::string_view> out;
  for (const auto g : graphemes(text)) out.push_back(g.text);
  return out;
}

const avionix_tests::suite utf8{
    "utf8",
    {
        {"ascii",
         [] {
           const auto d = decode_utf8("A");
           check(d.valid);
           check_equal(d.length, std::uint8_t{1});
           check(d.value == U'A');
         }},
        {"multibyte",
         [] {
           check(decode_utf8("\xC3\xA9").value == U'é');
           check(decode_utf8("\xE4\xB8\xAD").value == U'中');
           const auto emoji = decode_utf8("\xF0\x9F\x98\x80");
           check(emoji.value == U'\U0001F600');
           check_equal(emoji.length, std::uint8_t{4});
         }},
        {"invalid_lead_byte",
         [] {
           const auto d = decode_utf8("\xFF");
           check(!d.valid);
           check(d.value == replacement_character);
           check_equal(d.length, std::uint8_t{1});
         }},
        {"overlong_rejected",
         [] {
           check(!decode_utf8("\xC0\xAF").valid);
           check(!decode_utf8("\xE0\x80\xAF").valid);
         }},
        {"surrogates_rejected", [] { check(!decode_utf8("\xED\xA0\x80").valid); }},
        {"above_max_rejected", [] { check(!decode_utf8("\xF4\x90\x80\x80").valid); }},
        {"truncated_consumes_prefix",
         [] {
           const auto d = decode_utf8("\xE4\xB8");
           check(!d.valid);
           check_equal(d.length, std::uint8_t{2});
         }},
        {"bad_continuation_stops_early",
         [] {
           // Lead byte then ASCII: consume only the lead so 'A' decodes next.
           const auto d = decode_utf8(
               "\xE4"
               "A");
           check(!d.valid);
           check_equal(d.length, std::uint8_t{1});
         }},
        {"encode_round_trip",
         [] {
           for (const char32_t cp : {U'A', U'é', U'中', U'\U0001F600', U'\U0010FFFF'}) {
             std::array<char, 4> bytes{};
             const std::size_t n = encode_utf8(cp, bytes);
             const auto d = decode_utf8({bytes.data(), n});
             check(d.valid && d.value == cp && d.length == n);
           }
         }},
        {"encode_surrogate_is_replacement",
         [] {
           std::array<char, 4> bytes{};
           const std::size_t n = encode_utf8(0xD800, bytes);
           check(decode_utf8({bytes.data(), n}).value == replacement_character);
         }},
    }};

const avionix_tests::suite segmentation{
    "grapheme",
    {
        {"ascii_is_one_per_byte",
         [] { check_equal(clusters("abc").size(), std::size_t{3}); }},
        {"crlf_is_one_cluster",
         [] {
           const auto c = clusters("a\r\nb");
           check_equal(c.size(), std::size_t{3});
           check(c[1] == "\r\n");
         }},
        {"combining_mark_joins",
         [] {
           // e + COMBINING ACUTE ACCENT
           const auto c = clusters("e\xCC\x81x");
           check_equal(c.size(), std::size_t{2});
           check(c[0] == "e\xCC\x81");
         }},
        {"emoji_zwj_sequence",
         [] {
           // MAN ZWJ WOMAN ZWJ GIRL
           const std::string family =
               "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91"
               "\xA7";
           const auto c = clusters(family);
           check_equal(c.size(), std::size_t{1});
         }},
        {"emoji_modifier",
         [] {
           // WAVING HAND + skin tone modifier
           const auto c = clusters("\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD");
           check_equal(c.size(), std::size_t{1});
         }},
        {"regional_indicators_pair",
         [] {
           // Two flags: US, DE
           const auto c = clusters(
               "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA");
           check_equal(c.size(), std::size_t{2});
         }},
        {"hangul_jamo_sequence",
         [] {
           // L V T jamo form one syllable cluster.
           const auto c = clusters("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8");
           check_equal(c.size(), std::size_t{1});
         }},
        {"control_is_its_own_cluster",
         [] {
           check_equal(clusters("a\x01"
                                "b")
                           .size(),
                       std::size_t{3});
         }},
        {"invalid_bytes_make_progress",
         [] { check_equal(clusters("\xFF\xFE").size(), std::size_t{2}); }},
    }};

const avionix_tests::suite width{
    "width",
    {
        {"ascii", [] { check_equal(display_width("hello"), std::size_t{5}); }},
        {"cjk_is_wide",
         [] {
           check_equal(display_width("\xE4\xB8\xAD\xE6\x96\x87"), std::size_t{4});
         }},
        {"combining_is_zero",
         [] { check_equal(display_width("e\xCC\x81"), std::size_t{1}); }},
        {"emoji_is_wide",
         [] { check_equal(display_width("\xF0\x9F\x98\x80"), std::size_t{2}); }},
        {"flag_is_wide",
         [] {
           check_equal(display_width("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"),
                       std::size_t{2});
         }},
        {"zwj_family_is_two",
         [] {
           check_equal(display_width("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"),
                       std::size_t{2});
         }},
        {"vs16_widens_text_symbol",
         [] {
           // HEAVY BLACK HEART (text default) + VS16
           check_equal(display_width("\xE2\x9D\xA4"), std::size_t{1});
           check_equal(display_width("\xE2\x9D\xA4\xEF\xB8\x8F"), std::size_t{2});
         }},
        {"controls_are_zero",
         [] { check_equal(display_width("a\tb\x1b"), std::size_t{2}); }},
        {"byte_length_is_not_width",
         [] {
           const std::string s = "\xC3\xA9t\xC3\xA9";  // "été"
           check_equal(s.size(), std::size_t{5});
           check_equal(display_width(s), std::size_t{3});
         }},
        {"fullwidth_forms",
         [] { check_equal(display_width("\xEF\xBC\xA1"), std::size_t{2}); }},
        {"prefix_fitting_does_not_split_wide",
         [] {
           const std::string_view s =
               "a\xE4\xB8\xAD"
               "b";  // a中b
           check_equal(prefix_fitting_width(s, 2), std::size_t{1});
           check_equal(prefix_fitting_width(s, 3), std::size_t{4});
         }},
    }};

}  // namespace
