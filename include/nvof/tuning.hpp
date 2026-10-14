#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace nvof::tuning {
enum Parameter : unsigned {
#define NVOF_TUNABLE(name, value, lo, hi, group, unit, description) name,
#include "tuning_parameters.def"
#undef NVOF_TUNABLE
    count
};
using Values = std::array<float, count>;
inline constexpr Values defaults = {{
#define NVOF_TUNABLE(name, value, lo, hi, group, unit, description) float(value),
#include "tuning_parameters.def"
#undef NVOF_TUNABLE
}};
static_assert(sizeof(Values) % 16 == 0, "HLSL constant buffer packing");
// Shared wire layout: 16 little-endian uint32s, then two tightly packed float arrays.
// Every access, including controller reads, must hold the named mutex.
struct Shared {
    uint32_t magic, version, bytes, parameter_count;
    uint32_t pid, alive, requested_revision, applied_revision;
    uint32_t rejected_revision, error_code, pair_count, phase_count;
    uint32_t width, height, prepared, schema_hash;
    Values requested, applied;
};
static_assert(sizeof(Shared) == 64 + sizeof(Values) * 2, "IPC ABI");
inline constexpr uint32_t magic = 0x4e565431, version = 1;
uint32_t schema_hash() noexcept;
bool validate(const Values&) noexcept;
// Optional complete startup snapshot. Missing, disabled or invalid profiles
// return the compiled baseline; live MCP updates still use the same schema.
Values load_startup_profile(const std::wstring& path);

// Experimental only. No GPU work or blocking IPC on the controller's thread.
// Render-thread calls use a zero-timeout mutex and keep the last snapshot if busy.
class Channel {
public:
    Channel();
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    void begin_pair() noexcept;
    void publish(unsigned width, unsigned height, bool prepared) noexcept;
    void rendered() noexcept;
    const Values& values() const noexcept { return values_; }
    uint32_t revision() const noexcept { return revision_; }
    const std::string& id() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Values values_ = defaults;
    uint32_t revision_ = 1, pairs_ = 0, phases_ = 0;
};
}
