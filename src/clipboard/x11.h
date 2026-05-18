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

#include <functional>
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

    // Two-phase copy that matches xclip's lifecycle:
    //   * acquire+verify selection ownership in the caller process
    //   * then fork; the parent exits (so shell sees the command return only
    //     after Xwayland/compositor can already see the new X11 owner) and
    //     the child stays around to serve SelectionRequests.
    // This is *significantly* faster from the perspective of an outside
    // observer polling the clipboard, because the bridge propagation can
    // start at "shell-return - epsilon" instead of "shell-return + 100-200ms"
    // (which is what happens if the caller daemonizes before doing any X).
    //
    // `after_fork_in_parent` (if set, and detach is true) runs in the parent
    // process *after* fork() but *before* _exit(). The child is concurrently
    // entering the SelectionRequest loop, so the callback may safely trigger
    // operations that depend on the X owner being responsive (e.g. probing
    // the wayland-side data_control device to verify the new selection has
    // propagated). The callback's return value is ignored — the parent always
    // _exit(0)s afterwards. The callback's responsibility is solely to *delay*
    // _exit until some condition holds, not to abort.
    bool copy_acquire_then_detach(Selection sel, CopyData data,
                                  bool oneshot, bool detach,
                                  std::function<void()> after_fork_in_parent = {});

private:
    std::string display_;
};

}  // namespace wlclip::clipboard
