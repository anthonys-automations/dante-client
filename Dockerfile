# Minimal Alpine image with a patched dante libdsocks.so at
# /usr/lib/libdsocks.so. Add the application and its packages in a later layer.
#   docker build -t <image>:<tag> <this directory>

# libdsocks.so built using Alpine's patches and flags plus
# dante-connectchild-workers.patch and dante-ipv6-gateway.patch.
FROM docker.io/library/alpine:3.24 AS dante
ARG DANTE_VERSION=1.4.4
ARG DANTE_SHA256=1973c7732f1f9f0a4c0ccf2c1ce462c7c25060b25643ea90f9b98f53a813faec
ARG APORTS=https://raw.githubusercontent.com/alpinelinux/aports/3.24-stable/community/dante
RUN apk add --no-cache build-base linux-headers curl
WORKDIR /src
COPY dante-connectchild-workers.patch dante-ipv6-gateway.patch dante-threads.patch dante-readv-iovcnt.patch dante-gai-numeric-ipv6.patch ./
# Alpine's patches are checked against the sha512sums of the APKBUILD they come with.
RUN curl -fsSLO "https://www.inet.no/dante/files/dante-${DANTE_VERSION}.tar.gz" \
 && echo "${DANTE_SHA256}  dante-${DANTE_VERSION}.tar.gz" | sha256sum -c - \
 && curl -fsSLO "${APORTS}/APKBUILD" \
 && for p in ifdev gaierrval dante-no-bindresvport; do curl -fsSLO "${APORTS}/${p}.patch"; done \
 && sed -n '/^sha512sums="/,/"/p' APKBUILD | sed 's/^sha512sums="//' | grep '\.patch' | sha512sum -c - \
 && tar xzf "dante-${DANTE_VERSION}.tar.gz"
WORKDIR /src/dante-${DANTE_VERSION}
# -z stack-size: musl gives new threads 128 KiB stacks, and libdsocks (its UDP
# path and logger) runs on whichever thread makes the socket call and
# overflows that. musl takes the default thread stack size from the largest
# PT_GNU_STACK among the program and its startup libraries, LD_PRELOAD
# included, so 2 MiB here applies to every thread of the preloaded program.
RUN for p in ifdev gaierrval dante-no-bindresvport dante-connectchild-workers dante-ipv6-gateway dante-threads dante-readv-iovcnt dante-gai-numeric-ipv6; do \
      patch --batch --forward --fuzz=0 -p1 < "../${p}.patch" || exit 1; \
    done \
 && CFLAGS="-Os -fstack-clash-protection -Wformat -Werror=format-security -Wno-implicit-function-declaration -D_GNU_SOURCE" \
    LDFLAGS="-Wl,-z,stack-size=2097152" \
    ac_cv_func_sched_setscheduler=no ./configure --prefix=/usr \
 && make \
 && install -D -m 0755 dlib/.libs/libdsocks.so /out/libdsocks.so \
 && strip --strip-unneeded /out/libdsocks.so \
 && readelf -lW /out/libdsocks.so | grep -q 'GNU_STACK.* 0x200000 RW'

FROM dante AS tested
RUN apk add --no-cache coreutils python3
COPY test/ /test/
RUN --network=none /test/run.sh /out/libdsocks.so

FROM docker.io/library/alpine:3.24
COPY --from=tested /out/libdsocks.so /usr/lib/libdsocks.so
# Override when building a release; ad-hoc builds remain identifiable.
ARG GIT_COMMIT=unknown
ENV GIT_COMMIT=${GIT_COMMIT}
