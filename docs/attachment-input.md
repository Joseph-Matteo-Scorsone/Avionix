# Attachment input

Avionix delivers input and reads the local platform clipboard. The application
owns attachment import, validation, storage, and sending. Avionix never opens a
pasted path. Paths outside a project have no special meaning to this library.

## Backend capabilities

| Backend              | Input delivered                                                                    | Native file drops | Explicit clipboard images            |
| -------------------- | ---------------------------------------------------------------------------------- | ----------------- | ------------------------------------ |
| Windows console      | `ReadConsoleInputW` records translated to UTF-8 and escape sequences, then decoded | No                | Local Win32 CF_DIB, 24/32-bit BI_RGB |
| Linux POSIX terminal | Bytes from terminal descriptor, decoded as UTF-8 and escape sequences              | No                | Unsupported                          |
| macOS POSIX terminal | Bytes from terminal descriptor, decoded as UTF-8 and escape sequences              | No                | Unsupported                          |

Windows mouse records and SGR mouse drag sequences describe cell positions and
buttons. They contain no file paths. Avionix does not register an OLE drop target,
handle WM_DROPFILES, own an AppKit window, or connect to X11/Wayland data devices.
No current backend produces `file_drop_event`.

`file_drop_event` owns a vector of UTF-8 path strings. It reserves a typed delivery
contract for a backend with real native drop input. It does not imply native drop
support today. Check `attachment_capabilities().native_file_drop`.

A terminal may translate a desktop file drag into quoted or escaped path text.
Avionix delivers that text as `paste_event` if bracketed, otherwise as key events.
There is no reliable way to distinguish unbracketed paste from typing. Quoting,
multiple-path separators, and bracketed paste depend on the terminal. Avionix
neither parses these paths nor guesses that they are attachments. For a terminal
chat client, an explicit application action can interpret selected input as paths.

Relevant platform contracts:

- [Windows console input records](https://learn.microsoft.com/en-us/windows/console/input-record-str)
- [Windows clipboard formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats)
- [Windows bitmap header](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader)
- [Wayland data offers and drops](https://wayland.freedesktop.org/docs/html/apa.html)
- [macOS NSPasteboard](https://developer.apple.com/documentation/appkit/nspasteboard)

Linux image access would require an X11 or Wayland clipboard adapter, including
display availability and selection ownership handling. macOS would require an
AppKit pasteboard adapter. Neither is implemented. No helper programs are spawned.
OSC 52 text copy remains unchanged and is not an image retrieval protocol.

## Intercepting paste

```cpp
app.on_event([&](const avionix::event& e) {
  if (const auto* p = avionix::as<avionix::paste_event>(e)) {
    return handle_paste(p->text); // true consumes, false permits insertion
  }
  return false;
}, true);
```

The early hook runs before widget routing. Returning false continues through the
focused path once. Do not call the focused control manually and then return false.
The late hook only sees events the widgets and built-in handlers leave unhandled.
The early hook is a single replaceable callback, so combine application shortcuts
and paste interception in that callback. A retained paste must be copied because
the hook receives a reference valid only during dispatch.

## Explicit clipboard image requests

`attachment_capabilities().clipboard_image` reports compiled platform support,
not current clipboard contents or permission to access them. The clipboard is
local to the Avionix process. An SSH process cannot read the client desktop
clipboard through this API; WSL uses the unsupported POSIX implementation.

`request_clipboard_image(limits)` returns `std::future<clipboard_image_result>`.
It starts a worker for platform retrieval and conversion. Keep the future, poll
`wait_for(0)` from an application timer, and call `get()` only when ready. Dropping
an unfinished future or calling `get()` early can wait. On shutdown, restore the
terminal before destroying an unfinished future. Thread creation can throw and
unexpected worker exceptions propagate through `get()`.

No shortcut is installed. The example chooses F2. Ctrl+V and terminal paste keep
their existing behavior unless the application consumes those events.

Successful results own tightly packed, top-down RGBA8 pixels in `rgba8`, with
`stride == width * 4`. Alpha is always 255 because BI_RGB's fourth byte is reserved.
No PNG/JPEG encoding or color profile conversion occurs. The application owns any
encoding it needs. Data never enters a text control.

Expected errors are distinct:

| Error                | Meaning                                                               |
| -------------------- | --------------------------------------------------------------------- |
| `unsupported`        | No clipboard adapter for this platform                                |
| `no_image`           | No recognized image format is available                               |
| `platform_failure`   | Clipboard open, retrieval, or lock failed, including contention       |
| `unsupported_format` | Recognized image has an unsupported header or format                  |
| `malformed_data`     | Truncated or inconsistent dimensions, planes, palette, or pixel bytes |
| `allocation_limit`   | Input/output size or dimensions exceed limits, or allocation fails    |

Windows accepts CF_DIB with a 40-byte BITMAPINFOHEADER, one plane, 24/32-bit
uncompressed BI_RGB. Other DIB headers, compression, bitfields, and native image
formats are not decoded. CF_BITMAP/CF_DIBV5 availability without CF_DIB reports
unsupported format. Arbitrary registered formats are not enumerated. A clipboard
containing only an unrecognized custom image format reports no image.

Defaults cap source and output independently at 64 MiB and either dimension at
16384 pixels. Callers may lower or raise these limits. All header arithmetic uses
64-bit unsigned values before size_t conversion. The decoder checks source row
padding, palette offset, buffer extent, optional declared image size, dimensions,
and output size before allocation. The platform checks GlobalSize before locking.
Clipboard memory remains locked until the worker finishes copying pixels, then
RAII unlocks and closes it. A slow clipboard owner can delay the worker; there is
no timeout or cancellation guarantee.

## Example and verification

Build `attachments` with `zig build examples` or CMake. It demonstrates paste
interception, a native-drop handler for future capable backends, F2 image requests,
metadata, F3 removal, and unsupported capability feedback. It stores data only in
memory. Native drops cannot be demonstrated with the current terminal backends.

The attachment tests use synthetic DIB bytes, synthetic drop events, decoder input,
and `application::simulate`. They verify ownership, Unicode and multiple paths,
consumed/unhandled paste, exactly-once insertion, malformed buffers, format rejection,
and allocation limits. The unsupported request test runs only on POSIX builds.
Existing widget tests cover selection, captured dragging, paste sanitation,
scrolling, and keyboard edits. These tests do not prove desktop drag or screenshot
behavior in a real terminal. No interactive terminal or desktop clipboard behavior
has been verified as part of this change.

Validation on the Windows development host:

- `zig build test`: 230 passed, 0 failed, 603 checks.
- `zig build examples`: all examples compile and link.
- `zig build -Dtarget=x86_64-linux-gnu`: library cross-compiles.
- `zig build -Dtarget=aarch64-macos`: library cross-compiles.

Cross-compilation does not run POSIX tests. The POSIX unsupported-request test is
present but was not executed here. CMake configuration was updated but not built.
