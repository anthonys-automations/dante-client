/*
 * An AF_INET socket bound to 0.0.0.0:P, connected through an IPv6 SOCKS
 * server while [::]:P is taken by another (IPv6-only) socket: libdsocks
 * replaces the socket with an IPv6 one to reach the server and must not
 * fail just because it can not keep the source port.
 *
 * usage: bindcheck    (SOCKS_CONF routes 192.0.2.0/24 via an IPv6 server
 *                      running slowsocks.py)
 * Exits non-zero unless the connect works and "hello" arrives; run.sh also
 * fails it if libdsocks logged EADDRINUSE on the way.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
   struct sockaddr_in6 six;
   struct sockaddr_in four, dst;
   socklen_t len;
   char buf[16];
   ssize_t r;
   int v6, s, one = 1;

   /* take [::]:P, IPv6 only, so the same port stays free in IPv4. */
   v6 = socket(AF_INET6, SOCK_STREAM, 0);
   setsockopt(v6, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
   memset(&six, 0, sizeof(six));
   six.sin6_family = AF_INET6;
   six.sin6_addr   = in6addr_any;
   if (bind(v6, (struct sockaddr *)&six, sizeof(six)) != 0 || listen(v6, 1) != 0) {
      perror("bind [::]:0");
      return 1;
   }
   len = sizeof(six);
   getsockname(v6, (struct sockaddr *)&six, &len);

   s = socket(AF_INET, SOCK_STREAM, 0);
   memset(&four, 0, sizeof(four));
   four.sin_family      = AF_INET;
   four.sin_addr.s_addr = htonl(INADDR_ANY);
   four.sin_port        = six.sin6_port;
   if (bind(s, (struct sockaddr *)&four, sizeof(four)) != 0) {
      perror("bind 0.0.0.0:P");
      return 1;
   }

   memset(&dst, 0, sizeof(dst));
   dst.sin_family      = AF_INET;
   dst.sin_port        = htons(6881);
   dst.sin_addr.s_addr = htonl(0xc0000201); /* 192.0.2.1 */
   /* non-blocking, as event-loop programs connect. */
   fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
   if (connect(s, (struct sockaddr *)&dst, sizeof(dst)) != 0
   &&  errno != EINPROGRESS) {
      printf("FAIL: connect() with 0.0.0.0:%u bound and [::]:%u taken: %s\n",
             ntohs(six.sin6_port), ntohs(six.sin6_port), strerror(errno));
      return 1;
   }
   {
      struct pollfd pfd = { s, POLLIN, 0 };
      int rc;

      while ((rc = poll(&pfd, 1, 10000)) == -1 && errno == EINTR)
         ;
      if (rc != 1) {
         printf("FAIL: no data within 10s after connect\n");
         return 1;
      }
   }
   if ((r = read(s, buf, sizeof(buf))) <= 0 || memmem(buf, (size_t)r, "hello", 5) == NULL) {
      printf("FAIL: no \"hello\" after connect\n");
      return 1;
   }
   printf("ok: connect with the source port taken in IPv6 (port %u)\n",
          ntohs(six.sin6_port));
   return 0;
}
