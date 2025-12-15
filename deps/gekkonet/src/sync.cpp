#include "sync.h"

#include <memory>
#include <cstring>
#include <climits>
#include <iostream>

Gekko::SyncSystem::SyncSystem()
{
	_current_frame = GameInput::NULL_FRAME;
	_input_size = 0;
	_num_players = 0;
	_input_buffers = nullptr;
}

void Gekko::SyncSystem::Init(u8 num_players, u32 input_size)
{
	_input_size = input_size;
	_num_players = num_players;
	_current_frame = GameInput::NULL_FRAME + 1;

    _input_buffers = std::make_unique<InputBuffer[]>(num_players);
	// on creation setup input buffers
	for (int i = 0; i < _num_players; i++) {
		_input_buffers[i].Init(0, 0, input_size);
	}
}

void Gekko::SyncSystem::AddLocalInput(Handle player, u8* input)
{
	// drop inputs from incorrect handles
    if (player >= _num_players || player < 0) {
        return;
    }

	_input_buffers[player].AddLocalInput(_current_frame, input);
}

void Gekko::SyncSystem::AddRemoteInput(Handle player, u8* input, Frame frame)
{
	// drop inputs from incorrect handles
    if (player >= _num_players || player < 0) {
        return;
    }

	_input_buffers[player].AddInput(frame, input);
}

void Gekko::SyncSystem::IncrementFrame()
{
	_current_frame++;
}

bool Gekko::SyncSystem::GetSpectatorInputs(std::unique_ptr<u8[]>& inputs, Frame frame) 
{
    auto all_input = std::make_unique<u8[]>(_input_size * _num_players);
	for (u8 i = 0; i < _num_players; i++) {
		auto inp = _input_buffers[i].GetInput(frame, false);

		if (inp->frame == GameInput::NULL_FRAME) {
			return false;
		}

		std::memcpy(all_input.get() + (i * _input_size), (void*)inp->input.get(), _input_size);
	}
	inputs = std::move(all_input);
	return true;
}

bool Gekko::SyncSystem::GetCurrentInputs(std::unique_ptr<u8[]>& inputs, Frame& frame)
{
    auto all_input = std::make_unique<u8[]>(_input_size * _num_players);
    for (u8 i = 0; i < _num_players; i++) {
        auto& buf = _input_buffers[i];
        auto inp = buf.GetInput(_current_frame, true);

        if (inp->frame == GameInput::NULL_FRAME) {
            /* Synthesize a neutral input for the missing frame so we do not stall.
             * This can happen if we have not received anything yet for a peer. */
            auto neutral = std::make_unique<u8[]>(_input_size);
            std::memset(neutral.get(), 0, _input_size);
            /* Force-write even if the frame is behind the most recent received input
             * so we do not drop it due to frame ordering checks. */
            buf.ForceFill(_current_frame, neutral.get());
            inp = buf.GetInput(_current_frame, false);
        }

        if (inp->frame == GameInput::NULL_FRAME) {
            /* Throttle noisy diagnostics: first few misses, then every 120. */
            static unsigned miss_tick = 0;
            if (miss_tick < 5 || (miss_tick % 120) == 0) {
                std::cerr << "[gekkonet] GetCurrentInputs missing player="
                          << (int)i
                          << " cur_frame=" << _current_frame
                          << " last_recv=" << buf.GetLastReceivedFrame()
                          << std::endl;
            }
            miss_tick++;
            return false;
        }

        std::memcpy(all_input.get() + (i * _input_size), (void*)inp->input.get(), _input_size);
    }
	frame = _current_frame;
	inputs = std::move(all_input);

    /* Throttle success diagnostics to confirm inputs are available. */
    {
        static unsigned success_tick = 0;
        if (success_tick < 3 || (success_tick % 240) == 0)
        {
            Frame min_last = INT_MAX;
            for (u8 i = 0; i < _num_players; i++)
                min_last = std::min(min_last, _input_buffers[i].GetLastReceivedFrame());
            if (min_last == INT_MAX)
                min_last = GameInput::NULL_FRAME;
            std::cerr << "[gekkonet] GetCurrentInputs OK frame="
                      << _current_frame
                      << " min_last_recv=" << min_last
                      << std::endl;
        }
        success_tick++;
    }
	return true;
}

bool Gekko::SyncSystem::GetLocalInputs(std::vector<Handle>& handles, std::unique_ptr<u8[]>& inputs, Frame frame)
{	
	inputs.reset();
    auto all_input = std::make_unique<u8[]>(_input_size * handles.size());
	for (u8 i = 0; i < handles.size(); i++) {
		auto inp = _input_buffers[handles[i]].GetInput(frame, true);

		if (inp->frame == GameInput::NULL_FRAME) {	
			return false;
		}

		std::memcpy(all_input.get() + (i * _input_size), (void*)inp->input.get(), _input_size);
	}
	inputs = std::move(all_input);
	return true;
}

void Gekko::SyncSystem::SetLocalDelay(Handle player, u8 delay)
{
	_input_buffers[player].SetDelay(delay);
}

u8 Gekko::SyncSystem::GetLocalDelay(Handle player)
{
	return _input_buffers[player].GetDelay();
}

void Gekko::SyncSystem::SetInputPredictionWindow(Handle player, u8 input_window)
{
	_input_buffers[player].SetInputPredictionWindow(input_window);
}

Frame Gekko::SyncSystem::GetCurrentFrame()
{
	return _current_frame;
}

void Gekko::SyncSystem::SetCurrentFrame(Frame frame)
{
	_current_frame = frame;
}

Frame Gekko::SyncSystem::GetMinIncorrectFrame()
{
	Frame min = INT_MAX;
	for (i32 i = 0; i < _num_players; i++) {
		Frame frame = _input_buffers[i].GetIncorrectPredictionFrame();
		if (frame == GameInput::NULL_FRAME) {
			continue;
		}
		min = std::min(frame, min);
	}
	return min == INT_MAX ? GameInput::NULL_FRAME : min;
}

Frame Gekko::SyncSystem::GetMinReceivedFrame()
{
	Frame min = INT_MAX;
	for (i32 i = 0; i < _num_players; i++) {
		Frame frame = _input_buffers[i].GetLastReceivedFrame();
		min = std::min(frame, min);
	}
	return min == INT_MAX ? GameInput::NULL_FRAME : min;
}

Frame Gekko::SyncSystem::GetLastReceivedFrom(Handle player)
{
    if (player >= 0 && player < _num_players) {
        return _input_buffers[player].GetLastReceivedFrame();
    }

    return GameInput::NULL_FRAME;
}

void Gekko::SyncSystem::ForceReceivedFrame(Handle player, Frame frame, u8* input)
{
    if (!input || player < 0 || player >= _num_players || frame < 0) {
        return;
    }

    auto& buf = _input_buffers[player];
    Frame last = buf.GetLastReceivedFrame();
    Frame start = (last == GameInput::NULL_FRAME) ? 0 : last + 1;
    if (frame < start) {
        return;
    }

    for (Frame f = start; f <= frame; ++f) {
        buf.AddInput(f, input);
    }
}
