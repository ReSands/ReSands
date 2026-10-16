// Live native window, game side: sends each captured frame (PNF5, built by pop_native_live.cpp) to
// the viewer (pop_native_view --live <port>) over a local TCP connection, from a thread of its own
// so the game never waits for the viewer: while a frame is being sent the game builds none (busy());
// reconnects every second while the viewer is not running. Kept in its own file so the socket
// headers (Winsock on Windows) stay out of the other translation units.
#include "../../../../../../tools/native/src/live_socket.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace pop_live_client
{
    namespace
    {
        std::mutex g_mutex;
        std::condition_variable g_cv, g_idle;
        std::vector<uint8_t> g_pending;
        bool g_have = false, g_started = false;
        std::atomic<uint64_t> g_connection{0};
        uint64_t g_sent = 0, g_dropped = 0;

        void run(int port)
        {
            pop_live::Socket s = pop_live::kNoSocket;
            bool warned = false;
            for (;;)
            {
                if (s == pop_live::kNoSocket)
                {
                    s = pop_live::connectLocal(port);
                    if (s == pop_live::kNoSocket)
                    {
                        if (!warned)
                            std::fprintf(stderr, "[pop:live] no viewer on 127.0.0.1:%d yet (pop_native_view live - x.png --live %d); retrying\n",
                                         port, port);
                        warned = true;
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        continue;
                    }
                    std::fprintf(stderr, "[pop:live] connected to the viewer on 127.0.0.1:%d\n", port);
                    warned = false;
                    {
                        // a frame built for the previous connection may refer to textures this viewer
                        // never got: drop it and let the game build the next one for this connection
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_have = false;
                        g_pending.clear();
                    }
                    ++g_connection;
                }
                std::vector<uint8_t> frame;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_cv.wait(lock, [] { return g_have; });
                    frame.swap(g_pending);
                    g_have = false;
                }
                g_idle.notify_all();
                if (!pop_live::sendFrame(s, frame))
                {
                    std::fprintf(stderr, "[pop:live] viewer gone; reconnecting\n");
                    pop_live::closeSocket(s);
                    s = pop_live::kNoSocket;
                    continue;
                }
                ++g_sent;
            }
        }
    }

    void start(int port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_started)
            return;
        g_started = true;
        std::thread(run, port).detach();
    }

    uint64_t connection() { return g_connection; }

    // a frame is still waiting to be sent: the game skips building the next one (frames carry each
    // texture once, so a built frame must never be dropped)
    bool busy()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_have;
    }

    void waitIdle()
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_idle.wait_for(lock, std::chrono::seconds(5), [] { return !g_have; });
    }

    void submit(std::vector<uint8_t> &&frame)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_have)
                ++g_dropped;
            g_pending = std::move(frame);
            g_have = true;
        }
        g_cv.notify_one();
    }
}
