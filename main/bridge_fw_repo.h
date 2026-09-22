// The bridge's own firmware releases, fetched from its GitHub repository.
//
// Lists the GitHub releases that carry an "svs_bridge.bin" asset, so the web UI
// can offer an over-the-air update straight from GitHub. Works anonymously,
// which requires the repository to be public; a private repository answers 404.
#pragma once

#include <string>
#include <vector>
#include "esp_err.h"

namespace bridge_fw_repo {

struct Release {
    std::string tag;         // e.g. "v0.1.0"
    std::string name;        // release title
    std::string notes_url;   // html_url of the release page
    bool prerelease;
    size_t size;             // svs_bridge.bin asset size in bytes
    std::string url;         // svs_bridge.bin browser_download_url
};

// Lists releases newest first (only those with an svs_bridge.bin asset).
esp_err_t list(std::vector<Release> &out, std::string *error);

// Resolves a tag from the last listing to its asset URL and size.
bool resolve(const std::string &tag, std::string &url, size_t &size);

}  // namespace bridge_fw_repo
