#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "panaudia/ring_buffer.h"
#include "panaudia/jitter_buffer.h"

#include <vector>

using namespace panaudia;

// --- RingBuffer benchmarks ---

TEST_CASE("RingBuffer write latency", "[bench][ring_buffer]") {
    RingBuffer rb(4096, 1);
    std::vector<float> buf(240, 0.5f);

    BENCHMARK("write 240 mono frames") {
        return rb.write(buf.data(), 240);
    };
}

TEST_CASE("RingBuffer read latency", "[bench][ring_buffer]") {
    RingBuffer rb(4096, 1);
    std::vector<float> write_buf(240, 0.5f);
    std::vector<float> read_buf(240);

    // Keep buffer fed
    BENCHMARK_ADVANCED("read 240 mono frames")(Catch::Benchmark::Chronometer meter) {
        rb.write(write_buf.data(), 240);
        meter.measure([&] {
            return rb.read(read_buf.data(), 240);
        });
    };
}

TEST_CASE("RingBuffer write+read cycle", "[bench][ring_buffer]") {
    RingBuffer rb(4096, 2);  // stereo
    std::vector<float> write_buf(480, 0.5f);  // 240 stereo frames
    std::vector<float> read_buf(480);

    BENCHMARK("write+read 240 stereo frames") {
        rb.write(write_buf.data(), 240);
        return rb.read(read_buf.data(), 240);
    };
}

// --- JitterBuffer benchmarks ---

static JitterBufferConfig bench_config(uint32_t channels = 1) {
    return {
        48000, channels, 60, 20, 10, 200, 1000, 16,
    };
}

TEST_CASE("JitterBuffer write latency", "[bench][jitter_buffer]") {
    JitterBuffer jb(bench_config());
    std::vector<float> buf(240, 0.5f);

    BENCHMARK("write 240 floats") {
        jb.write(buf.data(), 240);
        return 0;
    };
}

TEST_CASE("JitterBuffer read latency (PLAYING)", "[bench][jitter_buffer]") {
    JitterBuffer jb(bench_config());
    std::vector<float> write_buf(240, 0.5f);
    std::vector<float> read_buf(240);

    // Fill to playing state
    std::vector<float> fill(2880, 0.5f);
    jb.write(fill.data(), 2880);

    BENCHMARK_ADVANCED("read 240 floats")(Catch::Benchmark::Chronometer meter) {
        // Keep buffer fed between iterations
        jb.write(write_buf.data(), 240);
        meter.measure([&] {
            return jb.read(read_buf.data(), 240);
        });
    };
}

TEST_CASE("JitterBuffer write+read cycle", "[bench][jitter_buffer]") {
    JitterBuffer jb(bench_config());
    std::vector<float> write_buf(240, 0.5f);
    std::vector<float> read_buf(240);

    // Fill to playing state
    std::vector<float> fill(2880, 0.5f);
    jb.write(fill.data(), 2880);
    jb.read(read_buf.data(), 240);  // transition to PLAYING

    BENCHMARK("write+read 240 mono floats") {
        jb.write(write_buf.data(), 240);
        return jb.read(read_buf.data(), 240);
    };
}

TEST_CASE("JitterBuffer write+read stereo cycle", "[bench][jitter_buffer]") {
    JitterBuffer jb(bench_config(2));
    std::vector<float> write_buf(480, 0.5f);  // 240 samples * 2ch
    std::vector<float> read_buf(480);

    // Fill to playing state
    std::vector<float> fill(5760, 0.5f);  // 2880 samples * 2ch
    jb.write(fill.data(), 5760);
    jb.read(read_buf.data(), 480);

    BENCHMARK("write+read 240 stereo samples") {
        jb.write(write_buf.data(), 480);
        return jb.read(read_buf.data(), 480);
    };
}
