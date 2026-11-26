# =============================================================================
# AegisHMI Linux 构建镜像
#
# 用途：在没有 Windows 环境的机器（或 CI）上验证同一套 CMake 工程可编译、可跑测试。
# 构建（在 aegishmi 目录下执行）：
#   docker build -f docker/Dockerfile -t aegishmi:latest .
# 运行单元测试：
#   docker run --rm aegishmi:latest ctest --test-dir /src/build --output-on-failure
#
# 说明：
#   - Ubuntu 24.04 自带 Qt 6.4，足够覆盖本工程用到的 API；
#   - 构建阶段顺带执行一次测试，镜像构建失败即代表测试失败；
#   - GUI 程序（aegis_hmi）在容器里不启动，只编译。
# =============================================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV QT_QPA_PLATFORM=offscreen

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        python3 \
        ca-certificates \
        qt6-base-dev \
        qt6-serialport-dev \
        libqt6sql6-sqlite \
        libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/build \
    && ctest --test-dir /src/build --output-on-failure

CMD ["/bin/bash"]
