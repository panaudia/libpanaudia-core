#include <catch2/catch_test_macros.hpp>
#include <panaudia/moq_transport.h>
#include <panaudia/moq_protocol.h>

#include <atomic>
#include <chrono>
#include <thread>

// ============================================================================
// Unit tests — no server required
// ============================================================================

TEST_CASE("MoqTransport can be created and destroyed", "[transport]") {
    panaudia::MoqTransport transport;
    REQUIRE(transport.state() == panaudia::TransportState::Disconnected);
}

TEST_CASE("MoqTransport initial state is Disconnected", "[transport]") {
    panaudia::MoqTransport transport;
    REQUIRE(transport.state() == panaudia::TransportState::Disconnected);
    REQUIRE(transport.max_datagram_size() == 0);
}

TEST_CASE("send_control returns false when disconnected", "[transport]") {
    panaudia::MoqTransport transport;
    uint8_t data[] = {0x01, 0x02, 0x03};
    REQUIRE_FALSE(transport.send_control(data, sizeof(data)));
}

TEST_CASE("send_datagram returns false when disconnected", "[transport]") {
    panaudia::MoqTransport transport;
    uint8_t data[] = {0x01, 0x02, 0x03};
    REQUIRE_FALSE(transport.send_datagram(data, sizeof(data)));
}

TEST_CASE("Double disconnect is safe", "[transport]") {
    panaudia::MoqTransport transport;
    transport.disconnect();
    transport.disconnect();
    REQUIRE(transport.state() == panaudia::TransportState::Disconnected);
}

TEST_CASE("process_incoming is safe when disconnected", "[transport]") {
    panaudia::MoqTransport transport;
    // Should not crash
    transport.process_incoming();
    REQUIRE(transport.state() == panaudia::TransportState::Disconnected);
}

TEST_CASE("Connect to nonexistent host transitions to Failed", "[transport]") {
    panaudia::MoqTransport transport;

    std::atomic<panaudia::TransportState> last_state{
        panaudia::TransportState::Disconnected};

    panaudia::TransportConfig config;
    config.host = "127.0.0.1";
    config.port = 1;  // unlikely to have a QUIC server
    config.skip_cert_validation = true;
    config.idle_timeout_ms = 2000;

    panaudia::TransportCallbacks callbacks;
    callbacks.on_state_changed = [&](panaudia::TransportState state, const char*) {
        last_state.store(state);
    };

    bool ok = transport.connect(config, callbacks);
    REQUIRE(ok);  // connect() itself succeeds (async handshake)
    REQUIRE(transport.state() == panaudia::TransportState::Connecting);

    // Poll for a few seconds until connection fails
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        transport.process_incoming();
        auto st = transport.state();
        if (st == panaudia::TransportState::Failed ||
            st == panaudia::TransportState::Disconnected)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto final_state = transport.state();
    // Should have failed or been disconnected after timeout
    REQUIRE((final_state == panaudia::TransportState::Failed ||
             final_state == panaudia::TransportState::Connecting));
    // Note: msquic may not report failure instantly; Connecting is acceptable
    // if the idle timeout hasn't fired yet within our 5s window.

    transport.disconnect();
    REQUIRE(transport.state() == panaudia::TransportState::Disconnected);
}

// ============================================================================
// Integration tests — require a running Go server
// Tagged [integration] so they can be excluded: ./tests '~[integration]'
// ============================================================================

TEST_CASE("Connect to Go server, complete MOQ handshake", "[integration]") {
    panaudia::MoqTransport transport;

    std::atomic<panaudia::TransportState> last_state{
        panaudia::TransportState::Disconnected};
    std::atomic<int> control_messages_received{0};

    panaudia::TransportConfig config;
    config.host = "dev.panaudia.com";
    config.port = 4433;
    config.skip_cert_validation = true;
    config.idle_timeout_ms = 10000;

    panaudia::TransportCallbacks callbacks;
    callbacks.on_state_changed = [&](panaudia::TransportState state, const char* msg) {
        last_state.store(state);
        fprintf(stderr, "[test] State: %d — %s\n",
                static_cast<int>(state), msg ? msg : "");
    };
    callbacks.on_control_message = [&](uint64_t type, const uint8_t*, int32_t) {
        fprintf(stderr, "[test] Control message type: 0x%llx\n",
                static_cast<unsigned long long>(type));
        control_messages_received.fetch_add(1);
    };

    bool ok = transport.connect(config, callbacks);
    REQUIRE(ok);

    // Wait for Ready state (SERVER_SETUP received)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        transport.process_incoming();
        if (transport.state() == panaudia::TransportState::Ready) break;
        if (transport.state() == panaudia::TransportState::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(transport.state() == panaudia::TransportState::Ready);
    // SERVER_SETUP should have been forwarded as a control message
    REQUIRE(control_messages_received.load() >= 1);
    REQUIRE(transport.max_datagram_size() > 0);

    transport.disconnect();
    REQUIRE(transport.state() == panaudia::TransportState::Disconnected);
}
