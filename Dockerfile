FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Default: build and test
CMD ["bash", "-c", "\
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) && \
    echo '--- Tests ---' && \
    ./build/panaudia-core-tests && \
    echo '--- Benchmarks ---' && \
    ./build/panaudia-bench \
"]
