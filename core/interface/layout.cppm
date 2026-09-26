module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

export module avionix.interface.layout;

export import avionix.entity.constraint;
import avionix.entity.geometry;
import avionix.entity.event;
import avionix.interface.widget;

// Layout containers. Each frame a container splits its area with
// avionix.entity.constraint and renders every child into its slot:
//
//   container area ──split(constraints)──► slot rects ──► child render_in
//
// Containers own their children (std::unique_ptr<component>) unless a child
// was added with add_borrowed().

export namespace avionix {

class stack : public component {
 public:
  explicit stack(direction axis, std::uint32_t spacing = 0)
      : axis_{axis}, spacing_{spacing} {}

  // Adds a child that owns `value`. Returns a reference to the stored
  // value; it stays valid while this container lives.
  template <typename T>
    requires std::derived_from<std::remove_cvref_t<T>, component> ||
             widget<std::remove_cvref_t<T>>
  std::remove_cvref_t<T>& add(constraint size, T&& value) {
    auto made = make_component(std::forward<T>(value));
    auto& ref = made.value;
    insert(size, std::move(made.owner));
    return ref;
  }

  component& add(constraint size, std::unique_ptr<component> child) {
    component& ref = *child;
    insert(size, std::move(child));
    return ref;
  }

  // Adds a child owned elsewhere. `value` must outlive this container.
  template <typename T>
  T& add_borrowed(constraint size, T& value) {
    insert(size, std::make_unique<borrowed<T>>(value));
    return value;
  }

  [[nodiscard]] std::size_t size() const noexcept { return children_.size(); }

  void set_spacing(std::uint32_t spacing) noexcept { spacing_ = spacing; }
  void set_constraint(std::size_t index, constraint size) {
    constraints_.at(index) = size;
  }

  void render(render_context& context) override {
    slots_.resize(children_.size());
    split(bounds(context.extent()), axis_, constraints_, slots_, spacing_);
    for (std::size_t i = 0; i < children_.size(); ++i) {
      if (slots_[i].empty()) continue;
      render_context sub = context.child(slots_[i]);
      children_[i]->render_in(sub);
    }
  }

  void children(std::vector<component*>& out) override {
    for (auto& child : children_) out.push_back(child.get());
  }

 private:
  void insert(constraint size, std::unique_ptr<component> child) {
    constraints_.push_back(size);
    children_.push_back(std::move(child));
  }

  direction axis_;
  std::uint32_t spacing_;
  std::vector<constraint> constraints_{};
  std::vector<std::unique_ptr<component>> children_{};
  std::vector<rect> slots_{};  // reused every frame
};

// Children side by side, left to right.
class row final : public stack {
 public:
  explicit row(std::uint32_t spacing = 0) : stack{direction::horizontal, spacing} {}
};

// Children stacked top to bottom.
class column final : public stack {
 public:
  explicit column(std::uint32_t spacing = 0) : stack{direction::vertical, spacing} {}
};

// Insets a single child by fixed margins.
class padding final : public component {
 public:
  template <typename T>
    requires std::derived_from<std::remove_cvref_t<T>, component> ||
                 widget<std::remove_cvref_t<T>>
  padding(std::uint32_t horizontal, std::uint32_t vertical, T&& child)
      : horizontal_{horizontal},
        vertical_{vertical},
        child_{make_component(std::forward<T>(child)).owner} {}

  void render(render_context& context) override {
    const rect inner =
        bounds(context.extent()).inset(horizontal_, vertical_, horizontal_, vertical_);
    if (inner.empty()) return;
    render_context sub = context.child(inner);
    child_->render_in(sub);
  }

  void children(std::vector<component*>& out) override { out.push_back(child_.get()); }

 private:
  std::uint32_t horizontal_;
  std::uint32_t vertical_;
  std::unique_ptr<component> child_;
};

}  // namespace avionix
