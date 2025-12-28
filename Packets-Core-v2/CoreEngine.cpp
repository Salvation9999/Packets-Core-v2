#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "CoreEngine.h"
#include "ResponsePipeSender.h"

#include "NetworkHook.h"
#include "PacketCaptureSender.h"
#include "CommandPipeReceiver.h"
#include "MemoryCommandReceiver.h"
#include "PacketQueue.h"


static bool g_started = false;


bool CoreStart()
{
    if (g_started) 
        return true;

    g_stop = false; 

    if (!StartNamedPipeSender())
        return false;

    // Start send pipe receiver (Python -> DLL for TCP_SEND)
    if (!StartSendPipeReceiver())
    {
        StopNamedPipeSender();
        return false;
    }

    // Start memory pipe receiver (Python -> DLL for READ_MEM)
    if (!StartMemPipeReceiver())
    {
        StopSendPipeReceiver();
        StopNamedPipeSender();
        return false;
    }

    if (!StartRspPipeSender())
    {
        StopMemPipeReceiver();
        StopSendPipeReceiver();
        StopNamedPipeSender();
        return false;
    }

    if (!InitNetHook())
    {
        StopRspPipeSender();
        StopMemPipeReceiver();
        StopSendPipeReceiver();
        StopNamedPipeSender();
        return false;
    }

    StartNetHookWatchdog();

    g_started = true;
    return true;
}

void CoreStop()
{

    if (!g_started)
        return;


    g_stop = true;


    PacketQueueNotifyAll();


    StopNetHookWatchdog();
    UninitNetHook();
    StopRspPipeSender();
    StopMemPipeReceiver();
    StopSendPipeReceiver();
    StopNamedPipeSender();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        while (!g_queue.empty())
            g_queue.pop();
    }

    g_started = false;
}
