/*
 * The SOCKS server ends a UDP association (closes its control connection and
 * the relay) while the program keeps using its UDP socket: libdsocks must
 * notice and set up a new association, instead of sending into the void.
 *
 * usage: udpdropcheck WAIT_SECONDS    (slowsocks.py with SLOWSOCKS_UDP_TTL
 *                                      shorter than WAIT_SECONDS)
 * Exits non-zero unless the datagrams before and after the server ended the
 * association are both echoed.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int echo(int s, const struct sockaddr_in *to, const char *msg)
{
   struct pollfd pfd = { s, POLLIN, 0 };
   char buf[64];
   ssize_t r;
   int tries;

   for (tries = 0; tries < 3; ++tries) {   /* UDP: allow a lost datagram. */
      if (sendto(s, msg, strlen(msg), 0, (const struct sockaddr *)to,
                 sizeof(*to)) == -1) {
         printf("sendto(\"%s\"): %s\n", msg, strerror(errno));
         continue;
      }
      if (poll(&pfd, 1, 2000) == 1
      &&  (r = recv(s, buf, sizeof(buf) - 1, 0)) > 0) {
         buf[r] = '\0';
         if (strcmp(buf, msg) == 0)
            return 1;
      }
   }
   return 0;
}

int main(int argc, char **argv)
{
   int wait = argc > 1 ? atoi(argv[1]) : 3, s, ok1, ok2;
   struct sockaddr_in to;

   s = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&to, 0, sizeof(to));
   to.sin_family      = AF_INET;
   to.sin_port        = htons(9);
   to.sin_addr.s_addr = htonl(0xc0000201); /* 192.0.2.1, via the proxy */

   ok1 = echo(s, &to, "before");
   sleep((unsigned int)wait);              /* the server ends the association */
   ok2 = echo(s, &to, "after");

   printf("%s: echo before the server ended the association: %s, after: %s\n",
          ok1 && ok2 ? "ok" : "FAIL", ok1 ? "yes" : "no", ok2 ? "yes" : "no");
   return !(ok1 && ok2);
}
