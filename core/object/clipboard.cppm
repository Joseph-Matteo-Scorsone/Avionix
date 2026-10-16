module;
#include <cstddef>
#include <expected>
#include <future>
#include <new>
#include <span>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

export module avionix.object.clipboard;
export import avionix.entity.clipboard;

export namespace avionix {

struct attachment_input_capabilities {
  bool native_file_drop{};
  bool clipboard_image{};
};

// Describes the compiled backend, not whether an image is currently available.
[[nodiscard]] constexpr attachment_input_capabilities
attachment_capabilities() noexcept {
#if defined(_WIN32)
  return {false, true};
#else
  return {false, false};
#endif
}

namespace clipboard_detail {
inline clipboard_image_result read(clipboard_limits limits) {
#if defined(_WIN32)
  if (!OpenClipboard(nullptr))
    return std::unexpected(clipboard_error::platform_failure);
  struct close_guard {
    ~close_guard() { CloseClipboard(); }
  } close;
  if (!IsClipboardFormatAvailable(CF_DIB)) {
    if (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_BITMAP))
      return std::unexpected(clipboard_error::unsupported_format);
    return std::unexpected(clipboard_error::no_image);
  }
  const auto handle = GetClipboardData(CF_DIB);
  if (!handle) return std::unexpected(clipboard_error::platform_failure);
  const auto length = GlobalSize(handle);
  if (length > limits.max_bytes)
    return std::unexpected(clipboard_error::allocation_limit);
  if (length < 40) return std::unexpected(clipboard_error::malformed_data);
  const auto* data = static_cast<const std::byte*>(GlobalLock(handle));
  if (!data) return std::unexpected(clipboard_error::platform_failure);
  struct unlock_guard {
    HGLOBAL handle;
    ~unlock_guard() { GlobalUnlock(handle); }
  } unlock{handle};
  try {
    return decode_clipboard_dib(std::span{data, length}, limits);
  } catch (const std::bad_alloc&) {
    return std::unexpected(clipboard_error::allocation_limit);
  }
#else
  (void)limits;
  return std::unexpected(clipboard_error::unsupported);
#endif
}
}  // namespace clipboard_detail

// Starts a worker. Retain the future and poll wait_for(0) on the UI thread.
// get() and destruction of an unfinished future can wait. No default keybinding.
[[nodiscard]] inline std::future<clipboard_image_result> request_clipboard_image(
    clipboard_limits limits = {}) {
  return std::async(std::launch::async,
                    [limits] { return clipboard_detail::read(limits); });
}
}  // namespace avionix
