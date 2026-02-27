#include "panaudia/core.h"

namespace panaudia {

struct PanaudiaCore::Impl {
    SessionConfig config;
    ConnectionState state = ConnectionState::Disconnected;
};

PanaudiaCore::PanaudiaCore() : impl_(new Impl()) {}

PanaudiaCore::~PanaudiaCore() {
    delete impl_;
}

void PanaudiaCore::configure(const SessionConfig& config) {
    impl_->config = config;
}

void PanaudiaCore::connect() {
    // TODO: Phase 4
}

void PanaudiaCore::disconnect() {
    // TODO: Phase 4
}

void PanaudiaCore::update_jwt(const std::string& jwt) {
    impl_->config.jwt = jwt;
}

TrackHandle* PanaudiaCore::get_track(const std::string& /*name*/) {
    // TODO: Phase 4
    return nullptr;
}

void PanaudiaCore::write_audio(TrackHandle* /*track*/,
                                const float* /*samples*/,
                                uint32_t /*frame_count*/,
                                uint64_t /*host_time*/) {
    // TODO: Phase 4
}

uint32_t PanaudiaCore::read_audio(TrackHandle* /*track*/,
                                   float* /*buffer*/,
                                   uint32_t /*frame_count*/,
                                   uint64_t /*host_time*/) {
    // TODO: Phase 4
    return 0;
}

void PanaudiaCore::send_data(TrackHandle* /*track*/,
                              const uint8_t* /*data*/,
                              uint32_t /*data_len*/) {
    // TODO: Phase 4
}

ConnectionState PanaudiaCore::get_connection_state() const {
    return impl_->state;
}

BufferStatus PanaudiaCore::get_buffer_status(TrackHandle* /*track*/) const {
    return {};
}

SessionStats PanaudiaCore::get_stats() const {
    return {impl_->state, 0, 0, 0, 0, 0, 0.0};
}

}  // namespace panaudia
