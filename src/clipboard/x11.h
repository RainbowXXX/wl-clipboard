// X11 selection backend (uses libxcb directly).
//
// Useful for:
//   - Testing the XWayland → Wayland bridge (compositor brings whatever we
//     set as the X CLIPBOARD/PRIMARY across to Wayland clients).
//   - Plain X11 sessions with no Wayland at all.
//
// Talks to whatever X server $DISPLAY points to. INCR (large-data) protocol
// is NOT implemented — payloads must fit in a single property (typically a
// few MiB depending on the server). For ordinary text clipboard this is more
// than enough.

#pragma once

#include "clipboard/backend.h"

#include <string>

namespace wlclip::wayland { struct SeatInfo; }

namespace wlclip::clipboard {

class X11Backend final : public Backend {
public:
    // `display` is forwarded to xcb_connect (use empty/null for $DISPLAY).
    // The seat selector is ignored — X11 has its own per-session selection.
    X11Backend(std::string display, std::string /*seat*/)
        : display_(std::move(display)) {}

    const char* name() const override { return "X11 selection (xcb)"; }

    bool copy(const wayland::SeatInfo&, Selection sel,
              CopyData data, bool oneshot) override;
    bool paste(const wayland::SeatInfo&, Selection sel,
               const std::string& prefer_mime, bool list_only,
               PasteResult& out) override;

private:
    std::string display_;
};

}  // namespace wlclip::clipboard
