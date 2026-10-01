#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/OtaInterfaces.h"

namespace cauce::app {

bool parseOtaManifestJson(const char* json, OtaRelease& out);

}  // namespace cauce::app
