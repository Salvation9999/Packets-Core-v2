#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "MemoryReaderX86.h"
#include <cstring>
#include <algorithm>

static bool SafeMemcpy(void* dest, const void* src, size_t size)
{
    __try
    {
        memcpy(dest, src, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool SafeReadByte(uint32_t address, uint8_t& outByte)
{
    __try
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
        outByte = *p;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadMemorySafe(
    uint32_t address,
    ReadMemType type,
    uint16_t size,
    std::vector<uint8_t>& outData,
    uint32_t& outStatus
)
{
    outData.clear();
    outStatus = MEM_STATUS_OK;

    if (address == 0)
    {
        outStatus = MEM_STATUS_INVALID_ADDRESS;
        return true;
    }

    switch (type)
    {
    case ReadMemType::U32:
    {
        outData.resize(4);
        if (!SafeMemcpy(outData.data(), reinterpret_cast<const void*>(address), 4))
        {
            outData.clear();
            outStatus = MEM_STATUS_INVALID_ADDRESS;
        }
        break;
    }

    case ReadMemType::U64:
    {
        outData.resize(8);
        if (!SafeMemcpy(outData.data(), reinterpret_cast<const void*>(address), 8))
        {
            outData.clear();
            outStatus = MEM_STATUS_INVALID_ADDRESS;
        }
        break;
    }

    case ReadMemType::BYTES:
    {
        if (size == 0 || size > MEM_MAX_SIZE)
        {
            outStatus = MEM_STATUS_INVALID_SIZE;
            break;
        }

        outData.resize(size);
        if (!SafeMemcpy(outData.data(), reinterpret_cast<const void*>(address), size))
        {
            outData.clear();
            outStatus = MEM_STATUS_INVALID_ADDRESS;
        }
        break;
    }

    case ReadMemType::STR_Z:
    {
        if (size == 0 || size > MEM_MAX_SIZE)
        {
            outStatus = MEM_STATUS_INVALID_SIZE;
            break;
        }

        outData.reserve(size);
        uint32_t currentAddr = address;
        uint16_t count = 0;

        while (count < size)
        {
            uint8_t byte = 0;
            if (!SafeReadByte(currentAddr, byte))
            {
                outData.clear();
                outStatus = MEM_STATUS_INVALID_ADDRESS;
                break;
            }

            if (byte == 0)
            {
                break;
            }

            outData.push_back(byte);
            currentAddr++;
            count++;
        }

        if (count >= size && outStatus == MEM_STATUS_OK)
        {
            outStatus = MEM_STATUS_STRING_TOO_LONG;
        }
        break;
    }

    case ReadMemType::STR_FIXED:
    {
        if (size == 0 || size > MEM_MAX_SIZE)
        {
            outStatus = MEM_STATUS_INVALID_SIZE;
            break;
        }

        outData.resize(size);
        if (!SafeMemcpy(outData.data(), reinterpret_cast<const void*>(address), size))
        {
            outData.clear();
            outStatus = MEM_STATUS_INVALID_ADDRESS;
        }
        break;
    }

    default:
    {
        outStatus = MEM_STATUS_INVALID_TYPE;
        break;
    }
    }

    return true;
}
