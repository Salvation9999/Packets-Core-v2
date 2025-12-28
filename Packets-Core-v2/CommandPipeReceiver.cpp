#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "CommandPipeReceiver.h"
#include "PacketQueue.h"

#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>

#include <winsock2.h>

// ===================== Protocol Definitions =====================

static bool ReadAll(HANDLE hPipe, void* data, DWORD len)
{
    uint8_t* p = static_cast<uint8_t*>(data);
    DWORD remaining = len;

    while (remaining > 0)
    {
        DWORD read = 0;
        if (!ReadFile(hPipe, p, remaining, &read, nullptr) || read == 0)
            return false;
        p += read;
        remaining -= read;
    }
    return true;
}

#pragma pack(push, 1)
struct CmdHeaderV2
{
    uint32_t magic;
    uint16_t ver;
    uint8_t cmd;
    uint8_t reserved0;
    uint32_t flags;
    uint64_t sock;
    uint32_t payload_len;
};
#pragma pack(pop)

static constexpr uint32_t kCmdMagic_CMD2 = 0x32444D43;
static constexpr uint16_t kCmdVerV2 = 2;

// ===================== Multi-Client Architecture =====================

struct SendClient
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE thread = nullptr;
    std::atomic<bool> closed{ false };
};

static std::mutex g_sendClientsMtx;
static std::vector<std::shared_ptr<SendClient>> g_sendClients;

static constexpr int kSendAcceptThreads = 4;
static HANDLE g_sendAcceptThreads[kSendAcceptThreads] = { nullptr };

// ===================== Command Processing Thread =====================

static DWORD WINAPI SendClientThreadMain(LPVOID p)
{
    auto client = reinterpret_cast<SendClient*>(p);
    HANDLE hPipe = client->pipe;

    while (!g_stop && !client->closed.load())
    {
        CmdHeaderV2 hdr{};

        if (!ReadAll(hPipe, &hdr, sizeof(hdr)))
            break;

        if (hdr.magic != kCmdMagic_CMD2 || hdr.ver != kCmdVerV2)
            break;

        if (hdr.payload_len > (1u << 20))
            break;

        std::vector<uint8_t> payload;
        payload.resize(hdr.payload_len);

        if (hdr.payload_len > 0)
        {
            if (!ReadAll(hPipe, payload.data(), hdr.payload_len))
                break;
        }

        if (hdr.cmd == kCommandTcpSend)
        {
            SOCKET s = (SOCKET)(uintptr_t)hdr.sock;
            const char* buf = payload.empty() ? nullptr : (const char*)payload.data();
            int len = (int)payload.size();
            int flags = (int)hdr.flags;

            if (buf && len > 0)
                send(s, buf, len, flags);
        }
    }

    client->closed.store(true);

    if (hPipe != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        client->pipe = INVALID_HANDLE_VALUE;
    }

    return 0;
}

// ===================== Client Cleanup =====================

static void CleanupClosedSendClients()
{
    std::lock_guard<std::mutex> lock(g_sendClientsMtx);

    for (auto& c : g_sendClients)
    {
        if (!c)
            continue;
        if (!c->closed.load())
            continue;

        if (c->thread)
        {
            if (WaitForSingleObject(c->thread, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(c->thread);
                c->thread = nullptr;
            }
        }
    }

    g_sendClients.erase(std::remove_if(g_sendClients.begin(), g_sendClients.end(),
        [](const std::shared_ptr<SendClient>& c) {
            return !c || c->closed.load();
        }), g_sendClients.end());
}

// ===================== Accept Thread =====================

static DWORD WINAPI SendAcceptThreadMain(LPVOID)
{
    while (!g_stop)
    {
        HANDLE hPipe = CreateNamedPipeA(
            kPacketsCoreCommandPipeName,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            1 << 16,
            1 << 16,
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            Sleep(200);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected)
        {
            CloseHandle(hPipe);
            Sleep(50);
            continue;
        }

        auto client = std::make_shared<SendClient>();
        client->pipe = hPipe;
        client->thread = CreateThread(nullptr, 0, SendClientThreadMain, client.get(), 0, nullptr);

        if (!client->thread)
        {
            client->closed.store(true);
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_sendClientsMtx);
            g_sendClients.emplace_back(client);
        }

        CleanupClosedSendClients();
        Sleep(5);
    }

    return 0;
}

// ===================== Public API =====================

bool StartSendPipeReceiver()
{
    for (auto h : g_sendAcceptThreads)
    {
        if (h)
            return true;
    }

    for (int i = 0; i < kSendAcceptThreads; ++i)
    {
        g_sendAcceptThreads[i] = CreateThread(nullptr, 0, SendAcceptThreadMain, nullptr, 0, nullptr);
        if (!g_sendAcceptThreads[i])
        {
            g_stop = true;
            for (int j = 0; j < i; ++j)
            {
                if (g_sendAcceptThreads[j])
                {
                    WaitForSingleObject(g_sendAcceptThreads[j], 2000);
                    CloseHandle(g_sendAcceptThreads[j]);
                    g_sendAcceptThreads[j] = nullptr;
                }
            }
            return false;
        }
    }

    return true;
}

void StopSendPipeReceiver()
{
    bool anyThread = false;
    for (auto h : g_sendAcceptThreads)
    {
        if (h)
        {
            anyThread = true;
            break;
        }
    }

    if (!anyThread)
        return;

    g_stop = true;

    for (int i = 0; i < kSendAcceptThreads; ++i)
    {
        if (!g_sendAcceptThreads[i])
            continue;
        WaitForSingleObject(g_sendAcceptThreads[i], 2000);
        CloseHandle(g_sendAcceptThreads[i]);
        g_sendAcceptThreads[i] = nullptr;
    }

    std::vector<std::shared_ptr<SendClient>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_sendClientsMtx);
        snapshot = g_sendClients;
    }

    for (auto& c : snapshot)
    {
        if (!c)
            continue;
        c->closed.store(true);
        if (c->thread)
        {
            WaitForSingleObject(c->thread, 1000);
            CloseHandle(c->thread);
            c->thread = nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_sendClientsMtx);
        g_sendClients.clear();
    }
}
