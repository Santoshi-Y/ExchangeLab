FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tools ./tools

RUN cmake \
        -S . \
        -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEXCHANGELAB_BUILD_TESTS=OFF \
        -DEXCHANGELAB_BUILD_BENCHMARKS=OFF \
    && cmake --build build --parallel \
        --target \
            exchange_lab \
            exchange_websocket_gateway \
            exchange_fix_gateway \
            exchange_replay

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /data

COPY --from=build /src/build/exchange_lab /app/bin/exchange_lab
COPY --from=build /src/build/exchange_websocket_gateway /app/bin/exchange_websocket_gateway
COPY --from=build /src/build/exchange_fix_gateway /app/bin/exchange_fix_gateway
COPY --from=build /src/build/exchange_replay /app/bin/exchange_replay
COPY docker/backend-entrypoint.sh /app/backend-entrypoint.sh

RUN chmod +x /app/backend-entrypoint.sh

VOLUME ["/data"]

EXPOSE 9000 8080 9878

HEALTHCHECK --interval=5s --timeout=2s --start-period=5s --retries=10 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/9000' || exit 1

ENTRYPOINT ["/app/backend-entrypoint.sh"]
