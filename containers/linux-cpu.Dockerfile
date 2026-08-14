FROM ubuntu:24.04@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
      clang=1:18.0-59~exp2 \
      cmake=3.28.3-1build7 \
      ffmpeg=7:6.1.1-3ubuntu5 \
      libopenh264-7=2.4.1+dfsg-1 \
      libopenh264-dev=2.4.1+dfsg-1 \
      make=4.3-4.1build2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY CMakeLists.txt CMakePresets.json ./
COPY include ./include
COPY protocols ./protocols
COPY src ./src
COPY tests/native ./tests/native
COPY tools/freeze_m0_protocol.cpp ./tools/freeze_m0_protocol.cpp
COPY tools/hash_m0_browser_source.cpp ./tools/hash_m0_browser_source.cpp
COPY tools/validate_m0_benchmark.py ./tools/validate_m0_benchmark.py
COPY schemas ./schemas

ENV CXX=clang++

CMD ["sh", "-c", "cmake --preset linux-cpu && cmake --build --preset linux-cpu --parallel && ctest --preset linux-cpu"]
