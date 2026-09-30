#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unistd.h>

namespace fluxrec::engine {

enum class ControlCommandType : std::uint8_t {
    save_replay,
    start_replay,
    stop_replay,
    set_audio_volume,
    set_audio_mute,
    stop,
};

struct ControlCommand {
    ControlCommandType type{ControlCommandType::stop};
    // Set only for save_replay. The reader accepts absolute paths up to the
    // configured line limit, including paths that contain spaces.
    std::string path;
    std::string audio_track_id;
    float audio_gain{1.0F};
    bool audio_muted{};
};

struct ControlPipeStats {
    std::uint64_t commands_enqueued{};
    std::uint64_t commands_popped{};
    std::uint64_t invalid_lines{};
    std::uint64_t oversized_lines{};
    std::uint64_t queue_full_lines{};
    bool eof{};
    bool stop_command_seen{};
    bool running{};
    // Zero means no read/poll error; otherwise this is the saved errno value.
    int read_error{};
};

// Reads one command per line from input_fd on a dedicated thread. The default
// protocol is:
//
//   save_replay /absolute/output/path.mkv\n
//   start_replay\n
//   stop_replay\n
//   set_audio_volume desktop 0.75\n
//   set_audio_mute microphone on\n
//   stop\n
// The reader never writes to the descriptor. Accepted commands are delivered
// through a fixed-capacity, single-producer/single-consumer queue. try_pop()
// is nonblocking and is intended to be called from the engine's control path.
//
// EOF stops the reader after parsing a final unterminated line. close() also
// stops it promptly: the reader uses poll() with a bounded timeout, and reads
// a single byte only after the descriptor reports readiness.
class ControlPipe final {
public:
    static constexpr std::size_t queue_capacity = 16;
    static constexpr std::size_t max_line_bytes = 4096;

    // input_fd is duplicated internally, so closing this object never closes
    // the caller's descriptor. Use exactly one ControlPipe per input stream.
    explicit ControlPipe(int input_fd = STDIN_FILENO);
    ~ControlPipe();

    ControlPipe(const ControlPipe&) = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;
    ControlPipe(ControlPipe&&) = delete;
    ControlPipe& operator=(ControlPipe&&) = delete;

    // Returns false immediately when no command is ready. There must be one
    // consumer; the input worker is the sole producer.
    [[nodiscard]] bool try_pop(ControlCommand& command) noexcept;

    // Requests shutdown and joins the input worker. Repeated calls are safe.
    void close() noexcept;

    [[nodiscard]] ControlPipeStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::engine
