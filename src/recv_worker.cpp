// recv_worker.cpp — Recv dispatch lives in SessionManager::dispatch_audio_datagram()
// and dispatch_data_datagram() in session_manager.cpp, called from the
// on_datagram msquic callback. No separate recv worker thread needed.
