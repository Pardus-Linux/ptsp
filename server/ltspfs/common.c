/*
 * common.c: support routines for ltspfs.
 *
 * Copyright 2005, Scott Balneaves
 * This file is licensed under the GNU GPL.  Please see the "Copying" file
 * for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "common.h"
#include "ltspfs.h"

/*
 * Open up a client socket.
 */

int
opensocket(char *hostname, int port)
{
  struct sockaddr_in serv_addr;
  struct hostent *server;
  int s;

  /*
   * Open up our socket
   */

  s = socket (AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    fprintf (stderr, "ERROR opening socket\n");
    exit(1);
  }

  server = gethostbyname (hostname);
  if (server == NULL) {
    fprintf (stderr, "ERROR, no such host\n");
    exit(1);
  }

  memset ((char *) &serv_addr, 0, sizeof (serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy ((char *) server->h_addr,
	 (char *) &serv_addr.sin_addr.s_addr, server->h_length);
  serv_addr.sin_port = htons (port);
  if (connect(s, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    fprintf (stderr, "ERROR connecting\n");
    exit(1);
  }

  return s;
}

/*
 * _readn: read n bytes from the socket
 * The select() function is used to handle timeouts.  On a timeout, the
 * timeout_function is called.
 */

int
_readn(register int fd, register char *ptr, register int nbytes,
       void (*timeout_function)(), int doselect)
{
  int nleft, nread;
  int r;
  fd_set set;					/* For select */
  struct timeval ltspfs_timeout;                /* Timeout */
  struct timeval *timeout_ptr = &ltspfs_timeout;

  FD_ZERO(&set);
  FD_SET(fd, &set);

  if (!doselect)
    timeout_ptr = NULL;

  nleft = nbytes;

  while (nleft > 0) {
    ltspfs_timeout.tv_sec  = LTSPFS_TIMEOUT;
    ltspfs_timeout.tv_usec = 0;
    r = select(FD_SETSIZE, &set, NULL, NULL, timeout_ptr);
    if (r < 0)
      return r;
    else if (r == 0)
      timeout_function();	/* it's expected that this will never return */
    else {
      nread = read(fd, ptr, nleft);
      if (nread < 0)
        return (nread);		/* error, return < 0 */
      else if (nread == 0)
        break;			/* EOF */

      nleft -= nread;
      ptr   += nread;
    }
  }

  return (nbytes - nleft);	/* return >= 0 */
}

/*
 * writen: write n bytes to the socket
 */

int
_writen(register int fd, register char *ptr, register int nbytes,
        void (*timeout_function)(), int doselect)
{
  int nleft, nwritten;
  int r;
  fd_set set;					/* For select */
  struct timeval ltspfs_timeout;                /* Timeout */
  struct timeval *timeout_ptr = &ltspfs_timeout;

  FD_ZERO(&set);
  FD_SET(fd, &set);

  if (!doselect)
    timeout_ptr = NULL;

  nleft = nbytes;

  while (nleft > 0) {
    ltspfs_timeout.tv_sec  = LTSPFS_TIMEOUT;
    ltspfs_timeout.tv_usec = 0;
    r = select(FD_SETSIZE, NULL, &set, NULL, timeout_ptr);
    if (r < 0)
      return r;
    else if (r == 0)
      timeout_function();	/* it's expected that this will never return */
    else {
      nwritten = write(fd, ptr, nleft);
      if (nwritten <= 0)
        return (nwritten);	/* error, return < 0 */

      nleft -= nwritten;
      ptr   += nwritten;
    }
  }

  return (nbytes - nleft);	/* return >= 0 */
}

/*
 * These are the user functions.  They handle some things with less parameters.
 */

int
readn(register int fd, register char *ptr, register int maxlen)
{
  return _readn(fd, ptr, maxlen, &timeout, 1);
}

int
writen(register int fd, register char *ptr, register int nbytes)
{
  return _writen(fd, ptr, nbytes, &timeout, 1);
}
