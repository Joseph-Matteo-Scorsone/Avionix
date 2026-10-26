module;
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

export module avionix.entity.latex;

namespace {
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct math_parser {
  std::string_view source;
  std::size_t at{};
  bool valid{true};

  void spaces() {
    while (at < source.size() && (source[at] == ' ' || source[at] == '\n' ||
                                  source[at] == '\r' || source[at] == '\t'))
      ++at;
  }
  std::string argument(unsigned depth) {
    spaces();
    if (at == source.size()) {
      valid = false;
      return {};
    }
    if (source[at] == '{') {
      ++at;
      return sequence(depth + 1, true);
    }
    return atom(depth + 1);
  }
  std::string atom(unsigned depth) {
    if (depth > 24) {
      valid = false;
      return {};
    }
    const char c = source[at++];
    if (c == '{') return sequence(depth + 1, true);
    if (c == '}') {
      valid = false;
      return {};
    }
    if (c != '\\') {
      // Copy one UTF-8 code point, leaving validation to the Unicode renderer.
      const auto start = at - 1;
      while (at < source.size() &&
             (static_cast<unsigned char>(source[at]) & 0xc0U) == 0x80U)
        ++at;
      return std::string{source.substr(start, at - start)};
    }
    const auto start = at;
    while (at < source.size() && letter(source[at])) ++at;
    auto command = source.substr(start, at - start);
    if (command.empty()) {
      if (at == source.size()) {
        valid = false;
        return {};
      }
      const char escaped = source[at++];
      if (escaped == ',' || escaped == ';' || escaped == ':' || escaped == ' ')
        return " ";
      if (escaped == '!') return {};
      if (std::string_view{"{}%$#_&|"}.find(escaped) != std::string_view::npos)
        return std::string(1, escaped);
      valid = false;
      return {};
    }
    if (command == "frac" || command == "dfrac" || command == "tfrac") {
      auto numerator = argument(depth);
      auto denominator = argument(depth);
      return "(" + numerator + ")/(" + denominator + ")";
    }
    if (command == "sqrt") {
      spaces();
      if (at < source.size() && source[at] == '[') {
        valid = false;
        return {};
      }
      return "√(" + argument(depth) + ")";
    }
    if (command == "text" || command == "mathrm" || command == "mathbf" ||
        command == "mathit" || command == "operatorname") {
      spaces();
      if (at == source.size() || source[at] != '{') {
        valid = false;
        return {};
      }
      if (command != "text") return argument(depth);
      const auto begin = ++at;
      while (at < source.size() && source[at] != '}' && source[at] != '{' &&
             source[at] != '\\')
        ++at;
      if (at == source.size() || source[at] != '}') {
        valid = false;
        return {};
      }
      auto result = std::string{source.substr(begin, at - begin)};
      ++at;
      return result;
    }
    if (command == "left" || command == "right") {
      spaces();
      if (at < source.size() && source[at] == '.') ++at;
      return {};
    }
    static constexpr std::pair<std::string_view, std::string_view> symbols[]{
        {"alpha", "α"},
        {"beta", "β"},
        {"gamma", "γ"},
        {"delta", "δ"},
        {"epsilon", "ϵ"},
        {"varepsilon", "ε"},
        {"zeta", "ζ"},
        {"eta", "η"},
        {"theta", "θ"},
        {"vartheta", "ϑ"},
        {"iota", "ι"},
        {"kappa", "κ"},
        {"lambda", "λ"},
        {"mu", "μ"},
        {"nu", "ν"},
        {"xi", "ξ"},
        {"pi", "π"},
        {"rho", "ρ"},
        {"sigma", "σ"},
        {"tau", "τ"},
        {"upsilon", "υ"},
        {"phi", "ϕ"},
        {"varphi", "φ"},
        {"chi", "χ"},
        {"psi", "ψ"},
        {"omega", "ω"},
        {"Gamma", "Γ"},
        {"Delta", "Δ"},
        {"Theta", "Θ"},
        {"Lambda", "Λ"},
        {"Xi", "Ξ"},
        {"Pi", "Π"},
        {"Sigma", "Σ"},
        {"Phi", "Φ"},
        {"Psi", "Ψ"},
        {"Omega", "Ω"},
        {"times", "×"},
        {"cdot", "·"},
        {"div", "÷"},
        {"pm", "±"},
        {"mp", "∓"},
        {"le", "≤"},
        {"leq", "≤"},
        {"ge", "≥"},
        {"geq", "≥"},
        {"ne", "≠"},
        {"neq", "≠"},
        {"approx", "≈"},
        {"equiv", "≡"},
        {"infty", "∞"},
        {"sum", "∑"},
        {"prod", "∏"},
        {"int", "∫"},
        {"partial", "∂"},
        {"nabla", "∇"},
        {"in", "∈"},
        {"notin", "∉"},
        {"subset", "⊂"},
        {"subseteq", "⊆"},
        {"cup", "∪"},
        {"cap", "∩"},
        {"forall", "∀"},
        {"exists", "∃"},
        {"emptyset", "∅"},
        {"to", "→"},
        {"rightarrow", "→"},
        {"leftarrow", "←"},
        {"Rightarrow", "⇒"},
        {"leftrightarrow", "↔"},
        {"Leftrightarrow", "⇔"},
        {"ldots", "…"},
        {"cdots", "⋯"},
        {"langle", "⟨"},
        {"rangle", "⟩"},
        {"sin", "sin"},
        {"cos", "cos"},
        {"tan", "tan"},
        {"log", "log"},
        {"ln", "ln"},
        {"exp", "exp"},
        {"lim", "lim"},
        {"min", "min"},
        {"max", "max"},
        {"quad", "  "},
        {"qquad", "    "}};
    for (auto [name, value] : symbols)
      if (command == name) return std::string{value};
    valid = false;
    return {};
  }
  std::string sequence(unsigned depth, bool grouped = false) {
    if (depth > 24) {
      valid = false;
      return {};
    }
    std::string out;
    while (valid && at < source.size()) {
      if (source[at] == '}') {
        if (!grouped) {
          valid = false;
          return {};
        }
        ++at;
        return out;
      }
      if (source[at] == '^' || source[at] == '_') {
        const bool superscript = source[at++] == '^';
        const auto value = argument(depth);
        constexpr std::string_view keys = "0123456789+-=()in";
        static constexpr std::string_view upper[]{"⁰", "¹", "²", "³", "⁴", "⁵",
                                                  "⁶", "⁷", "⁸", "⁹", "⁺", "⁻",
                                                  "⁼", "⁽", "⁾", "ⁱ", "ⁿ"};
        static constexpr std::string_view lower[]{"₀", "₁", "₂", "₃", "₄", "₅",
                                                  "₆", "₇", "₈", "₉", "₊", "₋",
                                                  "₌", "₍", "₎", "ᵢ", "ₙ"};
        std::string script;
        for (char ch : value) {
          const auto index = keys.find(ch);
          if (index == std::string_view::npos) {
            script.clear();
            break;
          }
          script += superscript ? upper[index] : lower[index];
        }
        if (script.empty())
          out += std::string{superscript ? "^(" : "_("} + value + ")";
        else
          out += script;
      } else if (source[at] == '\n' || source[at] == '\r' || source[at] == '\t') {
        ++at;
        out += ' ';
      } else
        out += atom(depth);
    }
    if (grouped) valid = false;
    return out;
  }
};
}  // namespace

export namespace avionix {
// A bounded terminal math subset. Unknown commands or malformed groups return
// nullopt so callers can preserve the complete source. No TeX execution or I/O.
[[nodiscard]] inline std::optional<std::string> format_latex(std::string_view source) {
  if (source.size() > 65536) return std::nullopt;
  math_parser parser{source};
  auto result = parser.sequence(0);
  if (!parser.valid) return std::nullopt;
  return result;
}
}  // namespace avionix
