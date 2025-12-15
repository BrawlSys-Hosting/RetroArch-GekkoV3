/*
 * netplay_gekkonet.c
 *
 * Thin wrapper around the GekkoNet C API intended to be used as a
 * rollback netplay backend for RetroArch.
 *
 * This file deliberately avoids depending on RetroArch internals.
 * The frontend (RetroArch) must provide:
 *
 *   - A way to serialize/unserialize the emulated core.
 *   - A way to run exactly one frame (`retro_run()`).
 *   - A way to pack/unpack controller input blobs.
 *
 * High level usage (from RetroArch side):
 *
 *   1. Define ra_gekkonet_params_t based on user settings.
 *   2. Initialize a ra_gekkonet_ctx_t with ra_gekkonet_init().
 *   3. Add actors (local/remote/spectators) with ra_gekkonet_add_actor().
 *   4. Each frame:
 *        - Pack local input into a blob of size params->input_size.
 *        - Call ra_gekkonet_push_local_input().
 *        - Call ra_gekkonet_update().
 *        - In your input callback, read current frame input from
 *          ra_gekkonet_get_current_input().
 *
 * Event handlers now map directly to GekkoGameEvent using the fields
 * in deps/gekkonet/include/gekkonet.h (SaveEvent, LoadEvent, AdvanceEvent).
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>

#include "../../encodings/crc32.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "netplay_gekkonet.h"

/* Simple logging macros. Prefer RetroArch's logger when available so
 * messages show up in the normal log. Fallback to stderr otherwise.
 */
#ifndef GEKKONET_LOG
#ifdef HAVE_CONFIG_H
#include "../../verbosity.h"
#define GEKKONET_LOG(fmt, ...)  RARCH_LOG("[gekkonet] " fmt "\n", ##__VA_ARGS__)
#define GEKKONET_WARN(fmt, ...) RARCH_WARN("[gekkonet] " fmt "\n", ##__VA_ARGS__)
#define GEKKONET_ERR(fmt, ...)  RARCH_ERR("[gekkonet] " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define GEKKONET_LOG(fmt, ...)
#define GEKKONET_WARN(fmt, ...)
#define GEKKONET_ERR(fmt, ...)
#endif
#endif

/* Simple portable string copy helper (no strlcpy on MSVC). */
static void ra_gekkonet_strlcpy(char *dst, size_t dst_sz, const char *src)
{
   if (!dst || dst_sz == 0)
      return;
   if (!src)
   {
      dst[0] = '\0';
      return;
   }
#ifdef _MSC_VER
   strncpy(dst, src, dst_sz - 1);
   dst[dst_sz - 1] = '\0';
#else
   strncpy(dst, src, dst_sz - 1);
   dst[dst_sz - 1] = '\0';
#endif
}

typedef struct ra_gekkonet_udp_adapter
{
    GekkoNetAdapter api;
    int             sockfd;
    unsigned short  port;
    struct ra_gekkonet_ctx *owner;
} ra_gekkonet_udp_adapter_t;

static void ra_gekkonet_udp_adapter_destroy(ra_gekkonet_udp_adapter_t *adapter);

static ra_gekkonet_udp_adapter_t *g_udp_adapter        = NULL;
static GekkoNetResult           **g_udp_results        = NULL;
static size_t                     g_udp_results_cap    = 0;

/* TCP snapshot helpers --------------------------------------------------- */
static int ra_gekkonet_tcp_connect(const char *host, unsigned short port);
static int ra_gekkonet_tcp_listen(unsigned short port);
static int ra_gekkonet_tcp_accept(int listen_fd);
static void ra_gekkonet_tcp_close(int fd);
static bool ra_gekkonet_tcp_send_all(int fd, const void *buf, size_t len);
static ssize_t ra_gekkonet_tcp_recv_some(int fd, void *buf, size_t len);

static bool ra_gekkonet_addr_known(const ra_gekkonet_ctx_t *ctx,
                                   const char              *addr)
{
    size_t i;
    if (!ctx || !addr)
        return true;
    for (i = 0; i < ctx->remote_addrs_count; i++)
    {
        if (ctx->remote_addrs[i] && strcmp(ctx->remote_addrs[i], addr) == 0)
            return true;
    }
    return false;
}

static void ra_gekkonet_remember_addr(ra_gekkonet_ctx_t *ctx,
                                      const char        *addr)
{
    char *copy;
    if (!ctx || !addr)
        return;

    if (ra_gekkonet_addr_known(ctx, addr))
        return;

    if (ctx->remote_addrs_count >= ctx->remote_addrs_cap)
    {
        size_t new_cap = ctx->remote_addrs_cap ? ctx->remote_addrs_cap * 2 : 4;
        char **tmp = (char**)realloc(ctx->remote_addrs, new_cap * sizeof(char*));
        if (!tmp)
            return;
        memset(tmp + ctx->remote_addrs_cap, 0,
               (new_cap - ctx->remote_addrs_cap) * sizeof(char*));
        ctx->remote_addrs = tmp;
        ctx->remote_addrs_cap = new_cap;
    }

    copy = (char*)malloc(strlen(addr) + 1);
    if (!copy)
        return;
    strcpy(copy, addr);

    ctx->remote_addrs[ctx->remote_addrs_count++] = copy;
}
static void ra_gekkonet_udp_send(GekkoNetAddress *addr,
                                 const char      *data,
                                 int              length);
static GekkoNetResult **ra_gekkonet_udp_receive(int *length);

static void ra_gekkonet_send_probe_str(const char *addr_string);

/* TCP snapshot channel helpers ------------------------------------------- */
static bool ra_gekkonet_tcp_ensure_connection(ra_gekkonet_ctx_t *ctx)
{
   if (!ctx || ctx->tcp_port == 0)
      return false;

   /* Client actively connects. */
   if (ctx->tcp_fd < 0 && ctx->tcp_is_client && ctx->tcp_host[0])
   {
      ctx->tcp_fd = ra_gekkonet_tcp_connect(ctx->tcp_host, ctx->tcp_port);
      if (ctx->tcp_fd >= 0)
         GEKKONET_LOG("TCP snapshot connected to %s:%hu", ctx->tcp_host, ctx->tcp_port);
      else
         GEKKONET_WARN("TCP snapshot connect failed (%s:%hu)", ctx->tcp_host, ctx->tcp_port);
   }

   /* Host listens and accepts one peer. */
   if (!ctx->tcp_is_client && ctx->tcp_fd < 0)
   {
      if (ctx->tcp_listen_fd < 0)
      {
         ctx->tcp_listen_fd = ra_gekkonet_tcp_listen(ctx->tcp_port);
         if (ctx->tcp_listen_fd < 0)
            GEKKONET_WARN("TCP snapshot listen failed on %hu", ctx->tcp_port);
         else
            GEKKONET_LOG("TCP snapshot listening on %hu", ctx->tcp_port);
      }
      if (ctx->tcp_listen_fd >= 0)
      {
         int fd = ra_gekkonet_tcp_accept(ctx->tcp_listen_fd);
         if (fd >= 0)
         {
            ctx->tcp_fd = fd;
            GEKKONET_LOG("TCP snapshot accepted connection");
         }
      }
   }
   return ctx->tcp_fd >= 0;
}

static bool ra_gekkonet_tcp_send_snapshot(ra_gekkonet_ctx_t *ctx,
                                          const void *data,
                                          unsigned int size,
                                          unsigned int crc,
                                          unsigned int frame)
{
   struct snapshot_hdr {
      uint32_t magic;
      uint32_t size;
      uint32_t crc;
      uint32_t frame;
   } hdr;

   if (!ctx || !data || size == 0)
      return false;
   if (!ra_gekkonet_tcp_ensure_connection(ctx))
      return false;

   hdr.magic = htonl(0x474B534E); /* "GKSN" */
   hdr.size  = htonl(size);
   hdr.crc   = htonl(crc);
   hdr.frame = htonl(frame);

   if (!ra_gekkonet_tcp_send_all(ctx->tcp_fd, &hdr, sizeof(hdr)))
      return false;
   if (!ra_gekkonet_tcp_send_all(ctx->tcp_fd, data, size))
      return false;

   GEKKONET_LOG("TCP snapshot sent size=%u crc=%08X frame=%u", size, crc, frame);
   return true;
}

static void ra_gekkonet_poll_tcp_snapshot(ra_gekkonet_ctx_t *ctx)
{
   struct snapshot_hdr {
      uint32_t magic;
      uint32_t size;
      uint32_t crc;
      uint32_t frame;
   } hdr;

   if (!ctx || ctx->tcp_fd < 0)
      return;

   /* If we don't have the header yet, try to read it. */
   if (!ctx->tcp_snap_header_read)
   {
      ssize_t r = ra_gekkonet_tcp_recv_some(ctx->tcp_fd, &hdr, sizeof(hdr));
      if (r == 0)
      {
         GEKKONET_WARN("TCP snapshot socket closed");
         ra_gekkonet_tcp_close(ctx->tcp_fd);
         ctx->tcp_fd = -1;
         if (ctx->tcp_snap_buf) { free(ctx->tcp_snap_buf); ctx->tcp_snap_buf = NULL; }
         ctx->tcp_snap_expected = ctx->tcp_snap_received = 0;
         ctx->tcp_snap_header_read = false;
         ctx->tcp_snap_crc = 0;
         ctx->tcp_snap_frame = 0;
         return;
      }
      if (r == (ssize_t)sizeof(hdr))
      {
         hdr.magic = ntohl(hdr.magic);
         hdr.size  = ntohl(hdr.size);
         hdr.crc   = ntohl(hdr.crc);
         hdr.frame = ntohl(hdr.frame);
         if (hdr.magic != 0x474B534E || hdr.size == 0)
         {
            GEKKONET_WARN("TCP snapshot header invalid (magic=%08X size=%u)", hdr.magic, hdr.size);
            return;
         }
         if (ctx->tcp_snap_buf)
         {
            free(ctx->tcp_snap_buf);
            ctx->tcp_snap_buf = NULL;
         }
         ctx->tcp_snap_buf = (unsigned char*)malloc(hdr.size);
         if (!ctx->tcp_snap_buf)
         {
            GEKKONET_WARN("TCP snapshot alloc failed (%u bytes)", hdr.size);
            return;
         }
         ctx->tcp_snap_expected = hdr.size;
         ctx->tcp_snap_received = 0;
         ctx->tcp_snap_header_read = true;
         ctx->tcp_snap_crc = hdr.crc;
         ctx->tcp_snap_frame = hdr.frame;
      }
      return;
   }

   if (ctx->tcp_snap_header_read && ctx->tcp_snap_buf && ctx->tcp_snap_received < ctx->tcp_snap_expected)
   {
      ssize_t r = ra_gekkonet_tcp_recv_some(ctx->tcp_fd,
                                            ctx->tcp_snap_buf + ctx->tcp_snap_received,
                                            ctx->tcp_snap_expected - ctx->tcp_snap_received);
      if (r == 0)
      {
         GEKKONET_WARN("TCP snapshot socket closed during payload");
         ra_gekkonet_tcp_close(ctx->tcp_fd);
         ctx->tcp_fd = -1;
         if (ctx->tcp_snap_buf) { free(ctx->tcp_snap_buf); ctx->tcp_snap_buf = NULL; }
         ctx->tcp_snap_expected = ctx->tcp_snap_received = 0;
         ctx->tcp_snap_header_read = false;
         ctx->tcp_snap_crc = 0;
         ctx->tcp_snap_frame = 0;
         return;
      }
      if (r > 0)
         ctx->tcp_snap_received += (unsigned int)r;
   }

   if (ctx->tcp_snap_header_read &&
       ctx->tcp_snap_buf &&
       ctx->tcp_snap_received >= ctx->tcp_snap_expected)
   {
      unsigned int crc = ctx->tcp_snap_crc;
      unsigned int frame = ctx->tcp_snap_frame;
      if (crc == 0)
      {
         crc = 0xFFFFFFFFu;
         for (unsigned int i = 0; i < ctx->tcp_snap_expected; i++)
         {
            crc ^= ctx->tcp_snap_buf[i];
            for (int j = 0; j < 8; j++)
               crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
         }
         crc ^= 0xFFFFFFFFu;
      }
      gekko_queue_snapshot_apply(ctx->session,
                                 ctx->tcp_snap_buf,
                                 ctx->tcp_snap_expected,
                                 crc,
                                 frame);
      GEKKONET_LOG("TCP snapshot received size=%u crc=%08X", ctx->tcp_snap_expected, crc);

      free(ctx->tcp_snap_buf);
      ctx->tcp_snap_buf = NULL;
      ctx->tcp_snap_expected = ctx->tcp_snap_received = 0;
      ctx->tcp_snap_header_read = false;
      ctx->tcp_snap_crc = 0;
      ctx->tcp_snap_frame = 0;
   }
}

#ifdef _WIN32
static bool ra_gekkonet_wsa_init(void)
{
    static bool initialized = false;
    if (initialized)
        return true;

    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
    }

    initialized = true;
    return true;
}
#else
static bool ra_gekkonet_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

static void ra_gekkonet_udp_free(void *ptr)
{
    if (ptr)
        free(ptr);
}

static void ra_gekkonet_udp_close(int fd)
{
    if (fd < 0)
        return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

/* --- TCP helpers (for snapshot transfer) ------------------------------- */
static void ra_gekkonet_tcp_close(int fd)
{
    if (fd < 0)
        return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static int ra_gekkonet_tcp_connect(const char *host, unsigned short port)
{
    if (!host || !*host)
        return -1;
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* Make non-blocking to avoid UI freeze. */
#ifdef _WIN32
    {
        u_long on = 1;
        ioctlsocket(fd, FIONBIO, &on);
    }
#else
    ra_gekkonet_set_nonblock(fd);
#endif

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
    {
        ra_gekkonet_tcp_close(fd);
        return -1;
    }
    int res = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (res == 0)
        return fd;

    /* If in-progress, poll briefly for completion to avoid long stalls. */
#ifdef _WIN32
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
    {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv = {0, 100 * 1000}; /* 100ms */
        if (select(fd + 1, NULL, &wset, NULL, &tv) > 0)
        {
            int so_error = 0;
            int slen = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &slen);
            if (so_error == 0)
                return fd;
        }
    }
#else
    if (errno == EINPROGRESS)
    {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv = {0, 100 * 1000}; /* 100ms */
        if (select(fd + 1, NULL, &wset, NULL, &tv) > 0)
        {
            int so_error = 0;
            socklen_t slen = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen);
            if (so_error == 0)
                return fd;
        }
    }
#endif

    ra_gekkonet_tcp_close(fd);
    return -1;
}

static int ra_gekkonet_tcp_listen(unsigned short port)
{
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0)
    {
        ra_gekkonet_tcp_close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0)
    {
        ra_gekkonet_tcp_close(fd);
        return -1;
    }
#ifndef _WIN32
    ra_gekkonet_set_nonblock(fd);
#else
    {
        u_long on = 1;
        ioctlsocket(fd, FIONBIO, &on);
    }
#endif
    return fd;
}

static int ra_gekkonet_tcp_accept(int listen_fd)
{
    if (listen_fd < 0)
        return -1;
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int fd = (int)accept(listen_fd, (struct sockaddr*)&sa, &slen);
    if (fd < 0)
        return -1;
    return fd;
}

static bool ra_gekkonet_tcp_send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char*)buf;
    size_t sent = 0;
    while (sent < len)
    {
        int r = (int)send(fd, p + sent, (int)(len - sent), 0);
        if (r <= 0)
            return false;
        sent += (size_t)r;
    }
    return true;
}

static ssize_t ra_gekkonet_tcp_recv_some(int fd, void *buf, size_t len)
{
#ifdef MSG_DONTWAIT
    return recv(fd, (char*)buf, (int)len, MSG_DONTWAIT);
#else
    return recv(fd, (char*)buf, (int)len, 0);
#endif
}

static ra_gekkonet_udp_adapter_t *ra_gekkonet_udp_adapter_create(unsigned short port)
{
    ra_gekkonet_udp_adapter_t *adapter;
    struct sockaddr_in addr;
    bool tried_ephemeral = false;

#ifdef _WIN32
    if (!ra_gekkonet_wsa_init())
        return NULL;
#endif

    adapter = (ra_gekkonet_udp_adapter_t*)calloc(1, sizeof(*adapter));
    if (!adapter)
        return NULL;

    adapter->sockfd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (adapter->sockfd < 0)
    {
        free(adapter);
        return NULL;
    }

#ifndef _WIN32
    if (!ra_gekkonet_set_nonblock(adapter->sockfd))
    {
        ra_gekkonet_udp_close(adapter->sockfd);
        free(adapter);
        return NULL;
    }
#else
    {
        u_long on = 1;
        if (ioctlsocket(adapter->sockfd, FIONBIO, &on) != 0)
        {
            ra_gekkonet_udp_close(adapter->sockfd);
            free(adapter);
            return NULL;
        }
    }
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

   if (bind(adapter->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
   {
        /* If the requested port is in use (e.g., host+client on same machine),
         * fall back to an ephemeral port so the session can still start. */
        if (port != 0)
        {
            GEKKONET_ERR("UDP bind failed on port %hu", port);
            tried_ephemeral = true;
            addr.sin_port = htons(0);
            if (bind(adapter->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            {
                ra_gekkonet_udp_close(adapter->sockfd);
                free(adapter);
                return NULL;
            }
        }
        else
        {
            /* Port zero was requested but still failed. */
            ra_gekkonet_udp_close(adapter->sockfd);
            free(adapter);
            return NULL;
        }
    }

    /* Record the actual bound port for logging. */
    {
        struct sockaddr_in bound;
        socklen_t blen = sizeof(bound);
        if (getsockname(adapter->sockfd, (struct sockaddr*)&bound, &blen) == 0)
            adapter->port = ntohs(bound.sin_port);
        else
            adapter->port = port;
    }

    if (tried_ephemeral)
        GEKKONET_WARN("Falling back to ephemeral UDP port %hu (requested %hu was busy)", adapter->port, port);

    adapter->api.send_data    = ra_gekkonet_udp_send;
    adapter->api.receive_data = ra_gekkonet_udp_receive;
    adapter->api.free_data    = ra_gekkonet_udp_free;

    g_udp_adapter             = adapter;
    return adapter;
}

static void ra_gekkonet_udp_adapter_destroy(ra_gekkonet_udp_adapter_t *adapter)
{
    if (!adapter)
        return;

    ra_gekkonet_udp_close(adapter->sockfd);
    adapter->sockfd = -1;

    if (g_udp_results)
    {
        free(g_udp_results);
        g_udp_results     = NULL;
        g_udp_results_cap = 0;
    }

    adapter->owner = NULL;

    if (g_udp_adapter == adapter)
        g_udp_adapter = NULL;

    free(adapter);
}

static bool ra_gekkonet_parse_addr(const GekkoNetAddress *addr,
                                   char *host,
                                   size_t host_sz,
                                   unsigned short *port)
{
    const size_t len = addr ? addr->size : 0;
    char         buf[128];
    const char  *colon;

    if (!addr || !addr->data || len == 0 || len >= sizeof(buf))
        return false;

    memcpy(buf, addr->data, len);
    buf[len] = '\0';

    colon = strrchr(buf, ':');
    if (!colon || colon == buf)
        return false;

    if (port)
        *port = (unsigned short)strtoul(colon + 1, NULL, 10);

    if (host && host_sz)
    {
        size_t host_len = (size_t)(colon - buf);
        if (host_len >= host_sz)
            host_len = host_sz - 1;
        memcpy(host, buf, host_len);
        host[host_len] = '\0';
    }
    return true;
}

static bool ra_gekkonet_normalize_addr(const char *addr_in,
                                       char       *out,
                                       size_t      out_sz)
{
    const char *colon;
    char host[128];
    char portstr[16];
    struct addrinfo hints, *res = NULL;

    if (!addr_in || !out || out_sz == 0)
        return false;

    colon = strrchr(addr_in, ':');
    if (!colon || colon == addr_in)
        return false;

    {
        size_t host_len = (size_t)(colon - addr_in);
        if (host_len >= sizeof(host))
            host_len = sizeof(host) - 1;
        memcpy(host, addr_in, host_len);
        host[host_len] = '\0';
    }
    snprintf(portstr, sizeof(portstr), "%s", colon + 1);

    /* If host is already numeric, keep it. */
    {
        struct in_addr tmp;
        if (inet_pton(AF_INET, host, &tmp) == 1)
        {
            snprintf(out, out_sz, "%s:%s", host, portstr);
            return true;
        }
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return false;

    {
        char ipbuf[64];
        bool ok = false;
        if (res->ai_addr &&
            inet_ntop(AF_INET, &((struct sockaddr_in*)res->ai_addr)->sin_addr,
                      ipbuf, sizeof(ipbuf)))
        {
            snprintf(out, out_sz, "%s:%s", ipbuf, portstr);
            ok = true;
        }
        freeaddrinfo(res);
        return ok;
    }
}

static void ra_gekkonet_udp_send(GekkoNetAddress *addr,
                                 const char      *data,
                                 int              length)
{
    struct sockaddr_in dst;
   char host[64];
   unsigned short port = 0;

   if (!g_udp_adapter || !addr || !data || length <= 0)
       return;

   memset(&dst, 0, sizeof(dst));
   if (!ra_gekkonet_parse_addr(addr, host, sizeof(host), &port))
       return;

   dst.sin_family = AF_INET;
   dst.sin_port   = htons(port);
   if (inet_pton(AF_INET, host, &dst.sin_addr) != 1)
       return;

   GEKKONET_LOG("UDP send to %s:%hu len=%d", host, port, length);
    sendto(g_udp_adapter->sockfd, data, length, 0,
           (struct sockaddr*)&dst, (socklen_t)sizeof(dst));
}

/* Fire a small UDP packet to prime NATs and trigger host auto-add. */
static void ra_gekkonet_send_probe_str(const char *addr_string)
{
    GekkoNetAddress addr;
    const char ping[] = "hi";

    if (!g_udp_adapter || !addr_string || !*addr_string)
        return;

    memset(&addr, 0, sizeof(addr));
    addr.data = (void*)addr_string;
    addr.size = (unsigned int)strlen(addr_string);

    GEKKONET_LOG("Sending UDP probe to %s", addr_string);
    ra_gekkonet_udp_send(&addr, ping, (int)sizeof(ping));
}

void ra_gekkonet_send_probe(const char *addr_string)
{
    ra_gekkonet_send_probe_str(addr_string);
}

static GekkoNetResult **ra_gekkonet_udp_receive(int *length)
{
    int count = 0;

    if (length)
        *length = 0;

    if (!g_udp_adapter || !length)
        return NULL;

    for (;;)
    {
        unsigned char buffer[2048];
        struct sockaddr_in src;
#ifdef _WIN32
        int               slen = (int)sizeof(src);
#else
        socklen_t         slen = (socklen_t)sizeof(src);
#endif
        int recvd;

        memset(&src, 0, sizeof(src));
        recvd = (int)recvfrom(g_udp_adapter->sockfd, (char*)buffer,
                              sizeof(buffer), 0, (struct sockaddr*)&src, &slen);

        if (recvd <= 0)
        {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR)
                break;
            GEKKONET_WARN("UDP recv error %d", err);
#else
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
                break;
            GEKKONET_WARN("UDP recv error %d", errno);
#endif
            break;
        }

        if ((size_t)count >= g_udp_results_cap)
        {
            size_t new_cap = g_udp_results_cap ? g_udp_results_cap * 2 : 8;
            void  *tmp     = realloc(g_udp_results, new_cap * sizeof(*g_udp_results));
            if (!tmp)
                break;
            g_udp_results     = (GekkoNetResult**)tmp;
            g_udp_results_cap = new_cap;
        }

        {
            char ipbuf[64];
            char addrbuf[96];
            const char *ip = inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
            unsigned short port = ntohs(src.sin_port);
            GekkoNetResult *res = (GekkoNetResult*)malloc(sizeof(*res));
            size_t addr_len;

            if (!res || !ip)
            {
                free(res);
                break;
            }

            snprintf(addrbuf, sizeof(addrbuf), "%s:%hu", ip, port);
            addr_len      = strlen(addrbuf);
            res->addr.data = malloc(addr_len);
            if (!res->addr.data)
            {
                free(res);
                break;
            }
            memcpy(res->addr.data, addrbuf, addr_len);
            res->addr.size = (unsigned int)addr_len;

            res->data = malloc((size_t)recvd);
            if (!res->data)
            {
                ra_gekkonet_udp_free(res->addr.data);
                free(res);
                break;
            }
            memcpy(res->data, buffer, (size_t)recvd);
            res->data_len = (unsigned int)recvd;

            if (g_udp_adapter && g_udp_adapter->owner)
            {
            ra_gekkonet_ctx_t *owner = g_udp_adapter->owner;
            if (!ra_gekkonet_addr_known(owner, addrbuf) &&
                owner->remote_actor_count + owner->local_actor_count < (int)owner->cfg.num_players)
            {
                GEKKONET_LOG("Auto-adding remote actor for %s", addrbuf);
                if (ra_gekkonet_add_actor(owner, RemotePlayer, addrbuf) < 0)
                    GEKKONET_WARN("Failed to auto-add remote actor for %s", addrbuf);
                    else
                        GEKKONET_LOG("Auto-add success for %s", addrbuf);
                }
            }

            g_udp_results[count++] = res;
        }
    }

    /* Throttle noisy UDP logging: show first 4 batches then every 60th. */
    {
        static unsigned recv_tick = 0;
        if (count > 0 && (recv_tick < 4 || (recv_tick % 60) == 0))
        {
            const char *last = (const char*)g_udp_results[count - 1]->addr.data;
            GEKKONET_LOG("UDP recv batch count=%d last=%s len=%u",
                  count, last ? last : "(null)", g_udp_results[count - 1]->data_len);
        }
        recv_tick++;
    }

    *length = count;
    return count > 0 ? g_udp_results : NULL;
}

/* Optional helper: expose current input pointer so RetroArch's
 * input_state_net() can fetch it.
 */
const void *ra_gekkonet_get_current_input(const ra_gekkonet_ctx_t *ctx)
{
    return ctx ? ctx->current_input : NULL;
}

/* Initialize GekkoNet session with given parameters and callbacks.
 * Returns true on success, false on failure.
 */
bool ra_gekkonet_init(ra_gekkonet_ctx_t              *ctx,
                      const ra_gekkonet_params_t     *params,
                      ra_gekkonet_save_state_cb       save_cb,
                      ra_gekkonet_load_state_cb       load_cb)
{
   if (!ctx || !params)
       return false;

   memset(ctx, 0, sizeof(*ctx));

  ctx->save_cb      = save_cb;
  ctx->load_cb      = load_cb;
  ctx->run_frame_cb = NULL; /* set later */
  ctx->state_size   = params->state_size;
  ctx->input_size   = params->input_size;
  ctx->current_input_buf = NULL;
   ctx->current_input = NULL;
   ctx->owns_adapter = false;
   ctx->advanced_frame = false;
   ctx->bound_port      = 0;
   ctx->tcp_fd          = -1;
   ctx->tcp_listen_fd   = -1;
   ctx->tcp_port        = 0;
   ctx->tcp_is_client   = false;
   ctx->tcp_host[0]     = '\0';
   ctx->tcp_snap_buf    = NULL;
   ctx->tcp_snap_expected = 0;
   ctx->tcp_snap_received = 0;
   ctx->tcp_snap_header_read = false;
   ctx->tcp_snap_crc = 0;
   ctx->tcp_snap_frame = 0;
   ctx->actor_count = 0;
   for (int i = 0; i < MAX_USERS; i++) {
      ctx->actor_handles[i] = -1;
        ctx->actor_ports[i] = -1;
   }
   ctx->queued_inputs      = NULL;
   ctx->queued_input_head  = 0;
   ctx->queued_input_count = 0;
   ctx->queued_input_cap   = 0;
   ctx->input_blob_size    = 0;

    if (!gekko_create(&ctx->session))
    {
        GEKKONET_ERR("gekko_create() failed");
        return false;
    }

    memset(&ctx->cfg, 0, sizeof(ctx->cfg));
    ctx->cfg.num_players             = params->num_players;
    ctx->cfg.max_spectators          = params->max_spectators;
    ctx->cfg.input_prediction_window = params->input_prediction_window;
    ctx->cfg.spectator_delay         = params->spectator_delay;
    ctx->cfg.input_size              = params->input_size;
    ctx->cfg.state_size              = params->state_size;
   ctx->cfg.limited_saving          = params->limited_saving;
   ctx->cfg.post_sync_joining       = params->post_sync_joining;
   ctx->cfg.desync_detection        = params->desync_detection;

   size_t buf_sz = (size_t)params->input_size * MAX_USERS;
   ctx->current_input_buf = calloc(1, buf_sz);
   if (!ctx->current_input_buf)
   {
       GEKKONET_ERR("allocating input buffer (%zu bytes) failed", buf_sz);
       gekko_destroy(ctx->session);
       ctx->session = NULL;
       return false;
   }
   ctx->current_input = ctx->current_input_buf;
   ctx->input_blob_size = (size_t)ctx->cfg.input_size * ctx->cfg.num_players;
   if (ctx->input_blob_size == 0)
      ctx->input_blob_size = buf_sz;

   /* Preallocate a small ring buffer for queued AdvanceEvent inputs. */
   ctx->queued_input_cap  = 256;
   if (ctx->input_blob_size == 0 ||
         (ctx->queued_inputs = (unsigned char*)calloc(
            ctx->queued_input_cap, ctx->input_blob_size)) == NULL)
   {
      ctx->queued_input_cap = 0;
      GEKKONET_WARN("queued input buffer unavailable (size=%zu)", ctx->input_blob_size);
   }
   /* Allow saves/loads immediately; some backends request a save before any advance. */
   ctx->ready_for_state = true;
   ctx->bound_port      = 0;

   GEKKONET_LOG("cfg: num_players=%u input_size=%u state_size=%u",
                 (unsigned)ctx->cfg.num_players,
                 (unsigned)ctx->cfg.input_size,
                 (unsigned)ctx->cfg.state_size);

   /* Use a simple UDP adapter bound to the requested port. */
   ctx->adapter = (GekkoNetAdapter*)ra_gekkonet_udp_adapter_create(params->port);
   if (!ctx->adapter)
   {
       GEKKONET_ERR("gekkonet udp adapter (%hu) failed", params->port);
        gekko_destroy(ctx->session);
        ctx->session = NULL;
        return false;
   }
   ctx->owns_adapter = true;
   ((ra_gekkonet_udp_adapter_t*)ctx->adapter)->owner = ctx;
   ctx->bound_port = ((ra_gekkonet_udp_adapter_t*)ctx->adapter)->port;
   GEKKONET_LOG("gekkonet udp adapter created adapter=%p bound_port=%hu", (void*)ctx->adapter, ctx->bound_port);

   gekko_start(ctx->session, &ctx->cfg);
   gekko_net_adapter_set(ctx->session, ctx->adapter);

   ctx->active = true;
    GEKKONET_LOG("GekkoNet session started: %u players, %u spectators (port=%hu)",
                 (unsigned)ctx->cfg.num_players,
                 (unsigned)ctx->cfg.max_spectators,
                 ctx->bound_port);
    return true;
}

/* Set callback used when GekkoNet tells us to advance a frame. */
void ra_gekkonet_set_run_frame_cb(ra_gekkonet_ctx_t       *ctx,
                                  ra_gekkonet_run_frame_cb cb)
{
    if (!ctx)
        return;
    ctx->run_frame_cb = cb;
}

/* Set optional callback for high-level session events (connect/disconnect/etc). */
void ra_gekkonet_set_session_event_cb(ra_gekkonet_ctx_t            *ctx,
                                      ra_gekkonet_session_event_cb  cb,
                                      void                         *userdata)
{
    if (!ctx)
        return;
    ctx->session_event_cb       = cb;
    ctx->session_event_userdata = userdata;
}

/* Explicitly set which RetroArch port a given actor handle maps to. */
bool ra_gekkonet_set_actor_port(ra_gekkonet_ctx_t *ctx, int handle, int port)
{
    if (!ctx || port < 0 || port >= MAX_USERS || handle < 0)
        return false;
    /* Find existing entry */
    for (int i = 0; i < ctx->actor_count; i++)
    {
        if (ctx->actor_handles[i] == handle)
        {
            ctx->actor_ports[i] = port;
            return true;
        }
    }
    /* If not found and space remains, add it. */
    if (ctx->actor_count < MAX_USERS)
    {
        ctx->actor_handles[ctx->actor_count] = handle;
        ctx->actor_ports[ctx->actor_count]   = port;
        ctx->actor_count++;
        return true;
    }
    return false;
}

void ra_gekkonet_set_tcp_params(ra_gekkonet_ctx_t *ctx,
                                bool               is_client,
                                const char        *peer_host,
                                unsigned short     port)
{
    if (!ctx)
        return;
    ctx->tcp_is_client = is_client;
    ctx->tcp_port = port;
    ctx->tcp_host[0] = '\0';
    if (peer_host && *peer_host)
        ra_gekkonet_strlcpy(ctx->tcp_host, sizeof(ctx->tcp_host), peer_host);
}

/* Destroy session and free resources. */
void ra_gekkonet_deinit(ra_gekkonet_ctx_t *ctx)
{
   if (!ctx || !ctx->active)
       return;

   ra_gekkonet_tcp_close(ctx->tcp_fd);
   ra_gekkonet_tcp_close(ctx->tcp_listen_fd);
   ctx->tcp_fd = ctx->tcp_listen_fd = -1;
   if (ctx->tcp_snap_buf) {
      free(ctx->tcp_snap_buf);
      ctx->tcp_snap_buf = NULL;
      ctx->tcp_snap_expected = ctx->tcp_snap_received = 0;
      ctx->tcp_snap_header_read = false;
      ctx->tcp_snap_crc = 0;
      ctx->tcp_snap_frame = 0;
   }

   /* NOTE: GekkoNet manages the lifetime of adapter/session; you just
    * destroy the session. If your adapter allocated any extra memory,
    * free it here.
    */
   if (ctx->session)
        gekko_destroy(ctx->session);

    if (ctx->owns_adapter && ctx->adapter)
    {
        ra_gekkonet_udp_adapter_destroy((ra_gekkonet_udp_adapter_t*)ctx->adapter);
        ctx->adapter = NULL;
    }

    for (size_t i = 0; i < ctx->remote_addrs_count; i++)
        free(ctx->remote_addrs[i]);
    free(ctx->remote_addrs);
    ctx->remote_addrs       = NULL;
    ctx->remote_addrs_count = 0;
    ctx->remote_addrs_cap   = 0;

   if (ctx->current_input_buf)
       free(ctx->current_input_buf);
   ctx->current_input_buf  = NULL;
   ctx->current_input      = NULL;

   if (ctx->queued_inputs)
      free(ctx->queued_inputs);
   ctx->queued_inputs      = NULL;
   ctx->queued_input_head  = 0;
   ctx->queued_input_count = 0;
   ctx->queued_input_cap   = 0;
   ctx->input_blob_size    = 0;

   ctx->session       = NULL;
   ctx->owns_adapter  = false;
   ctx->active        = false;
   ctx->local_actor_count  = 0;
    ctx->remote_actor_count = 0;
}

/* Add an actor (local/remote/spectator).
 *
 * addr_string:
 *   - For RemotePlayer/Spectator: something like "ip:port".
 *   - For LocalPlayer: may be NULL or ignored, depending on how GekkoNet
 *     uses addresses. Check gekkonet.h.
 *
 * Returns actor handle (>= 0) or < 0 on failure.
 */
int ra_gekkonet_add_actor(ra_gekkonet_ctx_t *ctx,
                          GekkoPlayerType     type,
                          const char         *addr_string)
{
    GekkoNetAddress addr;
    int handle;
    char normalized[128];
    const char *addr_to_use = addr_string;

    if (!ctx || !ctx->session)
        return -1;

    if (type == RemotePlayer &&
        ctx->remote_actor_count + ctx->local_actor_count >= (int)ctx->cfg.num_players)
    {
        GEKKONET_WARN("max players reached; ignoring remote actor");
        return -1;
    }

    if (addr_string && ra_gekkonet_normalize_addr(addr_string, normalized, sizeof(normalized)))
        addr_to_use = normalized;

    memset(&addr, 0, sizeof(addr));

   if (addr_to_use && *addr_to_use)
   {
      /* Store the string with a terminator, but keep size set to the
       * non-NUL length so NetAddress comparisons match received packets. */
      size_t len = strlen(addr_to_use);
      char  *buf = (char*)malloc(len + 1);
      if (!buf)
         return -1;
      memcpy(buf, addr_to_use, len);
      buf[len]  = '\0';
      addr.data = buf;
      addr.size = (unsigned int)len;
   }
    else
    {
        addr.data = NULL;
        addr.size = 0;
    }

   handle = gekko_add_actor(ctx->session, type, &addr);
    if (handle < 0)
    {
        GEKKONET_ERR("gekko_add_actor() failed (type=%d)", (int)type);
        if (addr.data)
            free(addr.data);
        return -1;
    }

    /* Record handle; port can be assigned later explicitly. */
    if (ctx->actor_count < MAX_USERS)
    {
        ctx->actor_handles[ctx->actor_count] = handle;
        ctx->actor_ports[ctx->actor_count]   = -1;
        ctx->actor_count++;
    }

    GEKKONET_LOG("Added actor handle %d (type=%d)%s%s port=%d",
                 handle, (int)type,
                 addr_to_use ? " addr=" : "",
                 addr_to_use ? addr_to_use : "",
                 (ctx->actor_count > 0) ? ctx->actor_ports[ctx->actor_count - 1] : -1);

    /* If this is an explicit remote actor with a known address, send a probe now. */
    if (type == RemotePlayer && addr_to_use && *addr_to_use)
        ra_gekkonet_send_probe_str(addr_to_use);
    /* NOTE: If addr_string was duplicated, you should keep it alive for
     * as long as the actor exists. For brevity, this skeleton doesn't
     * track them; consider extending ra_gekkonet_ctx_t to do so.
     */

    if (type == LocalPlayer)
        ctx->local_actor_count++;
    else if (type == RemotePlayer)
    {
        ctx->remote_actor_count++;
        if (addr_string && *addr_string)
            ra_gekkonet_remember_addr(ctx, addr_string);
    }

    return handle;
}

/* Convenience wrapper to set local delay for an actor in frames. */
void ra_gekkonet_set_local_delay(ra_gekkonet_ctx_t *ctx,
                                 int                actor_handle,
                                 unsigned char      delay_frames)
{
    if (!ctx || !ctx->session)
        return;
    gekko_set_local_delay(ctx->session, actor_handle, delay_frames);
}

/* Grow the queued input ring when full. */
static bool ra_gekkonet_grow_queue(ra_gekkonet_ctx_t *ctx)
{
    size_t new_cap;
    unsigned char *new_buf;

    if (!ctx || ctx->input_blob_size == 0)
        return false;
    new_cap = ctx->queued_input_cap ? ctx->queued_input_cap * 2 : 256;
    new_buf = (unsigned char*)calloc(new_cap, ctx->input_blob_size);
    if (!new_buf)
        return false;

    /* Copy existing queued items in order to the new buffer. */
    for (size_t i = 0; i < ctx->queued_input_count; i++)
    {
        size_t src_idx = (ctx->queued_input_head + i) % ctx->queued_input_cap;
        memcpy(new_buf + (i * ctx->input_blob_size),
               ctx->queued_inputs + (src_idx * ctx->input_blob_size),
               ctx->input_blob_size);
    }

    free(ctx->queued_inputs);
    ctx->queued_inputs     = new_buf;
    ctx->queued_input_cap  = new_cap;
    ctx->queued_input_head = 0;
    return true;
}

bool ra_gekkonet_enqueue_current_input(ra_gekkonet_ctx_t *ctx)
{
    if (!ctx || !ctx->current_input_buf || ctx->input_blob_size == 0)
        return false;
    if (ctx->queued_input_cap == 0)
        return false;
    if (ctx->queued_input_count >= ctx->queued_input_cap)
    {
        if (!ra_gekkonet_grow_queue(ctx))
            return false;
    }

    size_t tail = (ctx->queued_input_head + ctx->queued_input_count) % ctx->queued_input_cap;
    memcpy(ctx->queued_inputs + (tail * ctx->input_blob_size),
           ctx->current_input_buf,
           ctx->input_blob_size);
    ctx->queued_input_count++;
    return true;
}

bool ra_gekkonet_dequeue_next_input(ra_gekkonet_ctx_t *ctx)
{
    if (!ctx || ctx->queued_input_count == 0 || ctx->input_blob_size == 0)
        return false;
    if (!ctx->current_input_buf)
        return false;

    /* Zero the buffer to avoid stale data in unused ports. */
    memset(ctx->current_input_buf, 0, ctx->input_blob_size);

    memcpy(ctx->current_input_buf,
           ctx->queued_inputs + (ctx->queued_input_head * ctx->input_blob_size),
           ctx->input_blob_size);
    ctx->current_input = ctx->current_input_buf;

    ctx->queued_input_head =
        (ctx->queued_input_head + 1) % ctx->queued_input_cap;
    ctx->queued_input_count--;
    return true;
}

size_t ra_gekkonet_queued_input_count(const ra_gekkonet_ctx_t *ctx)
{
    return ctx ? ctx->queued_input_count : 0;
}

/* Push a local input blob for the given actor. The blob must have the
 * same layout and size as params->input_size passed to init.
 */
bool ra_gekkonet_push_local_input(ra_gekkonet_ctx_t *ctx,
                                  int                actor_handle,
                                  const void        *input_blob)
{
    if (!ctx || !ctx->session || !input_blob)
        return false;

    gekko_add_local_input(ctx->session, actor_handle, (void*)input_blob);
    return true;
}

bool ra_gekkonet_send_snapshot(ra_gekkonet_ctx_t *ctx)
{
    if (!ctx || !ctx->session || !ctx->save_cb || !ctx->state_size)
        return false;

    void *buf = malloc(ctx->state_size);
    if (!buf)
        return false;

    unsigned int out_size = ctx->state_size;
    unsigned int crc = 0;
    bool ok = ctx->save_cb(buf, ctx->state_size, &out_size, &crc);
    if (!ok)
    {
        free(buf);
        return false;
    }
    if (out_size == 0 || out_size > ctx->state_size)
        out_size = ctx->state_size;

    /* Compute CRC if callback did not provide one. */
    if (crc == 0)
    {
        unsigned int c = 0xFFFFFFFFu;
        const unsigned char *p = (const unsigned char*)buf;
        for (unsigned int i = 0; i < out_size; i++)
        {
            c ^= p[i];
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ (0xEDB88320u & (-(int)(c & 1)));
        }
        crc = c ^ 0xFFFFFFFFu;
    }

    /* Prefer TCP snapshot channel for reliability/speed. Fallback to UDP if TCP fails. */
    bool sent = false;
    if (ctx->tcp_port)
        sent = ra_gekkonet_tcp_send_snapshot(ctx, buf, out_size, crc, 0);
    if (!sent)
        sent = gekko_send_snapshot(ctx->session, buf, out_size, crc, 0);
    if (sent)
        GEKKONET_LOG("snapshot send started (size=%u crc=%08X via %s)", out_size, crc,
                     (ctx->tcp_port && ctx->tcp_fd >= 0) ? "TCP" : "UDP");
    else
        GEKKONET_WARN("snapshot send failed");
    free(buf);
    return sent;
}

/* --- Internal helpers for event handling -------------------------------- */

static void ra_gekkonet_handle_save(ra_gekkonet_ctx_t    *ctx,
                                    const GekkoGameEvent *ev)
{
    if (!ctx || !ev || !ctx->save_cb)
        return;

    if (!ev->data.save.state || !ev->data.save.state_len)
        return;

    /* Clamp reported size to our known buffer size to avoid overruns. */
    if (*ev->data.save.state_len > ctx->state_size)
        *ev->data.save.state_len = ctx->state_size;

    if (!ctx->save_cb(ev->data.save.state,
                      *ev->data.save.state_len,
                      ev->data.save.state_len,
                      ev->data.save.checksum))
    {
        GEKKONET_WARN("save_state callback failed (frame=%d)", ev->data.save.frame);
        return;
    }

    ctx->ready_for_state = true;
}

static void ra_gekkonet_handle_load(ra_gekkonet_ctx_t    *ctx,
                                    const GekkoGameEvent *ev)
{
    if (!ctx || !ev || !ctx->load_cb)
        return;

    if (!ev->data.load.state || ev->data.load.state_len == 0)
        return;

    if (!ctx->load_cb(ev->data.load.state, ev->data.load.state_len))
    {
        GEKKONET_WARN("load_state callback failed (frame=%d, len=%u)",
            ev->data.load.frame, ev->data.load.state_len);
        return;
    }

    GEKKONET_LOG("load frame=%d len=%u", ev->data.load.frame, ev->data.load.state_len);
}

static void ra_gekkonet_handle_advance(ra_gekkonet_ctx_t    *ctx,
                                       const GekkoGameEvent *ev)
{
   if (!ctx || !ev)
       return;

    if (!ctx->current_input_buf || !ev->data.adv.inputs)
        return;

    {
        size_t cap = (size_t)ctx->input_size * MAX_USERS;
        size_t blob_sz = ctx->input_blob_size ? ctx->input_blob_size : cap;
        unsigned int to_copy = ev->data.adv.input_len;
        if (cap == 0)
            return;
        if (to_copy > blob_sz)
        {
            GEKKONET_WARN("input blob size mismatch (got %u, max %zu)",
                          ev->data.adv.input_len, blob_sz);
            to_copy = (unsigned int)blob_sz;
        }
        unsigned char *buf = (unsigned char*)ctx->current_input_buf;
        memset(buf, 0, blob_sz);
        memcpy(buf, ev->data.adv.inputs, to_copy);

        /* Reorder into port order if we have an actor map (order is actor join order). */
        if (ctx->actor_count > 0)
        {
            size_t tmp_sz             = blob_sz;
            unsigned int players      = (unsigned int)ctx->actor_count;
            unsigned int max_players  = ctx->cfg.num_players ? ctx->cfg.num_players : MAX_USERS;
            unsigned int per          = ctx->input_size;
            unsigned char *tmp        = (unsigned char*)calloc(1, tmp_sz);

            if (!tmp)
                GEKKONET_WARN("input reorder alloc failed (size=%zu)", tmp_sz);
            else
            {
                for (unsigned int idx = 0; idx < players && idx < max_players; idx++)
                {
                    int port = ctx->actor_ports[idx];
                    if (port < 0)
                        port = (int)idx; /* fallback to join order if not set */
                    if (port < 0 || port >= MAX_USERS)
                        continue;

                    /* Bounds-check against configured buffer sizes. */
                    size_t dst_off = (size_t)port * per;
                    size_t src_off = (size_t)idx * per;
                    if (dst_off + per > tmp_sz || src_off + per > blob_sz)
                        continue;

                    memcpy(tmp + dst_off, buf + src_off, per);
                }
                memcpy(buf, tmp, tmp_sz);
                free(tmp);
            }
        }
    }

    ctx->current_input = ctx->current_input_buf;

    if (!ra_gekkonet_enqueue_current_input(ctx))
        GEKKONET_WARN("advance input enqueue failed; dropping frame=%d", ev->data.adv.frame);

    GEKKONET_LOG("advance frame=%d len=%u rollback=%d (cfg input_size=%u players=%u)",
        ev->data.adv.frame,
        ev->data.adv.input_len,
        ev->data.adv.rolling_back,
        (unsigned)ctx->cfg.input_size,
        (unsigned)ctx->cfg.num_players);

    if (ctx->run_frame_cb)
        ctx->run_frame_cb();

   /* After the first successful advance/run, we can safely serialize. */
   ctx->ready_for_state = true;
   ctx->advanced_frame  = true;
}

static void ra_gekkonet_process_game_events(ra_gekkonet_ctx_t *ctx)
{
   int count = 0;
   GekkoGameEvent **events;
   int adv_cnt  = 0;
   int save_cnt = 0;
   int load_cnt = 0;
   int first_adv_frame = -1;

   if (!ctx || !ctx->session)
       return;

   ctx->current_input = NULL;
   ctx->advanced_frame = false;

   events = gekko_update_session(ctx->session, &count);
   if (!events || count <= 0)
       return;

    for (int i = 0; i < count; i++)
    {
        const GekkoGameEvent *ev = events[i];
        if (!ev)
            continue;

        switch (ev->type)
        {
            case SaveEvent:
                save_cnt++;
                ra_gekkonet_handle_save(ctx, ev);
                break;
            case LoadEvent:
                load_cnt++;
                ra_gekkonet_handle_load(ctx, ev);
                break;
            case AdvanceEvent:
                adv_cnt++;
                if (first_adv_frame < 0)
                    first_adv_frame = ev->data.adv.frame;
                ra_gekkonet_handle_advance(ctx, ev);
                break;
            case EmptyGameEvent:
            default:
                break;
        }
    }

   /* Throttled summary to spot stalls without spamming logs. */
   {
       static unsigned evt_log_tick = 0;
       if ((adv_cnt || save_cnt || load_cnt) &&
           (evt_log_tick < 5 || (evt_log_tick % 120) == 0))
       {
            if (adv_cnt > 0)
                GEKKONET_LOG("game events: total=%d adv=%d save=%d load=%d first_adv_frame=%d",
                    count, adv_cnt, save_cnt, load_cnt, first_adv_frame);
            else
                GEKKONET_LOG("game events: total=%d adv=%d save=%d load=%d",
                    count, adv_cnt, save_cnt, load_cnt);
        }
        evt_log_tick++;
    }
}

static void ra_gekkonet_process_session_events(ra_gekkonet_ctx_t *ctx)
{
    int count = 0;
    GekkoSessionEvent **events;

    if (!ctx || !ctx->session)
        return;

    events = gekko_session_events(ctx->session, &count);
    if (!events || count <= 0)
        return;

    for (int i = 0; i < count; i++)
    {
        const GekkoSessionEvent *ev = events[i];
        if (!ev)
            continue;

        GEKKONET_LOG("session event type=%d", ev->type);

        /* Application-specific handling is up to RetroArch. We just forward
         * the event to the optional callback if present.
         */
        if (ctx->session_event_cb)
            ctx->session_event_cb(ev, ctx->session_event_userdata);
    }
}

/* --- Main per-frame update entry point ---------------------------------- */

/* Call this once per frontend frame, after pushing local input via
 * ra_gekkonet_push_local_input().
 *
 * A typical RetroArch loop would be:
 *   1. Pack inputs.
 *   2. ra_gekkonet_push_local_input(...).
 *   3. ra_gekkonet_update(...).
 *
 * Inside this call, GekkoNet might emit Save/Load/Advance events. Once you
 * fill in the TODOs above, those events will call back into your save/load/
 * run_frame callbacks.
 */
void ra_gekkonet_update(ra_gekkonet_ctx_t *ctx)
{
    if (!ctx || !ctx->session || !ctx->active)
        return;

    /* Maintain TCP snapshot channel (accept/connect + poll). */
    if (ctx->tcp_port)
    {
        ra_gekkonet_tcp_ensure_connection(ctx);
        ra_gekkonet_poll_tcp_snapshot(ctx);
    }

    /* Let GekkoNet process incoming/outgoing packets. */
    gekko_network_poll(ctx->session);

    /* Deliver high-level session events to the frontend. */
    ra_gekkonet_process_session_events(ctx);

    /* Deliver game events (save/load/advance). */
    ra_gekkonet_process_game_events(ctx);
}
