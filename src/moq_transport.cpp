#include <panaudia/moq_transport.h>
#include <panaudia/moq_protocol.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200)  // nonstandard extension: zero-sized array
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#pragma GCC diagnostic ignored "-Wextra-semi"
#endif
#include <msquic.h>
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <vector>

namespace panaudia {

// ---------------------------------------------------------------------------
// SendContext — used for async StreamSend cleanup
// ---------------------------------------------------------------------------

struct SendContext {
    uint8_t* data_buf;
    QUIC_BUFFER* quic_buf;
};

// ---------------------------------------------------------------------------
// MoqTransport::Impl
// ---------------------------------------------------------------------------

struct MoqTransport::Impl {
    // --- msquic handles ---
    const QUIC_API_TABLE* msquic = nullptr;
    HQUIC registration = nullptr;
    HQUIC configuration = nullptr;
    HQUIC connection = nullptr;
    HQUIC control_stream = nullptr;

    // --- State ---
    std::atomic<TransportState> current_state{TransportState::Disconnected};
    std::atomic<uint32_t> max_dgram_size{0};

    // --- Callbacks ---
    TransportCallbacks callbacks;

    // --- msquic thread-safe queues (plain C++ only) ---
    std::queue<std::vector<uint8_t>> pending_control_data;
    std::mutex control_data_mutex;

    // --- Connection event flags (set by msquic callbacks) ---
    std::atomic<bool> pending_connected{false};
    std::atomic<bool> pending_transport_shutdown{false};
    std::atomic<bool> pending_peer_shutdown{false};

    // --- Control stream reassembly buffer (caller thread only, used in process_incoming) ---
    std::vector<uint8_t> recv_buffer;

    // --- Disconnect guard ---
    std::atomic<bool> disconnecting{false};

    // =========================================================================
    // Lifecycle
    // =========================================================================

    bool init_quic(const TransportConfig& config) {
        QUIC_STATUS status = MsQuicOpen2(&msquic);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] MsQuicOpen2 failed: 0x%x\n", status);
            return false;
        }

        // Registration with low-latency profile
        QUIC_REGISTRATION_CONFIG reg_config = {};
        reg_config.AppName = "panaudia-core";
        reg_config.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;

        status = msquic->RegistrationOpen(&reg_config, &registration);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] RegistrationOpen failed: 0x%x\n", status);
            return false;
        }

        // ALPN buffer
        QUIC_BUFFER alpn_buffer;
        alpn_buffer.Length = static_cast<uint32_t>(strlen(moq::kMoqAlpn));
        alpn_buffer.Buffer = const_cast<uint8_t*>(
            reinterpret_cast<const uint8_t*>(moq::kMoqAlpn));

        // QUIC settings
        QUIC_SETTINGS settings = {};
        settings.IsSet.DatagramReceiveEnabled = TRUE;
        settings.DatagramReceiveEnabled = TRUE;
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.IdleTimeoutMs = config.idle_timeout_ms;
        settings.IsSet.PeerBidiStreamCount = TRUE;
        settings.PeerBidiStreamCount = 10;
        settings.IsSet.PeerUnidiStreamCount = TRUE;
        settings.PeerUnidiStreamCount = 10;

        status = msquic->ConfigurationOpen(
            registration, &alpn_buffer, 1,
            &settings, sizeof(settings),
            nullptr, &configuration);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] ConfigurationOpen failed: 0x%x\n", status);
            return false;
        }

        // TLS credential (client mode)
        QUIC_CREDENTIAL_CONFIG cred_config = {};
        cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
        if (config.skip_cert_validation) {
            cred_config.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        }

        status = msquic->ConfigurationLoadCredential(configuration, &cred_config);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] ConfigurationLoadCredential failed: 0x%x\n", status);
            return false;
        }

        return true;
    }

    void cleanup_quic() {
        if (control_stream && msquic) {
            msquic->StreamClose(control_stream);
            control_stream = nullptr;
        }

        if (connection && msquic) {
            msquic->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            msquic->ConnectionClose(connection);
            connection = nullptr;
        }

        if (configuration && msquic) {
            msquic->ConfigurationClose(configuration);
            configuration = nullptr;
        }

        if (registration && msquic) {
            msquic->RegistrationClose(registration);
            registration = nullptr;
        }

        if (msquic) {
            MsQuicClose(msquic);
            msquic = nullptr;
        }

        // Reset state
        max_dgram_size.store(0);
        pending_connected.store(false);
        pending_transport_shutdown.store(false);
        pending_peer_shutdown.store(false);
        recv_buffer.clear();

        // Drain queues
        {
            std::lock_guard<std::mutex> lock(control_data_mutex);
            while (!pending_control_data.empty()) pending_control_data.pop();
        }
    }

    // =========================================================================
    // Connection
    // =========================================================================

    bool start_connection(const TransportConfig& config) {
        QUIC_STATUS status = msquic->ConnectionOpen(
            registration,
            static_connection_callback,
            this,
            &connection);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] ConnectionOpen failed: 0x%x\n", status);
            return false;
        }

        status = msquic->ConnectionStart(
            connection,
            configuration,
            QUIC_ADDRESS_FAMILY_UNSPEC,
            config.host.c_str(),
            config.port);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] ConnectionStart failed: 0x%x\n", status);
            return false;
        }

        return true;
    }

    // =========================================================================
    // MOQ Session Start (called when QUIC CONNECTED fires)
    // =========================================================================

    void start_moq_session() {
        QUIC_STATUS status = msquic->StreamOpen(
            connection,
            QUIC_STREAM_OPEN_FLAG_NONE,  // bidirectional
            static_stream_callback,
            this,
            &control_stream);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] StreamOpen failed: 0x%x\n", status);
            set_state(TransportState::Failed, "Failed to open control stream");
            return;
        }

        status = msquic->StreamStart(control_stream, QUIC_STREAM_START_FLAG_NONE);
        if (QUIC_FAILED(status)) {
            fprintf(stderr, "[panaudia] StreamStart failed: 0x%x\n", status);
            set_state(TransportState::Failed, "Failed to start control stream");
            return;
        }

        // Send CLIENT_SETUP
        auto setup = moq::build_client_setup();
        send_on_control_stream(setup.data(), static_cast<uint32_t>(setup.size()));
    }

    // =========================================================================
    // Control Stream Send
    // =========================================================================

    bool send_on_control_stream(const uint8_t* data, uint32_t len) {
        if (!control_stream || !msquic) return false;

        size_t alloc_size = static_cast<size_t>(len);
        auto* data_buf = static_cast<uint8_t*>(malloc(alloc_size));
        if (!data_buf) return false;
        memcpy(data_buf, data, len);

        auto* quic_buf = static_cast<QUIC_BUFFER*>(malloc(sizeof(QUIC_BUFFER)));
        if (!quic_buf) { free(data_buf); return false; }
        quic_buf->Buffer = data_buf;
        quic_buf->Length = len;

        auto* ctx = static_cast<SendContext*>(malloc(sizeof(SendContext)));
        if (!ctx) { free(data_buf); free(quic_buf); return false; }
        ctx->data_buf = data_buf;
        ctx->quic_buf = quic_buf;

        QUIC_STATUS status = msquic->StreamSend(
            control_stream,
            quic_buf, 1,
            QUIC_SEND_FLAG_NONE,
            ctx);

        if (QUIC_FAILED(status)) {
            free(data_buf);
            free(quic_buf);
            free(ctx);
            return false;
        }

        return true;
    }

    // =========================================================================
    // Datagram Send (single-block allocation, same pattern as UE plugin)
    // =========================================================================

    bool send_datagram_impl(const uint8_t* data, uint32_t len) {
        if (!msquic || !connection || len == 0) return false;

        // Single allocation: [QUIC_BUFFER][payload bytes]
        // Freed in DATAGRAM_SEND_STATE_CHANGED callback
        size_t total_size = sizeof(QUIC_BUFFER) + static_cast<size_t>(len);
        auto* block = static_cast<uint8_t*>(malloc(total_size));
        if (!block) return false;

        auto* qb = reinterpret_cast<QUIC_BUFFER*>(block);
        qb->Buffer = block + sizeof(QUIC_BUFFER);
        qb->Length = len;
        memcpy(qb->Buffer, data, len);

        QUIC_STATUS status = msquic->DatagramSend(
            connection, qb, 1,
            QUIC_SEND_FLAG_NONE, block);

        if (QUIC_FAILED(status)) {
            free(block);
            return false;
        }

        return true;
    }

    // =========================================================================
    // State Management
    // =========================================================================

    void set_state(TransportState new_state, const char* message) {
        current_state.store(new_state);
        if (callbacks.on_state_changed) {
            callbacks.on_state_changed(new_state, message);
        }
    }

    // =========================================================================
    // msquic Static Callbacks -> Instance Methods
    // =========================================================================

    static QUIC_STATUS QUIC_API static_connection_callback(
        HQUIC conn, void* context, QUIC_CONNECTION_EVENT* event)
    {
        (void)conn;
        auto* self = static_cast<Impl*>(context);
        return self->on_connection_event(event);
    }

    static QUIC_STATUS QUIC_API static_stream_callback(
        HQUIC stream, void* context, QUIC_STREAM_EVENT* event)
    {
        (void)stream;
        auto* self = static_cast<Impl*>(context);
        return self->on_stream_event(event);
    }

    // =========================================================================
    // Connection Event Handler (msquic thread — no blocking!)
    // =========================================================================

    QUIC_STATUS on_connection_event(QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            pending_connected.store(true);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            fprintf(stderr, "[panaudia] QUIC transport shutdown: 0x%llx\n",
                    static_cast<unsigned long long>(
                        event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
            pending_transport_shutdown.store(true);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            fprintf(stderr, "[panaudia] QUIC peer shutdown: %llu\n",
                    static_cast<unsigned long long>(
                        event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
            pending_peer_shutdown.store(true);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            // Accept server-initiated streams with our stream callback
            msquic->SetCallbackHandler(
                event->PEER_STREAM_STARTED.Stream,
                reinterpret_cast<void*>(static_stream_callback),
                this);
            break;

        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
            if (event->DATAGRAM_STATE_CHANGED.SendEnabled) {
                max_dgram_size.store(event->DATAGRAM_STATE_CHANGED.MaxSendLength);
            }
            if (callbacks.on_datagram_state_changed) {
                callbacks.on_datagram_state_changed(
                    event->DATAGRAM_STATE_CHANGED.MaxSendLength);
            }
            break;

        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
            {
                // Parse + dispatch datagram inline on msquic thread (zero-copy)
                const uint8_t* raw = event->DATAGRAM_RECEIVED.Buffer->Buffer;
                uint32_t raw_len = event->DATAGRAM_RECEIVED.Buffer->Length;

                if (callbacks.on_datagram) {
                    moq::ObjectDatagram dg;
                    if (moq::parse_object_datagram(raw, static_cast<int32_t>(raw_len), dg)) {
                        callbacks.on_datagram(
                            dg.track_alias, dg.group_id, dg.object_id,
                            dg.priority, dg.payload, dg.payload_len);
                    }
                }
            }
            break;

        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
            {
                // Free single-block send context on terminal states
                void* raw_ctx = event->DATAGRAM_SEND_STATE_CHANGED.ClientContext;
                auto dg_state = event->DATAGRAM_SEND_STATE_CHANGED.State;
                if (raw_ctx &&
                    dg_state != QUIC_DATAGRAM_SEND_UNKNOWN &&
                    dg_state != QUIC_DATAGRAM_SEND_SENT &&
                    dg_state != QUIC_DATAGRAM_SEND_LOST_SUSPECT)
                {
                    free(raw_ctx);
                }
            }
            break;

        default:
            break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    // =========================================================================
    // Stream Event Handler (msquic thread — no blocking!)
    // =========================================================================

    QUIC_STATUS on_stream_event(QUIC_STREAM_EVENT* event) {
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            {
                // Copy received bytes and queue for process_incoming()
                for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                    const uint8_t* data = event->RECEIVE.Buffers[i].Buffer;
                    uint32_t len = event->RECEIVE.Buffers[i].Length;

                    std::vector<uint8_t> buf(data, data + len);
                    {
                        std::lock_guard<std::mutex> lock(control_data_mutex);
                        pending_control_data.push(std::move(buf));
                    }
                }
            }
            break;

        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            // Free the SendContext allocated in send_on_control_stream
            if (event->SEND_COMPLETE.ClientContext) {
                auto* ctx = static_cast<SendContext*>(
                    event->SEND_COMPLETE.ClientContext);
                free(ctx->data_buf);
                free(ctx->quic_buf);
                free(ctx);
            }
            break;

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            break;

        default:
            break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    // =========================================================================
    // process_incoming — drain queues, reassemble, parse control messages
    // =========================================================================

    void drain_and_parse() {
        // 1. Check connection event flags
        if (pending_connected.exchange(false)) {
            set_state(TransportState::Connected, "QUIC connected");
            start_moq_session();
        }

        if (pending_transport_shutdown.exchange(false)) {
            if (!disconnecting.load()) {
                set_state(TransportState::Failed, "Transport shutdown");
            }
        }

        if (pending_peer_shutdown.exchange(false)) {
            if (!disconnecting.load()) {
                set_state(TransportState::Failed, "Peer shutdown");
            }
        }

        // 2. Drain control data queue into reassembly buffer
        {
            std::vector<uint8_t> chunk;
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(control_data_mutex);
                    if (pending_control_data.empty()) break;
                    chunk = std::move(pending_control_data.front());
                    pending_control_data.pop();
                }
                recv_buffer.insert(recv_buffer.end(), chunk.begin(), chunk.end());
            }
        }

        // 3. Parse complete control messages from reassembly buffer
        if (recv_buffer.empty()) return;

        int32_t offset = 0;
        int32_t buffer_len = static_cast<int32_t>(recv_buffer.size());

        while (offset < buffer_len) {
            moq::MessageType type;
            const uint8_t* content;
            int32_t content_len;
            int32_t consumed;

            if (!moq::parse_control_message(
                    recv_buffer.data() + offset,
                    buffer_len - offset,
                    type, content, content_len, consumed))
            {
                break;  // incomplete message, wait for more data
            }

            uint64_t raw_type = static_cast<uint64_t>(type);

            // Handle SERVER_SETUP internally to transition to Ready
            if (type == moq::MessageType::ServerSetup) {
                moq::ServerSetupResult result;
                if (moq::parse_server_setup(content, content_len, result)) {
                    set_state(TransportState::Ready, "MOQ session ready");
                }
            }

            // Always forward to callback (including SERVER_SETUP)
            if (callbacks.on_control_message) {
                callbacks.on_control_message(raw_type, content, content_len);
            }

            offset += consumed;
        }

        // Remove consumed bytes
        if (offset > 0) {
            recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + offset);
        }
    }
};

// ---------------------------------------------------------------------------
// MoqTransport public API
// ---------------------------------------------------------------------------

MoqTransport::MoqTransport()
    : impl_(new Impl())
{
}

MoqTransport::~MoqTransport() {
    disconnect();
    delete impl_;
}

bool MoqTransport::connect(const TransportConfig& config,
                            const TransportCallbacks& callbacks)
{
    if (impl_->current_state.load() != TransportState::Disconnected) {
        return false;
    }

    impl_->disconnecting.store(false);
    impl_->callbacks = callbacks;
    impl_->set_state(TransportState::Connecting, "Connecting");

    if (!impl_->init_quic(config)) {
        impl_->set_state(TransportState::Failed, "Failed to initialize QUIC");
        impl_->cleanup_quic();
        return false;
    }

    if (!impl_->start_connection(config)) {
        impl_->set_state(TransportState::Failed, "Failed to start QUIC connection");
        impl_->cleanup_quic();
        return false;
    }

    return true;
}

void MoqTransport::disconnect() {
    impl_->disconnecting.store(true);
    impl_->cleanup_quic();
    impl_->set_state(TransportState::Disconnected, "Disconnected");
}

bool MoqTransport::send_control(const uint8_t* data, uint32_t len) {
    if (impl_->current_state.load() < TransportState::Connected) {
        return false;
    }
    return impl_->send_on_control_stream(data, len);
}

bool MoqTransport::send_datagram(const uint8_t* data, uint32_t len) {
    if (impl_->current_state.load() < TransportState::Connected) {
        return false;
    }
    return impl_->send_datagram_impl(data, len);
}

void MoqTransport::process_incoming() {
    impl_->drain_and_parse();
}

TransportState MoqTransport::state() const {
    return impl_->current_state.load();
}

uint32_t MoqTransport::max_datagram_size() const {
    return impl_->max_dgram_size.load();
}

}  // namespace panaudia
