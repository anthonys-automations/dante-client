/*
 * getaddrinfo(3) through libdsocks with "resolveprotocol: fake": numeric
 * addresses must resolve normally in either family (e.g. "::" to listen
 * on), while names are still faked as IPv4 without a DNS lookup.
 *
 * usage: gaicheck    (with SOCKS_CONF pointing to a config with
 *                     "resolveprotocol: fake")
 * Exits non-zero if a check fails.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

static int lookup(const char *node, int family, int protocol, int flags,
                  int expect_family, const char *expect_addr)
{
   struct addrinfo hints, *res = NULL;
   char addr[INET6_ADDRSTRLEN] = "-";
   int rc, ok;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family   = family;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = protocol;
   hints.ai_flags    = flags;

   rc = getaddrinfo(node, "9092", &hints, &res);
   if (rc == 0) {
      if (res->ai_family == AF_INET)
         inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr,
                   addr, sizeof(addr));
      else if (res->ai_family == AF_INET6)
         inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr,
                   addr, sizeof(addr));
   }

   if (expect_family == 0)          /* must fail */
      ok = rc != 0;
   else
      ok = rc == 0 && res->ai_family == expect_family
         && (expect_addr == NULL || strcmp(addr, expect_addr) == 0);

   printf("%s getaddrinfo(\"%s\", family %d, protocol %d, flags %#x): "
          "rc %d (%s), family %d, %s\n",
          ok ? "ok  " : "FAIL", node, family, protocol, flags, rc,
          rc == 0 ? "-" : gai_strerror(rc), rc == 0 ? res->ai_family : 0, addr);
   if (!ok)
      ++failures;

   if (rc == 0 && ok && expect_family == AF_INET6 && (flags & AI_PASSIVE)) {
      /* what a server does with it: listen on the IPv6 wildcard. */
      int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol), one = 1;

      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      if (s == -1 || bind(s, res->ai_addr, res->ai_addrlen) != 0
      ||  listen(s, 1) != 0) {
         perror("FAIL bind/listen on the result");
         ++failures;
      }
      else
         printf("ok   bind+listen on [%s]:9092\n", addr);
      if (s != -1)
         close(s);
   }

   if (rc == 0)
      freeaddrinfo(res);
   return ok;
}

int main(void)
{
   lookup("::",          AF_UNSPEC, 0,           AI_PASSIVE, AF_INET6, "::");
   lookup("::",          AF_UNSPEC, IPPROTO_TCP, AI_PASSIVE, AF_INET6, "::");
   lookup("::1",         AF_INET6,  0,           0,          AF_INET6, "::1");
   lookup("::",          AF_INET,   0,           0,          0,        NULL);
   lookup("127.0.0.1",   AF_UNSPEC, IPPROTO_TCP, 0,          AF_INET,  "127.0.0.1");
   lookup("127.0.0.1",   AF_INET,   0,           0,          AF_INET,  "127.0.0.1");
   /* a name: faked (an IPv4 address from the fake range), no DNS lookup. */
   lookup("example.invalid", AF_UNSPEC, 0,       0,          AF_INET,  NULL);

   printf("%s: %d failed checks\n", failures ? "FAIL" : "ok", failures);
   return failures != 0;
}
