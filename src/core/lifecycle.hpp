#pragma once

#include <cstdint>
#include <expected>

#include "lws/options.hpp"

// Internal core header. The lifecycle of a streamer as one pure table: it is created, then
// running, then stopped. There is one start and one stop, and no restart. Every call asks the table
// what the state becomes, or why the call is refused.
namespace lws {

enum class RunState : std::uint8_t { Created, Running, Stopped };

enum class Control : std::uint8_t { SetCallback, Start, Push, Flush, Stop };

// The state after a call that succeeds, or the refusal. Setting the callback only works before
// start; a second start, and any start after stop, is AlreadyStarted; push, flush and stop need a
// running streamer, and a stopped one is no longer running.
[[nodiscard]] constexpr std::expected<RunState, Error> next_state(RunState state,
                                                                  Control call) noexcept {
    switch (state) {
    case RunState::Created:
        switch (call) {
        case Control::SetCallback:
            return RunState::Created;
        case Control::Start:
            return RunState::Running;
        case Control::Push:
        case Control::Flush:
        case Control::Stop:
            return std::unexpected(Error::NotRunning);
        }
        break;
    case RunState::Running:
        switch (call) {
        case Control::SetCallback:
        case Control::Start:
            return std::unexpected(Error::AlreadyStarted);
        case Control::Push:
        case Control::Flush:
            return RunState::Running;
        case Control::Stop:
            return RunState::Stopped;
        }
        break;
    case RunState::Stopped:
        switch (call) {
        case Control::SetCallback:
        case Control::Start:
            return std::unexpected(Error::AlreadyStarted);
        case Control::Push:
        case Control::Flush:
        case Control::Stop:
            return std::unexpected(Error::NotRunning);
        }
        break;
    }
    return std::unexpected(Error::NotRunning);
}

} // namespace lws
