FROM mcr.microsoft.com/playwright@sha256:dcc5531e97840b9b5e794f2814476b21571c5124a3fca2267d73041f56e7580e

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
      libarchive13t64=3.7.2-2ubuntu0.8 \
      libgif7=5.2.2-1ubuntu1.2 \
      liblept5=1.82.0-3build4 \
      libtesseract5=5.3.4-1build5 \
      tesseract-ocr=5.3.4-1build5 \
      tesseract-ocr-eng=1:4.1.0-2 \
      tesseract-ocr-osd=1:4.1.0-2 \
    && rm -rf /var/lib/apt/lists/*

RUN test "$(dpkg-query -W -f='${Version}' libarchive13t64)" = '3.7.2-2ubuntu0.8' \
    && test "$(dpkg-query -W -f='${Version}' libgif7)" = '5.2.2-1ubuntu1.2' \
    && test "$(dpkg-query -W -f='${Version}' liblept5)" = '1.82.0-3build4' \
    && test "$(dpkg-query -W -f='${Version}' libtesseract5)" = '5.3.4-1build5' \
    && test "$(dpkg-query -W -f='${Version}' tesseract-ocr)" = '5.3.4-1build5' \
    && test "$(dpkg-query -W -f='${Version}' tesseract-ocr-eng)" = '1:4.1.0-2' \
    && test "$(dpkg-query -W -f='${Version}' tesseract-ocr-osd)" = '1:4.1.0-2'

RUN printf '%s  %s\n' \
      '9f831cab7525c3dab04af41bda35182af7ea1df9dceeaaa2f3bf207ac45c06a5' \
      '/usr/bin/tesseract' \
      '0d3b71cd757860c6639918c1b2d9407c8652ee22babc8d5c5f18cc35dde6334b' \
      '/usr/lib/x86_64-linux-gnu/libtesseract.so.5.0.3' \
      'b31a50179665871ddb6bdbcfeefc8673febaa29a006e78136024d4083b6ee5e5' \
      '/usr/lib/x86_64-linux-gnu/liblept.so.5.0.4' \
      | sha256sum --check --strict

COPY eng.traineddata /opt/glyphrelay/tessdata/eng.traineddata

RUN printf '%s  %s\n' \
      '7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2' \
      '/opt/glyphrelay/tessdata/eng.traineddata' \
      | sha256sum --check --strict

ENV TESSDATA_PREFIX=/opt/glyphrelay/tessdata
WORKDIR /workspace
