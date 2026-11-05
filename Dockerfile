# The ParaMesh container image (X-package): the same files the installer puts in /opt/paramesh,
# with pmd and the website started by tools/setup/docker-entrypoint.sh.
#
#   docker run -d --name paramesh --network host --cap-add SYS_PTRACE --init \
#       ghcr.io/group21-computer-networks/paramesh
#
# Host networking is needed: discovery uses UDP multicast and jobs open TCP ports of their own.
# SYS_PTRACE is needed because Docker's default seccomp profile allows userfaultfd only with it.

FROM ubuntu:24.04 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake ninja-build g++ git python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN /src/tools/setup/package.sh /dist \
    && mkdir -p /opt/paramesh \
    && file="$(ls /dist/paramesh-setup-*.sh)" \
    && line="$(awk '/^__PAYLOAD__$/ {print NR + 1; exit}' "$file")" \
    && tail -n +"$line" "$file" | tar -xz -C /opt/paramesh

FROM ubuntu:24.04
LABEL org.opencontainers.image.source="https://github.com/Group21-Computer-Networks/Paramesh" \
      org.opencontainers.image.description="ParaMesh: distributed shared memory for a pool of Linux machines" \
      org.opencontainers.image.licenses="NOASSERTION"
# gcc and pkg-config build programs against the library; the playground compiles with them too.
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 gcc g++ libc6-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/paramesh /opt/paramesh
COPY tools/setup/docker-entrypoint.sh /usr/local/bin/paramesh-start
RUN for tool in pmd pm pmrun; do ln -s /opt/paramesh/bin/$tool /usr/local/bin/$tool; done \
    && mkdir -p /usr/lib/pkgconfig && ln -s /opt/paramesh/lib/pkgconfig/paramesh.pc /usr/lib/pkgconfig/paramesh.pc
WORKDIR /opt/paramesh/apps
EXPOSE 47000/udp 47001/tcp 47002/tcp 47080/tcp
ENTRYPOINT ["paramesh-start"]
