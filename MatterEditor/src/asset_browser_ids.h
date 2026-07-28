#ifndef VIEWER_ASSET_BROWSER_IDS_H
#define VIEWER_ASSET_BROWSER_IDS_H

#include <cstddef>
#include <string>
#include <string_view>

namespace viewer {

inline std::string required_child_row_identity(std::string_view module,
                                               std::string_view params_json,
                                               std::size_t occurrence) {
    std::string identity = "required-child/";
    const auto append_component = [&identity](std::string_view value) {
        identity += std::to_string(value.size());
        identity.push_back(':');
        identity.append(value.data(), value.size());
        identity.push_back('/');
    };
    append_component(module);
    append_component(params_json);
    identity += std::to_string(occurrence);
    return identity;
}

}  // namespace viewer

#endif  // VIEWER_ASSET_BROWSER_IDS_H
