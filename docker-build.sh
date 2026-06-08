#!/usr/bin/env bash
# Build and test libpanaudia-core in a Linux container.
# Usage:
#   ./docker-build.sh          # build + test
#   ./docker-build.sh tsan     # build + test under ThreadSanitizer

set -euo pipefail
cd "$(dirname "$0")"

IMAGE="panaudia-core-linux"

docker build -t "$IMAGE" .

if [[ "${1:-}" == "tsan" ]]; then
    echo "=== Running with ThreadSanitizer ==="
    docker run --rm "$IMAGE" bash -c "\
        cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_CXX_FLAGS='-fsanitize=thread' \
            -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' && \
        cmake --build build-tsan -j\$(nproc) && \
        ./build-tsan/panaudia-core-tests '~[stress]' && \
        ./build-tsan/panaudia-statecache-tests \
    "
else
    docker run --rm "$IMAGE"
fi
