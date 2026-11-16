# Build from the repository root:
#   docker build -f server/Dockerfile -t mini-dbms .
FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential ca-certificates cmake git ninja-build \
       libjsoncpp-dev uuid-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
COPY src ./src
COPY server ./server

RUN cmake -S server -B /build -G Ninja \
       -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build /build --target minidb_server --parallel 2

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       libjsoncpp25 libuuid1 zlib1g libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && install -d -o 10001 -g 10001 /data

COPY --from=build /build/minidb_server /usr/local/bin/minidb_server

USER 10001:10001
WORKDIR /data
VOLUME ["/data"]
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/minidb_server"]
CMD ["--host", "0.0.0.0", "--data-dir", "/data"]
