#ifndef RARCH_NETPLAY_GEKKONET_H
#define RARCH_NETPLAY_GEKKONET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC 1
#endif

#if __has_include("gekkonet.h")
#include "gekkonet.h"
#else
/* Fallback for build systems that don't add deps/gekkonet/include
 * to the include path (e.g., some MSVC project files). */
#include "../../deps/gekkonet/include/gekkonet.h"
#endif
#include "../../input/input_defines.h"

/* Simple per-player input layout used for GekkoNet blobs. */
typedef struct ra_gekkonet_pad_input
{
   uint32_t buttons;
   int16_t  analog_x[2];
   int16_t  analog_y[2];
} ra_gekkonet_pad_input_t;

/* One blob per frame across all players. */
typedef struct ra_gekkonet_input
{
   ra_gekkonet_pad_input_t players[MAX_USERS];
} ra_gekkonet_input_t;

typedef struct ra_gekkonet_params
{
   unsigned char num_players;
   unsigned char max_spectators;
   unsigned char input_prediction_window;
   unsigned char spectator_delay;
   unsigned int  input_size;
   unsigned int  state_size;
   unsigned short port;
   bool limited_saving;
   bool post_sync_joining;
   bool desync_detection;
} ra_gekkonet_params_t;

typedef bool (*ra_gekkonet_save_state_cb)(
      void         *dst,
      unsigned int  capacity,
      unsigned int *out_size,
      unsigned int *out_crc);

typedef bool (*ra_gekkonet_load_state_cb)(
      const void   *src,
      unsigned int  size);

typedef void (*ra_gekkonet_run_frame_cb)(void);

typedef void (*ra_gekkonet_session_event_cb)(
      const GekkoSessionEvent *event,
      void                    *userdata);

typedef struct ra_gekkonet_ctx
{
   GekkoSession    *session;
   GekkoNetAdapter *adapter;
   GekkoConfig      cfg;
   unsigned short   bound_port;
   int              actor_handles[MAX_USERS];
   int              actor_ports[MAX_USERS];
   int              actor_count;

   ra_gekkonet_save_state_cb      save_cb;
   ra_gekkonet_load_state_cb      load_cb;
   ra_gekkonet_run_frame_cb       run_frame_cb;
   ra_gekkonet_session_event_cb   session_event_cb;
   void                          *session_event_userdata;

   unsigned int state_size;
   unsigned int input_size;

   void       *current_input_buf;
   const void *current_input;

   char       **remote_addrs;
   size_t       remote_addrs_count;
   size_t       remote_addrs_cap;
   int          local_actor_count;
   int          remote_actor_count;

   /* TCP snapshot channel */
   int          tcp_fd;
   int          tcp_listen_fd;
   unsigned short tcp_port;
   bool         tcp_is_client;
   char         tcp_host[256];
   /* incoming snapshot receive buffer */
   unsigned char *tcp_snap_buf;
   unsigned int  tcp_snap_expected;
   unsigned int  tcp_snap_received;
   bool          tcp_snap_header_read;
   unsigned int  tcp_snap_crc;
   unsigned int  tcp_snap_frame;

   /* Queued AdvanceEvent inputs so the frontend can run them one by one. */
   unsigned char *queued_inputs;
   size_t         queued_input_head;
   size_t         queued_input_count;
   size_t         queued_input_cap;
   size_t         input_blob_size;

   bool ready_for_state;
   bool owns_adapter;
   bool active;
   bool advanced_frame;
} ra_gekkonet_ctx_t;

const void *ra_gekkonet_get_current_input(const ra_gekkonet_ctx_t *ctx);

bool ra_gekkonet_init(ra_gekkonet_ctx_t              *ctx,
                      const ra_gekkonet_params_t     *params,
                      ra_gekkonet_save_state_cb       save_cb,
                      ra_gekkonet_load_state_cb       load_cb);

void ra_gekkonet_set_run_frame_cb(ra_gekkonet_ctx_t       *ctx,
                                  ra_gekkonet_run_frame_cb cb);

void ra_gekkonet_set_session_event_cb(ra_gekkonet_ctx_t            *ctx,
                                      ra_gekkonet_session_event_cb  cb,
                                      void                         *userdata);

/* Explicitly map a Gekko actor handle to a RetroPad port index. */
bool ra_gekkonet_set_actor_port(ra_gekkonet_ctx_t *ctx,
                                int                handle,
                                int                port);

/* Optional: configure TCP snapshot channel (host/client, peer host, port). */
void ra_gekkonet_set_tcp_params(ra_gekkonet_ctx_t *ctx,
                                bool               is_client,
                                const char        *peer_host,
                                unsigned short     port);

void ra_gekkonet_deinit(ra_gekkonet_ctx_t *ctx);

int ra_gekkonet_add_actor(ra_gekkonet_ctx_t *ctx,
                          GekkoPlayerType     type,
                          const char         *addr_string);

void ra_gekkonet_set_local_delay(ra_gekkonet_ctx_t *ctx,
                                 int                actor_handle,
                                 unsigned char      delay_frames);

bool ra_gekkonet_push_local_input(ra_gekkonet_ctx_t *ctx,
                                  int                actor_handle,
                                  const void        *input_blob);

void ra_gekkonet_update(ra_gekkonet_ctx_t *ctx);

/* Host-only: serialize and send a snapshot to the peer. */
bool ra_gekkonet_send_snapshot(ra_gekkonet_ctx_t *ctx);

/* Fire a one-shot UDP probe to a given "ip:port" string using the current adapter. */
void ra_gekkonet_send_probe(const char *addr_string);

/* Push the most recent AdvanceEvent input into the queue for later runs. */
bool ra_gekkonet_enqueue_current_input(ra_gekkonet_ctx_t *ctx);

/* Pop the next queued input into current_input_buf/current_input. */
bool ra_gekkonet_dequeue_next_input(ra_gekkonet_ctx_t *ctx);

/* Inspect pending queued inputs. */
size_t ra_gekkonet_queued_input_count(const ra_gekkonet_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RARCH_NETPLAY_GEKKONET_H */
