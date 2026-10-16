# Community Credits node. Build context (see build.sh): package/ holds an unpacked release package
# (the csnode/ directory: node, executor, JDK, configuration), docker/ holds this directory.
FROM ubuntu:24.04
LABEL org.opencontainers.image.source=https://github.com/EnzinoBB/node \
      org.opencontainers.image.description="Community Credits node"

RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-nacl python3-base58 zstd curl ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && userdel -r ubuntu \
    && useradd -u 1000 -m -s /usr/sbin/nologin credits \
    && mkdir -p /data && chown credits:credits /data

COPY package/ /opt/credits/
RUN mv /opt/credits/config.ini /opt/credits/config.ini.template
COPY docker/entrypoint.sh docker/keygen.py docker/healthcheck.sh docker/snapshot-restore.sh /opt/credits/

USER credits
WORKDIR /data
VOLUME /data
EXPOSE 9510/tcp 9510/udp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=60s --start-period=15m CMD ["/opt/credits/healthcheck.sh"]
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/credits/entrypoint.sh"]
