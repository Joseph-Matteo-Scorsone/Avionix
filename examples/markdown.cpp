// A Markdown document. Drag to highlight text. Drag past the top or bottom
// and the document scrolls with the highlight. Releasing copies it.
// Click a link to record the URL. Press q to quit.

#include <print>
#include <string>

import avionix;

int main() {
  using namespace avionix;
  application app;

  column root;
  auto& frame = root.add(constraint::fill(), block{"Notes"});
  auto& doc = frame.set_child(markdown_view{R"(# Notes

Drag across the text to highlight it. Drag past the bottom and the
document scrolls. Releasing the mouse copies the highlight.

Open the [Avionix repository](https://github.com/Joseph-Matteo-Scorsone/Avionix).

# Read `import avionix;`

- headings, lists, and code
- [x] links you can click
)"});

  auto& status =
      root.add(constraint::fixed(1), text{" Click a link, or press q to quit."});
  doc.on_link([&](const std::string& url) { status.set(" Link: " + url); });

  app.on_event([&](const event& e) {
    if (const auto* key = as<key_event>(e); key && key->is(U'q')) {
      app.quit();
      return true;
    }
    return false;
  });

  if (auto result = app.run(root); !result) {
    std::println(stderr, "markdown: {}", result.error().message());
    return 1;
  }
  return 0;
}
