/*
 * Checks that an AF_INET program works through libdsocks whatever the
 * family of the SOCKS server (see ipv6-proxy.md), against slowsocks.py.
 *
 * usage: v6check TCP_FAMILY UDP_FAMILY    (4 or 6)
 * The families are those the sockets must end up with to reach the server:
 * the family of the server's address, and of its UDP relay address.
 * Exits non-zero if a check fails.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                     \
do {                                                         \
   if (!(cond)) {                                            \
      ++failures;                                            \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
   }                                                         \
} while (0)

static struct sockaddr_in
testnet(int port)
{
   struct sockaddr_in sin;

   memset(&sin, 0, sizeof(sin));
   sin.sin_family      = AF_INET;
   sin.sin_port        = htons(port);
   sin.sin_addr.s_addr = htonl(0xc0000201); /* 192.0.2.1 */
   return sin;
}

static int
domainof(int s)
{
   int val = -1;
   socklen_t len = sizeof(val);

   getsockopt(s, SOL_SOCKET, SO_DOMAIN, &val, &len);
   return val;
}

static int
fam(int v)
{
   return v == 6 ? AF_INET6 : AF_INET;
}

/* the real local address, bypassing libdsocks' getsockname(). */
static int
realsockname(int s, struct sockaddr_storage *ss)
{
   socklen_t len = sizeof(*ss);

   return (int)syscall(SYS_getsockname, s, (struct sockaddr *)ss, &len);
}

static int
readhello(int s)
{
   char buf[64];
   ssize_t n = recv(s, buf, sizeof(buf), 0);

   return n > 0 && memmem(buf, n, "hello", 5) != NULL;
}

static void
tcp(int tcpfamily, const char *bindto)
{
   struct sockaddr_in sin = testnet(80), peer;
   struct sockaddr_storage ss;
   struct stat before, after;
   socklen_t len;
   int s, tos = 0x10, val, inport = 0;

   printf("## tcp, bind: %s\n", bindto);

   s = socket(AF_INET, SOCK_STREAM, 0);
   CHECK(setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0,
         "%s", strerror(errno));
   CHECK(fcntl(s, F_SETFD, FD_CLOEXEC) == 0, "%s", strerror(errno));

   if (strcmp(bindto, "none") != 0) {
      struct sockaddr_in local;

      memset(&local, 0, sizeof(local));
      local.sin_family = AF_INET;
      inet_pton(AF_INET, bindto, &local.sin_addr);
      /* really bound; without a bind route, libdsocks' bind() only pretends. */
      CHECK(syscall(SYS_bind, s, (struct sockaddr *)&local, sizeof(local))
            == 0, "%s", strerror(errno));
      CHECK(realsockname(s, &ss) == 0, "%s", strerror(errno));
      inport = ((struct sockaddr_in *)&ss)->sin_port;
   }

   fstat(s, &before);
   CHECK(connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0,
         "connect: %s", strerror(errno));
   CHECK(readhello(s), "no hello");
   fstat(s, &after);

   CHECK(domainof(s) == fam(tcpfamily), "SO_DOMAIN is %d", domainof(s));
   if (tcpfamily == 4)
      CHECK(before.st_ino == after.st_ino, "socket replaced for IPv4 server");

   CHECK(fcntl(s, F_GETFD) == FD_CLOEXEC, "fd flags %d", fcntl(s, F_GETFD));
   CHECK((fcntl(s, F_GETFL) & O_NONBLOCK) == 0, "became non-blocking");

   if (tcpfamily == 6) {
      len = sizeof(val);
      val = -1;
      CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &val, &len) == 0
            && val == tos, "IPV6_TCLASS is %d", val);
   }
   else {
      len = sizeof(val);
      val = -1;
      CHECK(getsockopt(s, IPPROTO_IP, IP_TOS, &val, &len) == 0 && val == tos,
            "IP_TOS is %d", val);
   }

   /*
    * Linux accepts IPv4-level options on an AF_INET6 socket (they apply to
    * IPv4-mapped traffic only), so a program setting them after connect
    * gets no error even though the socket was replaced.
    */
   val = 0x20;
   CHECK(setsockopt(s, IPPROTO_IP, IP_TOS, &val, sizeof(val)) == 0,
         "IP_TOS after connect: %s", strerror(errno));

   if (inport != 0 && strcmp(bindto, "0.0.0.0") == 0) {
      CHECK(realsockname(s, &ss) == 0, "%s", strerror(errno));
      CHECK(ss.ss_family == fam(tcpfamily), "real family %d", ss.ss_family);
      CHECK(((struct sockaddr_in *)&ss)->sin_port == inport, /* same offset */
            "bound port %d lost (%d)", ntohs(inport),
            ntohs(((struct sockaddr_in *)&ss)->sin_port));
   }

   len = sizeof(ss);
   memset(&ss, 0, sizeof(ss));
   CHECK(getsockname(s, (struct sockaddr *)&ss, &len) == 0, "%s",
         strerror(errno));
   CHECK(ss.ss_family == AF_INET && len == sizeof(struct sockaddr_in),
         "getsockname family %d len %d", ss.ss_family, (int)len);

   len = sizeof(peer);
   memset(&peer, 0, sizeof(peer));
   CHECK(getpeername(s, (struct sockaddr *)&peer, &len) == 0, "%s",
         strerror(errno));
   CHECK(len == sizeof(peer) && peer.sin_family == AF_INET
         && peer.sin_addr.s_addr == sin.sin_addr.s_addr
         && peer.sin_port == sin.sin_port,
         "getpeername family %d len %d", peer.sin_family, (int)len);

   close(s);
}

static int
udprecv(int s, char *buf, size_t buflen, struct sockaddr_in *from,
        socklen_t *fromlen)
{
   struct pollfd pfd = { s, POLLIN, 0 };
   ssize_t n;

   if (poll(&pfd, 1, 3000) != 1)
      return -1;
   *fromlen = sizeof(*from);
   if ((n = recvfrom(s, buf, buflen - 1, 0, (struct sockaddr *)from,
                     fromlen)) < 0)
      return -1;
   buf[n] = 0;
   return (int)n;
}

static void
udpcheck(int s, int port, const char *msg)
{
   struct sockaddr_in to = testnet(port), from;
   socklen_t fromlen;
   char buf[64];

   CHECK(sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to))
         == (ssize_t)strlen(msg), "sendto: %s", strerror(errno));
   memset(&from, 0, sizeof(from));
   CHECK(udprecv(s, buf, sizeof(buf), &from, &fromlen) > 0,
         "recvfrom: %s", strerror(errno));
   CHECK(strcmp(buf, msg) == 0, "got \"%s\"", buf);
   CHECK(fromlen == sizeof(from) && from.sin_family == AF_INET
         && from.sin_addr.s_addr == to.sin_addr.s_addr
         && from.sin_port == to.sin_port,
         "from family %d len %d", from.sin_family, (int)fromlen);
}

static void
udp(int udpfamily)
{
   struct sockaddr_storage ss;
   socklen_t len;
   int s;

   printf("## udp\n");

   s = socket(AF_INET, SOCK_DGRAM, 0);
   udpcheck(s, 9, "ping");
   CHECK(domainof(s) == fam(udpfamily), "SO_DOMAIN is %d", domainof(s));
   udpcheck(s, 6666, "pong"); /* the IPv6-sourced reply must be dropped. */
   udpcheck(s, 9, "again");

   len = sizeof(ss);
   CHECK(getsockname(s, (struct sockaddr *)&ss, &len) == 0, "%s",
         strerror(errno));
   CHECK(ss.ss_family == AF_INET && len == sizeof(struct sockaddr_in),
         "getsockname family %d len %d", ss.ss_family, (int)len);

   close(s);
}

int
main(int argc, char **argv)
{
   int tcpfamily = argc > 1 ? atoi(argv[1]) : 4,
       udpfamily = argc > 2 ? atoi(argv[2]) : tcpfamily;

   tcp(tcpfamily, "none");
   tcp(tcpfamily, "127.0.0.1");
   tcp(tcpfamily, "0.0.0.0");
   udp(udpfamily);

   printf("%s: %d failed checks\n", failures ? "FAIL" : "ok", failures);
   return failures != 0;
}
