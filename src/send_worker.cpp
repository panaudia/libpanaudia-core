// send_worker.cpp — Send logic lives in SessionManager::poll_outbound_tracks(),
// send_audio_frame(), and send_pcm_frame() in session_manager.cpp.
// No separate send worker class needed — the session thread polls ring buffers.
