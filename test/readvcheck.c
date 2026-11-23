/*
 * readv(2) through libdsocks with several iovecs: connect (blocking) to a
 * TEST-NET address routed to slowsocks.py, which sends "hello\n" after the
 * CONNECT reply, and once it has all arrived read it with one readv() into
 * 4 iovecs of 2 bytes each, kept in a struct between guard values.  Fails
 * unless that one call returns all 6 bytes split across the iovecs, with
 * the guards intact.  (The bug this guards against used only the first
 * iovec and took stack garbage for the others: short reads, or received
 * data written over the caller's stack.)
 *
 * usage: readvcheck
 */
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define GUARD 0x5a5a5a5a5a5a5a5aULL

int main(void)
{
   static const char expect[] = "hello\n";
   struct {
      uint64_t     before;
      struct iovec iov[4];
      char         b[4][2];
      uint64_t     after[8];
   } v;
   struct sockaddr_in sin;
   int s, i;

   if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      return 1;
   }
   memset(&sin, 0, sizeof(sin));
   sin.sin_family      = AF_INET;
   sin.sin_port        = htons(6881);
   sin.sin_addr.s_addr = htonl(0xc0000201); /* 192.0.2.1 */
   if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
      perror("connect");
      return 1;
   }

   {
      struct pollfd pfd = { s, POLLIN, 0 };
      ssize_t n;

      /* all of "hello\n" comes in one write right after the reply. */
      if (poll(&pfd, 1, 10000) != 1) {
         printf("FAIL: no data\n");
         return 1;
      }
      usleep(200 * 1000);

      memset(v.b, 0, sizeof(v.b));
      v.before = GUARD;
      for (i = 0; i < 8; ++i)
         v.after[i] = GUARD;
      for (i = 0; i < 4; ++i) {
         v.iov[i].iov_base = v.b[i];
         v.iov[i].iov_len  = sizeof(v.b[i]);
      }

      n = readv(s, v.iov, 4);

      for (i = 0; i < 8; ++i)
         if (v.before != GUARD || v.after[i] != GUARD) {
            printf("FAIL: readv() overwrote memory next to the iovecs\n");
            return 1;
         }
      for (i = 0; i < 4; ++i)
         if (v.iov[i].iov_base != v.b[i] || v.iov[i].iov_len != sizeof(v.b[i])) {
            printf("FAIL: readv() changed iovec %d\n", i);
            return 1;
         }
      if (n != 6 || memcmp((char *)v.b, expect, 6) != 0) {
         printf("FAIL: readv() returned %ld, iovecs hold \"%.8s\" (expected "
                "6 bytes, \"hello\\n\")\n", (long)n, (char *)v.b);
         return 1;
      }
   }

   printf("ok: readv() with 4 iovecs\n");
   return 0;
}
