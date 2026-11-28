/*
 * Resolver calls from several threads must not switch libdsocks off for the
 * other threads.  libdsocks marks "running the system resolver" so the
 * resolver's own sockets go native; that mark used to be one counter for the
 * whole process, updated with plain ++/-- from every thread: while one thread
 * resolved, all threads' connects went native, and a lost update could leave
 * it set for good (no connect was proxied any more).
 *
 * usage: dnsracecheck SECONDS    (slowsocks.py on the configured gateway)
 * Threads call getaddrinfo() on a numeric address in a loop (it reaches the
 * system resolver) while the main thread checks, again and again, that a
 * non-blocking connect is proxied (slowsocks.py answers "hello").  Then the
 * same once more after the threads stopped.  Exits non-zero on any connect
 * that did not go through the proxy.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;

static void *resolver(void *arg)
{
   (void)arg;
   while (!stop) {
      struct addrinfo hints, *res;

      memset(&hints, 0, sizeof(hints));
      hints.ai_socktype = SOCK_STREAM;
      if (getaddrinfo("127.0.0.1", "80", &hints, &res) == 0)
         freeaddrinfo(res);
   }
   return NULL;
}

/* one non-blocking connect; 1 if the proxy answered "hello" */
static int proxied(int i)
{
   struct sockaddr_in sin;
   struct pollfd pfd;
   char buf[16];
   ssize_t r = -1;
   int s, rc, tries;

   s = socket(AF_INET, SOCK_STREAM, 0);
   fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
   memset(&sin, 0, sizeof(sin));
   sin.sin_family      = AF_INET;
   sin.sin_port        = htons(6881 + i);
   sin.sin_addr.s_addr = htonl(0xc0000201 + i % 200);
   if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0 && errno != EINPROGRESS) {
      close(s);
      return 0;   /* e.g. ENETUNREACH: went to the kernel unproxied */
   }
   pfd.fd = s;
   pfd.events = POLLIN;
   for (tries = 0; tries < 50; ++tries) {   /* up to ~5 s, despite EINTR */
      if ((rc = poll(&pfd, 1, 100)) == 1) {
         r = read(s, buf, sizeof(buf));
         break;
      }
   }
   close(s);
   return r > 0 && memmem(buf, (size_t)r, "hello", 5) != NULL;
}

int main(int argc, char **argv)
{
   int secs = argc > 1 ? atoi(argv[1]) : 5, i, bad_during = 0, bad_after = 0, n = 0;
   pthread_t tid[4];
   time_t end;

   for (i = 0; i < 4; ++i)
      pthread_create(&tid[i], NULL, resolver, NULL);

   end = time(NULL) + secs;
   while (time(NULL) < end) {
      if (!proxied(n))
         ++bad_during;
      ++n;
   }

   stop = 1;
   for (i = 0; i < 4; ++i)
      pthread_join(tid[i], NULL);

   for (i = 0; i < 5; ++i)
      if (!proxied(1000 + i))
         ++bad_after;

   printf("%s: %d of %d connects not proxied while other threads resolved, "
          "%d of 5 after\n",
          bad_during + bad_after == 0 ? "ok" : "FAIL", bad_during, n, bad_after);
   return bad_during + bad_after != 0;
}
