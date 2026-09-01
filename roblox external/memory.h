#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <atomic>

class MemoryClass {
public:
    HANDLE Handle = nullptr;
    template <typename T> T Read(uint64_t address) {
        T out{};
        if (Handle) ReadProcessMemory(Handle, (void*)address, &out, sizeof(T), nullptr);
        return out;
    }
    template <typename T> void Write(uint64_t address, T value) {
        if (Handle) WriteProcessMemory(Handle, (void*)address, &value, sizeof(T), nullptr);
    }
    bool ReadRaw(uint64_t address, void* buffer, size_t size) {
        return Handle && ReadProcessMemory(Handle, (void*)address, buffer, size, nullptr);
    }
    bool WriteRaw(uint64_t address, const void* data, size_t size) {
        return Handle && WriteProcessMemory(Handle, (void*)address, data, size, nullptr);
    }
};

extern MemoryClass g_memory;

namespace mem {
    extern std::atomic<HANDLE> roblox_h;
    extern std::atomic<uint32_t> process_id;
    bool grabroblox_h();
}

bool is_valid_address(uintptr_t address);

std::string fetchstring(uint64_t address);

template <typename T> T read(uint64_t address) {
    if (!is_valid_address(address)) return T{};
    return g_memory.Read<T>(address);
}

template <typename T> bool write(uint64_t address, T value) {
    if (!is_valid_address(address)) return false;
    g_memory.Write(address, value);
    return true;
}

// drops cached class-name lookups (call when re-attaching to a new process)
void clear_instance_caches();

bool read_raw(uint64_t address, void* buffer, size_t size);
bool write_raw(uint64_t address, const void* data, size_t size);

struct instance {
    uintptr_t address = 0;
    bool is_valid() const { return address != 0; }
    std::string get_name() const;
    std::string get_class_name() const;
    std::vector<instance> get_children() const;
    instance read_child(const std::string& child_name) const;
    instance read_service(const std::string& service_name) const;
    instance model_instance() const;
    instance local_player() const;
};
