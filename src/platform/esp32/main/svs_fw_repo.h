// Official SVS firmware releases, fetched from the repository the official
// SVS Management Utility uses:
// https://github.com/Arthrimus/SVS_Firmware_Repository
//
// New releases show up without updating the bridge. Downloads are checked
// against the git blob SHA-1 that GitHub reports for each file.
#pragma once

#include <string>
#include <vector>
#include "esp_err.h"

namespace svs_fw_repo {

struct Release {
    std::string name;     // file name, e.g. "SVS_FW_1.21.hex"
    std::string version;  // e.g. "1.21", "1.20 BETA"
    bool beta;
    size_t size;
};

// Lists the .hex files in the repository, newest version first.
esp_err_t list(std::vector<Release> &out, std::string *error);

// Downloads one of the listed files.
esp_err_t download(const std::string &name, std::string &hex, std::string *error);

}  // namespace svs_fw_repo
