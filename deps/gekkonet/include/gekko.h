#pragma once

#include <vector>

#include "gekkonet.h"
#include "gekko_types.h"

#include "backend.h"
#include "event.h"
#include "sync.h"
#include "storage.h"

// define GekkoSession internally
struct GekkoSession {
    virtual void Init(GekkoConfig* config) = 0;
    virtual void SetLocalDelay(i32 player, u8 delay) = 0;
    virtual void SetNetAdapter(GekkoNetAdapter* adapter) = 0;
    virtual i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) = 0;
    virtual void AddLocalInput(i32 player, void* input) = 0;
    virtual GekkoGameEvent** UpdateSession(i32* count) = 0;
    virtual GekkoSessionEvent** Events(i32* count) = 0;
    virtual f32 FramesAhead() = 0;
    virtual void NetworkStats(i32 player, GekkoNetworkStats* stats) = 0;
    virtual void NetworkPoll() = 0;
    virtual void QueueSnapshotPush(const u8* data, u32 size, u32 crc, Frame frame) = 0;
        virtual void QueueSnapshotApply(const u8* data, u32 size, u32 crc, Frame frame) = 0;
        virtual void ApplyLocalSnapshot(const u8* data, u32 size, u32 crc, Frame frame) = 0;
    virtual ~GekkoSession();
};

namespace Gekko {

	class Session : public GekkoSession {
    public:
		Session();

        virtual void Init(GekkoConfig* config);

        virtual void SetLocalDelay(i32 player, u8 delay);

        virtual void SetNetAdapter(GekkoNetAdapter* adapter);

        virtual i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr);

        virtual void AddLocalInput(i32 player, void* input);

        virtual GekkoGameEvent** UpdateSession(i32* count);

        virtual GekkoSessionEvent** Events(i32* count);

        virtual f32 FramesAhead();

        virtual void NetworkStats(i32 player, GekkoNetworkStats* stats);

        virtual void NetworkPoll();

        /* Host: enqueue a snapshot to send to remotes. */
        void QueueSnapshotPush(const u8* data, u32 size, u32 crc, Frame frame);
        /* Client: receive a completed snapshot and mark it for loading. */
        void QueueSnapshotApply(const u8* data, u32 size, u32 crc, Frame frame);
        void ApplyLocalSnapshot(const u8* data, u32 size, u32 crc, Frame frame);

	private:
		void Poll();

		bool AllPlayersValid();

		void HandleReceivedInputs();

		void SendLocalInputs();

		u8 GetMinLocalDelay();

		bool IsSpectating();

		bool IsPlayingLocally();

		void AddDisconnectedPlayerInputs();

		void SendSpectatorInputs();

		void HandleRollback(std::vector<GekkoGameEvent*>& ev);

		bool AddAdvanceEvent(std::vector<GekkoGameEvent*>& ev, bool rolling_back);

		void AddSaveEvent(std::vector<GekkoGameEvent*>& ev);

		void AddLoadEvent(std::vector<GekkoGameEvent*>& ev);

		void HandleSavingConfirmedFrame(std::vector<GekkoGameEvent*>& ev);

		void UpdateLocalFrameAdvantage();

        bool ShouldDelaySpectator();

        void SendSessionHealthCheck();

        void SendNetworkHealthCheck();

        void SessionIntegrityCheck();

	private:
		bool _started;
		bool _just_started;

        bool _delay_spectator;

		Frame _last_saved_frame;

        Frame _last_sent_healthcheck;

		std::unique_ptr<u8[]> _disconnected_input;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

        MessageSystem _msg;

        StateStorage _storage;

        GameEventBuffer _game_event_buffer;

        std::vector<GekkoGameEvent*> _current_game_events;

        std::vector<u8> _pending_snapshot;
        bool _pending_snapshot_ready = false;
        u32 _pending_snapshot_crc = 0;
        Frame _pending_snapshot_frame = 0;
	};
}
