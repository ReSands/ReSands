// Frame stream between the running game (tools/overrides/pop_native_live.cpp, client) and the live
// native window (pop_native_view --live <port>, server): TCP on the local machine, messages of
// "PNFM", u32 payload size, payload (a PNF5 frame as the frame export writes it). Header-only,
// POSIX sockets or Winsock.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h clashes with a windows.h included earlier (which brings winsock.h); the calls used here
// are in both, so take whichever is already there
#if !defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#include <winsock2.h>
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pop_live
{
#if defined(_WIN32)
    using Socket = SOCKET;
    constexpr Socket kNoSocket = INVALID_SOCKET;
    inline void closeSocket(Socket s) { closesocket(s); }
#else
    using Socket = int;
    constexpr Socket kNoSocket = -1;
    inline void closeSocket(Socket s) { close(s); }
#endif

    inline bool startup()
    {
#if defined(_WIN32)
        static bool done = false;
        if (!done)
        {
            WSADATA wsa;
            done = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }
        return done;
#else
        signal(SIGPIPE, SIG_IGN); // a closed viewer must not kill the game
        return true;
#endif
    }

    inline bool sendAll(Socket s, const void *data, size_t n)
    {
        const char *p = static_cast<const char *>(data);
        while (n)
        {
            const int k = int(send(s, p, int(n > (1u << 30) ? (1u << 30) : n), 0));
            if (k <= 0)
                return false;
            p += k;
            n -= size_t(k);
        }
        return true;
    }

    inline bool recvAll(Socket s, void *data, size_t n)
    {
        char *p = static_cast<char *>(data);
        while (n)
        {
            const int k = int(recv(s, p, int(n > (1u << 30) ? (1u << 30) : n), 0));
            if (k <= 0)
                return false;
            p += k;
            n -= size_t(k);
        }
        return true;
    }

    // client: connects to 127.0.0.1:port (kNoSocket on failure)
    inline Socket connectLocal(int port)
    {
        if (!startup())
            return kNoSocket;
        Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket)
            return kNoSocket;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0)
        {
            closeSocket(s);
            return kNoSocket;
        }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
        return s;
    }

    // server: listens on 127.0.0.1:port (kNoSocket on failure)
    inline Socket listenLocal(int port)
    {
        if (!startup())
            return kNoSocket;
        Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket)
            return kNoSocket;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(s, 1) != 0)
        {
            closeSocket(s);
            return kNoSocket;
        }
        return s;
    }

    inline bool sendFrame(Socket s, const std::vector<uint8_t> &payload)
    {
        const uint32_t n = uint32_t(payload.size());
        return sendAll(s, "PNFM", 4) && sendAll(s, &n, 4) && sendAll(s, payload.data(), payload.size());
    }

    inline bool recvFrame(Socket s, std::vector<uint8_t> &payload)
    {
        char magic[4];
        uint32_t n = 0;
        if (!recvAll(s, magic, 4) || std::memcmp(magic, "PNFM", 4) != 0 || !recvAll(s, &n, 4) || n > (512u << 20))
            return false;
        payload.resize(n);
        return recvAll(s, payload.data(), n);
    }
}
