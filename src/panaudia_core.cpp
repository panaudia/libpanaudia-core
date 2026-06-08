#include "panaudia/core.h"
#include "panaudia/session_manager.h"

namespace panaudia {

struct PanaudiaCore::Impl {
    SessionManager session;
};

PanaudiaCore::PanaudiaCore() : impl_(new Impl()) {}

PanaudiaCore::~PanaudiaCore() {
    delete impl_;
}

void PanaudiaCore::configure(const SessionConfig& config) {
    impl_->session.configure(config);
}

void PanaudiaCore::connect() {
    impl_->session.connect();
}

void PanaudiaCore::disconnect() {
    impl_->session.disconnect();
}

void PanaudiaCore::update_jwt(const std::string& jwt) {
    impl_->session.update_jwt(jwt);
}

TrackHandle* PanaudiaCore::get_track(const std::string& name) {
    return impl_->session.get_track(name);
}

void PanaudiaCore::write_audio(TrackHandle* track,
                                const float* samples,
                                uint32_t frame_count,
                                uint64_t host_time) {
    impl_->session.write_audio(track, samples, frame_count, host_time);
}

uint32_t PanaudiaCore::read_audio(TrackHandle* track,
                                   float* buffer,
                                   uint32_t frame_count,
                                   uint64_t host_time) {
    return impl_->session.read_audio(track, buffer, frame_count, host_time);
}

void PanaudiaCore::send_data(TrackHandle* track,
                              const uint8_t* data,
                              uint32_t data_len) {
    impl_->session.send_data(track, data, data_len);
}

ConnectionState PanaudiaCore::get_connection_state() const {
    return impl_->session.get_connection_state();
}

BufferStatus PanaudiaCore::get_buffer_status(TrackHandle* track) const {
    return impl_->session.get_buffer_status(track);
}

SessionStats PanaudiaCore::get_stats() const {
    return impl_->session.get_stats();
}

const CacheMap* PanaudiaCore::get_cache_map(TrackHandle* track) const {
    return impl_->session.get_cache_map(track);
}

}  // namespace panaudia
