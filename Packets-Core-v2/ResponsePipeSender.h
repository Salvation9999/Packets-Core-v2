#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include "MemoryReaderX86.h"

bool StartRspPipeSender();

void StopRspPipeSender();

constexpr const char* kPacketsCoreResponsePipeName = "\\\\.\\pipe\\PacketsCoreRspPipe";

struct MemoryReadResponse
{
    uint32_t request_id = 0;
    uint32_t status = 0;
    uint32_t address = 0;
    ReadMemType type = ReadMemType::U32;
    uint16_t size = 0;
    std::vector<uint8_t> data;
};

void RspQueuePush(const MemoryReadResponse& item);

bool RspQueueTryPop(MemoryReadResponse& out, uint32_t timeoutMs);

void RspQueueNotifyAll();
