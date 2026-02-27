#include <catch2/catch_test_macros.hpp>
#include "panaudia/core.h"

TEST_CASE("PanaudiaCore can be created and destroyed", "[session]") {
    panaudia::PanaudiaCore core;
    REQUIRE(core.get_connection_state() == panaudia::ConnectionState::Disconnected);
}

TEST_CASE("PanaudiaCore can be configured", "[session]") {
    panaudia::PanaudiaCore core;

    panaudia::SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";

    core.configure(config);
    REQUIRE(core.get_connection_state() == panaudia::ConnectionState::Disconnected);
}

TEST_CASE("get_track returns nullptr before configure", "[session]") {
    panaudia::PanaudiaCore core;
    REQUIRE(core.get_track("nonexistent") == nullptr);
}
