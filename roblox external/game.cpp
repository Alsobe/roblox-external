#include "game.h"
#include "memory.h"
#include "offsets.h"
#include "cache.h"

namespace game {
    static instance ReadVisualEngine(uintptr_t base) {
        instance ve = read<instance>(base + Offsets::VisualEngine::Pointer);
        return ve;
    }

    instance ReadDatamodel(uintptr_t base) {
        instance ve = ReadVisualEngine(base);
        if (!ve.is_valid())
            return instance{};

        uintptr_t fakedm = read<uintptr_t>(ve.address + Offsets::VisualEngine::FakeDataModel);
        if (!is_valid_address(fakedm))
            return instance{};

        instance dm = read<instance>(fakedm + Offsets::FakeDataModel::RealDataModel);
        return dm;
    }

    std::string GetGameName(uintptr_t base) {
        instance dm = ReadDatamodel(base);
        if (!dm.is_valid()) return "";
        return dm.get_name();
    }

    uint64_t GetPlaceId(uintptr_t base) {
        instance dm = ReadDatamodel(base);
        if (!dm.is_valid()) return 0;
        return read<uint64_t>(dm.address + Offsets::DataModel::PlaceId);
    }
}
