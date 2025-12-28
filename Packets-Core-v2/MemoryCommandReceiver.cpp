#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "MemoryCommandReceiver.h"
#include "PacketQueue.h"
#include "MemoryReaderX86.h"
#include "ResponsePipeSender.h"

#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>

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

struct ReadMemPayload
{
    uint32_t request_id;
    uint32_t address;
    uint8_t type;
    uint16_t size;
    uint8_t reserved;
};
#pragma pack(pop)

static constexpr uint32_t kCmdMagic_CMD2 = 0x32444D43;
static constexpr uint16_t kCmdVerV2 = 2;

// ===================== Multi-Client Architecture =====================

struct MemClient
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE thread = nullptr;
    std::atomic<bool> closed{ false };
};

static std::mutex g_memClientsMtx;
static std::vector<std::shared_ptr<MemClient>> g_memClients;

static constexpr int kMemAcceptThreads = 4;
static HANDLE g_memAcceptThreads[kMemAcceptThreads] = { nullptr };

// ===================== Command Processing Thread =====================

static DWORD WINAPI MemClientThreadMain(LPVOID p)
{
    auto client = reinterpret_cast<MemClient*>(p);
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

        if (hdr.cmd == kCommandReadMemory)
        {
            if (hdr.payload_len < sizeof(ReadMemPayload))
                break;

            ReadMemPayload req{};
            memcpy(&req, payload.data(), sizeof(ReadMemPayload));

            std::vector<uint8_t> data;
            uint32_t status = 0;
            ReadMemorySafe(
                req.address,
                static_cast<ReadMemType>(req.type),
                req.size,
                data,
                status
            );

            MemoryReadResponse rsp;
            rsp.request_id = req.request_id;
            rsp.status = status;
            rsp.address = req.address;
            rsp.type = static_cast<ReadMemType>(req.type);
            rsp.size = req.size;
            rsp.data = std::move(data);

            RspQueuePush(rsp);
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

static void CleanupClosedMemClients()
{
    std::lock_guard<std::mutex> lock(g_memClientsMtx);

    for (auto& c : g_memClients)
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

    g_memClients.erase(std::remove_if(g_memClients.begin(), g_memClients.end(),
        [](const std::shared_ptr<MemClient>& c) {
            return !c || c->closed.load();
        }), g_memClients.end());
}

// ===================== Accept Thread =====================

static DWORD WINAPI MemAcceptThreadMain(LPVOID)
{
    while (!g_stop)
    {
        HANDLE hPipe = CreateNamedPipeA(
            kPacketsCoreMemoryPipeName,
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

        auto client = std::make_shared<MemClient>();
        client->pipe = hPipe;
        client->thread = CreateThread(nullptr, 0, MemClientThreadMain, client.get(), 0, nullptr);

        if (!client->thread)
        {
            client->closed.store(true);
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_memClientsMtx);
            g_memClients.emplace_back(client);
        }

        CleanupClosedMemClients();
        Sleep(5);
    }

    return 0;
}

// ===================== Public API =====================

bool StartMemPipeReceiver()
{
    for (auto h : g_memAcceptThreads)
    {
        if (h)
            return true;
    }

    for (int i = 0; i < kMemAcceptThreads; ++i)
    {
        g_memAcceptThreads[i] = CreateThread(nullptr, 0, MemAcceptThreadMain, nullptr, 0, nullptr);
        if (!g_memAcceptThreads[i])
        {
            g_stop = true;
            for (int j = 0; j < i; ++j)
            {
                if (g_memAcceptThreads[j])
                {
                    WaitForSingleObject(g_memAcceptThreads[j], 2000);
                    CloseHandle(g_memAcceptThreads[j]);
                    g_memAcceptThreads[j] = nullptr;
                }
            }
            return false;
        }
    }

    return true;
}

void StopMemPipeReceiver()
{
    bool anyThread = false;
    for (auto h : g_memAcceptThreads)
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

    for (int i = 0; i < kMemAcceptThreads; ++i)
    {
        if (!g_memAcceptThreads[i])
            continue;
        WaitForSingleObject(g_memAcceptThreads[i], 2000);
        CloseHandle(g_memAcceptThreads[i]);
        g_memAcceptThreads[i] = nullptr;
    }

    std::vector<std::shared_ptr<MemClient>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_memClientsMtx);
        snapshot = g_memClients;
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
        std::lock_guard<std::mutex> lock(g_memClientsMtx);
        g_memClients.clear();
    }
}
