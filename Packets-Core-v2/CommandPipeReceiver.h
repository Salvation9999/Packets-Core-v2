#pragma once

#include <cstdint>

bool StartSendPipeReceiver();
void StopSendPipeReceiver();

constexpr const char* kPacketsCoreCommandPipeName = "\\\\.\\pipe\\PacketsCoreSendPipe";

constexpr uint8_t kCommandTcpSend = 1;
