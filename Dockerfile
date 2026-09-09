# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

# Preparation-only product build. Demo Control owns invocation and export.
# The digest is the official linux/arm64 image, not a mutable multi-arch tag.
FROM debian:bookworm-slim@sha256:6bd27d44e6c32a66bbd72d7cb2b76a8ae3497ec2e5274a81abd1b37f6013fa1f AS toolchain
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG TARGETARCH
ARG BUILD_JOBS=4
ENV DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8 TZ=UTC SOURCE_DATE_EPOCH=1
RUN test "${TARGETARCH}" = arm64 && test "$(dpkg --print-architecture)" = arm64 && \
    case "${BUILD_JOBS}" in [1-8]) ;; *) exit 2 ;; esac
# Apt metadata and package selection are frozen; archive signatures still apply.
# Check-Valid-Until is disabled only because this is an immutable dated snapshot.
RUN rm /etc/apt/sources.list.d/debian.sources && \
    printf '%s\n' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260901T000000Z bookworm main' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20260901T000000Z bookworm-security main' \
      > /etc/apt/sources.list && \
    apt-get -o Acquire::Retries=0 update && \
    apt-get -o Acquire::Retries=0 install --no-install-recommends -y \
      build-essential binutils cmake ninja-build git ca-certificates curl pkg-config python3 perl && \
    mkdir -p /src /opt/bhs /build

FROM toolchain AS dependencies
ARG BUILD_JOBS=4
# Exact commits, not tags. The selected gRPC submodules are gitlink-pinned by
# that commit; unused BoringSSL/benchmark/test sources are not downloaded.
RUN git init /src/abseil && \
    git -C /src/abseil remote add origin https://github.com/abseil/abseil-cpp.git && \
    git -C /src/abseil fetch --depth 1 origin 54fac219c4ef0bc379dfffb0b8098725d77ac81b && \
    git -C /src/abseil checkout --detach FETCH_HEAD && \
    test "$(git -C /src/abseil rev-parse HEAD)" = 54fac219c4ef0bc379dfffb0b8098725d77ac81b && \
    cmake -S /src/abseil -B /build/abseil -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=/opt/bhs \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
      -DABSL_PROPAGATE_CXX_STD=ON -DABSL_ENABLE_INSTALL=ON -DABSL_BUILD_TESTING=OFF && \
    cmake --build /build/abseil --parallel "${BUILD_JOBS}" && cmake --install /build/abseil

RUN git init /src/protobuf && \
    git -C /src/protobuf remote add origin https://github.com/protocolbuffers/protobuf.git && \
    git -C /src/protobuf fetch --depth 1 origin a4cbdd3ed0042e8f9b9c30e8b0634096d9532809 && \
    git -C /src/protobuf checkout --detach FETCH_HEAD && \
    test "$(git -C /src/protobuf rev-parse HEAD)" = a4cbdd3ed0042e8f9b9c30e8b0634096d9532809 && \
    cmake -S /src/protobuf -B /build/protobuf -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=/opt/bhs \
      -DCMAKE_PREFIX_PATH=/opt/bhs -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
      -Dprotobuf_ABSL_PROVIDER=package -Dprotobuf_WITH_ZLIB=OFF && \
    cmake --build /build/protobuf --parallel "${BUILD_JOBS}" && cmake --install /build/protobuf

# Use the already accepted Factory OpenSSL release and archive hash. No TLS
# bypass, private key, system CA bundle or reusable certificate is exported.
RUN curl --fail --location --max-time 180 \
      https://github.com/openssl/openssl/releases/download/openssl-3.2.6/openssl-3.2.6.tar.gz \
      --output /src/openssl.tar.gz && \
    printf '%s\n' '89681a9ddaa9ed7cf25ea8ef61338db805200bae47d00510490623547380c148  /src/openssl.tar.gz' | sha256sum --check --strict && \
    tar -xzf /src/openssl.tar.gz -C /src && \
    cd /src/openssl-3.2.6 && \
    ./Configure linux-aarch64 no-shared no-module no-dso no-tests -fPIC \
      --prefix=/opt/bhs --openssldir=/opt/bhs/ssl --libdir=lib && \
    make -j "${BUILD_JOBS}" && make install_sw

RUN git init /src/grpc && \
    git -C /src/grpc remote add origin https://github.com/grpc/grpc.git && \
    git -C /src/grpc fetch --depth 1 origin e5ae3b6b44bf3b64d24bfb4b4f82556239b986db && \
    git -C /src/grpc checkout --detach FETCH_HEAD && \
    test "$(git -C /src/grpc rev-parse HEAD)" = e5ae3b6b44bf3b64d24bfb4b4f82556239b986db && \
    git -C /src/grpc submodule update --init --depth 1 \
      third_party/cares/cares third_party/re2 third_party/zlib \
      third_party/envoy-api third_party/googleapis third_party/opencensus-proto \
      third_party/opentelemetry third_party/protoc-gen-validate third_party/xds && \
    cmake -S /src/grpc -B /build/grpc -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=/opt/bhs \
      -DCMAKE_PREFIX_PATH=/opt/bhs -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF \
      -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
      -DgRPC_ABSL_PROVIDER=package -DgRPC_PROTOBUF_PROVIDER=package \
      -DgRPC_SSL_PROVIDER=package -DOPENSSL_ROOT_DIR=/opt/bhs -DOPENSSL_USE_STATIC_LIBS=ON \
      -DgRPC_CARES_PROVIDER=module -DgRPC_RE2_PROVIDER=module -DgRPC_ZLIB_PROVIDER=module \
      -DgRPC_BACKWARDS_COMPATIBILITY_MODE=ON && \
    cmake --build /build/grpc --parallel "${BUILD_JOBS}" && cmake --install /build/grpc

RUN git init /src/kuksa && \
    git -C /src/kuksa remote add origin https://github.com/eclipse-kuksa/kuksa-databroker.git && \
    git -C /src/kuksa fetch --depth 1 origin 30e5c13abc496d0b39aaa6c25acebb088b9902e3 && \
    git -C /src/kuksa checkout --detach FETCH_HEAD && \
    test "$(git -C /src/kuksa rev-parse HEAD)" = 30e5c13abc496d0b39aaa6c25acebb088b9902e3

FROM dependencies AS product
ARG BUILD_JOBS=4
ARG SOURCE_REVISION
ARG SOURCE_DATE_EPOCH
ENV SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH}
WORKDIR /src/service
COPY . /src/service
RUN [[ "${SOURCE_REVISION}" =~ ^[0-9a-f]{40}$ ]] && [[ "${SOURCE_DATE_EPOCH}" =~ ^[0-9]+$ ]] && \
    cmake -S /src/service -B /build/service -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=/usr \
      -DCMAKE_PREFIX_PATH=/opt/bhs -DOPENSSL_ROOT_DIR=/opt/bhs -DOPENSSL_USE_STATIC_LIBS=ON \
      -DCMAKE_EXE_LINKER_FLAGS='-static-libgcc -static-libstdc++ -Wl,--build-id=sha1' \
      -DCMAKE_CXX_FLAGS='-ffile-prefix-map=/src=. -fdebug-prefix-map=/src=.' \
      -DBHS_BUILD_KUKSA_RUNTIME=ON -DBHS_KUKSA_SOURCE_ROOT=/src/kuksa -DBUILD_TESTING=ON && \
    cmake --build /build/service --parallel "${BUILD_JOBS}" && \
    ctest --test-dir /build/service --output-on-failure --output-junit /build/service/ctest-results.xml && \
    DESTDIR=/build/rootfs cmake --install /build/service && \
    python3 tools/build_scaffold.py --runtime-root /build/rootfs --output /out \
      --source-revision "${SOURCE_REVISION}" --source-date-epoch "${SOURCE_DATE_EPOCH}" \
      --dependency-source-root /src --test-report /build/service/ctest-results.xml

# This is an artifact export target, not a deployable/runnable Docker service.
FROM scratch AS export
COPY --from=product /out/ /
