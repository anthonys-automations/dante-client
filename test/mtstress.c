/*
 * Multithreaded churn against libdsocks' non-blocking connect path, the way
 * an event-loop program with a connection manager behaves: several threads
 * open non-blocking sockets, connect(2), wait a random short time and close
 * them whether or not the connect finished, while other descriptors are
 * opened and closed so fd numbers get reused quickly.
 *
 * usage: mtstress THREADS SECONDS [MAXWAIT_MS]
 * Prints per-second totals. Fails (exit 1) if the process has more than one
 * child at any sample (libdsocks' connect-child; its workers are its own
 * children), or if fewer than 2 connects per thread per second got through
 * (threads stuck in libdsocks). A crash or hang is a failure too.
 * MTSTRESS_ABORT_AT=N raises SIGTRAP after N seconds, to inspect the
 * threads under a debugger.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static int maxwait_ms = 50;
static long connects, completed, failed, closed_inprogress;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void add(long *c) { pthread_mutex_lock(&mtx); ++*c; pthread_mutex_unlock(&mtx); }

static void *worker(void *arg)
{
   unsigned int seed = (unsigned int)(long)arg * 7919u + (unsigned int)time(NULL);

   while (!stop) {
      struct sockaddr_in sin;
      struct pollfd pfd;
      int s, r, pipefd[2];

      if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1)
         continue;
      fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
      memset(&sin, 0, sizeof(sin));
      sin.sin_family      = AF_INET;
      sin.sin_port        = htons(6881 + rand_r(&seed) % 1000);
      sin.sin_addr.s_addr = htonl(0xc0000201 + rand_r(&seed) % 200);

      add(&connects);
      if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0)
         add(&completed);
      else if (errno == EINPROGRESS) {
         pfd.fd = s;
         pfd.events = POLLIN | POLLOUT;
         r = poll(&pfd, 1, rand_r(&seed) % (maxwait_ms + 1));
         if (r == 1 && (pfd.revents & (POLLERR | POLLHUP)))
            add(&failed);
         else if (r == 1)
            add(&completed);
         else
            add(&closed_inprogress);
      }
      else
         add(&failed);

      close(s);

      /* recycle fd numbers with something that is not a socket. */
      if (pipe(pipefd) == 0) {
         close(pipefd[0]);
         close(pipefd[1]);
      }
   }
   return NULL;
}

/* number of live (non-zombie) processes whose parent is us. */
static int children(void)
{
   DIR *d = opendir("/proc");
   struct dirent *e;
   int n = 0;

   if (d == NULL)
      return -1;
   while ((e = readdir(d)) != NULL) {
      char path[300], buf[512], *p, state;
      int ppid;
      FILE *f;

      if (e->d_name[0] < '0' || e->d_name[0] > '9')
         continue;
      snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
      if ((f = fopen(path, "r")) == NULL)
         continue;
      if (fgets(buf, sizeof(buf), f) != NULL
      &&  (p = strrchr(buf, ')')) != NULL
      &&  sscanf(p + 1, " %c %d", &state, &ppid) == 2
      &&  ppid == (int)getpid() && state != 'Z')
         ++n;
      fclose(f);
   }
   closedir(d);
   return n;
}

int main(int argc, char **argv)
{
   int threads = argc > 1 ? atoi(argv[1]) : 4;
   int secs    = argc > 2 ? atoi(argv[2]) : 30;
   pthread_t *tid;
   int i, maxchildren = 0;

   if (argc > 3)
      maxwait_ms = atoi(argv[3]);

   signal(SIGPIPE, SIG_IGN); /* as daemons do */

   tid = calloc(threads, sizeof(*tid));
   for (i = 0; i < threads; ++i)
      pthread_create(&tid[i], NULL, worker, (void *)(long)i);

   for (i = 0; i < secs; ++i) {
      const char *a = getenv("MTSTRESS_ABORT_AT");
      struct timespec deadline;
      int c;

      /* sleep(3) returns early on every SIGIO/SIGCHLD; wait for real. */
      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_sec += 1;
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL)
      == EINTR)
         ;

      if ((c = children()) > maxchildren)
         maxchildren = c;

      if (a != NULL && atoi(a) == i + 1)
         raise(SIGTRAP); /* stop under a debugger to inspect the threads */
      pthread_mutex_lock(&mtx);
      printf("t=%ds connects=%ld completed=%ld failed=%ld closed_inprogress=%ld\n",
             i + 1, connects, completed, failed, closed_inprogress);
      pthread_mutex_unlock(&mtx);
      fflush(stdout);
   }
   stop = 1;
   for (i = 0; i < threads; ++i)
      pthread_join(tid[i], NULL);

   printf("most children at once=%d connects=%ld (minimum %ld)\n",
          maxchildren, connects, 2L * threads * secs);
   if (maxchildren > 1 || connects < 2L * threads * secs) {
      printf("FAIL\n");
      return 1;
   }
   printf("ok\n");
   return 0;
}
