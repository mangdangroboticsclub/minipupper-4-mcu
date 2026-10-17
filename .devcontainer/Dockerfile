ARG IDF_TAG=v5.5.4
FROM espressif/idf:${IDF_TAG}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

# ── System packages ──────────────────────────────────────────────
RUN apt-get update -y && apt-get install -y \
    cmake dfu-util libssl-dev udev nodejs npm \
    && rm -rf /var/lib/apt/lists/*

# ── Shell config ────────────────────────────────────────────────
RUN echo "source /opt/esp/idf/export.sh > /dev/null 2>&1" >> /root/.bashrc

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]

CMD ["/bin/bash"]