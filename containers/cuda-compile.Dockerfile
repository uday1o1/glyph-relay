FROM --platform=linux/amd64 nvidia/cuda:13.3.1-devel-ubuntu24.04@sha256:03c372fd9c65fe7739279f8c65473b315dc61efaaffab03e1e65bc7be7aee61e

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
      cmake=3.28.3-1build7 \
      git=1:2.43.0-1ubuntu7.3 \
      libglib2.0-dev=2.80.0-6ubuntu3.8 \
      libpipewire-0.3-dev=1.0.5-1ubuntu3.3 \
      libspa-0.2-dev=1.0.5-1ubuntu3.3 \
      ninja-build=1.11.1-2 \
      pkg-config=1.8.1-2build1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .

RUN mkdir -p .deps \
    && git clone --depth=1 --branch n13.1.15.0 \
      https://github.com/FFmpeg/nv-codec-headers.git \
      .deps/nv-codec-headers-n13.1.15.0 \
    && test "$(git -C .deps/nv-codec-headers-n13.1.15.0 rev-parse HEAD)" \
      = "0a6fba9a2820628b8103464f4c8753ee05838baa" \
    && test "$(sha256sum .deps/nv-codec-headers-n13.1.15.0/include/ffnvcodec/nvEncodeAPI.h | awk '{print $1}')" \
      = "8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228"

RUN cmake -S . -B build/cuda-compile -G Ninja \
      -DBUILD_TESTING=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CUDA_ARCHITECTURES=75 \
      -DGLYPHRELAY_ENABLE_CUDA=ON \
      -DGLYPHRELAY_ENABLE_WEBRTC_CONTRACTS=OFF \
      -DGLYPHRELAY_NV_CODEC_HEADERS_ROOT=/workspace/.deps/nv-codec-headers-n13.1.15.0 \
    && cmake --build build/cuda-compile --parallel
