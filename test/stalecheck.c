/*
 * Descriptor numbers whose earlier proxied connect finished (successfully or
 * not) and which the program then closed must work for a new socket.
 * libdsocks does not see close(2), so a stale entry for the old connect may
 * still be registered under the number; if it is taken for the new socket,
 * connect() returns the old result at once (EISCONN or the old error)
 * without connecting.
 *
 * usage: stalecheck N    (slowsocks.py refusing every CONNECT: FAIL_EVERY 1)
 * Round 1: N non-blocking connects; wait until each has failed; close all.
 * Round 2: N new sockets get the same numbers; each connect() must start a
 * new connection (EINPROGRESS), not fail at once.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int start(int i, int *err)
{
   struct sockaddr_in sin;
   int s = socket(AF_INET, SOCK_STREAM, 0);

   fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
   memset(&sin, 0, sizeof(sin));
   sin.sin_family      = AF_INET;
   sin.sin_port        = htons(6881 + i);
   sin.sin_addr.s_addr = htonl(0xc0000201 + i % 200);
   *err = connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0 ? 0 : errno;
   return s;
}

int main(int argc, char **argv)
{
   int n = argc > 1 ? atoi(argv[1]) : 20, i, err, immediate = 0;
   int *fd = calloc(n, sizeof(int)), *fd2 = calloc(n, sizeof(int));
   struct timespec until;

   for (i = 0; i < n; ++i)
      fd[i] = start(i, &err);

   /* wait for all of them to be refused (readable/hup) */
   clock_gettime(CLOCK_MONOTONIC, &until);
   until.tv_sec += 3;
   while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
      ;
   for (i = 0; i < n; ++i)
      close(fd[i]);

   for (i = 0; i < n; ++i) {
      fd2[i] = start(n + i, &err);
      if (err != EINPROGRESS && err != 0) {
         ++immediate;
         printf("  new fd %d: connect() failed at once: %s\n", fd2[i], strerror(err));
      }
      else if (err == 0)
         printf("  new fd %d: connect() \"succeeded\" at once\n", fd2[i]);
   }

   printf("%s: %d of %d connects on reused descriptor numbers failed without "
          "trying\n", immediate == 0 ? "ok" : "FAIL", immediate, n);
   return immediate != 0;
}
