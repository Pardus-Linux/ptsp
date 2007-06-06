/*
 * Copyright 2005, Scott Balneaves <sbalneav@ltsp.org>
 * ltspfsd.c
 * Mainline handling for the Linux Terminal Server Project File Server Daemon.
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rpc/xdr.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include "ltspfsd.h"
#include "common.h"

/*
 * Globals.
 */

int    debug;			/* Debug level.  0 = no debug */
int    readonly;		/* If true, make filesystem "read only" */
int    noauth;			/* If true, skip authentication */
int    syslogopen;		/* If true, then log to syslog, else stderr */
char   *mountpoint;		/* Point we're "mounted" to */
int    authenticated;	/* Mountpoint length */
int    mounted;			/* Automounter status */

/*
 * mainline
 */

int main(int argc, char *argv[])
{

  int sockfd, newsockfd, clilen, childpid;
  struct sockaddr_in cli_addr;
  int c;
  uid_t uid;
     
  authenticated = 0;
  mounted = 0;
  mountpoint = NULL;

  /*
   * We can begin to parse command line options.
   */

  debug = readonly = opterr = noauth = 0;
     
  while ((c = getopt (argc, argv, "rad")) != -1) {
    switch (c) {
      case 'r':
        readonly++;
        break;
      case 'a':
        noauth++;
        break;
      case 'd':
          debug++;
        break;
      case '?':
        fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
        return 1;
      default:
        break;
    }
  }
     
  /*
   * Some initialization work.  We call daemon(), which will chdir to /
   * so that the daemon won't lock any filesystems. The daemon() call also
   * closes STDIN, STDOUT, and STDERR, and forks.  Then, set up our umask,
   * and redirect the SIGCHLD and SIGTERM signals to handlers.  
   */

  if (!debug) {			/* if debug's false, then background */
    daemon(0,0);
    signal(SIGCHLD, sig_chld);
    signal(SIGTERM, sig_term);
  } else
    chdir("/");

    umask(0);

  /*
   * open our logging.
   */

  if (!debug) {
    openlog(argv[0], LOG_PID, LOG_USER);
    syslogopen++;
  }

  /*
   * Open our socket
   */

  sockfd = bindsocket(SERVER_PORT);

  /*
   * Log informational message to indicate program starting
   */

  info("Program started\n");
  
  /*
   * Calculate the length of the cli_addr structure (needed for the accept)
   */

  clilen = sizeof(cli_addr);

  for (;;) {

    /*
     * Accept incoming connection
     */

    newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0)
      error_die("Can't accept connection");

    if (!debug)			/* In debug mode, don't fork */
      childpid = fork();
    else
      childpid = 0;

    if (childpid < 0)
      error_die("fork error");	/* If fork fails, something's wrong */
    else if (childpid == 0) {
      close(sockfd);		/* Child process, pass work off */
      handle_connection(newsockfd);
      exit(OK);			/* exit child, SIGCHLD sent */
    }
    close(newsockfd);		/* parent */
  }
  return OK;			/* return 0 from main */
}

/*
 * handle_connection is where the "work" gets done.  Anything you want to "do"
 * as a network daemon should be done here.
 *
 * Dispatch ltspfs calls to ltspfs_dispatch.
 */

void handle_connection(int sockfd)
{
  XDR in;
  int n;
  int i, q;
  char line[LTSP_MAXBUF];
  char *lineptr;
  fd_set set;					/* For select */
  struct timeval automount_timeout;             /* Timeout */
  int nleft, nread;
  int r;


  for (;;) {
    FD_ZERO(&set);
    FD_SET(sockfd, &set);
    xdrmem_create(&in, line, LTSP_MAXBUF, XDR_DECODE);
    lineptr = line;

    /*
     * Sigh.  I really hate to have to re-duplicate most of the readn 
     * functionality here, but it's simply cleaner to not butcher the _readn
     * and _writen primitives just to handle automount timeouts. Soooo....
     */

    automount_timeout.tv_sec  = AUTOMOUNT_TIMEOUT;
    automount_timeout.tv_usec = 0;
    r = select(FD_SETSIZE, &set, NULL, NULL, &automount_timeout);
    if (r < 0)
      error_die("handle_connection: select error\n");
    else if (r == 0) {
      if (mounted)
        am_umount(mountpoint);		/* this will return */
      continue;			/* Back to the top of the for(;;) loop */
    }

    nleft = BYTES_PER_XDR_UNIT;

    while (nleft > 0) {
      nread = read(sockfd, lineptr, nleft);
      if (nread < 0)
        error_die("handle_connection: readline error\n");
      else if (nread == 0)
	return;			/* EOF, connection closed */

      nleft   -= nread;
      lineptr += nread;
    }
    
    xdr_int(&in, &i);
    if (debug)
      info("Packet length: %d\n", i);
    n = readn(sockfd, lineptr, (i - BYTES_PER_XDR_UNIT));
    if (n == 0)
      return;			/* connection closed */
    else if (n < 0)
      error_die("handle_connection: readline error\n");

    /*
     *  OK We've got our command line, pass this off to ltspfs_dispatch
     */

    if (debug) {
      info("Packet buffer: ");
      for (q = 0; q < i; q++)
        info("%x", line[q]);
      info("\n");
    }

    ltspfs_dispatch(sockfd, &in);
    
    /*
     * Well, we're back.  Reset our position to the beginning.
     */
    xdr_destroy(&in);
  }
}

/*
 * Signal handler function for SIGCHLD
 */

void sig_chld(int signo __attribute__((unused)))
{
    pid_t pid;
    int stat;

    pid = wait(&stat);		/* reap our child process */
    return;
}

/*
 * Signal handler function for SIGTERM
 */

void sig_term(int signo __attribute__((unused)))
{
    info("Recieved SIGTERM, shutting down\n");
    exit(OK);
}
