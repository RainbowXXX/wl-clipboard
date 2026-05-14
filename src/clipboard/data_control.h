#pragma once

#include "clipboard/backend.h"

namespace wlclip::wayland { class State; }

namespace wlclip::clipboard {

class DataControlBackend final : public Backend {
public:
    explicit DataControlBackend(wayland::State& state) : state_(state) {}

    const char* name() const override { return "zwlr_data_control_v1"; }

    bool copy(const wayland::SeatInfo& seat, Selection sel,
              CopyData data, bool oneshot) override;
    bool paste(const wayland::SeatInfo& seat, Selection sel,
               const std::string& prefer_mime, bool list_only,
               PasteResult& out) override;

private:
    wayland::State& state_;
};

}
