# GekkoNet Netplay Backend for RetroArch

## 1. Goals and scope

**Goal:** Integrate the [GekkoNet](https://github.com/HeatXD/GekkoNet) rollback netplay SDK into RetroArch as an optional netplay backend that:

- Uses libretro save states for rollback.
- Reuses RetroArch’s existing core/content loading, lobby, and menu UI.
- Preserves the existing built-in netplay implementation as an alternative backend.
- Exposes GekkoNet tuning parameters (prediction, delays, etc.) in RetroArch’s settings.

This document is aimed at RetroArch developers or downstream forks wanting a low-level design and starting point implementation.

---

## 2. Background

### 2.1 RetroArch netplay today

RetroArch’s built-in netplay:

- Uses a replay/rollback model in the frontend.
- Requires same core, same content (ROM/image), and a deterministic core on all peers.
- Uses libretro’s save-state API:
  - `retro_serialize_size()`
  - `retro_serialize()`
  - `retro_unserialize()`
- Talks over a custom protocol on top of TCP.

Netplay works best with cores that implement serialization; without it, behavior is degraded or unsupported.

### 2.2 GekkoNet overview

GekkoNet is a P2P rollback SDK in C/C++. The public C header (`gekkonet.h`) defines:

- A `GekkoSession` opaque handle.
- `GekkoConfig` describing players, spectators, prediction window, input/state sizes, and optional features like desync detection.
- A pluggable network adapter (`GekkoNetAdapter`) with:
  - `send_data`
  - `receive_data`
  - `free_data`
- `GekkoGameEvent`:
  - `AdvanceEvent` — advance the game by one frame with a given input blob.
  - `SaveEvent` — save/serialize state into a buffer.
  - `LoadEvent` — load/unserialize a previous state.
- `GekkoSessionEvent`:
  - Player connected/disconnected.
  - Player syncing.
  - Session started.
  - Desync detected, etc.

The typical integration pattern:

1. Create a `GekkoSession` and fill `GekkoConfig` with:
   - Number of players/spectators.
   - `input_size` (bytes per input blob).
   - `state_size` (bytes per serialized state).
2. Set up a `GekkoNetAdapter` (for example `gekko_default_adapter(port)`).
3. Add actors (`LocalPlayer`, `RemotePlayer`, `Spectator`).
4. On each tick:
   - Push local input (`gekko_add_local_input`).
   - Pump network (`gekko_network_poll`).
   - Handle session events (`gekko_session_events`).
   - Handle game events (`gekko_update_session`):
     - Save/load state via your own callbacks.
     - Advance the simulation exactly one frame on `AdvanceEvent`.

This maps naturally onto RetroArch’s “save state + run one frame” model.

### 2.3 Recommended GekkoNet header layout

`gekkonet.h` does not need to be in a specific hard-coded location, it just has to be on the compiler's include path.

If you follow RetroArch's existing conventions for third-party dependencies, a good layout is:

```text
RetroArch/
  deps/
    gekkonet/
      include/
        gekkonet.h
        ... (other GekkoNet headers)
      src/
        ... (GekkoNet sources, if building inside RetroArch)
```

Then, in `netplay_gekkonet.c`, you can simply use:

```c
#include "gekkonet.h"
```

And in the build system (Makefile or CMake), add the include directory to the compiler flags, for example:

```make
CFLAGS   += -Ideps/gekkonet/include
CXXFLAGS += -Ideps/gekkonet/include
```

(or the equivalent `target_include_directories` in CMake).

If you prefer to install GekkoNet system-wide instead of vendoring it, you can switch to:

```c
#include <gekkonet.h>
```

and add the appropriate `-I` flags (or `pkg-config` integration) so the header can be found.

---

## 3. Integration strategy

Possible levels:

1. Core-level: embed GekkoNet inside individual libretro cores.
2. External controller: a separate process uses the Network Control Interface to drive RetroArch over UDP.
3. Frontend backend (recommended): integrate GekkoNet as an alternative backend inside RetroArch’s netplay subsystem.

This document assumes **option 3**:

- RetroArch continues to:
  - Load cores/content.
  - Provide lobbies & UI.
  - Serialize/unserialize cores.
- GekkoNet:
  - Owns the rollback algorithm.
  - Owns the netplay transport.
  - Asks RetroArch to save/load states and to run frames.

---

## 4. High-level architecture

### 4.1 Netplay backend selection

Introduce a netplay backend enum:

```c
typedef enum
{
    NETPLAY_BACKEND_BUILTIN  = 0,
    NETPLAY_BACKEND_GEKKONET = 1
} netplay_backend_t;
```

Represented in config as something like:

- `netplay_backend = "builtin"` (default).
- `netplay_backend = "gekkonet"`.

When netplay is enabled, the core runloop will:

```c
if (settings->bools.netplay_enabled)
{
    if (settings->bools.netplay_backend_gekkonet)
        netplay_gekkonet_frame();
    else
        netplay_builtin_frame();
}
```

Where `netplay_gekkonet_frame()` ultimately calls into `ra_gekkonet_update()` plus some glue.

### 4.2 Per-frame flow with GekkoNet

Per frame:

1. RetroArch collects local input for each local player (pads, analog, etc.).
2. Packs it into an input blob of size `input_size`.
3. Calls `ra_gekkonet_push_local_input()` for each local actor.
4. Calls `ra_gekkonet_update()`:
   - Internally calls `gekko_network_poll()`.
   - Processes `GekkoSessionEvent` and forwards them to RetroArch UI (optional).
   - Processes `GekkoGameEvent`:
     - On `SaveEvent` → call libretro serialization (`retro_serialize()`).
     - On `LoadEvent` → call libretro unserialize (`retro_unserialize()`).
     - On `AdvanceEvent`:
       - Exposes a pointer to the current frame’s input blob.
       - Calls the “run one frame” callback (usually just `retro_run()`).
5. RetroArch’s `input_state_net()` callback reads the current frame’s decoded input from `ra_gekkonet_get_current_input()`.

---

## 5. Data mapping: RetroArch ↔ GekkoNet

### 5.1 Input blobs

GekkoNet sees input as a fixed-size byte array per frame (`input_size`).

You choose the layout. One common approach:

```c
typedef struct
{
    struct {
        uint32_t buttons;   /* RetroPad buttons as a bitfield */
        int16_t  analog_x[2];
        int16_t  analog_y[2];
    } players[NETPLAY_MAX_PLAYERS];
} ra_gekkonet_input_t;
```

Set:

```c
params.input_size = sizeof(ra_gekkonet_input_t);
```

Per frame:

1. Fill a `ra_gekkonet_input_t` using the same helpers the built-in netplay uses to get input per port.
2. Call:

```c
ra_gekkonet_push_local_input(&ctx, local_actor_handle, &input_blob);
```

RetroArch’s `input_state_net()` will later decode a blob of the same type back into per-port input.

### 5.2 Save states

`state_size` is taken from `retro_serialize_size()`.

Set:

```c
params.state_size = (unsigned int)retro_serialize_size();
```

Your save/load callbacks, passed into `ra_gekkonet_init()`, look like:

```c
static bool ra_gekkonet_save_state_cb(void *dst,
                                      unsigned int capacity,
                                      unsigned int *out_size,
                                      unsigned int *out_crc)
{
    if (!retro_serialize(dst, capacity))
        return false;

    if (out_size)
        *out_size = capacity;

    if (out_crc)
        *out_crc = crc32_calculate(dst, capacity);

    return true;
}

static bool ra_gekkonet_load_state_cb(const void *src,
                                      unsigned int size)
{
    return retro_unserialize(src, size);
}
```

`netplay_gekkonet.c` wires these into `GekkoGameEvent` save/load events.

---

## 6. GekkoNet session lifecycle in RetroArch

### 6.1 Creating a session

1. Compute parameters:

   ```c
   ra_gekkonet_params_t params;

   params.num_players             = settings->uints.netplay_max_players;
   params.max_spectators          = settings->uints.netplay_max_spectators;
   params.input_prediction_window = settings->uints.gekkonet_input_prediction;
   params.spectator_delay         = settings->uints.gekkonet_spectator_delay;
   params.input_size              = sizeof(ra_gekkonet_input_t);
   params.state_size              = (unsigned int)retro_serialize_size();
   params.port                    = settings->uints.netplay_udp_port;
   params.limited_saving          = settings->bools.gekkonet_limited_saving;
   params.post_sync_joining       = settings->bools.gekkonet_allow_late_join;
   params.desync_detection        = settings->bools.gekkonet_desync_detection;
   ```

2. Initialize context:

   ```c
   static ra_gekkonet_ctx_t g_gekkonet;

   if (!ra_gekkonet_init(&g_gekkonet, &params,
                         ra_gekkonet_save_state_cb,
                         ra_gekkonet_load_state_cb))
   {
       /* fall back to builtin netplay or error out */
   }
   ```

3. Set callbacks:

   ```c
   ra_gekkonet_set_run_frame_cb(&g_gekkonet, ra_gekkonet_run_frame_cb);
   ra_gekkonet_set_session_event_cb(&g_gekkonet,
                                    ra_gekkonet_session_event_cb,
                                    NULL);
   ```

   Where:

   ```c
   static void ra_gekkonet_run_frame_cb(void)
   {
       retro_run();
   }

   static void ra_gekkonet_session_event_cb(const GekkoSessionEvent *ev,
                                            void *userdata)
   {
       /* Map to RetroArch UI notifications, logs, etc. */
       (void)userdata;
   }
   ```

### 6.2 Adding actors

When hosting or joining:

- For each local player controlled by this instance:

  ```c
  int handle = ra_gekkonet_add_actor(&g_gekkonet, LocalPlayer, NULL);
  ```

- For each remote player, once you know their IP/port:

  ```c
  char addr_str[64];
  snprintf(addr_str, sizeof(addr_str), "%s:%hu", remote_ip, remote_port);

  int handle = ra_gekkonet_add_actor(&g_gekkonet,
                                     RemotePlayer,
                                     addr_str);
  ```

Optionally set per-player delay:

```c
ra_gekkonet_set_local_delay(&g_gekkonet, local_actor_handle,
                            settings->uints.gekkonet_local_delay);
```

### 6.3 Per-frame loop

Core netplay loop (GekkoNet backend):

```c
void netplay_gekkonet_frame(void)
{
    ra_gekkonet_input_t local_input_blob;

    /* 1. Pack local inputs into blob */
    memset(&local_input_blob, 0, sizeof(local_input_blob));
    pack_inputs_for_all_ports(&local_input_blob);

    /* 2. Push into GekkoNet for each local actor */
    ra_gekkonet_push_local_input(&g_gekkonet,
                                 local_primary_actor_handle,
                                 &local_input_blob);

    /* 3. Let GekkoNet handle networking and emit events */
    ra_gekkonet_update(&g_gekkonet);

    /* GekkoNet will:
     *  - Ask you to serialize/unserialize states.
     *  - Tell you when to run retro_run() for a given frame.
     */
}
```

Input callback:

```c
static const ra_gekkonet_input_t *get_current_gekkonet_input(void)
{
    const void *raw = ra_gekkonet_get_current_input(&g_gekkonet);
    return (const ra_gekkonet_input_t*)raw;
}

int16_t input_state_net(unsigned port,
                        unsigned device,
                        unsigned idx,
                        unsigned id)
{
    if (g_gekkonet_active)
    {
        const ra_gekkonet_input_t *frame = get_current_gekkonet_input();
        if (!frame)
            return 0;

        /* decode frame->players[port] into the requested device/id */
        /* ... */
    }

    /* builtin netplay behavior */
}
```

---

## 7. Menu & configuration integration

RetroArch lets you add config keys and menu options.

### 7.1 Config keys

Add defaults in `config.def.h`:

```c
#define DEFAULT_NETPLAY_BACKEND_GEKKONET       false
#define DEFAULT_GEKKONET_INPUT_PREDICTION      6
#define DEFAULT_GEKKONET_SPECTATOR_DELAY       10
#define DEFAULT_GEKKONET_MAX_SPECTATORS        16
#define DEFAULT_GEKKONET_DESYNC_DETECTION      true
```

Add fields in `configuration.h`:

```c
struct settings
{
    struct
    {
        bool netplay_backend_gekkonet;
        bool gekkonet_desync_detection;
        /* ... */
    } bools;

    struct
    {
        unsigned gekkonet_input_prediction;
        unsigned gekkonet_spectator_delay;
        unsigned gekkonet_max_spectators;
        unsigned netplay_udp_port;
        /* ... */
    } uints;
};
```

Wire them in `configuration.c` with `SETTING_BOOL` / `SETTING_UINT`.

### 7.2 Menu entries

Add menu labels, sublabels, and display list entries for:

- Netplay Backend: Built-in / GekkoNet.
- GekkoNet Input Prediction Window.
- GekkoNet Spectator Delay.
- GekkoNet Max Spectators.
- GekkoNet Desync Detection.

These show up in the Netplay category of the settings menu and in the desktop UI.

---

## 8. Network Control Interface (optional)

The Network Control Interface (NCI) is RetroArch’s UDP control channel which exposes commands like `NETPLAY_HOST_TOGGLE`, `SAVE_STATE`, `LOAD_STATE`, etc.

You do not have to modify NCI for GekkoNet, but you can:

- Add commands that query GekkoNet network statistics (ping, jitter, frames ahead).
- Add commands that indicate which backend is in use (built-in vs GekkoNet).
- Add commands that surface desync information.

---

## 9. Lobby integration

RetroArch’s lobby system can be reused unchanged, with one extra metadata field:

- Add `"backend": "builtin"` or `"backend": "gekkonet"` to each room.

When a client selects a lobby room:

- If backend is `"gekkonet"`, automatically enable the GekkoNet backend, or refuse to connect if unsupported.

This keeps UX familiar while avoiding incompatible sessions.

---

## 10. Compatibility and limitations

Constraints:

- Cores must support save states and be deterministic given the same inputs.
- Features that also use save states or time-manipulation (rewind, run-ahead) conflict with rollback:
  - Disable rewind and run-ahead when GekkoNet netplay is active.
- Heavy cores may need tuning for rollback (prediction window, delay, and so on).

Consider a per-core whitelist of “known good” GekkoNet netplay cores.

---

## 11. Testing and debugging

Suggested phases:

1. **Single instance / loopback**
   - Host and client on the same machine.
   - Confirm basic save/load/advance flows work and no desyncs occur.

2. **LAN testing**
   - Measure ping/jitter and frames ahead.
   - Compare feel vs built-in netplay.

3. **Adverse network conditions**
   - Inject latency/jitter/loss with `tc`/`netem` or similar.
   - Tune prediction window, local delay, and spectator delay.

4. **Instrumentation**
   - Add an overlay that shows:
     - Ping/jitter (using `GekkoNetworkStats`).
     - Frames ahead.
     - Rollback count.
     - Desync events.

---

## 12. Using `netplay_gekkonet.c`

This section explains how to wire the provided `netplay_gekkonet.c` into RetroArch.

### 12.1 Files

- `network/netplay/netplay_gekkonet.c` — the backend skeleton.
- `network/netplay/netplay_gekkonet.h` — recommended header declaring:
  - `ra_gekkonet_ctx_t`
  - `ra_gekkonet_params_t`
  - Public functions:
    - `ra_gekkonet_init()`
    - `ra_gekkonet_deinit()`
    - `ra_gekkonet_set_run_frame_cb()`
    - `ra_gekkonet_set_session_event_cb()`
    - `ra_gekkonet_add_actor()`
    - `ra_gekkonet_set_local_delay()`
    - `ra_gekkonet_push_local_input()`
    - `ra_gekkonet_update()`
    - `ra_gekkonet_get_current_input()`

### 12.2 Wiring it into the netplay subsystem

1. **Create a context**

   ```c
   #include "netplay_gekkonet.h"

   static ra_gekkonet_ctx_t g_gekkonet_ctx;
   static bool g_gekkonet_active = false;
   ```

2. **On netplay start (host or join)**

   - Build `ra_gekkonet_params_t` from settings.
   - Initialize the context:

     ```c
     if (settings->bools.netplay_backend_gekkonet)
     {
         if (ra_gekkonet_init(&g_gekkonet_ctx,
                              &params,
                              ra_gekkonet_save_state_cb,
                              ra_gekkonet_load_state_cb))
         {
             ra_gekkonet_set_run_frame_cb(&g_gekkonet_ctx,
                                          ra_gekkonet_run_frame_cb);
             ra_gekkonet_set_session_event_cb(&g_gekkonet_ctx,
                                              ra_gekkonet_session_event_cb,
                                              NULL);
             g_gekkonet_active = true;
         }
         else
         {
             g_gekkonet_active = false;
         }
     }
     ```

   - Add actors for each local and known remote player using `ra_gekkonet_add_actor()`.

3. **Per frame**

   ```c
   if (g_gekkonet_active)
   {
       ra_gekkonet_input_t blob;
       memset(&blob, 0, sizeof(blob));
       pack_inputs_for_all_ports(&blob);

       ra_gekkonet_push_local_input(&g_gekkonet_ctx,
                                    local_actor_handle,
                                    &blob);

       ra_gekkonet_update(&g_gekkonet_ctx);
   }
   else
   {
       /* existing built-in netplay logic */
   }
   ```

4. **Input callback**

   - Modify `input_state_net()` to branch based on backend:

     ```c
     int16_t input_state_net(unsigned port,
                             unsigned device,
                             unsigned idx,
                             unsigned id)
     {
         if (g_gekkonet_active)
         {
             const ra_gekkonet_input_t *frame =
                 (const ra_gekkonet_input_t*)
                 ra_gekkonet_get_current_input(&g_gekkonet_ctx);

             if (!frame)
                 return 0;

             /* decode frame->players[port] as needed */
             /* ... */
         }

         /* builtin netplay behavior */
     }
     ```

5. **Shutdown**

   - On netplay stop/disconnect:

     ```c
     if (g_gekkonet_active)
     {
         ra_gekkonet_deinit(&g_gekkonet_ctx);
         g_gekkonet_active = false;
     }
     ```

### 12.3 Filling in the TODOs

In `netplay_gekkonet.c`, fill in:

- `ra_gekkonet_handle_save()`
- `ra_gekkonet_handle_load()`
- `ra_gekkonet_handle_advance()`

Using the actual layout of `GekkoGameEvent` from your version of `gekkonet.h`. A typical mapping is:

- SaveEvent:
  - Use the provided state buffer pointer and length out-params.
  - Call `ctx->save_cb()`.
- LoadEvent:
  - Use the provided state buffer pointer and length.
  - Call `ctx->load_cb()`.
- AdvanceEvent:
  - Set `ctx->current_input` to the provided input pointer.
  - Call `ctx->run_frame_cb()` exactly once.

Once these are implemented, GekkoNet drives when frames run and which input they use, while RetroArch remains in charge of cores, content, and UI.

---

## 13. Summary

- RetroArch’s existing netplay infrastructure already uses save states and deterministic cores.
- GekkoNet provides a ready-made rollback + P2P networking engine.
- `netplay_gekkonet.c` implements a clean C wrapper you can plug into RetroArch as a new netplay backend.
- This document gives you the architecture + wiring instructions, including how to actually use `netplay_gekkonet.c` in the netplay loop and menus.
