#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wlclip::core {

bool starts_with(std::string_view s, std::string_view prefix);

// Best-effort MIME type guess from payload bytes / explicit hint.
std::string guess_mime(const std::vector<std::byte>& data,
                       std::string_view explicit_type);

// Pick the best MIME to receive from a list of advertised types, given a
// (possibly empty) preference.
std::string choose_mime(const std::vector<std::string>& available,
                        const std::string& prefer);

// Whether `mime` denotes textual data (used to decide trailing newline).
bool is_textual(std::string_view mime);

}
