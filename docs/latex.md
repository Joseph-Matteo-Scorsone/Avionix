# LaTeX math in terminal text

`markdown_view` detects math automatically when its source changes. No terminal
protocol, clipboard operation, external TeX installation, or option is required.
The original source remains available through `source()`.

Supported delimiters:

- `$...$` and `\(...\)` for inline math.
- `$$...$$` and `\[...\]` for display math. A display block may span lines;
  its opening delimiter must begin the trimmed line and its closing delimiter
  must end the trimmed line. Display blocks have spacing above and below them.

For example:

```cpp
avionix::markdown_view view{R"(
Energy is $E = mc^2$.

\[
\frac{-b \pm \sqrt{b^2 - 4ac}}{2a}
\]
)"};
```

The first formula displays as `E = mc²`. Fractions display as `(numerator)/(denominator)`
and roots as `√(expression)`. Scripts use Unicode superscript/subscript characters
where every character is available, otherwise `^(expression)` or `_(expression)`.
This is linear terminal formatting with normal wrapping, not graphical typesetting.
Font coverage and glyph appearance depend on the terminal.

The formatter supports Greek letters, common relations and arrows, sum/product/integral
symbols, elementary function names, `\frac`, `\dfrac`, `\tfrac`, `\sqrt`, grouping,
`\left`, `\right`, spacing commands, `\text`, `\mathrm`, `\mathbf`, `\mathit`, and
`\operatorname`. Font commands preserve their argument as plain text. `\text` accepts
a literal braced argument without nested groups or commands. Limits on sums and
integrals use the same inline script formatting as other expressions.

Inline code and fenced code stay literal, including fences labeled `latex`.
Escape a dollar sign with `\$`. Single-dollar math requires non-whitespace text
next to both delimiters and cannot end immediately before a digit. This leaves
common prices such as `$5 and $10` unchanged. Dollar delimiters are inherently
ambiguous; use `\(...\)` when prose contains currency. Bare commands outside
delimiters are ordinary Markdown text.

Unknown commands and malformed groups preserve the delimited formula rather than
partially converting it. Unsupported features include matrices, alignment
environments, macros, packages, indexed roots, accents, and full LaTeX documents.
Unclosed delimiters remain ordinary Markdown input, subject to normal Markdown
escaping and emphasis. Multiline display blocks are recognized at document level;
nested display blocks in quotes or lists are not parsed as separate blocks.

`format_latex(expression)` is also exported through `import avionix`. It returns
an owned `std::optional<std::string>`, with `nullopt` for unsupported or malformed
input. Pass the expression without delimiters. It limits input to 64 KiB and group
or argument recursion to 24 levels. It performs no I/O and executes no TeX commands.

Parsed inline formulas carry `markdown_mark::math`. Display blocks and rows use
`markdown_block::kind::math` and `markdown_role::math`. The widget colors math cyan.
Selection and copying use the formatted visible text. Editing controls continue
to preserve typed and pasted source without rewriting it.

Run `zig build test -- markdown` for parser, layout, fallback, and widget tests.
Run `zig build run-markdown` to inspect the example in your terminal.
