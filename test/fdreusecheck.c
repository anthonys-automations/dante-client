/*
 * A program gives up on a non-blocking connect(2) through libdsocks and
 * closes the socket while the proxy negotiation is still running, then
 * reuses the descriptor number for a file (as transmission does with peer
 * connections and the files it downloads into).  When the negotiation
 * finishes, libdsocks must neither dup2() its socket over the file nor keep
 * its own socket for the abandoned connect open.
 *
 * usage: fdreusecheck ROUNDS    (proxy should delay CONNECT replies ~1 s)
 * Then the same with the number reused by a new socket that connects too.
 * Exits non-zero if a file descriptor stopped being the file, or if
 * descriptors leaked.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int openfds(void)
{
   DIR *d = opendir("/proc/self/fd");
   struct dirent *e;
   int n = 0;

   if (d == NULL)
      return -1;
   while ((e = readdir(d)) != NULL)
      if (e->d_name[0] != '.')
         ++n;
   closedir(d);
   return n - 1;   /* the directory itself */
}

int main(int argc, char **argv)
{
   int rounds = argc > 1 ? atoi(argv[1]) : 20, i, before, after, bad = 0;
   int *file = calloc(rounds, sizeof(int)), *num = calloc(rounds, sizeof(int));
   char path[64];

   before = openfds();
   for (i = 0; i < rounds; ++i) {
      struct sockaddr_in sin;
      int s = socket(AF_INET, SOCK_STREAM, 0);

      fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
      memset(&sin, 0, sizeof(sin));
      sin.sin_family      = AF_INET;
      sin.sin_port        = htons(6881 + i);
      sin.sin_addr.s_addr = htonl(0xc0000201 + i % 200);
      if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0
      &&  errno != EINPROGRESS) {
         perror("connect");
         return 1;
      }
      num[i] = s;
      close(s);                         /* give up while negotiating ... */

      snprintf(path, sizeof(path), "/tmp/fdreusecheck.%d.%d", (int)getpid(), i);
      file[i] = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);   /* ... reuse */
      unlink(path);
      if (file[i] == -1) {
         perror("open");
         return 1;
      }
   }

   {  /* all negotiations finish; sleep(3) would return at the first SIGIO */
      struct timespec until;

      clock_gettime(CLOCK_MONOTONIC, &until);
      until.tv_sec += 3;
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
         ;
   }

   for (i = 0; i < rounds; ++i) {
      struct stat st;

      if (fstat(file[i], &st) != 0 || !S_ISREG(st.st_mode)
      ||  pwrite(file[i], "x", 1, 0) != 1) {
         printf("FAIL: fd %d (was socket fd %d) is no longer the file: %s\n",
                file[i], num[i], fstat(file[i], &st) != 0 ? strerror(errno) :
                S_ISSOCK(st.st_mode) ? "it is a socket now" : strerror(errno));
         ++bad;
      }
      close(file[i]);
   }

   /*
    * Again, but the number goes to a new socket with a connect of its own,
    * so the abandoned connect's entry is replaced rather than found stale.
    */
   for (i = 0; i < rounds; ++i) {
      struct sockaddr_in sin;
      int k;

      memset(&sin, 0, sizeof(sin));
      sin.sin_family = AF_INET;
      for (k = 0; k < 2; ++k) {
         int s = socket(AF_INET, SOCK_STREAM, 0);

         fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
         sin.sin_port        = htons(7881 + i * 2 + k);
         sin.sin_addr.s_addr = htonl(0xc0000201 + (i * 2 + k) % 200);
         (void)connect(s, (struct sockaddr *)&sin, sizeof(sin));
         if (k == 0)
            close(s);        /* abandoned; k == 1 likely gets its number */
         else
            file[i] = s;
      }
   }
   {
      struct timespec until;

      clock_gettime(CLOCK_MONOTONIC, &until);
      until.tv_sec += 3;
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
         ;
   }
   for (i = 0; i < rounds; ++i)
      close(file[i]);

   after = openfds();
   if (after > before + 2) {   /* show what is left over */
      DIR *d = opendir("/proc/self/fd");
      struct dirent *e;
      char link[300], target[128];
      ssize_t n;

      while (d != NULL && (e = readdir(d)) != NULL) {
         if (e->d_name[0] == '.')
            continue;
         snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
         if ((n = readlink(link, target, sizeof(target) - 1)) > 0) {
            struct sockaddr_storage a, b;
            socklen_t al = sizeof(a), bl = sizeof(b);
            char as[64] = "-", bs[64] = "-";
            int fd = atoi(e->d_name), dom = 0;
            socklen_t dl = sizeof(dom);

            target[n] = '\0';
            if (strncmp(target, "socket:", 7) == 0) {
               /* raw syscalls: libdsocks' wrappers would answer for it */
               syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_DOMAIN, &dom, &dl);
               if (syscall(SYS_getsockname, fd, &a, &al) == 0)
                  inet_ntop(a.ss_family, a.ss_family == AF_INET6 ?
                            (void *)&((struct sockaddr_in6 *)&a)->sin6_addr :
                            (void *)&((struct sockaddr_in *)&a)->sin_addr,
                            as, sizeof(as));
               if (syscall(SYS_getpeername, fd, &b, &bl) == 0)
                  inet_ntop(b.ss_family, b.ss_family == AF_INET6 ?
                            (void *)&((struct sockaddr_in6 *)&b)->sin6_addr :
                            (void *)&((struct sockaddr_in *)&b)->sin_addr,
                            bs, sizeof(bs));
               printf("  fd %s -> %s family %d local %s peer %s\n",
                      e->d_name, target, dom, as, bs);
            }
            else
               printf("  fd %s -> %s\n", e->d_name, target);
         }
      }
      if (d != NULL)
         closedir(d);
   }
   printf("%s: %d rounds, %d file descriptors replaced; open fds %d before, "
          "%d after\n",
          bad == 0 && after <= before + 2 ? "ok" : "FAIL", rounds, bad,
          before, after);
   return !(bad == 0 && after <= before + 2);
}
