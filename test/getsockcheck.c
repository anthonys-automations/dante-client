/*
 * getsockname(2) right after a non-blocking connect(2) through libdsocks,
 * from several threads, as libcurl does.  libdsocks used to wait for the
 * SIGIO that completes the connect with sigsuspend(), and in a threaded
 * program that SIGIO may go to another thread, so getsockname() hung.
 *
 * usage: getsockcheck THREADS ROUNDS    (proxy should delay CONNECT replies)
 * Exits non-zero if a getsockname() fails or blocks for more than 2 s (or
 * the whole run is killed by the caller's timeout).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int rounds;
static volatile int failures;
static double worst;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static double now(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *worker(void *arg)
{
   int i, id = (int)(long)arg;

   for (i = 0; i < rounds; ++i) {
      struct sockaddr_in sin, local;
      socklen_t len = sizeof(local);
      double t0, dt;
      int s, rc;

      s = socket(AF_INET, SOCK_STREAM, 0);
      fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
      memset(&sin, 0, sizeof(sin));
      sin.sin_family      = AF_INET;
      sin.sin_port        = htons(6881 + id * 100 + i);
      sin.sin_addr.s_addr = htonl(0xc0000201 + (id * 7 + i) % 200);
      if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0
      &&  errno != EINPROGRESS) {
         perror("connect");
         ++failures;
      }

      t0 = now();
      rc = getsockname(s, (struct sockaddr *)&local, &len);
      dt = now() - t0;

      pthread_mutex_lock(&mtx);
      if (dt > worst)
         worst = dt;
      if (rc != 0 || dt > 2.0) {
         printf("FAIL: thread %d round %d: getsockname() rc %d (%s) after "
                "%.3fs\n", id, i, rc, rc == 0 ? "-" : strerror(errno), dt);
         ++failures;
      }
      pthread_mutex_unlock(&mtx);

      close(s);
   }
   return NULL;
}

int main(int argc, char **argv)
{
   int threads = argc > 1 ? atoi(argv[1]) : 4, i;
   pthread_t *tid;
   struct timespec idle = { 0, 50 * 1000 * 1000 };

   rounds = argc > 2 ? atoi(argv[2]) : 5;
   tid    = calloc(threads, sizeof(*tid));
   for (i = 0; i < threads; ++i)
      pthread_create(&tid[i], NULL, worker, (void *)(long)i);

   /* an idle thread for SIGIO to land on, as in a real program. */
   for (i = 0; i < 20; ++i)
      nanosleep(&idle, NULL);

   for (i = 0; i < threads; ++i)
      pthread_join(tid[i], NULL);

   printf("%s: %d threads x %d getsockname() after connect, slowest %.3fs\n",
          failures ? "FAIL" : "ok", threads, rounds, worst);
   return failures != 0;
}
