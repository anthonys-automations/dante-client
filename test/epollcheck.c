/*
 * Non-blocking connect(2) through libdsocks, watched with epoll(7) the way
 * event loops (libevent, libuv, ...) do: register the socket right after
 * connect() returns EINPROGRESS, then wait for it to become writable and read
 * the "hello\n" slowsocks.py sends after the CONNECT reply.
 *
 * libdsocks completes such connects by dup2(2)ing the socket connected to the
 * proxy onto the program's descriptor.  epoll registrations belong to the
 * old open file, so unless libdsocks carries them over, the program is never
 * told that the connect finished.
 *
 * usage: epollcheck N    (N connects to TEST-NET addresses)
 * Exits non-zero unless every connect is reported writable and reads "hello".
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
   int n = argc > 1 ? atoi(argv[1]) : 10, i, ep, ok = 0, writable = 0;
   int *fd, *done;

   fd   = calloc(n, sizeof(*fd));
   done = calloc(n, sizeof(*done));
   if ((ep = epoll_create1(0)) == -1) {
      perror("epoll_create1");
      return 1;
   }

   for (i = 0; i < n; ++i) {
      struct sockaddr_in sin;
      struct epoll_event ev;

      fd[i] = socket(AF_INET, SOCK_STREAM, 0);
      fcntl(fd[i], F_SETFL, fcntl(fd[i], F_GETFL) | O_NONBLOCK);
      memset(&sin, 0, sizeof(sin));
      sin.sin_family      = AF_INET;
      sin.sin_port        = htons(6881 + i);
      sin.sin_addr.s_addr = htonl(0xc0000201 + i % 200);

      if (connect(fd[i], (struct sockaddr *)&sin, sizeof(sin)) != 0
      &&  errno != EINPROGRESS) {
         perror("connect");
         return 1;
      }

      memset(&ev, 0, sizeof(ev));
      ev.events  = EPOLLOUT | EPOLLIN;
      ev.data.u32 = (uint32_t)i;
      if (epoll_ctl(ep, EPOLL_CTL_ADD, fd[i], &ev) != 0) {
         perror("epoll_ctl");
         return 1;
      }
   }

   for (;;) {
      struct epoll_event evs[64];
      int k, m = epoll_wait(ep, evs, 64, 5000);

      if (m == -1 && errno == EINTR)
         continue;   /* libdsocks' SIGIO; event loops retry too */
      if (m <= 0)
         break;      /* timeout: whatever is left was never reported */

      for (k = 0; k < m; ++k) {
         char buf[16];
         ssize_t r;

         i = (int)evs[k].data.u32;
         if (done[i])
            continue;
         if (evs[k].events & EPOLLOUT)
            ++writable;
         if ((r = read(fd[i], buf, sizeof(buf))) == -1 && errno == EAGAIN)
            continue;   /* writable, data not there yet */
         done[i] = 1;
         if (r > 0 && memmem(buf, (size_t)r, "hello", 5) != NULL)
            ++ok;
         epoll_ctl(ep, EPOLL_CTL_DEL, fd[i], NULL);
      }
      if (ok == n)
         break;
   }

   printf("%s: %d/%d connects reported by epoll and read \"hello\" "
          "(%d EPOLLOUT events)\n", ok == n ? "ok" : "FAIL", ok, n, writable);
   return ok != n;
}
