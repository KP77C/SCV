// w3_net.cpp — an offline Winsock for WarCraft III (WS2_32 from Game.dll,
// WSOCK32 from War3.exe, both imported by ordinal).
//
// The game starts Winsock at boot and opens sockets for the LAN and
// Battle.net screens. On the Vita there is no network play yet, so this is a
// stack with no network behind it: startup and socket creation succeed,
// sends are swallowed, nothing ever arrives, and outgoing connections fail
// the way an unplugged cable would. Single-player never notices.
#include "sc_host.h"

#include "runtime/guest_scratch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using d2rt::Cpu;
using d2rt::Shim;

namespace sc {
namespace {

constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t WSAEWOULDBLOCK = 10035, WSAENOTSOCK = 10038, WSAENETUNREACH = 10051,
                   WSAENOTCONN = 10057, WSAHOST_NOT_FOUND = 11001;

uint32_t g_lasterr = 0;
uint32_t g_next_sock = 0x100;
uint32_t g_hostent = 0, g_ntoa = 0;

uint8_t rd8(Cpu& c, uint32_t va) { uint8_t b = 0; c.read(va, &b, 1); return b; }
void wr8(Cpu& c, uint32_t va, uint8_t b) { c.write(va, &b, 1); }
void wr16(Cpu& c, uint32_t va, uint16_t v) { c.write(va, &v, 2); }
void wrstr(Cpu& c, uint32_t va, const char* s) { c.write(va, s, (uint32_t)std::strlen(s) + 1); }

uint32_t fail(uint32_t err) { g_lasterr = err; return kSocketError; }
uint32_t bswap16(uint32_t v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }
uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

std::string gstr(Cpu& c, uint32_t p, size_t max = 256) {
    std::string s;
    for (size_t i = 0; p && i < max; ++i) { char ch = (char)rd8(c, p + (uint32_t)i); if (!ch) break; s += ch; }
    return s;
}

// A fd_set is { u_int fd_count; SOCKET fd_array[64]; }. Nothing is ever
// readable, writable or in error, so every set comes back empty.
void clear_fdset(Cpu& c, uint32_t p) { if (p) c.write_u32(p, 0); }

// One hostent for every lookup: the name asked for, resolving to 127.0.0.1.
uint32_t hostent_for(Cpu& c, const std::string& name) {
    if (!g_hostent) g_hostent = wx86_scratch_alloc(0x200);
    const uint32_t h = g_hostent, nm = h + 0x40, addr = h + 0x20, list = h + 0x28, alias = h + 0x30;
    std::string n = name.substr(0, 0x1BF);
    wrstr(c, nm, n.c_str());
    c.write_u32(addr, 0x0100007Fu);                 // 127.0.0.1, network order
    c.write_u32(list, addr); c.write_u32(list + 4, 0);
    c.write_u32(alias, 0);
    c.write_u32(h + 0, nm);                         // h_name
    c.write_u32(h + 4, alias);                      // h_aliases
    c.write_u32(h + 8, 2 | (4u << 16));             // h_addrtype AF_INET, h_length 4
    c.write_u32(h + 12, list);                      // h_addr_list
    return h;
}

void put_sockaddr_any(Cpu& c, uint32_t sa, uint32_t lenp) {
    if (!sa) return;
    c.write_u32(sa, 2); c.write_u32(sa + 4, 0x0100007Fu); c.write_u32(sa + 8, 0); c.write_u32(sa + 12, 0);
    if (lenp) c.write_u32(lenp, 16);
}

struct Fn { uint16_t ord; const char* name; uint8_t argc; uint32_t (*fn)(Cpu&); };

const Fn kNet[] = {
    { 1, "accept", 3,      [](Cpu&) { return fail(WSAEWOULDBLOCK); } },
    { 2, "bind", 3,        [](Cpu&) -> uint32_t { return 0; } },
    { 3, "closesocket", 1, [](Cpu&) -> uint32_t { return 0; } },
    { 4, "connect", 3,     [](Cpu&) { return fail(WSAENETUNREACH); } },
    { 5, "getpeername", 3, [](Cpu&) { return fail(WSAENOTCONN); } },
    { 6, "getsockname", 3, [](Cpu& c) -> uint32_t { put_sockaddr_any(c, c.arg(1), c.arg(2)); return 0; } },
    { 7, "getsockopt", 5,  [](Cpu& c) -> uint32_t {
        if (uint32_t v = c.arg(3)) c.write_u32(v, 0);
        if (uint32_t l = c.arg(4)) c.write_u32(l, 4);
        return 0; } },
    { 8, "htonl", 1,       [](Cpu& c) { return bswap32(c.arg(0)); } },
    { 9, "htons", 1,       [](Cpu& c) { return bswap16(c.arg(0)); } },
    { 10, "ioctlsocket", 3, [](Cpu& c) -> uint32_t {
        if (c.arg(1) == 0x4004667Fu && c.arg(2)) c.write_u32(c.arg(2), 0);   // FIONREAD: nothing queued
        return 0; } },
    { 11, "inet_addr", 1,  [](Cpu& c) -> uint32_t {
        unsigned a, b, d, e; std::string s = gstr(c, c.arg(0), 32);
        if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &d, &e) != 4 || a > 255 || b > 255 || d > 255 || e > 255)
            return 0xFFFFFFFFu;                       // INADDR_NONE
        return a | b << 8 | d << 16 | e << 24; } },
    { 12, "inet_ntoa", 1,  [](Cpu& c) -> uint32_t {
        if (!g_ntoa) g_ntoa = wx86_scratch_alloc(32);
        uint32_t v = c.arg(0); char buf[20];
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24);
        wrstr(c, g_ntoa, buf);
        return g_ntoa; } },
    { 13, "listen", 2,     [](Cpu&) -> uint32_t { return 0; } },
    { 14, "ntohl", 1,      [](Cpu& c) { return bswap32(c.arg(0)); } },
    { 15, "ntohs", 1,      [](Cpu& c) { return bswap16(c.arg(0)); } },
    { 16, "recv", 4,       [](Cpu&) { return fail(WSAEWOULDBLOCK); } },
    { 17, "recvfrom", 6,   [](Cpu&) { return fail(WSAEWOULDBLOCK); } },
    { 18, "select", 5,     [](Cpu& c) -> uint32_t {
        clear_fdset(c, c.arg(1)); clear_fdset(c, c.arg(2)); clear_fdset(c, c.arg(3));
        return 0; } },
    { 19, "send", 4,       [](Cpu& c) { return c.arg(2); } },
    { 20, "sendto", 6,     [](Cpu& c) { return c.arg(2); } },
    { 21, "setsockopt", 5, [](Cpu&) -> uint32_t { return 0; } },
    { 22, "shutdown", 2,   [](Cpu&) -> uint32_t { return 0; } },
    { 23, "socket", 3,     [](Cpu&) -> uint32_t { g_lasterr = 0; return g_next_sock += 4; } },
    { 51, "gethostbyaddr", 3, [](Cpu&) -> uint32_t { g_lasterr = WSAHOST_NOT_FOUND; return 0; } },
    { 52, "gethostbyname", 1, [](Cpu& c) -> uint32_t {
        std::string n = gstr(c, c.arg(0));
        static int logged = 0;
        if (logged++ < 4) log("[not-bound] gethostbyname(%s): no network, answering 127.0.0.1", n.c_str());
        return hostent_for(c, n); } },
    { 57, "gethostname", 2, [](Cpu& c) -> uint32_t {
        static const char kName[] = "vita";
        if (c.arg(1) < sizeof kName) return fail(10014);   // WSAEFAULT
        wrstr(c, c.arg(0), kName);
        return 0; } },
    { 111, "WSAGetLastError", 0, [](Cpu&) { return g_lasterr; } },
    { 112, "WSASetLastError", 1, [](Cpu& c) -> uint32_t { g_lasterr = c.arg(0); return 0; } },
    { 115, "WSAStartup", 2, [](Cpu& c) -> uint32_t {
        // WSADATA: wVersion, wHighVersion, szDescription[257], szSystemStatus[129],
        // iMaxSockets, iMaxUdpDg, lpVendorInfo (400 bytes on x86).
        const uint32_t d = c.arg(1), ver = c.arg(0) & 0xFFFF;
        if (!d) return 10014;
        { uint8_t z[400] = {}; c.write(d, z, sizeof z); }
        wr16(c, d, (uint16_t)(ver > 0x0202 ? 0x0202 : ver)); wr16(c, d + 2, 0x0202);
        static const char kDesc[] = "WinSock 2.0 (offline)", kStat[] = "Running";
        wrstr(c, d + 4, kDesc); wrstr(c, d + 261, kStat);
        wr16(c, d + 390, 0x7FFF); wr16(c, d + 392, 65467);
        log("[not-bound] WSAStartup(0x%04x): offline Winsock (no network play on this build)", ver);
        return 0; } },
    { 116, "WSACleanup", 0, [](Cpu&) -> uint32_t { return 0; } },
};

}  // namespace

void install_net(d2rt::Bridge& br) {
    for (const char* dll : { "WS2_32.dll", "WSOCK32.dll" })
        for (const Fn& f : kNet) {
            Shim s; s.fn = f.fn; s.argc = f.argc; s.stdcall_cleanup = true;
            s.tag = std::string(dll) + "!" + f.name;
            br.register_shim(dll, f.name, s);
            br.register_shim_ordinal(dll, f.ord, s);
        }
}

}  // namespace sc
