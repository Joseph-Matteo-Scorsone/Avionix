// Input decoding: keys, modifiers, mouse, paste, focus, split reads, and
// malformed sequences.

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.event;
import avionix.object.input_decoder;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

std::vector<event> decode_all(std::string_view bytes) {
  input_decoder d;
  std::vector<event> out;
  d.feed(bytes, out);
  d.flush(out);
  return out;
}

key_event only_key(std::string_view bytes) {
  const auto events = decode_all(bytes);
  if (events.size() != 1 || !std::holds_alternative<key_event>(events[0])) {
    return key_event{key::f12, 0xFFFF,
                     modifiers::super};  // sentinel that no test expects
  }
  return std::get<key_event>(events[0]);
}

const avionix_tests::suite keys{
    "input_keys",
    {
        {"printable_ascii", [] { check(only_key("a").is(U'a')); }},
        {"utf8_character", [] { check(only_key("\xC3\xA9").is(U'é')); }},
        {"enter_tab_backspace",
         [] {
           check(only_key("\r").is(key::enter));
           check(only_key("\t").is(key::tab));
           check(only_key("\x7f").is(key::backspace));
         }},
        {"ctrl_letters",
         [] {
           check(only_key("\x01").is(U'a', modifiers::ctrl));
           check(only_key("\x03").is(U'c', modifiers::ctrl));
           check(only_key("\x13").is(U's', modifiers::ctrl));
           check(only_key("\x1a").is(U'z', modifiers::ctrl));
         }},
        {"arrows_csi_and_ss3",
         [] {
           check(only_key("\x1b[A").is(key::up));
           check(only_key("\x1b[B").is(key::down));
           check(only_key("\x1bOC").is(key::right));
           check(only_key("\x1bOD").is(key::left));
         }},
        {"modified_arrow",
         [] {
           check(only_key("\x1b[1;5A").is(key::up, modifiers::ctrl));
           check(only_key("\x1b[1;2D").is(key::left, modifiers::shift));
           check(only_key("\x1b[1;3C").is(key::right, modifiers::alt));
         }},
        {"tilde_keys",
         [] {
           check(only_key("\x1b[3~").is(key::del));
           check(only_key("\x1b[5~").is(key::page_up));
           check(only_key("\x1b[6;5~").is(key::page_down, modifiers::ctrl));
           check(only_key("\x1b[15~").is(key::f5));
           check(only_key("\x1b[24~").is(key::f12));
           check(only_key("\x1b[1~").is(key::home));
           check(only_key("\x1b[4~").is(key::end));
         }},
        {"function_keys_ss3", [] { check(only_key("\x1bOP").is(key::f1)); }},
        {"shift_tab", [] { check(only_key("\x1b[Z").is(key::tab, modifiers::shift)); }},
        {"alt_prefix", [] { check(only_key("\x1bx").is(U'x', modifiers::alt)); }},
        {"csi_u",
         [] {
           check(only_key("\x1b[97;5u").is(U'a', modifiers::ctrl));
           check(only_key("\x1b[13;2u").is(key::enter, modifiers::shift));
         }},
        {"lone_escape_needs_flush",
         [] {
           input_decoder d;
           std::vector<event> out;
           d.feed("\x1b", out);
           check(out.empty());
           check(d.has_pending());
           d.flush(out);
           check_equal(out.size(), std::size_t{1});
           check(std::get<key_event>(out[0]).is(key::escape));
         }},
        {"double_escape",
         [] {
           const auto events = decode_all("\x1b\x1b");
           check_equal(events.size(), std::size_t{2});
         }},
        {"sequence_split_across_reads",
         [] {
           input_decoder d;
           std::vector<event> out;
           d.feed("\x1b[", out);
           d.feed("1;5", out);
           check(out.empty());
           d.feed("B", out);
           check_equal(out.size(), std::size_t{1});
           check(std::get<key_event>(out[0]).is(key::down, modifiers::ctrl));
         }},
        {"utf8_split_across_reads",
         [] {
           input_decoder d;
           std::vector<event> out;
           d.feed("\xE4\xB8", out);
           check(out.empty());
           d.feed("\xAD", out);
           check_equal(out.size(), std::size_t{1});
           check(std::get<key_event>(out[0]).is(U'中'));
         }},
        {"many_keys_in_one_read",
         [] {
           const auto events = decode_all("ab\x1b[Ac");
           check_equal(events.size(), std::size_t{4});
         }},
    }};

const avionix_tests::suite mouse{
    "input_mouse",
    {
        {"sgr_press_and_release",
         [] {
           const auto events = decode_all("\x1b[<0;10;5M\x1b[<0;10;5m");
           check_equal(events.size(), std::size_t{2});
           const auto& press = std::get<mouse_event>(events[0]);
           check(press.button == mouse_button::left);
           check(press.action == mouse_action::press);
           check(press.where == position{9, 4});
           check(std::get<mouse_event>(events[1]).action == mouse_action::release);
         }},
        {"wheel",
         [] {
           const auto events =
               decode_all("\x1b[<64;1;1M\x1b[<65;1;1M\x1b[<66;1;1M\x1b[<67;1;1M");
           check(std::get<mouse_event>(events[0]).button == mouse_button::wheel_up);
           check(std::get<mouse_event>(events[1]).button == mouse_button::wheel_down);
           check(std::get<mouse_event>(events[2]).button == mouse_button::wheel_left);
           check(std::get<mouse_event>(events[3]).button == mouse_button::wheel_right);
         }},
        {"drag_and_move",
         [] {
           const auto drag = std::get<mouse_event>(decode_all("\x1b[<32;2;2M")[0]);
           check(drag.action == mouse_action::drag);
           const auto move = std::get<mouse_event>(decode_all("\x1b[<35;2;2M")[0]);
           check(move.action == mouse_action::move);
         }},
        {"modifiers",
         [] {
           const auto m = std::get<mouse_event>(decode_all("\x1b[<16;1;1M")[0]);
           check(has(m.mods, modifiers::ctrl));
         }},
    }};

const avionix_tests::suite other{
    "input_other",
    {
        {"bracketed_paste",
         [] {
           const auto events = decode_all("\x1b[200~hello\x1b[Aworld\x1b[201~x");
           check_equal(events.size(), std::size_t{2});
           check(std::get<paste_event>(events[0]).text == "hello\x1b[Aworld");
           check(std::get<key_event>(events[1]).is(U'x'));
         }},
        {"paste_split_across_reads",
         [] {
           input_decoder d;
           std::vector<event> out;
           d.feed("\x1b[200~abc", out);
           d.feed("def\x1b[20", out);
           check(out.empty());
           check(d.in_paste());
           d.feed("1~", out);
           check_equal(out.size(), std::size_t{1});
           check(std::get<paste_event>(out[0]).text == "abcdef");
         }},
        {"paste_at_size_limit_is_delivered",
         [] {
           input_decoder d{5};
           std::vector<event> out;
           d.feed("\x1b[200~abcde\x1b[201~", out);
           check_equal(out.size(), std::size_t{1});
           check(std::get<paste_event>(out[0]).text == "abcde");
           check_equal(d.malformed_count(), std::uint64_t{0});
         }},
        {"oversized_paste_is_discarded_and_decoder_resynchronizes",
         [] {
           input_decoder d{5};
           std::vector<event> out;
           d.feed("\x1b[200~abcd", out);
           d.feed("efghijk", out);
           check(out.empty());
           check(d.in_paste());
           check_equal(d.malformed_count(), std::uint64_t{1});
           d.feed("gh\x1b[201~x", out);
           check_equal(out.size(), std::size_t{1});
           check(std::get<key_event>(out[0]).is(U'x'));
           check(!d.in_paste());
           check_equal(d.malformed_count(), std::uint64_t{1});
         }},
        {"focus_events",
         [] {
           const auto events = decode_all("\x1b[I\x1b[O");
           check(std::get<focus_event>(events[0]).gained);
           check(!std::get<focus_event>(events[1]).gained);
         }},
        {"malformed_sequence_is_skipped",
         [] {
           input_decoder d;
           std::vector<event> out;
           d.feed(
               "\x1b[1;\x01"
               "a",
               out);
           check(d.malformed_count() >= 1);
           // Decoding resumes after the bad bytes.
           check(!out.empty());
           check(std::holds_alternative<key_event>(out.back()));
         }},
        {"decode_one_reports_errors",
         [] {
           const auto incomplete = decode_one("\x1b[12");
           check(!incomplete && incomplete.error().kind == decode_failure::incomplete);
           const auto malformed = decode_one("\xFF");
           check(!malformed && malformed.error().kind == decode_failure::malformed);
           check(!malformed && malformed.error().consumed == 1);
         }},
        {"unknown_sequence_is_ignored",
         [] {
           const auto events = decode_all("\x1b[?1;2cz");
           check_equal(events.size(), std::size_t{1});
           check(std::get<key_event>(events[0]).is(U'z'));
         }},
        {"overlong_sequence_is_malformed",
         [] {
           std::string s = "\x1b[";
           s.append(100, '1');
           s += "A";
           input_decoder d;
           std::vector<event> out;
           d.feed(s, out);
           check(d.malformed_count() >= 1);
         }},
    }};

}  // namespace
