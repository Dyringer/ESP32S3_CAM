# ESP32-S3 / ESP-IDF development image
FROM ubuntu:24.04 AS idf_base

ARG IDF_VERSION=6.1
ARG IDF_TARGETS=esp32s3

# Docker setup for proper permission management without root login
COPY tools/docker/entrypoint.sh /usr/local/bin/entrypoint.sh

# Development tools + ESP-IDF host requirements
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        bash-completion \
        bison \
        build-essential \
        ccache \
        cmake \
        curl \
        dfu-util \
        flex \
        gdb \
        git \
        git-lfs \
        gosu \
        gperf \
        htop \
        lcov \
        libffi-dev \
        libncurses-dev \
        libssl-dev \
        libusb-1.0-0 \
        ninja-build \
        python3 \
        python3-pip \
        python3-setuptools \
        python3-venv \
        ruby \
        ruby-dev \
        sudo \
        tzdata \
        wget \
        xz-utils \
    # Ceedling for unit testing
    && gem install ceedling -v 1.0.1 \
    # clang-format 20 (matches esp-clang used by the ESP-IDF VS Code extension;
    # Ubuntu 24.04 only ships 18, which formats aligned blocks differently)
    && pip3 install --no-cache-dir --break-system-packages clang-format==20.1.8 \
    # Ubuntu 24.04 ships a default "ubuntu" user with UID 1000. It would shadow
    # the entrypoint's "docker" user for that UID and break sudo.
    && userdel -r ubuntu \
    # Docker entrypoint
    && chmod +x /usr/local/bin/entrypoint.sh \
    # Makefile auto-completion
    && curl -fsSL https://raw.githubusercontent.com/docker/cli/master/contrib/completion/bash/docker \
        -o /etc/bash_completion.d/docker.sh \
    && rm -rf /var/lib/apt/lists/*

# ESP-IDF — baked into image for offline builds.
# Tools (Xtensa toolchain, OpenOCD, gdb, ...) and the Python venv are installed
# system-wide under IDF_TOOLS_PATH; users only need to source export.sh.
ENV IDF_PATH=/opt/esp/esp-idf
ENV IDF_TOOLS_PATH=/opt/esp
ENV IDF_COMPONENT_MANAGER=1

RUN mkdir -p ${IDF_TOOLS_PATH} \
    && git clone --depth 1 --shallow-submodules --recursive \
        --branch v${IDF_VERSION} \
        https://github.com/espressif/esp-idf.git ${IDF_PATH} \
    && ${IDF_PATH}/install.sh ${IDF_TARGETS} \
    && rm -rf ${IDF_TOOLS_PATH}/dist \
    # Toolchain and Python env must be usable by the non-root container user
    && chmod -R a+rX ${IDF_TOOLS_PATH}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8
ENV TZ=Europe/Amsterdam

WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
