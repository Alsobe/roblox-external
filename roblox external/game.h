#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

struct instance;

namespace game {
    instance ReadDatamodel(uintptr_t base);
    std::string GetGameName(uintptr_t base);
    uint64_t GetPlaceId(uintptr_t base);
}
