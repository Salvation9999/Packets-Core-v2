#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "PacketCaptureSender.h"
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
#include <thread>
#include <vector>

// ===================== Protocol Definition =====================
//
// PKT2 v2 frame format (IPv4 only)
// [magic(4)][ver(2)][type(1)][af(1)][payload_len(4)]
// [sock(8)][local_ip(4)][local_port(2)][remote_ip(4)][remote_port(2)]
//
// Header total length: 32 bytes
//
#pragma pack(push, 1)
struct FrameHeaderV2
{
    uint32_t magic;       // Magic number 'PKT2' = 0x32544B50, for fast validation
    uint16_t ver;         // Protocol version: 2
    uint8_t type;         // Packet type: 1=SEND, 2=RECV
    uint8_t af;           // Address family: 4 = IPv4
    uint32_t payload_len; // Payload size in bytes
    uint64_t sock;        // SOCKET value, passed through as-is
    uint32_t local_ip;    // Local IPv4 address (network byte order)
    uint16_t local_port;  // Local port (network byte order)
    uint32_t remote_ip;   // Remote IPv4 address (network byte order)
    uint16_t remote_port; // Remote port (network byte order)
};
#pragma pack(pop)

static constexpr uint32_t kFrameMagic_PKT2 = 0x32544B50; // 'PKT2'
static constexpr uint16_t kFrameVerV2 = 2;
static constexpr uint8_t kAddrFamilyV4 = 4;

// ===================== Global State =====================
// Sender running flag
static std::atomic<bool> g_senderRunning{ false };

// Stop event, for interrupting overlapped I/O waits
static HANDLE g_stopEvent = nullptr;

// ===================== Thread Handles =====================

// Multiple accept threads to reduce ERROR_PIPE_BUSY, create 4 threads
static constexpr int kAcceptThreads = 4;
static HANDLE g_acceptThreads[kAcceptThreads] = { nullptr };

// Dispatch thread: takes data from PacketQueue and broadcasts to all clients
static HANDLE g_dispatchThread = nullptr;

// Named pipe buffer sizes
static constexpr DWORD kPipeOutBufferSize = 1 << 20;
static constexpr DWORD kPipeInBufferSize = 1 << 16;

// Client queue policy:
// Drop old frames when client queue exceeds this size
static constexpr size_t kClientMaxQueuedBytes = 4u * 1024u * 1024u; // 4MB


// ===================== IO Helper Functions =====================

// Write len bytes to pipe using Overlapped I/O
// Supports:
// - Partial writes
// - Stop event interruption
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
                // Wait for write completion or stop event
                HANDLE waits[2] = { ov.hEvent, g_stopEvent ? g_stopEvent : ov.hEvent };
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

// ===================== Client State =====================

// Represents one connected Python client
struct Client
{
    HANDLE pipe = INVALID_HANDLE_VALUE;   // Named pipe handle
    HANDLE thread = nullptr;              // Writer thread

    // Send queue: frame data
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<const std::vector<uint8_t>>> q;
    size_t queuedBytes = 0;

    // Close flag
    std::atomic<bool> closed{ false };
};

// All connected clients
static std::mutex g_clientsMtx;
static std::vector<std::shared_ptr<Client>> g_clients;

// ===================== Frame Building =====================

// Convert PacketItem to PKT2 protocol frame
static std::shared_ptr<const std::vector<uint8_t>> BuildFrameBytes(const PacketItem& item)
{
    FrameHeaderV2 hdr{};
    hdr.magic = kFrameMagic_PKT2;
    hdr.ver = kFrameVerV2;
    hdr.type = item.type;
    hdr.af = kAddrFamilyV4;
    hdr.payload_len = static_cast<uint32_t>(item.data.size());
    hdr.sock = static_cast<uint64_t>(item.sock);
    hdr.local_ip = item.local_ipv4;
    hdr.local_port = item.local_port;
    hdr.remote_ip = item.remote_ipv4;
    hdr.remote_port = item.remote_port;

    auto out = std::make_shared<std::vector<uint8_t>>();
    out->resize(sizeof(hdr) + hdr.payload_len);
    memcpy(out->data(), &hdr, sizeof(hdr));
    if (hdr.payload_len > 0)
        memcpy(out->data() + sizeof(hdr), item.data.data(), hdr.payload_len);
    return out;
}

// ===================== Client Queue Operations =====================

// Enqueue a frame to client queue
// Drop old frames if queue is full
static void ClientEnqueueFrame(const std::shared_ptr<Client>& c, const std::shared_ptr<const std::vector<uint8_t>>& frame)
{
    if (!c || !frame)
        return;

    std::unique_lock<std::mutex> lock(c->mtx);

    if (frame->size() > kClientMaxQueuedBytes)
        return;

    while (c->queuedBytes + frame->size() > kClientMaxQueuedBytes && !c->q.empty())
    {
        c->queuedBytes -= (c->q.front() ? c->q.front()->size() : 0);
        c->q.pop_front();
    }

    if (c->queuedBytes + frame->size() > kClientMaxQueuedBytes)
        return;

    c->queuedBytes += frame->size();
    c->q.emplace_back(frame);
    lock.unlock();
    c->cv.notify_one();
}

// ===================== Client Writer Thread =====================

// Each client has a writer thread
// Read frames from queue and write to pipe
static DWORD WINAPI ClientWriterThreadMain(LPVOID p)
{
    auto c = reinterpret_cast<Client*>(p);

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

    // Mark as closed and cleanup resources
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

// ===================== Client Cleanup =====================

// Cleanup closed clients and join threads
static void CleanupClosedClients()
{
    std::lock_guard<std::mutex> lock(g_clientsMtx);

    for (auto& c : g_clients)
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

    g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(), [](const std::shared_ptr<Client>& c) {
        return !c || c->closed.load();
    }), g_clients.end());
}

// ===================== Accept Thread =====================

// Accept Python client connections
static DWORD WINAPI AcceptThreadMain(LPVOID)
{
    while (!g_stop)
    {
        HANDLE hPipe = CreateNamedPipeA(
            kPacketsCoreCapturePipeName,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kPipeOutBufferSize,
            kPipeInBufferSize,
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

        HANDLE waits[2] = { ov.hEvent, g_stopEvent ? g_stopEvent : ov.hEvent };
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (wr == WAIT_OBJECT_0 + 1 || g_stop)
        {
            CancelIoEx(hPipe, &ov);
            CloseHandle(ov.hEvent);
            CloseHandle(hPipe);
            break;
        }

        CloseHandle(ov.hEvent);

        // Create client and start writer thread
        auto client = std::make_shared<Client>();
        client->pipe = hPipe;
        client->thread = CreateThread(nullptr, 0, ClientWriterThreadMain, client.get(), 0, nullptr);
        if (!client->thread)
        {
            client->closed.store(true);
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_clientsMtx);
            g_clients.emplace_back(client);
        }

        CleanupClosedClients();
        Sleep(5);
    }

    return 0;
}

// ===================== Dispatch Thread =====================

// Take data from PacketQueue and broadcast to all clients
static DWORD WINAPI DispatchThreadMain(LPVOID)
{
    while (!g_stop)
    {
        PacketItem item;
        if (!PacketQueueTryPop(item, 200))
        {
            CleanupClosedClients();
            continue;
        }

        auto frame = BuildFrameBytes(item);

        std::vector<std::shared_ptr<Client>> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_clientsMtx);
            snapshot = g_clients;
        }

        for (auto& c : snapshot)
        {
            if (!c || c->closed.load())
                continue;
            ClientEnqueueFrame(c, frame);
        }

        CleanupClosedClients();
    }

    return 0;
}

// ===================== Public Interface =====================

// Start named pipe sender
bool StartNamedPipeSender()
{
    for (auto h : g_acceptThreads)
    {
        if (h)
            return true;
    }
    if (g_dispatchThread)
        return true;

    if (!g_stopEvent)
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (!g_stopEvent)
        return false;

    ResetEvent(g_stopEvent);

    g_senderRunning.store(true);

    // Create accept threads
    for (int i = 0; i < kAcceptThreads; ++i)
    {
        g_acceptThreads[i] = CreateThread(nullptr, 0, AcceptThreadMain, nullptr, 0, nullptr);
        if (!g_acceptThreads[i])
        {
            g_stop = true;
            if (g_stopEvent)
                SetEvent(g_stopEvent);
            for (int j = 0; j < kAcceptThreads; ++j)
            {
                if (g_acceptThreads[j])
                {
                    WaitForSingleObject(g_acceptThreads[j], 2000);
                    CloseHandle(g_acceptThreads[j]);
                    g_acceptThreads[j] = nullptr;
                }
            }
            g_senderRunning.store(false);
            return false;
        }
    }

    // Create dispatch thread
    g_dispatchThread = CreateThread(nullptr, 0, DispatchThreadMain, nullptr, 0, nullptr);
    if (!g_dispatchThread)
    {
        g_stop = true;
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        for (int i = 0; i < kAcceptThreads; ++i)
        {
            if (g_acceptThreads[i])
            {
                WaitForSingleObject(g_acceptThreads[i], 2000);
                CloseHandle(g_acceptThreads[i]);
                g_acceptThreads[i] = nullptr;
            }
        }
        g_senderRunning.store(false);
        return false;
    }

    return true;
}

// Stop named pipe sender
void StopNamedPipeSender()
{
    bool anyAccept = false;
    for (auto h : g_acceptThreads)
    {
        if (h)
        {
            anyAccept = true;
            break;
        }
    }
    if (!anyAccept && !g_dispatchThread)
        return;

    g_stop = true;
    PacketQueueNotifyAll();

    if (g_stopEvent)
        SetEvent(g_stopEvent);

    // Wait for all accept threads
    for (int i = 0; i < kAcceptThreads; ++i)
    {
        if (!g_acceptThreads[i])
            continue;
        WaitForSingleObject(g_acceptThreads[i], 2000);
        CloseHandle(g_acceptThreads[i]);
        g_acceptThreads[i] = nullptr;
    }

    // Wait for dispatch thread
    if (g_dispatchThread)
    {
        WaitForSingleObject(g_dispatchThread, 2000);
        CloseHandle(g_dispatchThread);
        g_dispatchThread = nullptr;
    }

    // Close all clients
    std::vector<std::shared_ptr<Client>> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_clientsMtx);
        snapshot = g_clients;
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
        std::lock_guard<std::mutex> lock(g_clientsMtx);
        g_clients.clear();
    }

    g_senderRunning.store(false);
}
