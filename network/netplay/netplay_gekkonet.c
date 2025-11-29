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

/* Simple logging macros. You can override these via compiler flags
 * or by defining GEKKONET_LOG/GEKKONET_WARN/GEKKONET_ERR before
 * including this file.
 */
#ifndef GEKKONET_LOG
#include <stdio.h>
#define GEKKONET_LOG(fmt, ...)  fprintf(stderr, "[gekkonet] " fmt "\n", ##__VA_ARGS__)
#define GEKKONET_WARN(fmt, ...) fprintf(stderr, "[gekkonet WARN] " fmt "\n", ##__VA_ARGS__)
#define GEKKONET_ERR(fmt, ...)  fprintf(stderr, "[gekkonet ERROR] " fmt "\n", ##__VA_ARGS__)
#endif

typedef struct ra_gekkonet_udp_adapter
{
    GekkoNetAdapter api;
    int             sockfd;
    struct ra_gekkonet_ctx *owner;
} ra_gekkonet_udp_adapter_t;

static ra_gekkonet_udp_adapter_t *g_udp_adapter        = NULL;
static GekkoNetResult           **g_udp_results        = NULL;
static size_t                     g_udp_results_cap    = 0;

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

static ra_gekkonet_udp_adapter_t *ra_gekkonet_udp_adapter_create(unsigned short port)
{
    ra_gekkonet_udp_adapter_t *adapter;
    struct sockaddr_in addr;

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
        ra_gekkonet_udp_close(adapter->sockfd);
        free(adapter);
        return NULL;
    }

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

    sendto(g_udp_adapter->sockfd, data, length, 0,
           (struct sockaddr*)&dst, (socklen_t)sizeof(dst));
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
#else
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
                break;
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
                    if (ra_gekkonet_add_actor(owner, RemotePlayer, addrbuf) < 0)
                        GEKKONET_WARN("Failed to auto-add remote actor for %s", addrbuf);
                }
            }

            g_udp_results[count++] = res;
        }
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

   ctx->current_input_buf = calloc(1, params->input_size);
   if (!ctx->current_input_buf)
   {
       GEKKONET_ERR("allocating input buffer (%u bytes) failed",
                    params->input_size);
       gekko_destroy(ctx->session);
       ctx->session = NULL;
       return false;
   }
   ctx->current_input = ctx->current_input_buf;
   ctx->ready_for_state = false;

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

    gekko_net_adapter_set(ctx->session, ctx->adapter);
    gekko_start(ctx->session, &ctx->cfg);

    ctx->active = true;
    GEKKONET_LOG("GekkoNet session started: %u players, %u spectators",
                 (unsigned)ctx->cfg.num_players,
                 (unsigned)ctx->cfg.max_spectators);
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

/* Destroy session and free resources. */
void ra_gekkonet_deinit(ra_gekkonet_ctx_t *ctx)
{
    if (!ctx || !ctx->active)
        return;

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

    if (!ctx || !ctx->session)
        return -1;

    if (type == RemotePlayer &&
        ctx->remote_actor_count + ctx->local_actor_count >= (int)ctx->cfg.num_players)
    {
        GEKKONET_WARN("max players reached; ignoring remote actor");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));

    if (addr_string && *addr_string)
    {
        /* GekkoNet's default adapter treats addr.data as a pointer to a
         * char buffer containing "ip:port" or similar. To keep this
         * simple, we strdup() the string and never free it until shutdown.
         * You may want to store these pointers in the context and free
         * them in ra_gekkonet_deinit().
         */
        size_t len = strlen(addr_string) + 1;
        char  *buf = (char*)malloc(len);
        if (!buf)
            return -1;
        memcpy(buf, addr_string, len);
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

    GEKKONET_LOG("Added actor handle %d (type=%d)", handle, (int)type);
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

    GEKKONET_LOG("save begin frame=%d requested_len=%u", ev->data.save.frame,
        ev->data.save.state_len ? *ev->data.save.state_len : 0);

    if (!ctx->save_cb(ev->data.save.state,
                      *ev->data.save.state_len,
                      ev->data.save.state_len,
                      ev->data.save.checksum))
    {
        GEKKONET_WARN("save_state callback failed (frame=%d)", ev->data.save.frame);
        return;
    }

    ctx->ready_for_state = true;

    GEKKONET_LOG("save frame=%d len=%u crc=%u",
        ev->data.save.frame,
        ev->data.save.state_len ? *ev->data.save.state_len : 0,
        ev->data.save.checksum ? *ev->data.save.checksum : 0);
}

static void ra_gekkonet_handle_load(ra_gekkonet_ctx_t    *ctx,
                                    const GekkoGameEvent *ev)
{
    if (!ctx || !ev || !ctx->load_cb)
        return;

    if (!ctx->ready_for_state)
    {
        GEKKONET_WARN("load_state skipped (not ready; frame=%d)", ev->data.load.frame);
        return;
    }

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

    if (ev->data.adv.input_len < ctx->input_size)
    {
        GEKKONET_WARN("input blob size mismatch (got %u, expected %u)",
                      ev->data.adv.input_len, ctx->input_size);
        memset(ctx->current_input_buf, 0, ctx->input_size);
        memcpy(ctx->current_input_buf, ev->data.adv.inputs,
               ev->data.adv.input_len);
    }
    else
    {
        memcpy(ctx->current_input_buf, ev->data.adv.inputs, ctx->input_size);
    }

    ctx->current_input = ctx->current_input_buf;

    GEKKONET_LOG("advance frame=%d len=%u rollback=%d",
        ev->data.adv.frame, ev->data.adv.input_len, ev->data.adv.rolling_back);

    if (ctx->run_frame_cb)
        ctx->run_frame_cb();

    /* After the first successful advance/run, we can safely serialize. */
    ctx->ready_for_state = true;
}

static void ra_gekkonet_process_game_events(ra_gekkonet_ctx_t *ctx)
{
    int count = 0;
    GekkoGameEvent **events;

    if (!ctx || !ctx->session)
        return;

    ctx->current_input = NULL;

    events = gekko_update_session(ctx->session, &count);
    if (!events || count <= 0)
        return;

    GEKKONET_LOG("game events: %d", count);

    for (int i = 0; i < count; i++)
    {
        const GekkoGameEvent *ev = events[i];
        if (!ev)
            continue;

        switch (ev->type)
        {
            case SaveEvent:
                ra_gekkonet_handle_save(ctx, ev);
                break;
            case LoadEvent:
                ra_gekkonet_handle_load(ctx, ev);
                break;
            case AdvanceEvent:
                ra_gekkonet_handle_advance(ctx, ev);
                break;
            case EmptyGameEvent:
            default:
                break;
        }
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

    GEKKONET_LOG("session events: %d", count);

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

    GEKKONET_LOG("update start");
    /* Let GekkoNet process incoming/outgoing packets. */
    gekko_network_poll(ctx->session);

    /* Deliver high-level session events to the frontend. */
    ra_gekkonet_process_session_events(ctx);

    /* Deliver game events (save/load/advance). */
    ra_gekkonet_process_game_events(ctx);
    GEKKONET_LOG("update end");
}
