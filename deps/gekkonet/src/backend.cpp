#include "backend.h"
#include "input.h"
#include "event.h"
#include "compression.h"
#include "gekko.h"

#include <chrono>
#include <cassert>
#include <climits>
#include <iostream>
#include <string>
#include <cstring>
#include <algorithm>

#ifndef GEKKONET_TRACE
#define GEKKONET_TRACE 0
#endif
#define GEKKONET_TRACE_LOG(msg) \
    do { if (GEKKONET_TRACE) { std::cerr << "[gekkonet] " << msg << std::endl; } } while (0)

// register poly types.
namespace
{
    zpp::serializer::register_types<
        zpp::serializer::make_type<Gekko::SyncMsg, zpp::serializer::make_id("Gekko::SyncMsg")>,
        zpp::serializer::make_type<Gekko::InputMsg, zpp::serializer::make_id("Gekko::InputMsg")>,
        zpp::serializer::make_type<Gekko::InputAckMsg, zpp::serializer::make_id("Gekko::InputAckMsg")>,
        zpp::serializer::make_type<Gekko::SessionHealthMsg, zpp::serializer::make_id("Gekko::SessionHealthMsg")>,
        zpp::serializer::make_type<Gekko::NetworkHealthMsg, zpp::serializer::make_id("Gekko::NetworkHealthMsg")>,
        zpp::serializer::make_type<Gekko::SnapshotOfferMsg, zpp::serializer::make_id("Gekko::SnapshotOfferMsg")>,
        zpp::serializer::make_type<Gekko::SnapshotChunkMsg, zpp::serializer::make_id("Gekko::SnapshotChunkMsg")>,
        zpp::serializer::make_type<Gekko::SnapshotAckMsg, zpp::serializer::make_id("Gekko::SnapshotAckMsg")>
    > _;
}

Gekko::MessageSystem::MessageSystem()
{
	_input_size = 0;
	_last_added_input = GameInput::NULL_FRAME;
	_last_added_spectator_input = GameInput::NULL_FRAME;
    _last_sent_network_check = 0;

	// gen magic for session
	std::srand((unsigned int)std::time(nullptr));
	_session_magic = std::rand();

	history = AdvantageHistory();
    session_events = SessionEventSystem();
}

void Gekko::MessageSystem::Init(u32 input_size)
{
	_input_size = input_size;
	_last_added_input = GameInput::NULL_FRAME;
	_last_added_spectator_input = GameInput::NULL_FRAME;

	history.Init();
    ResetSnapshotRecv();
    ResetSnapshotSend();
}

void Gekko::MessageSystem::ResetAfterSnapshot(Frame frame)
{
    /* Clear send queues */
    while (!_player_input_send_list.empty()) {
        delete _player_input_send_list.front();
        _player_input_send_list.pop_front();
    }
    while (!_spectator_input_send_list.empty()) {
        delete _spectator_input_send_list.front();
        _spectator_input_send_list.pop_front();
    }
    /* Clear pending outputs/inputs */
    std::queue<std::unique_ptr<NetData>> empty_out;
    std::swap(_pending_output, empty_out);
    std::queue<std::unique_ptr<NetInputData>> empty_in;
    std::swap(_received_inputs, empty_in);

    _last_added_input = frame - 1;
    _last_added_spectator_input = frame - 1;
    _last_sent_input = InputSendCache();
    _last_sent_spectator_input = InputSendCache();
    _last_sent_network_check = 0;
    history.Init();
    local_health.clear();

    for (auto& p : remotes) {
        p->stats.last_acked_frame = frame;
        p->stats.last_received_frame = frame + 16;
        p->stats.last_received_message = TimeSinceEpoch();
        p->stats.rtt.clear();
    }
    for (auto& p : spectators) {
        p->stats.last_acked_frame = frame;
        p->stats.last_received_frame = frame;
        p->stats.last_received_message = TimeSinceEpoch();
        p->stats.rtt.clear();
    }
}


void Gekko::MessageSystem::AddInput(Frame input_frame, u8 input[])
{
	if (_last_added_input + 1 == input_frame) {
		_last_added_input++;
		_player_input_send_list.push_back(new u8[_input_size * locals.size()]);
		std::memcpy(_player_input_send_list.back(), input, _input_size * locals.size());

		// update history
		history.Update(input_frame);
	}

	const Frame min_ack = GetMinLastAckedFrame(false);
	const u32 diff = _last_added_input - min_ack;

	if (_player_input_send_list.size() > std::min(MAX_PLAYER_SEND_SIZE, diff)) {
		delete _player_input_send_list.front();
		_player_input_send_list.pop_front();
	}
}

void Gekko::MessageSystem::AddSpectatorInput(Frame input_frame, u8 input[])
{
	if (_last_added_spectator_input + 1 == input_frame) {
		_last_added_spectator_input++;
		_spectator_input_send_list.push_back(new u8[_input_size * (locals.size() + remotes.size())]);
		std::memcpy(_spectator_input_send_list.back(), input, _input_size * (locals.size() + remotes.size()));
	}

	const Frame min_ack = GetMinLastAckedFrame(true);
	const u32 diff = _last_added_spectator_input - min_ack;

	if (_spectator_input_send_list.size() > std::min(MAX_SPECTATOR_SEND_SIZE, diff)) {
		delete _spectator_input_send_list.front();
		_spectator_input_send_list.pop_front();
	}
}

void Gekko::MessageSystem::SendPendingOutput(GekkoNetAdapter* host)
{
    /* Handle snapshot transfer first. */
    SendSnapshotData(host);

	// add input packet
	if (!_player_input_send_list.empty() && !remotes.empty()) {
		AddPendingInput(false);
        // check for disconnects
        HandleTooFarBehindActors(false);
	}

	// add spectator input packet
	if (!_spectator_input_send_list.empty() && !spectators.empty()) {
		AddPendingInput(true);
        // check for disconnects
        HandleTooFarBehindActors(true);
	}

	// handle messages
    static u32 send_tick = 0;
    if ((send_tick++ % 120) == 0 || !_pending_output.empty())
        GEKKONET_TRACE_LOG("SendPendingOutput queue size=" << _pending_output.size());

	for (u32 i = 0; i < _pending_output.size(); i++) {
		auto& pkt = _pending_output.front();
        GEKKONET_TRACE_LOG("SendPendingOutput type=" << pkt->pkt.header.type
            << " magic=" << pkt->pkt.header.magic
            << " addr_size=" << pkt->addr.GetSize());
        if (pkt->pkt.header.type == Inputs || pkt->pkt.header.type == SpectatorInputs) {
            if (pkt->pkt.header.type == Inputs) {
                SendDataToAll(pkt.get(), host);
            } else {
                SendDataToAll(pkt.get(), host, true);
            }
        }
        else if ((pkt->pkt.header.type == SessionHealth || pkt->pkt.header.type == NetworkHealth ||
                  pkt->pkt.header.type == SnapshotOffer || pkt->pkt.header.type == SnapshotChunk) &&
                 pkt->addr.GetSize() == 0) {
            // broadcast to remotes (and spectators where applicable)
            SendDataToAll(pkt.get(), host);
            SendDataToAll(pkt.get(), host, true);
        }
        else {
            SendDataTo(pkt.get(), host);
        }
		// housekeeping
		_pending_output.pop();
	}
}

void Gekko::MessageSystem::HandleData(GekkoNetAdapter* host, GekkoNetResult** data, u32 length)
{
    for (u32 i = 0; i < length; i++) {
        auto res = data[i];
        auto addr = NetAddress(res->addr.data, res->addr.size);

        _bin_buffer.clear();

        try {
            _bin_buffer.insert(_bin_buffer.begin(), (u8*)res->data, (u8*)res->data + res->data_len);

            NetPacket pkt;
            zpp::serializer::memory_input_archive in(_bin_buffer);
            in(pkt.header, pkt.body);

            ParsePacket(addr, pkt);
        }
        catch (const std::exception&) {
            printf("failed to deserialize packet (len=%u)\n", res->data_len);
        }

        // cleanup :)
        host->free_data(res->addr.data);
        host->free_data(res->data);
        host->free_data(res);
    }
}

void Gekko::MessageSystem::SendSyncRequest(NetAddress* addr)
{
    if (!addr) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
	auto& message = _pending_output.back();

	message->addr.Copy(addr);

	message->pkt.header.type = SyncRequest;
	message->pkt.header.magic = 0;

    auto body = std::make_unique<SyncMsg>();
    body->rng_data = _session_magic;

    message->pkt.body = std::move(body);

    GEKKONET_TRACE_LOG("Queued SyncRequest addr_size=" << addr->GetSize() << " magic=" << _session_magic);
}

void Gekko::MessageSystem::SendSyncResponse(NetAddress* addr, u16 magic)
{
    if (!addr || magic == 0) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

	message->addr.Copy(addr);
	message->pkt.header.type = SyncResponse;
	message->pkt.header.magic = magic;

    auto body = std::make_unique<SyncMsg>();
    body->rng_data = _session_magic;

    message->pkt.body = std::move(body);

    GEKKONET_TRACE_LOG("Queued SyncResponse addr_size=" << addr->GetSize() << " magic=" << magic);
}

std::queue<std::unique_ptr<Gekko::NetInputData>>& Gekko::MessageSystem::LastReceivedInputs()
{
	return _received_inputs;
}

void Gekko::MessageSystem::SendInputAck(Handle player, Frame frame)
{
	auto plyr = GetPlayerByHandle(player);

    if (!plyr) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

	message->addr.Copy(&plyr->address);
	message->pkt.header.magic = plyr->session_magic;
	message->pkt.header.type = InputAck;

    auto body = std::make_unique<InputAckMsg>();
	body->ack_frame = frame;
	body->frame_advantage = history.GetLocalAdvantage();

    message->pkt.body = std::move(body);
}

std::vector<Handle> Gekko::MessageSystem::GetHandlesForAddress(NetAddress* addr)
{
	auto result = std::vector<Handle>();
	for (auto& player: remotes) {
		if (player->address.Equals(*addr)) {
			result.push_back(player->handle);
		}
	}
    if (result.empty()) {
        std::string addr_str;
        if (addr->GetAddress() && addr->GetSize() > 0)
            addr_str.assign(reinterpret_cast<const char*>(addr->GetAddress()), addr->GetSize());
        std::cerr << "[gekkonet] no handle match for addr=" << addr_str << "\n";
    }
	return result;
}

Gekko::Player* Gekko::MessageSystem::GetPlayerByHandle(Handle handle) 
{
	for (auto& player: remotes) {
		if (player->handle == handle) {
			return player.get();
		}
	}
	return nullptr;
}

Frame Gekko::MessageSystem::GetMinLastAckedFrame(bool spectator) 
{
	Frame min = INT_MAX;
	for (auto& player : spectator ? spectators : remotes) {
		if (player->GetStatus() == Connected) {
			min = std::min(player->stats.last_acked_frame, min);
		}
	}
	return min;
}

Frame Gekko::MessageSystem::GetLastAddedInput(bool spectator)
{
	return spectator ? _last_added_spectator_input : _last_added_input;
}

bool Gekko::MessageSystem::CheckStatusActors()
{
	i32 result = 0;
	u64 now = TimeSinceEpoch();

    std::vector<std::unique_ptr<Player>>* current = &remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }
        for (auto& player : *current) {
            if (player->GetStatus() == Initiating) {
                if (player->stats.last_sent_sync_message + NetStats::SYNC_MSG_DELAY < now) {
                    if (player->sync_num == 0) {
                        SendSyncRequest(&player->address);
                        player->stats.last_sent_sync_message = now;
                    }
                    else if (player->sync_num < NUM_TO_SYNC) {
                        SendSyncResponse(&player->address, player->session_magic);
                        player->stats.last_sent_sync_message = now;
                    }
                    else {
                        player->SetStatus(Connected);
                        session_events.AddPlayerConnectedEvent(player->handle);
                        result++;
                    }
                }
                result--;
            }
        }
	}

	return result == 0;
}

void Gekko::MessageSystem::SendSessionHealth(Frame frame, u32 checksum)
{
    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

    // the address and magic is set later so dont worry about it now
    message->pkt.header.type = SessionHealth;

    auto body = std::make_unique<SessionHealthMsg>();
    body->frame = frame;
    body->checksum = checksum;

    message->pkt.body = std::move(body);
}

void Gekko::MessageSystem::SendNetworkHealth()
{
    u64 now = TimeSinceEpoch();

    // dont want to spam the network with network health packets
    if (_last_sent_network_check + NetStats::NET_CHECK_DELAY > now) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

    // the address and magic is set later so dont worry about it now
    message->pkt.header.type = NetworkHealth;

    auto body = std::make_unique<NetworkHealthMsg>();
    body->send_time = now;
    body->received = false;

    message->pkt.body = std::move(body);

    _last_sent_network_check = now;
}

void Gekko::MessageSystem::HandleTooFarBehindActors(bool spectator)
{
    /* Temporarily disable disconnect logic during debugging to keep peers alive. */
    (void)spectator;
}

u64 Gekko::MessageSystem::TimeSinceEpoch()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void Gekko::MessageSystem::SendDataToAll(NetData* pkt, GekkoNetAdapter* host, bool spectators_only)
{
    auto& actors = spectators_only ? spectators : remotes;

    std::vector<u8> body_buffer;

    try {
        zpp::serializer::memory_output_archive out(body_buffer);
        out(pkt->pkt.body);
    }
    catch (const std::exception&)
    {
        printf("failed to serialize packet body\n");
        return;
    }


    for (auto& actor : actors) {
        _bin_buffer.clear();
        if (actor->address.GetSize() != 0 && actor->GetStatus() != Disconnected) {

            pkt->addr.Copy(&actor->address);
            pkt->pkt.header.magic = actor->session_magic;

            try {
                zpp::serializer::memory_output_archive out(_bin_buffer);
                out(pkt->pkt.header);
            }
            catch (const std::exception&)
            {
                printf("failed to serialize packet header\n");
                continue;
            }

            _bin_buffer.insert(
                _bin_buffer.end(),
                body_buffer.begin(),
                body_buffer.end()
            );

            auto addr = GekkoNetAddress();
            addr.data = actor->address.GetAddress();
            addr.size = actor->address.GetSize();

            host->send_data(&addr, (char*)_bin_buffer.data(), (int)_bin_buffer.size());
        }
    }
}

void Gekko::MessageSystem::SendDataTo(NetData* pkt, GekkoNetAdapter* host)
{
    _bin_buffer.clear();

    try {
        zpp::serializer::memory_output_archive out(_bin_buffer);
        out(pkt->pkt.header, pkt->pkt.body);
    }
    catch (const std::exception&)
    {
        printf("failed to serialize packet\n");
        return;
    }

    auto addr = GekkoNetAddress();
    addr.data = const_cast<void*>(static_cast<const void*>(pkt->addr.GetAddress()));
    addr.size = pkt->addr.GetSize();

    host->send_data(&addr, (char*)_bin_buffer.data(), (int)_bin_buffer.size());
}

void Gekko::MessageSystem::ParsePacket(NetAddress& addr, NetPacket& pkt)
{
    u64 now = TimeSinceEpoch();
    // update receive timers.
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                player->stats.last_received_message = now;
            }
        }
    }

    // handle packet.
    const bool allow_any_magic =
        (pkt.header.type == SyncRequest) ||
        (pkt.header.type == SnapshotOffer) ||
        (pkt.header.type == SnapshotChunk) ||
        (pkt.header.type == SnapshotAck);
    if (pkt.header.magic != _session_magic && !allow_any_magic) {
        GEKKONET_TRACE_LOG("ParsePacket dropped: magic mismatch (got " << pkt.header.magic
            << ", expected " << _session_magic << ") type=" << pkt.header.type);
        if (pkt.header.type == SyncRequest) {
            OnSyncRequest(addr, pkt);
        }
        else {
            printf("dropped packet!\n");
        }
    }
    else {
        GEKKONET_TRACE_LOG("ParsePacket type=" << pkt.header.type << " magic=" << pkt.header.magic);
        switch (pkt.header.type)
        {
        case SyncResponse:
            OnSyncResponse(addr, pkt);
            return;
        case SyncRequest:
            OnSyncRequest(addr, pkt);
            return;
        case Inputs:
        case SpectatorInputs:
            OnInputs(addr, pkt);
            return;
        case InputAck:
            OnInputAck(addr, pkt);
            return;
        case SessionHealth:
            OnSessionHealth(addr, pkt);
            return;
        case NetworkHealth:
            OnNetworkHealth(addr, pkt);
            return;
        case SnapshotOffer:
            OnSnapshotOffer(addr, pkt);
            return;
        case SnapshotChunk:
            OnSnapshotChunk(addr, pkt);
            return;
        case SnapshotAck:
            OnSnapshotAck(addr, pkt);
            return;
        default:
            std::cerr << "[gekkonet] cannot process unknown event type=" << (int)pkt.header.type
                      << " magic=" << pkt.header.magic << std::endl;
            return;
        }
    }
}

void Gekko::MessageSystem::OnSyncRequest(NetAddress& addr, NetPacket& pkt)
{
    i32 should_send = 0;
    u64 now = TimeSinceEpoch();
    auto body = (SyncMsg*)pkt.body.get();
    GEKKONET_TRACE_LOG("OnSyncRequest from addr size=" << addr.GetSize() << " rng=" << body->rng_data);
    GEKKONET_TRACE_LOG("OnSyncRequest from addr size=" << addr.GetSize() << " rng=" << body->rng_data);

    // handle requests and set the peer its session magic for both remotes and spectators
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                player->session_magic = body->rng_data;
                if (player->sync_num == 0) {
                    player->stats.last_sent_sync_message = now;
                    should_send++;
                }
            }
        }
    }

    if (should_send > 0) {
	    // send a packet containing the local session magic
	    SendSyncResponse(&addr, body->rng_data);
    }
}

void Gekko::MessageSystem::OnSyncResponse(NetAddress& addr, NetPacket& pkt)
{
    i32 should_send = 0;
    u64 now = TimeSinceEpoch();
    auto body = (SyncMsg*)pkt.body.get();
    GEKKONET_TRACE_LOG("OnSyncResponse from addr size=" << addr.GetSize() << " rng=" << body->rng_data);
    GEKKONET_TRACE_LOG("OnSyncResponse from addr size=" << addr.GetSize() << " rng=" << body->rng_data);

    // handle sync responses for both remotes and spectators
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->GetStatus() == Connected) continue;

            if (player->address.Equals(addr)) {
                player->session_magic = body->rng_data;
                if (player->sync_num < NUM_TO_SYNC) {
                    player->sync_num++;
                    should_send++;
                    player->stats.last_sent_sync_message = now;
                    session_events.AddPlayerSyncingEvent(
                        player->handle,
                        player->sync_num,
                        NUM_TO_SYNC
                    );
                    continue;
                }

                if (player->sync_num >= NUM_TO_SYNC) {
                    player->SetStatus(Connected);
                    session_events.AddPlayerConnectedEvent(player->handle);
                    continue;
                }
            }
        }
    }

    if (should_send > 0) {
    	// send a packet containing the local session magic
    	SendSyncResponse(&addr, body->rng_data);
    }
}

void Gekko::MessageSystem::OnInputs(NetAddress& addr, NetPacket& pkt)
{
    auto body = (InputMsg*)pkt.body.get();
    auto net_input = std::make_unique<NetInputData>();

    net_input->handles = GetHandlesForAddress(&addr);
    net_input->input.inputs = Compression::RLEDecode(body->inputs.data(), (u32)body->inputs.size());

    net_input->input.input_count = body->input_count;
    net_input->input.start_frame = body->start_frame;
    net_input->input.total_size = (u16)net_input->input.inputs.size();

    /* Throttle noisy logging of input batches. */
    static unsigned oninputs_tick = 0;
    if (oninputs_tick < 3 || (oninputs_tick % 240) == 0) {
        std::string addr_str;
        if (addr.GetAddress() && addr.GetSize() > 0)
            addr_str.assign(reinterpret_cast<const char*>(addr.GetAddress()), addr.GetSize());

        if (net_input->handles.empty()) {
            std::cerr << "[gekkonet] OnInputs from " << addr_str
                      << " start=" << body->start_frame
                      << " count=" << (int)body->input_count
                      << " but no matching handles\n";
        } else {
            std::cerr << "[gekkonet] OnInputs from " << addr_str
                      << " start=" << body->start_frame
                      << " count=" << (int)body->input_count
                      << " handles[0]=" << net_input->handles.front()
                      << " total_size=" << net_input->input.total_size << "\n";
        }
    }
    oninputs_tick++;

    for (auto handle : net_input->handles) {
        auto player = GetPlayerByHandle(handle);
        if (player) {
            player->stats.last_received_frame = TimeSinceEpoch();
        }
    }

    _received_inputs.push(std::move(net_input));
}

void Gekko::MessageSystem::OnInputAck(NetAddress& addr, NetPacket& pkt)
{
    auto body = (InputAckMsg*)pkt.body.get();
    // we should just update the ack frame for all handles where the address matches
	const Frame ack_frame = body->ack_frame;
    const i32 remote_advantage = body->frame_advantage;
    bool added_advantage = false;

    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                if (player->stats.last_acked_frame < ack_frame) {
                    player->stats.last_acked_frame = ack_frame;
                    // only add remote advantages once
                    if (!added_advantage && i == 0) {
                        history.AddRemoteAdvantage(remote_advantage);
                        added_advantage = true;
                    }
                }
            }
        }
    }
}

void Gekko::MessageSystem::OnSessionHealth(NetAddress& addr, NetPacket& pkt)
{
    auto body = (SessionHealthMsg*)pkt.body.get();

    const Frame frame = body->frame;
    const u32 checksum = body->checksum;

    for (auto& player : remotes) {
        if (player->address.Equals(addr)) {
            player->SetChecksum(frame, checksum);

            for (auto iter = player->session_health.begin();
                iter != player->session_health.end(); ) {
                if (iter->first < (_last_added_input - 100)) {
                    iter = player->session_health.erase(iter);
                } else {
                    ++iter;
                }
            }
            break;
        }
    }
}

void Gekko::MessageSystem::OnNetworkHealth(NetAddress& addr, NetPacket& pkt)
{
    auto body = (NetworkHealthMsg*)pkt.body.get();

    // ok if its not a returned packet then update it and send it back to its specifc peer.
    if (!body->received) {
        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();

        auto player = GetPlayerByHandle(GetHandlesForAddress(&addr).at(0));

        message->pkt.header.magic = player->session_magic;
        message->pkt.header.type = NetworkHealth;

        auto new_body = std::make_unique<NetworkHealthMsg>();
        new_body->send_time = body->send_time;
        new_body->received = true;

        message->pkt.body = std::move(new_body);
        message->addr.Copy(&addr);
        return;
    }

    // else update network stats
    u16 rtt_ms = (u16)(TimeSinceEpoch() - body->send_time);
    std::vector<std::unique_ptr<Player>>* current = &remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& actor : *current) {
            // add rtt times to a list 
            if (addr.Equals(actor->address)) {
                actor->stats.rtt.push_back(rtt_ms);
            }
            // cleanup
            if (actor->stats.rtt.size() > 10) {
                actor->stats.rtt.erase(actor->stats.rtt.begin());
            }
        }
    }
}

u32 Gekko::MessageSystem::ComputeCRC(const std::vector<u8>& data)
{
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < data.size(); i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
        }
    }
    crc ^= 0xFFFFFFFFu;
    return crc;
}

void Gekko::MessageSystem::ResetSnapshotRecv()
{
    _snapshot_recv = SnapshotRecvState();
}

void Gekko::MessageSystem::ResetSnapshotSend()
{
    _snapshot_send = SnapshotSendState();
}

void Gekko::MessageSystem::StartSnapshotSend(const u8* data, u32 size, u32 crc, Frame frame)
{
    if (!data || size == 0) {
        return;
    }
    _snapshot_send.active = true;
    _snapshot_send.buffer.assign(data, data + size);
    _snapshot_send.crc = crc ? crc : ComputeCRC(_snapshot_send.buffer);
    _snapshot_send.frame = frame;
    _snapshot_send.chunk_size = SNAPSHOT_CHUNK_SIZE;
    _snapshot_send.next_offset = 0;
    _snapshot_send.last_send_time = 0;
    GEKKONET_TRACE_LOG("SnapshotSend start size=" << size << " crc=" << _snapshot_send.crc);
}

void Gekko::MessageSystem::SendSnapshotData(GekkoNetAdapter* host)
{
    if (!_snapshot_send.active || !_session) {
        return;
    }

    const u32 total = (u32)_snapshot_send.buffer.size();
    const u16 chunk_size = _snapshot_send.chunk_size;
    const u64 now = TimeSinceEpoch();

    /* If we've sent everything, wait for ack instead of resending the whole thing. */
    if (_snapshot_send.next_offset >= total) {
        return;
    }

    /* Send the offer periodically until we get an ack. */
    if (_snapshot_send.next_offset == 0 || (now - _snapshot_send.last_send_time) > 250) {
        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();
        message->pkt.header.type = SnapshotOffer;
        message->pkt.header.magic = _session_magic;
        auto body = std::make_unique<SnapshotOfferMsg>();
        body->total_size = total;
        body->crc = _snapshot_send.crc;
        body->frame = _snapshot_send.frame;
        body->chunk_size = chunk_size;
        message->pkt.body = std::move(body);
        GEKKONET_TRACE_LOG("Sending SnapshotOffer size=" << total << " crc=" << _snapshot_send.crc);
        std::cerr << "[gekkonet] SnapshotOffer size=" << total
                  << " crc=" << _snapshot_send.crc
                  << " chunk=" << chunk_size << std::endl;
        _snapshot_send.last_send_time = now;
    }

    /* Burst out a few chunks per call to avoid flooding. */
    u32 sent = 0;
    const u32 burst = 256; /* aggressive resend on loss */
    while (_snapshot_send.next_offset < total && sent < burst) {
        u32 remaining = total - _snapshot_send.next_offset;
        u32 to_copy = std::min<u32>(remaining, chunk_size);
        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();
        message->pkt.header.type = SnapshotChunk;
        message->pkt.header.magic = _session_magic;
        auto body = std::make_unique<SnapshotChunkMsg>();
        body->offset = _snapshot_send.next_offset;
        body->data.insert(body->data.end(),
            _snapshot_send.buffer.begin() + _snapshot_send.next_offset,
            _snapshot_send.buffer.begin() + _snapshot_send.next_offset + to_copy);
        message->pkt.body = std::move(body);
        if (sent == 0 || (sent % 64) == 0) {
            std::cerr << "[gekkonet] SnapshotChunk off=" << (_snapshot_send.next_offset)
                      << " len=" << to_copy << "/" << total << std::endl;
        }
        _snapshot_send.next_offset += to_copy;
        sent++;
    }
    if (sent > 0)
        _snapshot_send.last_send_time = now;
}

void Gekko::MessageSystem::AckSnapshot(u32 highest, bool complete)
{
    if (_snapshot_send.active && complete) {
        GEKKONET_TRACE_LOG("SnapshotSend complete acked highest=" << highest);
        ResetSnapshotSend();
    }
    else if (_snapshot_send.active) {
        GEKKONET_TRACE_LOG("SnapshotSend ack highest=" << highest << " complete=" << complete);
        /* Nudge the sender to resume from the acknowledged offset to avoid
         * endlessly replaying early chunks. */
        if (highest < _snapshot_send.buffer.size()) {
            _snapshot_send.next_offset = highest;
            _snapshot_send.last_send_time = 0;
        }
    }
}

void Gekko::MessageSystem::OnSnapshotOffer(NetAddress& addr, NetPacket& pkt)
{
    auto body = (SnapshotOfferMsg*)pkt.body.get();
    if (!body)
        return;

    /* If we’re already receiving the same snapshot (size+crc), ignore repeat offers
     * so we don’t reset progress mid-transfer. */
    if (_snapshot_recv.active &&
        _snapshot_recv.expected_size == body->total_size &&
        _snapshot_recv.crc == body->crc)
    {
        return;
    }

    _snapshot_recv.active = true;
    _snapshot_recv.expected_size = body->total_size;
    _snapshot_recv.crc = body->crc;
    _snapshot_recv.frame = body->frame;
    _snapshot_recv.chunk_size = body->chunk_size ? body->chunk_size : SNAPSHOT_CHUNK_SIZE;
    _snapshot_recv.buffer.resize(body->total_size);
    u32 chunks = (body->total_size + _snapshot_recv.chunk_size - 1) / _snapshot_recv.chunk_size;
    _snapshot_recv.received_bitmap.assign((chunks + 7) / 8, 0);
    _snapshot_recv.received_bytes = 0;
    _snapshot_recv.highest_complete = 0;
    GEKKONET_TRACE_LOG("SnapshotOffer recv size=" << body->total_size << " crc=" << body->crc);
    std::cerr << "[gekkonet] SnapshotOffer recv size=" << body->total_size
              << " crc=" << body->crc
              << " chunk=" << _snapshot_recv.chunk_size << std::endl;
}

void Gekko::MessageSystem::OnSnapshotChunk(NetAddress& addr, NetPacket& pkt)
{
    if (!_snapshot_recv.active)
        return;
    auto body = (SnapshotChunkMsg*)pkt.body.get();
    if (!body || body->offset >= _snapshot_recv.expected_size)
        return;

    u32 offset = body->offset;
    u32 cap = std::min<u32>(_snapshot_recv.chunk_size, _snapshot_recv.expected_size - offset);
    if (body->data.size() > cap)
        return;

    std::memcpy(&_snapshot_recv.buffer[offset], body->data.data(), body->data.size());
    std::cerr << "[gekkonet] SnapshotChunk recv off=" << offset
              << " len=" << body->data.size()
              << " bytes=" << _snapshot_recv.received_bytes + body->data.size()
              << "/" << _snapshot_recv.expected_size << std::endl;

    u32 chunk_idx = offset / _snapshot_recv.chunk_size;
    u32 byte_idx = chunk_idx / 8;
    u8 bit = 1u << (chunk_idx % 8);
    if (!(_snapshot_recv.received_bitmap[byte_idx] & bit)) {
        _snapshot_recv.received_bitmap[byte_idx] |= bit;
        _snapshot_recv.received_bytes += (u32)body->data.size();
        if (offset + body->data.size() > _snapshot_recv.highest_complete)
            _snapshot_recv.highest_complete = offset + (u32)body->data.size();
        if ((_snapshot_recv.received_bytes % (1024 * 128)) < body->data.size()) {
            std::cerr << "[gekkonet] Snapshot progress "
                      << _snapshot_recv.received_bytes << "/"
                      << _snapshot_recv.expected_size << std::endl;
        }
        /* Periodic partial ack to help the sender skip ahead. */
        if ((_snapshot_recv.received_bytes % (1024 * 256)) < body->data.size()) {
            _pending_output.push(std::make_unique<NetData>());
            auto& message = _pending_output.back();
            message->pkt.header.type = SnapshotAck;
            message->pkt.header.magic = _session_magic;
            auto ack = std::make_unique<SnapshotAckMsg>();
            ack->highest = _snapshot_recv.highest_complete;
            ack->crc = 0;
            ack->complete = false;
            message->pkt.body = std::move(ack);
            message->addr.Copy(&addr);
        }
    }

    if (_snapshot_recv.received_bytes >= _snapshot_recv.expected_size) {
        u32 crc = ComputeCRC(_snapshot_recv.buffer);
        bool ok = (crc == _snapshot_recv.crc);
        GEKKONET_TRACE_LOG("Snapshot recv complete crc=" << crc << " expected=" << _snapshot_recv.crc);
        const u32 completed_size = _snapshot_recv.expected_size;
        if (ok && _session) {
            _session->QueueSnapshotApply(_snapshot_recv.buffer.data(),
                                         completed_size,
                                         _snapshot_recv.crc,
                                         _snapshot_recv.frame);
            std::cerr << "[gekkonet] Snapshot apply queued size=" << completed_size
                      << " crc=" << crc << std::endl;
        }
        /* Send a completion ack back to the sender. */
        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();
        message->pkt.header.type = SnapshotAck;
        message->pkt.header.magic = _session_magic;
        auto ack = std::make_unique<SnapshotAckMsg>();
        ack->highest = completed_size;
        ack->crc = crc;
        ack->complete = ok;
        message->pkt.body = std::move(ack);
        message->addr.Copy(&addr);

        ResetSnapshotRecv();
        AckSnapshot(completed_size, ok);
    }
}

void Gekko::MessageSystem::OnSnapshotAck(NetAddress& addr, NetPacket& pkt)
{
    auto body = (SnapshotAckMsg*)pkt.body.get();
    if (!body)
        return;
    if (body->complete && _session) {
        /* Apply the snapshot locally on host when the client signals completion. */
        _session->ApplyLocalSnapshot(_snapshot_send.buffer.data(),
                                     (u32)_snapshot_send.buffer.size(),
                                     _snapshot_send.crc,
                                     _snapshot_send.frame);
    }
    AckSnapshot(body->highest, body->complete);
}

void Gekko::MessageSystem::AddPendingInput(bool spectator)
{
    u64 now = TimeSinceEpoch();

	const auto& send_list = spectator ? _spectator_input_send_list : _player_input_send_list;

    auto& cache = spectator ? _last_sent_spectator_input : _last_sent_input;
	const Frame last_added = spectator ? _last_added_spectator_input : _last_added_input;

    // just copy and resend the same data instead of doing calculations
    if (cache.frame != GameInput::NULL_FRAME && last_added == cache.frame) {
        // only resend if enough time has passed. we dont want to spam.
        if (cache.last_send_time + InputSendCache::INPUT_RESEND_DELAY > now) {
            return;
        }

        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();

        message->pkt.header.type = spectator ? SpectatorInputs : Inputs;

        auto body = std::make_unique<InputMsg>();
        body->Copy(&cache.data);

        message->pkt.body = std::move(body);

        cache.last_send_time = now;
        return;
    }

	const u32 num_players = spectator ? (u32)(locals.size() + remotes.size()) : (u32)locals.size();
	const u32 total_input_size = _input_size * num_players;
	const u32 input_count = (u32)send_list.size();
	const u32 total_size = total_input_size * input_count;

    auto inputs = std::make_unique<u8[]>(total_size);

    const u32 offset_per_player = total_size / num_players;

	u32 idx = 0;
	for (auto& input : send_list) {
        // line up all players input in series.
        // this should make RLE encoding more efficent in the end.
        // before P1|P2|P1|P2 now P1|P1|P2|P2
        // i should probably not be copying this much. fix this later TODO
        if (num_players > 1) {
            for (u32 i = 0; i < num_players; i++) {
                auto dst = &inputs[(idx * _input_size) + (i * offset_per_player)];
                auto src = &input[i * _input_size];
                std::memcpy(dst, src, _input_size);
            }
        }
        else {
            std::memcpy(&inputs[idx * _input_size], input, _input_size);
        }
		idx++;
	}

    auto comp = Compression::RLEEncode(inputs.get(), total_size);

    _pending_output.push(std::make_unique<NetData>());
	auto& message = _pending_output.back();

    message->pkt.header.type = spectator ? SpectatorInputs : Inputs;

    auto body = std::make_unique<InputMsg>();

    body->total_size = (u16)comp.size();
	body->input_count = (u8)send_list.size();
    /* Clamp start_frame to 0 to avoid sending negative frame indexes on first batch. */
    if (send_list.empty())
    {
        body->start_frame = 0;
    }
    else
    {
        Frame earliest = last_added - (Frame)send_list.size() + 1;
        if (earliest < 0)
            earliest = 0;
        body->start_frame = earliest;
    }

    body->inputs = std::move(comp);

    /* Log first few input packets to ensure they are being produced. */
    static unsigned input_send_tick = 0;
    if (!spectator && (input_send_tick < 5 || (input_send_tick % 240) == 0)) {
        std::cerr << "[gekkonet] SendInputs start=" << body->start_frame
                  << " count=" << (int)body->input_count
                  << " total_size=" << body->total_size
                  << " locals=" << locals.size()
                  << " remotes=" << remotes.size()
                  << std::endl;
    }
    input_send_tick++;

    // save to the cache for later use.
    cache.frame = last_added;
    cache.data.Copy(body.get());
    cache.last_send_time = now;

    message->pkt.body = std::move(body);
}

void Gekko::AdvantageHistory::Init()
{
    _adv_index = 0;
	_local_frame_adv = 0;

    std::memset(_remote_frame_adv, 0, HISTORY_SIZE * sizeof(i8));
	std::memset(_local, 0, HISTORY_SIZE * sizeof(i8));
	std::memset(_remote, 0, HISTORY_SIZE * sizeof(i8));
}

void Gekko::AdvantageHistory::Update(Frame frame)
{
	const u32 update_frame = std::max(frame, 0);

	_local[update_frame % HISTORY_SIZE] = _local_frame_adv;

	i32 sum = 0;
	for (i8 num : _remote_frame_adv) {
        sum += num;
	}

    sum /= HISTORY_SIZE;
    _remote[update_frame % HISTORY_SIZE] = sum;
}

f32 Gekko::AdvantageHistory::GetAverageAdvantage()
{
	f32 sum_local = 0.f;
	f32 sum_remote = 0.f;

	for (i32 i = 0; i < HISTORY_SIZE; i++) {
		sum_local += _local[i];
		sum_remote += _remote[i];
	}

	f32 avg_local = sum_local / HISTORY_SIZE;
	f32 avg_remote = sum_remote / HISTORY_SIZE;

	// return the frames ahead
	return (avg_local - avg_remote);
}

void Gekko::AdvantageHistory::SetLocalAdvantage(i8 adv) {
	_local_frame_adv = adv;
}

void Gekko::AdvantageHistory::AddRemoteAdvantage(i8 adv) {
    _remote_frame_adv[_adv_index % HISTORY_SIZE] = adv;
    _adv_index++;
}

i8 Gekko::AdvantageHistory::GetLocalAdvantage() {
	return _local_frame_adv;
}
