FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Default: build and test
CMD ["bash", "-c", "\
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) && \
    echo '--- Core tests ---' && \
    ./build/panaudia-core-tests '~[integration]~[4d]' && \
    echo '--- Statecache tests ---' && \
    ./build/panaudia-statecache-tests && \
    echo '--- Benchmarks ---' && \
    ./build/panaudia-bench \
"]
