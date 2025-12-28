#pragma once

#include <cstdint>

bool StartMemPipeReceiver();
void StopMemPipeReceiver();

constexpr const char* kPacketsCoreMemoryPipeName = "\\\\.\\pipe\\PacketsCoreMemPipe";

constexpr uint8_t kCommandReadMemory = 2;
