#pragma once

#include <cstdint>
#include <vector>

enum class ReadMemType : uint8_t
{
    U32 = 1,
    U64 = 2,
    BYTES = 3,
    STR_Z = 4,
    STR_FIXED = 5
};

// ---------------- Status Codes ----------------

constexpr uint32_t MEM_STATUS_OK = 0;
constexpr uint32_t MEM_STATUS_INVALID_ADDRESS = 1;
constexpr uint32_t MEM_STATUS_INVALID_TYPE = 2;
constexpr uint32_t MEM_STATUS_INVALID_SIZE = 3;
constexpr uint32_t MEM_STATUS_STRING_TOO_LONG = 4;
constexpr uint32_t MEM_STATUS_UNKNOWN_ERROR = 99;

constexpr uint16_t MEM_MAX_SIZE = 1024;

// ---------------- Core Function --------------
bool ReadMemorySafe(
    uint32_t address,
    ReadMemType type,
    uint16_t size,
    std::vector<uint8_t>& outData,
    uint32_t& outStatus
);
