/*
 * Fire N non-blocking connect(2)s (as an event-loop program would) and
 * report how long each connect() call blocked the calling thread, then wait
 * for all of them and count successes (SOCKS reply ok and "hello" received).
 *
 * usage: nbconnect N WAIT_SECONDS EXPECTED_SUCCESSES MIN_SECONDS
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

static double now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
   if (argc != 5) {
      fprintf(stderr, "usage: nbconnect N WAIT_SECONDS EXPECTED_SUCCESSES MIN_SECONDS\n");
      return 2;
   }
   int n = atoi(argv[1]), successes = atoi(argv[3]);
   double waitmax = atof(argv[2]), minwait = atof(argv[4]);
   int *fd = calloc(n, sizeof(int)), *state = calloc(n, sizeof(int));
   size_t *received = calloc(n, sizeof(*received));
   double start = now(), maxblock = 0, sumblock = 0;
   int i, connected = 0, failed = 0, inprogress = 0, immediate_fail = 0;

   for (i = 0; i < n; ++i) {
      struct sockaddr_in sin;
      double t0, dt;

      fd[i] = socket(AF_INET, SOCK_STREAM, 0);
      if (fd[i] == -1 || fcntl(fd[i], F_SETFL, O_NONBLOCK) == -1) {
         perror("non-blocking socket");
         return 1;
      }
      memset(&sin, 0, sizeof(sin));
      sin.sin_family = AF_INET;
      sin.sin_port   = htons(6881 + i);
      sin.sin_addr.s_addr = htonl(0xc0000201 + (i % 200)); /* 192.0.2.x */

      t0 = now();
      if (connect(fd[i], (struct sockaddr *)&sin, sizeof(sin)) < 0) {
         if (errno == EINPROGRESS)
            ++inprogress;
         else {
            state[i] = -1;
            ++immediate_fail;
         }
      }
      dt = now() - t0;
      sumblock += dt;
      if (dt > maxblock)
         maxblock = dt;
   }
   printf("issued %d connects in %.3fs: max connect() block %.3fs, "
          "avg %.4fs, EINPROGRESS %d, immediate failures %d\n",
          n, now() - start, maxblock, sumblock / n, inprogress, immediate_fail);
   fflush(stdout);

   /* wait for the outcome of each: readable "hello" = success. */
   while (now() - start < waitmax) {
      struct pollfd *pfd = calloc(n, sizeof(*pfd));
      int pending = 0;

      for (i = 0; i < n; ++i) {
         pfd[i].fd     = state[i] == 0 ? fd[i] : -1;
         pfd[i].events = POLLIN;
         pending += state[i] == 0;
      }
      if (pending == 0) {
         free(pfd);
         break;
      }
      poll(pfd, n, 200);
      for (i = 0; i < n; ++i) {
         static const char hello[] = "hello\n";
         char buf[sizeof(hello)];
         ssize_t r;

         if (state[i] != 0 || pfd[i].revents == 0)
            continue;
         r = recv(fd[i], buf, sizeof(hello) - 1 - received[i], 0);
         if (r > 0) {
            if (memcmp(buf, hello + received[i], r) != 0) {
               fprintf(stderr, "unexpected proxy greeting on connection %d\n", i);
               return 1;
            }
            received[i] += r;
            if (received[i] == sizeof(hello) - 1)
               state[i] = 1;
         }
         else if (r == 0 || (errno != EAGAIN && errno != EINTR))
            state[i] = -1;
      }
      free(pfd);
   }
   for (i = 0; i < n; ++i) {
      connected += state[i] == 1;
      failed    += state[i] == -1;
   }
   printf("after %.1fs: success %d, failed %d, still pending %d\n",
          now() - start, connected, failed, n - connected - failed);
   fflush(stdout);
   if (connected != successes || failed != n - successes ||
       maxblock >= 1.0 || now() - start < minwait) {
      fprintf(stderr, "expected %d successes, %d failures, no pending, "
              "connect() under 1s and completion after at least %.1fs\n",
              successes, n - successes, minwait);
      return 1;
   }
   return 0;
}
