#include "engine/audio_session.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    fluxrec::engine::AudioSessionSnapshot snapshot;
    snapshot.ids = {"desktop", "track-1"};
    snapshot.names = {"Desktop audio", "Extra track"};
    snapshot.tracks.resize(2);
    snapshot.tracks[1].monitor_error = "speakers missing";

    std::ostringstream output;
    fluxrec::engine::writeAudioTelemetryObject(output, snapshot, true);
    const std::string json = output.str();

    require(json.find("\"tracks\":[") != std::string::npos,
            "serialized audio object must contain a tracks array");
    require(json.find("\"desktop\"") != std::string::npos,
            "serialized tracks must include id desktop");
    require(json.find("\"track-1\"") != std::string::npos,
            "serialized tracks must include id track-1");
    require(json.find("speakers missing") != std::string::npos &&
                json.find("\"monitor_error\"") != std::string::npos,
            "serialized tracks must include monitor_error");
    require(json.find("\"desktop\":") == std::string::npos,
            "serialized audio object must not use a desktop object key");
    require(json.find("\"microphone\":") == std::string::npos,
            "serialized audio object must not use a microphone object key");

    std::cout << "audio telemetry shape uses tracks[] with ids and monitor_error\n";
    return 0;
}
