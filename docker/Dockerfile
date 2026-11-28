# Stage 1: Development Environment
FROM alpine:3.24 AS dev

# libxml2-dev is only for the XSD validation tests; the production image does not get it.
RUN apk add --no-cache \
    bash \
    boost-dev \
    ca-certificates \
    git \
    clang \
    clang-extra-tools \
    compiler-rt \
    cppcheck \
    g++ \
    libc++-dev \
    libxml2-dev \
    cmake \
    ninja \
    gdb \
    lldb \
    valgrind \
    valgrind-scripts \
    linux-headers \
    make \
    nodejs \
    npm \
    perl

# lcov 1.16 comes from its release archive, checked by SHA-256. lcov 2 (up to 2.6) rejects GCC 15
# coverage data: it expects a taken branch on every line that ran, but GCC puts exception cleanup
# paths that never run on closing braces. The download from GitHub is tried up to three times.
RUN for wait_seconds in 30 90 0; do \
        wget -q -O /tmp/lcov.tar.gz \
            https://github.com/linux-test-project/lcov/releases/download/v1.16/lcov-1.16.tar.gz && \
            break; \
        [ "$wait_seconds" -gt 0 ] || exit 1; \
        echo "lcov download failed. Retrying in $wait_seconds s..."; \
        sleep "$wait_seconds"; \
    done && \
    echo "987031ad5528c8a746d4b52b380bc1bffe412de1f2b9c2ba5224995668e3240b  /tmp/lcov.tar.gz" | \
        sha256sum -c - && \
    tar -xzf /tmp/lcov.tar.gz -C /tmp && \
    make -C /tmp/lcov-1.16 install && \
    rm -rf /tmp/lcov.tar.gz /tmp/lcov-1.16

WORKDIR /workspace

CMD ["/bin/bash"]

# Stage 2: Frontend. Builds the four files that the server embeds (see frontend/).
# `npm ci --ignore-scripts` installs exactly the versions of package-lock.json and never runs an
# install script of a dependency. The files are written to /workspace/build/frontend/dist.
FROM alpine:3.24 AS frontend

RUN apk add --no-cache nodejs npm

WORKDIR /workspace/frontend
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci --ignore-scripts

COPY frontend/ ./
RUN npm run build

# Stage 3: Builder. Only what the static release build needs.
FROM alpine:3.24 AS builder

RUN apk add --no-cache \
    boost-dev \
    ca-certificates \
    clang \
    cmake \
    g++ \
    git \
    linux-headers \
    ninja

WORKDIR /workspace
COPY . /workspace
COPY --from=frontend /workspace/build/frontend/dist /workspace/frontend-dist

# The dependency downloads (FetchContent) go to a BuildKit cache folder, so a rebuild after a
# source change does not download them again. COPY above changes with every source change.
RUN --mount=type=cache,target=/deps \
    for wait_seconds in 30 90 0; do \
        CC=clang CXX=clang++ cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release \
            -DTAXBROKER_STATIC=ON -DTAXBROKER_BUILD_TESTS=OFF \
            -DTAXBROKER_FRONTEND_DIR=/workspace/frontend-dist -DFETCHCONTENT_BASE_DIR=/deps && \
            break; \
        [ "$wait_seconds" -gt 0 ] || exit 1; \
        echo "Configure failed, maybe a dependency download. Retrying in $wait_seconds s..."; \
        sleep "$wait_seconds"; \
    done && \
    cmake --build build --target taxbroker taxbroker_server -j$(nproc) && \
    strip build/src/taxbroker build/src/taxbroker_server

# Stage 4: Production
FROM alpine:3.24 AS prod
# The executables are static, so no runtime libraries are needed.
RUN apk add --no-cache ca-certificates && \
    addgroup -S app && adduser -S app -G app && \
    mkdir -p /app /var/log/taxbroker /var/lib/taxbroker && \
    chown -R app:app /app /var/log/taxbroker /var/lib/taxbroker

WORKDIR /app
COPY --from=builder --chown=app:app /workspace/build/src/taxbroker_server /app/taxbroker_server
COPY --from=builder --chown=app:app /workspace/build/src/taxbroker /app/taxbroker
# The static executables contain third-party code; its licences ship next to them. The notices
# name the Alpine versions of musl, the GCC runtime and Boost: update them with an Alpine upgrade.
COPY --from=builder --chown=app:app \
    /workspace/LICENSE /workspace/THIRD_PARTY_NOTICES.txt /app/

USER app

EXPOSE 8080
CMD ["./taxbroker_server"]

# Stage 5: Linux packages. The two static binaries of the builder, the packaging files and the
# tools that make the tar.gz, the .deb and the AppImage (`packaging/linux/make_packages.sh`, run
# by `scripts/package.sh`). `dpkg` gives dpkg-deb, `squashfs-tools` the image of the AppImage,
# `desktop-file-utils` the check of the menu entry and `imagemagick` (with its JPEG reader) the PNG icon.
FROM alpine:3.24 AS package

RUN apk add --no-cache \
    bash \
    ca-certificates \
    coreutils \
    desktop-file-utils \
    dpkg \
    findutils \
    gzip \
    imagemagick \
    imagemagick-jpeg \
    squashfs-tools \
    tar

# The AppImage runtime (type 2, release 20251108) is the first part of every AppImage. It is
# checked by SHA-256 (the value is from the release asset when this was written; the release is
# a numbered tag, so the file does not change). The notices of its libraries are in
# packaging/linux/APPIMAGE_RUNTIME_NOTICES.txt. The download is tried up to three times.
RUN for wait_seconds in 30 90 0; do \
        wget -q -O /tmp/runtime-x86_64 \
            https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 && \
            break; \
        [ "$wait_seconds" -gt 0 ] || exit 1; \
        echo "Runtime download failed. Retrying in $wait_seconds s..."; \
        sleep "$wait_seconds"; \
    done && \
    echo "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d  /tmp/runtime-x86_64" | \
        sha256sum -c -

WORKDIR /package
COPY --from=builder /workspace/build/src/taxbroker /workspace/build/src/taxbroker_server /package/
COPY --from=builder /workspace/LICENSE /workspace/THIRD_PARTY_NOTICES.txt /package/
COPY --from=builder /workspace/docs/assets/icon.jpg /package/icon.jpg
COPY --from=builder /workspace/packaging/linux /package/packaging
RUN mv /tmp/runtime-x86_64 /package/runtime-x86_64

# The packages go to /out (a mounted folder); the time of every file is SOURCE_DATE_EPOCH.
CMD ["/package/packaging/make_packages.sh"]

# Stage 6: Browser tests. The development image plus Chromium, for the end-to-end tests of the
# frontend (frontend/e2e). It starts from the built taxbrokerreport-dev image (made
# by scripts/build.sh image, loaded in CI), so it never builds the dev stage again. Playwright's
# own browser builds need glibc, so the Alpine package is used.
FROM taxbrokerreport-dev AS e2e

RUN apk add --no-cache chromium
