#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wlclip::wayland { struct SeatInfo; }

namespace wlclip::clipboard {

enum class Selection {
    Regular,
    Primary,
};

struct CopyData {
    std::vector<std::byte>   bytes;
    std::vector<std::string> mime_types;  // advertised to peers
};

struct PasteResult {
    std::string              mime;
    std::vector<std::byte>   bytes;
    std::vector<std::string> available;   // all advertised mimes
};

// Concrete protocol backends implement this interface.
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;

    virtual bool copy(const wayland::SeatInfo& seat,
                      Selection sel,
                      CopyData data,
                      bool oneshot) = 0;

    virtual bool paste(const wayland::SeatInfo& seat,
                       Selection sel,
                       const std::string& prefer_mime,
                       bool list_only,
                       PasteResult& out) = 0;
};

}  // namespace wlclip::clipboard
