#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ResponsePipeSender.h"
#include "PacketQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#pragma pack(push, 1)
struct RspFrameHeaderV2
{
    uint32_t magic;
    uint16_t ver;
    uint8_t rsp_type;
    uint8_t reserved0;
    uint32_t payload_len;
};

struct RspPayloadV2
{
    uint32_t request_id;
    uint32_t status;
    uint32_t address;
    uint8_t type;
    uint16_t size;
    uint8_t reserved1;
};
#pragma pack(pop)

static constexpr uint32_t kRspMagic_RSP2 = 0x32505352;
static constexpr uint16_t kRspVerV2 = 2;
static constexpr uint8_t kRspTypeReadMem = 1;

static std::mutex g_rspMutex;
static std::condition_variable g_rspCv;
static std::queue<MemoryReadResponse> g_rspQueue;

static constexpr size_t kMaxRspQueueSize = 1000;

void RspQueuePush(const MemoryReadResponse& item)
{
    std::lock_guard<std::mutex> lock(g_rspMutex);

    while (g_rspQueue.size() >= kMaxRspQueueSize)
    {
        g_rspQueue.pop();
    }

    g_rspQueue.push(item);
    g_rspCv.notify_one();
}

bool RspQueueTryPop(MemoryReadResponse& out, uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(g_rspMutex);

    if (!g_rspCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
        return g_stop || !g_rspQueue.empty();
    }))
    {
        return false;
    }

    if (g_stop || g_rspQueue.empty())
        return false;

    out = g_rspQueue.front();
    g_rspQueue.pop();
    return true;
}

void RspQueueNotifyAll()
{
    g_rspCv.notify_all();
}

static std::atomic<bool> g_rspSenderRunning{ false };

static HANDLE g_rspStopEvent = nullptr;

static constexpr int kRspAcceptThreads = 4;
static HANDLE g_rspAcceptThreads[kRspAcceptThreads] = { nullptr };

static HANDLE g_rspDispatchThread = nullptr;

static constexpr DWORD kRspPipeOutBufferSize = 1 << 20;
static constexpr DWORD kRspPipeInBufferSize = 1 << 16;

static constexpr size_t kRspClientMaxQueuedBytes = 4u * 1024u * 1024u;

static bool WriteAllOverlapped(HANDLE hPipe, const void* data, DWORD len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    DWORD remaining = len;

    while (remaining > 0)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            return false;

        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, p, remaining, &written, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                HANDLE waits[2] = { ov.hEvent, g_rspStopEvent ? g_rspStopEvent : ov.hEvent };
                DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr == WAIT_OBJECT_0 + 1 || g_stop)
                {
                    CancelIoEx(hPipe, &ov);
                    CloseHandle(ov.hEvent);
                    return false;
                }

                if (!GetOverlappedResult(hPipe, &ov, &written, FALSE))
                {
                    CloseHandle(ov.hEvent);
                    return false;
                }
            }
            else
            {
                CloseHandle(ov.hEvent);
                return false;
            }
        }

        CloseHandle(ov.hEvent);

        if (written == 0)
            return false;

        p += written;
        remaining -= written;
    }

    return true;
}

struct RspClient
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE thread = nullptr;


    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<const std::vector<uint8_t>>> q;
    size_t queuedBytes = 0;


    std::atomic<bool> closed{ false };
};


static std::mutex g_rspClientsMtx;
static std::vector<std::shared_ptr<RspClient>> g_rspClients;

static std::shared_ptr<const std::vector<uint8_t>> BuildRspFrameBytes(const MemoryReadResponse& item)
{
    RspFrameHeaderV2 hdr{};
    hdr.magic = kRspMagic_RSP2;
    hdr.ver = kRspVerV2;
    hdr.rsp_type = kRspTypeReadMem;
    hdr.reserved0 = 0;
    hdr.payload_len = sizeof(RspPayloadV2) + static_cast<uint32_t>(item.data.size());

    RspPayloadV2 payload{};
    payload.request_id = item.request_id;
    payload.status = item.status;
    payload.address = item.address;
    payload.type = static_cast<uint8_t>(item.type);
    payload.size = item.size;
    payload.reserved1 = 0;

    auto out = std::make_shared<std::vector<uint8_t>>();
    out->resize(sizeof(hdr) + sizeof(payload) + item.data.size());

    memcpy(out->data(), &hdr, sizeof(hdr));
    memcpy(out->data() + sizeof(hdr), &payload, sizeof(payload));
    if (!item.data.empty())
    {
        memcpy(out->data() + sizeof(hdr) + sizeof(payload), item.data.data(), item.data.size());
    }

    return out;
}


static void RspClientEnqueueFrame(const std::shared_ptr<RspClient>& c, const std::shared_ptr<const std::vector<uint8_t>>& frame)
{
    if (!c || !frame)
        return;

    std::unique_lock<std::mutex> lock(c->mtx);

    if (frame->size() > kRspClientMaxQueuedBytes)
        return;

    while (c->queuedBytes + frame->size() > kRspClientMaxQueuedBytes && !c->q.empty())
    {
        c->queuedBytes -= (c->q.front() ? c->q.front()->size() : 0);
        c->q.pop_front();
    }

    if (c->queuedBytes + frame->size() > kRspClientMaxQueuedBytes)
        return;

    c->queuedBytes += frame->size();
    c->q.emplace_back(frame);
    lock.unlock();
    c->cv.notify_one();
}


static DWORD WINAPI RspClientWriterThreadMain(LPVOID p)
{
    auto c = reinterpret_cast<RspClient*>(p);

    while (!g_stop)
    {
        std::shared_ptr<const std::vector<uint8_t>> frame;
        {
            std::unique_lock<std::mutex> lock(c->mtx);
            c->cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return g_stop || c->closed.load() || !c->q.empty();
            });

            if (g_stop || c->closed.load())
                break;

            if (c->q.empty())
                continue;

            const size_t sz = c->q.front() ? c->q.front()->size() : 0;
            frame = c->q.front();
            c->q.pop_front();
            c->queuedBytes -= sz;
        }

        if (!frame || frame->empty())
            continue;

        if (!WriteAllOverlapped(c->pipe, frame->data(), static_cast<DWORD>(frame->size())))
            break;
    }


    c->closed.store(true);

    if (c->pipe != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(c->pipe);
        DisconnectNamedPipe(c->pipe);
        CloseHandle(c->pipe);
        c->pipe = INVALID_HANDLE_VALUE;
    }

    return 0;
}


static void CleanupClosedRspClients()
{
    std::lock_guard<std::mutex> lock(g_rspClientsMtx);

    for (auto& c : g_rspClients)
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

    g_rspClients.erase(std::remove_if(g_rspClients.begin(), g_rspClients.end(), [](const std::shared_ptr<RspClient>& c) {
        return !c || c->closed.load();
    }), g_rspClients.end());
}


static DWORD WINAPI RspAcceptThreadMain(LPVOID)
{
    while (!g_stop)
    {
        HANDLE hPipe = CreateNamedPipeA(
            kPacketsCoreResponsePipeName,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kRspPipeOutBufferSize,
            kRspPipeInBufferSize,
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            Sleep(200);
            continue;
        }

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
        {
            CloseHandle(hPipe);
            Sleep(50);
            continue;
        }

        BOOL ok = ConnectNamedPipe(hPipe, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED)
            {
                SetEvent(ov.hEvent);
            }
            else if (err != ERROR_IO_PENDING)
            {
                CloseHandle(ov.hEvent);
                CloseHandle(hPipe);
                Sleep(50);
                continue;
            }
        }

        HANDLE waits[2] = { ov.hEvent, g_rspStopEvent ? g_rspStopEvent : ov.hEvent };
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (wr == WAIT_OBJECT_0 + 1 || g_stop)
        {
            CancelIoEx(hPipe, &ov);
            CloseHandle(ov.hEvent);
            CloseHandle(hPipe);
            break;
        }

        CloseHandle(ov.hEvent);

        auto client = std::make_shared<RspClient>();
        client->pipe = hPipe;
        client->thread = CreateThread(nullptr, 0, RspClientWriterThreadMain, client.get(), 0, nullptr);
        if (!client->thread)
        {
            client->closed.store(true);
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_rspClientsMtx);
            g_rspClients.emplace_back(client);
        }

        CleanupClosedRspClients();
        Sleep(5);
    }

    return 0;
}

static DWORD WINAPI RspDispatchThreadMain(LPVOID)
{
    while (!g_stop)
    {
        MemoryReadResponse item;
        if (!RspQueueTryPop(item, 200))
        {
            CleanupClosedRspClients();
            continue;
        }

        auto frame = BuildRspFrameBytes(item);

        std::vector<std::shared_ptr<RspClient>> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_rspClientsMtx);
            snapshot = g_rspClients;
        }

        for (auto& c : snapshot)
        {
            if (!c || c->closed.load())
                continue;
            RspClientEnqueueFrame(c, frame);
        }

        CleanupClosedRspClients();
    }

    return 0;
}

bool StartRspPipeSender()
{
    for (auto h : g_rspAcceptThreads)
    {
        if (h)
            return true;
    }
    if (g_rspDispatchThread)
        return true;

    if (!g_rspStopEvent)
        g_rspStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (!g_rspStopEvent)
        return false;

    ResetEvent(g_rspStopEvent);

    g_rspSenderRunning.store(true);

    for (int i = 0; i < kRspAcceptThreads; ++i)
    {
        g_rspAcceptThreads[i] = CreateThread(nullptr, 0, RspAcceptThreadMain, nullptr, 0, nullptr);
        if (!g_rspAcceptThreads[i])
        {
            g_stop = true;
            if (g_rspStopEvent)
                SetEvent(g_rspStopEvent);
            for (int j = 0; j < kRspAcceptThreads; ++j)
            {
                if (g_rspAcceptThreads[j])
                {
                    WaitForSingleObject(g_rspAcceptThreads[j], 2000);
                    CloseHandle(g_rspAcceptThreads[j]);
                    g_rspAcceptThreads[j] = nullptr;
                }
            }
            g_rspSenderRunning.store(false);
            return false;
        }
    }

    g_rspDispatchThread = CreateThread(nullptr, 0, RspDispatchThreadMain, nullptr, 0, nullptr);
    if (!g_rspDispatchThread)
    {
        g_stop = true;
        if (g_rspStopEvent)
            SetEvent(g_rspStopEvent);
        for (int i = 0; i < kRspAcceptThreads; ++i)
        {
            if (g_rspAcceptThreads[i])
            {
                WaitForSingleObject(g_rspAcceptThreads[i], 2000);
                CloseHandle(g_rspAcceptThreads[i]);
                g_rspAcceptThreads[i] = nullptr;
            }
        }
        g_rspSenderRunning.store(false);
        return false;
    }

    return true;
}

void StopRspPipeSender()
{
    bool anyAccept = false;
    for (auto h : g_rspAcceptThreads)
    {
        if (h)
        {
            anyAccept = true;
            break;
        }
    }
    if (!anyAccept && !g_rspDispatchThread)
        return;

    g_stop = true;
    RspQueueNotifyAll();

    if (g_rspStopEvent)
        SetEvent(g_rspStopEvent);

    for (int i = 0; i < kRspAcceptThreads; ++i)
    {
        if (!g_rspAcceptThreads[i])
            continue;
        WaitForSingleObject(g_rspAcceptThreads[i], 2000);
        CloseHandle(g_rspAcceptThreads[i]);
        g_rspAcceptThreads[i] = nullptr;
    }

    if (g_rspDispatchThread)
    {
        WaitForSingleObject(g_rspDispatchThread, 2000);
        CloseHandle(g_rspDispatchThread);
        g_rspDispatchThread = nullptr;
    }

    std::vector<std::shared_ptr<RspClient>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_rspClientsMtx);
        snapshot = g_rspClients;
    }

    for (auto& c : snapshot)
    {
        if (!c)
            continue;
        c->closed.store(true);
        c->cv.notify_all();
        if (c->thread)
        {
            WaitForSingleObject(c->thread, 1000);
            CloseHandle(c->thread);
            c->thread = nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_rspClientsMtx);
        g_rspClients.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_rspMutex);
        while (!g_rspQueue.empty())
            g_rspQueue.pop();
    }

    g_rspSenderRunning.store(false);
}
