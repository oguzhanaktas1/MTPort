#pragma once

#include <string>
#include <vector>

#include "Backend.hpp"
#include "Json.hpp"

namespace mtport::cli {

enum class CheckStatus { Ok, Warn, Fail, Info, NotRun };

struct Check {
    std::string id;
    CheckStatus status{CheckStatus::Info};
    std::string summary;
    std::string remedy;  // actionable text, empty when OK
};

const char* toString(CheckStatus);

std::vector<Check> runDoctor(const BackendSpec& spec);
Json doctorToJson(const std::vector<Check>&);

}  // namespace mtport::cli
