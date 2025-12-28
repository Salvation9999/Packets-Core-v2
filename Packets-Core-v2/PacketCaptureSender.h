#pragma once

#include <windows.h>

bool StartNamedPipeSender();
void StopNamedPipeSender();

constexpr const char* kPacketsCoreCapturePipeName = "\\\\.\\pipe\\PacketsCorePipe";
