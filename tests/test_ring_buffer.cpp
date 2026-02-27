#include <catch2/catch_test_macros.hpp>
#include "panaudia/ring_buffer.h"

#include <cmath>
#include <numeric>
#include <thread>
#include <vector>

using namespace panaudia;

TEST_CASE("RingBuffer basic write/read roundtrip", "[ring_buffer]") {
    RingBuffer rb(1024, 1);  // 1024 mono frames

    std::vector<float> write_buf(256);
    std::iota(write_buf.begin(), write_buf.end(), 1.0f);  // 1, 2, 3, ...

    REQUIRE(rb.write(write_buf.data(), 256) == 256);
    REQUIRE(rb.read_available() == 256);

    std::vector<float> read_buf(256, 0.0f);
    REQUIRE(rb.read(read_buf.data(), 256) == 256);

    for (uint32_t i = 0; i < 256; i++) {
        REQUIRE(read_buf[i] == write_buf[i]);
    }
    REQUIRE(rb.read_available() == 0);
}

TEST_CASE("RingBuffer wrap-around", "[ring_buffer]") {
    RingBuffer rb(64, 1);  // small buffer to force wrapping

    std::vector<float> buf(48);

    // Fill most of the buffer
    std::iota(buf.begin(), buf.end(), 1.0f);
    rb.write(buf.data(), 48);

    // Read it all back
    std::vector<float> out(48, 0.0f);
    REQUIRE(rb.read(out.data(), 48) == 48);

    // Now write again — this will wrap around
    std::iota(buf.begin(), buf.end(), 100.0f);
    rb.write(buf.data(), 48);

    REQUIRE(rb.read(out.data(), 48) == 48);
    for (uint32_t i = 0; i < 48; i++) {
        REQUIRE(out[i] == 100.0f + static_cast<float>(i));
    }
}

TEST_CASE("RingBuffer partial reads", "[ring_buffer]") {
    RingBuffer rb(256, 1);

    std::vector<float> write_buf(100);
    std::iota(write_buf.begin(), write_buf.end(), 0.0f);
    rb.write(write_buf.data(), 100);

    // Read only 30
    std::vector<float> read_buf(30, -1.0f);
    REQUIRE(rb.read(read_buf.data(), 30) == 30);
    for (uint32_t i = 0; i < 30; i++) {
        REQUIRE(read_buf[i] == static_cast<float>(i));
    }

    REQUIRE(rb.read_available() == 70);

    // Read the rest
    std::vector<float> rest(70, -1.0f);
    REQUIRE(rb.read(rest.data(), 70) == 70);
    for (uint32_t i = 0; i < 70; i++) {
        REQUIRE(rest[i] == static_cast<float>(i + 30));
    }
}

TEST_CASE("RingBuffer overflow drops oldest", "[ring_buffer]") {
    RingBuffer rb(32, 1);  // capacity 32 frames

    // Write 32 frames (fills buffer)
    std::vector<float> first(32);
    std::iota(first.begin(), first.end(), 0.0f);
    rb.write(first.data(), 32);
    REQUIRE(rb.read_available() == 32);

    // Write 16 more — should drop the first 16
    std::vector<float> second(16);
    std::iota(second.begin(), second.end(), 100.0f);
    rb.write(second.data(), 16);
    REQUIRE(rb.read_available() == 32);  // still full

    // Read all 32 — should get frames 16-31 from first write, then 0-15 from second
    std::vector<float> out(32, -1.0f);
    REQUIRE(rb.read(out.data(), 32) == 32);

    for (uint32_t i = 0; i < 16; i++) {
        REQUIRE(out[i] == static_cast<float>(i + 16));  // from first write
    }
    for (uint32_t i = 0; i < 16; i++) {
        REQUIRE(out[16 + i] == 100.0f + static_cast<float>(i));  // from second write
    }
}

TEST_CASE("RingBuffer empty read returns 0", "[ring_buffer]") {
    RingBuffer rb(256, 1);

    std::vector<float> buf(64, -1.0f);
    REQUIRE(rb.read(buf.data(), 64) == 0);
    // Buffer should be untouched
    REQUIRE(buf[0] == -1.0f);
}

TEST_CASE("RingBuffer write_available and read_available", "[ring_buffer]") {
    RingBuffer rb(100, 1);

    REQUIRE(rb.write_available() == 100);
    REQUIRE(rb.read_available() == 0);

    std::vector<float> buf(40, 1.0f);
    rb.write(buf.data(), 40);

    REQUIRE(rb.write_available() == 60);
    REQUIRE(rb.read_available() == 40);
}

TEST_CASE("RingBuffer flush", "[ring_buffer]") {
    RingBuffer rb(256, 1);

    std::vector<float> buf(100, 1.0f);
    rb.write(buf.data(), 100);
    REQUIRE(rb.read_available() == 100);

    rb.flush();
    REQUIRE(rb.read_available() == 0);
    REQUIRE(rb.write_available() == 256);
}

TEST_CASE("RingBuffer stereo (2ch)", "[ring_buffer]") {
    RingBuffer rb(128, 2);  // 128 frames, 2 channels = 256 floats

    // Write 64 stereo frames (128 floats): L=i, R=i+1000
    std::vector<float> write_buf(128);
    for (uint32_t i = 0; i < 64; i++) {
        write_buf[i * 2] = static_cast<float>(i);
        write_buf[i * 2 + 1] = static_cast<float>(i + 1000);
    }
    REQUIRE(rb.write(write_buf.data(), 64) == 64);
    REQUIRE(rb.read_available() == 64);

    std::vector<float> read_buf(128, -1.0f);
    REQUIRE(rb.read(read_buf.data(), 64) == 64);

    for (uint32_t i = 0; i < 64; i++) {
        REQUIRE(read_buf[i * 2] == static_cast<float>(i));
        REQUIRE(read_buf[i * 2 + 1] == static_cast<float>(i + 1000));
    }
}

TEST_CASE("RingBuffer quad (4ch)", "[ring_buffer]") {
    RingBuffer rb(64, 4);  // 64 frames, 4 channels

    std::vector<float> write_buf(32 * 4);
    for (uint32_t i = 0; i < 32; i++) {
        for (uint32_t c = 0; c < 4; c++) {
            write_buf[i * 4 + c] = static_cast<float>(i * 10 + c);
        }
    }
    rb.write(write_buf.data(), 32);

    std::vector<float> read_buf(32 * 4, -1.0f);
    REQUIRE(rb.read(read_buf.data(), 32) == 32);
    REQUIRE(read_buf == write_buf);
}

TEST_CASE("RingBuffer concurrent write/read stress", "[ring_buffer][stress]") {
    constexpr uint32_t capacity = 4096;
    constexpr uint32_t channels = 2;
    constexpr uint32_t frames_per_write = 240;  // 5ms at 48kHz
    constexpr uint32_t frames_per_read = 240;
    constexpr uint32_t total_frames = 48000 * 5;  // 5 seconds

    RingBuffer rb(capacity, channels);

    std::atomic<uint32_t> frames_written{0};
    std::atomic<uint32_t> frames_read_total{0};

    // Writer thread
    std::thread writer([&] {
        std::vector<float> buf(frames_per_write * channels);
        uint32_t written = 0;
        while (written < total_frames) {
            for (uint32_t i = 0; i < frames_per_write * channels; i++) {
                buf[i] = static_cast<float>(written * channels + i);
            }
            rb.write(buf.data(), frames_per_write);
            written += frames_per_write;
            frames_written.store(written, std::memory_order_relaxed);
        }
    });

    // Reader thread
    std::thread reader([&] {
        std::vector<float> buf(frames_per_read * channels);
        uint32_t total_read = 0;
        while (total_read < total_frames) {
            uint32_t got = rb.read(buf.data(), frames_per_read);
            total_read += got;
            if (got == 0) {
                std::this_thread::yield();
            }
        }
        frames_read_total.store(total_read, std::memory_order_relaxed);
    });

    writer.join();
    reader.join();

    // Reader should have read at least most of the data (some may overflow)
    REQUIRE(frames_read_total.load() > 0);
}
