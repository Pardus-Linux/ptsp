/*
 * ltspfs: a FUSE module for implementing a diskless thin client filesysem
 * to be used for local device access.
 *
 * Copyright 2005, Scott Balneaves
 * This file is licensed under the GNU GPL.  Please see the "Copying" file
 * for details.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statfs.h>
#include <stdlib.h>
#include <signal.h>
#include <fuse.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <rpc/xdr.h>
#include "ltspfs.h"
#include "common.h"

/*
 * Globals.
 */

static        pthread_mutex_t lock;		/* mutex needed for Fuse */
static int    sockfd;				/* Global socket */
static char   *fuse_mount_point;		/* Local mount point */
static struct fuse_context *fc = NULL;		/* Fuse context for uid */

/*
 * init_pkt()
 *
 * Sets up an input and output packets.
 */

void
init_pkt(XDR *in, XDR *out, char *inbuf, char *outbuf)
{
  int i = 0;
  xdrmem_create(in,  inbuf,  LTSP_MAXBUF, XDR_DECODE);
  xdrmem_create(out, outbuf, LTSP_MAXBUF, XDR_ENCODE);
  xdr_int(out, &i);				/* reserve length field */
}


/*
 * timeout():
 * This handles a timeout on a read or write operation. 
 * Close the socket, unmount the fuse mount, and exit the program.
 * This will improve over time, retry's are a possibility, etc.
 */

void
timeout()
{
  close(sockfd);
  fuse_unmount(fuse_mount_point);
  exit(0);
}

/*
 * readpacket():
 * Helper command to read packets in, since we first have to read the packet
 * length, then the rest of the packet.
 */

int readpacket(XDR *in, char *packetbuffer)
{
  char *pktptr = packetbuffer;
  int len;

  /*
   * First, read in the first BYTES_PER_XDR_UNIT bytes, which should
   * have the packet length in them.
   */

  readn(sockfd, pktptr, BYTES_PER_XDR_UNIT);	/* Read length */
  xdr_int(in, &len);				/* decode it */
  len -= BYTES_PER_XDR_UNIT;			/* reduce count */
  pktptr += BYTES_PER_XDR_UNIT;			/* skip over count in buffer */
  return readn(sockfd, pktptr, len);		/* and read the rest */
}

/*
 * packetlen():
 * Fixes up the length of the packet.  Returns the packet length.
 */

int
writepacket(XDR *out, char *packetbuffer)
{
  int i;
  
  i = xdr_getpos(out);				/* Grab the current streampos */
  xdr_setpos(out, 0);				/* rewind to beginning */

  /*
   * Since the first thing we do when initializing a packet is leave a space
   * for an int at the beginning, this is the space for the packet length.
   * Now, write out the proper length back at the beginning.
   */

  xdr_int(out, &i);				/* Write proper length */
  i = writen(sockfd, packetbuffer, i);		/* Write the packet to socket */

  /*
   *  since we've finished sending, destroy the output stream
   */

  xdr_destroy(out);
  return i;
}

/*
 * send_recv():
 *
 * For most of the functions, this will handle the communications.  Mutexes
 * handle the locking and unlocking of the communication stream.
 */

void
send_recv(XDR *in, XDR *out, char *inbuf, char *outbuf)
{
  pthread_mutex_lock(&lock);			/* Lock mutex */
  writepacket(out, outbuf);			/* Send out packet */
  readpacket(in, inbuf);			/* Read response */
  pthread_mutex_unlock(&lock);			/* Unlock mutex */
}

#if FUSE_MINOR_VERSION >= 3
/*
 * ping_timeout:
 *
 * This function will handle sendig a "PING" packet to the server once every
 * x minutes.   If it doesn't get a response back, then it will exit the
 * timeout function will unmount the ltspfs filesystem and exit. 
 */

void
ping_timeout(void *nothing __attribute__((unused)))
{
  XDR    in, out;
  int    i;
  char   pingin[LTSP_MAXBUF];
  char   pingout[LTSP_MAXBUF];
  struct timespec ping_interval;

  init_pkt(&in, &out, pingin, pingout);		/* Initialize packets */

  i = LTSPFS_PING;
  xdr_int(&out, &i);
  i = xdr_getpos(&out);
  xdr_setpos(&out, 0);
  xdr_int(&out, &i);

  ping_interval.tv_sec  = PING_INTERVAL;
  ping_interval.tv_nsec = 0;

  while (TRUE)
  {
    nanosleep(&ping_interval, NULL);
    pthread_mutex_lock(&lock);			/* Lock mutex */
    writen(sockfd, pingout, i);			/* Send command */
    readpacket(&in, pingin);			/* Read response */
    xdr_setpos(&in, 0);
    pthread_mutex_unlock(&lock);		/* Unlock mutex */
  }
}
#endif

/*
 * parse_return:
 * Simplifies several functions by handling simple "000" or 001|errno returns.
 */

static int
parse_return(XDR *xdr)
{
  int res, retcode;

  /*
   * rewind to the beginning.
   */

  xdr_setpos(xdr, 0);			/* rewind to the beginning of packet */
  xdr_int(xdr, &res);			/* read over the length */

  if (!xdr_int(xdr, &res))		/* try to grab the return code */
    retcode = EACCES;			/* Couldnt grab, so goto out */
  else if (!res)			/* If OK, goto out */
    retcode =  OK;
  else if (!xdr_int(xdr, &retcode)) 	/* If fail, then grab the code */
    retcode = EACCES;

  xdr_destroy(xdr);			/* Last bit, so destroy the stream */
  return -retcode;
}

/*
 * ltspfs_sendauth:
 *
 * Grabs our $DISPLAY, and sends our XAUTH info for verification on the other
 * side.
 */

int
ltspfs_sendauth()
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  char xauth_command[LTSP_MAXBUF];		/* xauth command */
  char *display;				/* DISPLAY environment var */
  int  size;
  char *auth_file;				/* buffer to hold file */
  FILE *pcmd;
  int  opcode = LTSPFS_XAUTH;

  /*
   * Get the xauth token for our display.
   */

  display = getenv("DISPLAY");
  if (!display) {
    fprintf(stderr, "Error: $DISPLAY variable not set!\n");
    exit(1);
  }

  /*
   * If our DISPLAY variable starts with "localhost", we're probably trying
   * to be run under Ubuntu, which tunnels the X connection over ssh.  At
   * this point, dapper hasn't been officially released yet, so we'll
   * send a "dummy" packet if this is the case.  This will require the
   * -a option on the server.  We'll figure this out later.
   */

  if (!strncmp(display, "localhost", 9)) {
    auth_file = strdup("DUMMY AUTH");
    size = strlen(auth_file);
  } else {
    sprintf(xauth_command, "xauth extract - %s", display);
  
    /*
     * Allocate a buffer.
     */

    if ((auth_file = malloc(BUFSIZ)) == NULL) {
      fprintf(stderr, "Cannot malloc %d bytes\n", BUFSIZ);
      exit(1);
    }

    pcmd = popen(xauth_command, "r");

    if (!pcmd) {
      free(auth_file);
      fprintf(stderr, "Error: could not execute \"%s\"\n", xauth_command);
      exit(1);
    }

    size = fread(auth_file, sizeof(char), BUFSIZ, pcmd);

    pclose(pcmd);
  }

  /*
   * Now, send the authorization.
   */

  init_pkt(&in, &out, inbuf, outbuf);
  xdr_int(&out, &opcode);			/* build opcode */
  xdr_int(&out, &size);				/* build auth packet size */

  writepacket(&out, outbuf);			/* Send command */
  writen(sockfd, auth_file, size);		/* Send authfile */
  readpacket(&in, inbuf);			/* Read response */
  free(auth_file);
  return parse_return(&in);
}

/*
 * ltspfs_getattr:
 *
 * Handles the getattr filesystem call.  
 */

static int
ltspfs_getattr(const char *path, struct stat *stbuf)
{
  XDR   out, in;
  char  outbuf[LTSP_MAXBUF];
  char  inbuf[LTSP_MAXBUF];
  char  *ptr = (char *)path;
  int   opcode = LTSPFS_GETATTR;
  int   res;
  uid_t uid;
  gid_t gid;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */
  
  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  if (!xdr_int(&in, &res))		 	/* Did we get error? */
    return -EACCES;				/* bad arg */
  if (res)
    return parse_return(&in);			/* bad result */

  /*
   * Parse the return and populate the structure
   */

  if (!fc)					/* Initialized fc? */
    fc = fuse_get_context();			/* Grab the context */

  if (!xdr_u_longlong_t(&in, &stbuf->st_dev))
    return -EACCES;
  if (!xdr_u_longlong_t(&in, &stbuf->st_ino))
    return -EACCES;
  if (!xdr_u_int  (&in, &stbuf->st_mode))
    return -EACCES;
  if (!xdr_u_int  (&in, &stbuf->st_nlink))
    return -EACCES;
  if (!xdr_u_int  (&in, &uid))
    return -EACCES;
  if (!xdr_u_int  (&in, &gid))
    return -EACCES;

  /* 
   * We get back the uid and gid from the remote filesystem, but we don't
   * use it.  Basically, we use the fuse context, which tells us who
   * mounted the filesystem.  This way, the user always "owns" the files
   * on the remote media.  This should probably by an overridable option,
   * just on the off chance that someone DOES have a "real" filesystem
   * (i.e. one that knows about userids) on the remote side.
   */

  stbuf->st_uid = fc->uid;
  stbuf->st_gid = fc->gid;

  if (!xdr_u_longlong_t(&in, &stbuf->st_rdev))
    return -EACCES;
  if (!xdr_longlong_t(&in, &stbuf->st_size))
    return -EACCES;
  if (!xdr_long (&in, &stbuf->st_blksize))
    return -EACCES;
  if (!xdr_longlong_t(&in, &stbuf->st_blocks))
    return -EACCES;
  if (!xdr_long (&in, &stbuf->st_atime))
    return -EACCES;
  if (!xdr_long (&in, &stbuf->st_mtime))
    return -EACCES;
  if (!xdr_long (&in, &stbuf->st_ctime))
    return -EACCES;
  xdr_destroy(&in);

  return OK;
}

/*
 * ltspfs_readlink:
 *
 * Handles the readlink filesystem call.  
 */

static int
ltspfs_readlink(const char *path, char *buf, size_t size)
{
  XDR  out, in;
  char outbuf[LTSP_MAXBUF];
  char inbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_READLINK;
  int  ret, retcode;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  /*
   * Parse the return and populate returning link name buffer.
   */

  if (!xdr_int(&in, &ret))
    return -EACCES;
  if (ret) {
    if (!xdr_int(&in, &retcode)) {
      retcode = EACCES;
    } 
    xdr_destroy(&in);
    return -retcode;
  }
  
  ptr = buf;

  if (!xdr_string(&in, &ptr, PATH_MAX))		/* return link target */
    return -EACCES;
	
  xdr_destroy(&in);

  return OK;
}

#if FUSE_MINOR_VERSION < 3
/*
 * ltspfs_getdir:
 *
 * Handles the readdir filesystem call.  
 * The logic's a little complicated on this one.  You've got a few different
 * scenarios here:
 * 1) filler returns nonzero, server's still sending direntries.  You want
 *    to stop calling the filler command, but keep reading from the server
 *    so you don't have "leftovers" next function call.
 * 2) filler's ok, but the server on the remote end sends us a "001" error.
 * 3) (hopefully 99.99% of the time) everything's a-ok.
 */

static int
ltspfs_getdir(const char *path, fuse_dirh_t h, fuse_dirfil_t filler)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_READDIR;
  int  r = 0;
  int  statcode;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  pthread_mutex_lock(&lock);			/* Lock mutex */
  writepacket(&out, outbuf);
  readpacket(&in, inbuf);			/* Read response */

  xdr_int(&in, &statcode);
  while (statcode == LTSP_STATUS_CONT) {	/* Continue? */
    if (!r) {
      ino_t inode;
      unsigned char type;
      char dirpath[PATH_MAX];

      ptr = dirpath;
      xdr_u_longlong_t(&in, &inode);
      xdr_u_char(&in, &type);
      xdr_string(&in, &ptr, PATH_MAX); 
      r = filler(h, dirpath, type, inode);
    }

    xdr_setpos(&in, 0);				/* rewind data packet */
    readpacket(&in, inbuf);			/* Read the next line */
    xdr_int(&in, &statcode);			/* And grab the statcode */
  }

  pthread_mutex_unlock(&lock);			/* Unlock mutex */

  if (r)					/* if filler died */
    return r;					/* return it first */
  else
    return parse_return(&in);			/* Return result */
}
#else
/*
 * ltspfs_readdir:
 *
 * Handles the readdir filesystem call.  
 * The logic's a little complicated on this one.  You've got a few different
 * scenarios here:
 * 1) filler returns nonzero, server's still sending direntries.  You want
 *    to stop calling the filler command, but keep reading from the server
 *    so you don't have "leftovers" next function call.
 * 2) filler's ok, but the server on the remote end sends us a "001" error.
 * 3) (hopefully 99.99% of the time) everything's a-ok.
 */

static int
ltspfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	       off_t offset, struct fuse_file_info *fi)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_READDIR;
  int  r = 0;
  int  statcode;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  pthread_mutex_lock(&lock);			/* Lock mutex */
  writepacket(&out, outbuf);
  readpacket(&in, inbuf);			/* Read response */

  xdr_int(&in, &statcode);
  while (statcode == LTSP_STATUS_CONT) {	/* Continue? */
    if (!r) {					/* filler ok? */
      struct stat st;
      unsigned char type;
      char dirpath[PATH_MAX];

      ptr = dirpath;
      memset(&st, 0, sizeof (st));
      xdr_u_longlong_t(&in, &st.st_ino);	/* grab returned inode */
      xdr_u_char(&in, &type);			/* grab returned type */
      xdr_string(&in, &ptr, PATH_MAX); 		/* grab dirent name */
      
      st.st_mode = type << 12;			/* More magic */
      r = filler(buf, dirpath, &st, 0);		/* Call the filler function */
    }						/* endif !r */
    xdr_setpos(&in, 0);				/* reset our input buffer */
    readpacket(&in, inbuf);			/* Read the next line */
    xdr_int(&in, &statcode);			/* And grab the statcode */
  }

  pthread_mutex_unlock(&lock);			/* Unlock mutex */

  if (r)					/* if filler died */
    return r;					/* return it first */
  else
    return parse_return(&in);			/* Return result */
}
#endif


/*
 * ltspfs_mknod:
 *
 * Handles the mknod filesystem call.  
 */

static int
ltspfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_MKNOD;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_u_int(&out, &mode);			/* build mode */
  xdr_u_longlong_t(&out, &rdev);		/* build rdev */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_mkdir:
 *
 * Handles the mkdir filesystem call.  
 */

static int
ltspfs_mkdir(const char *path, mode_t mode)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_MKDIR;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_u_int(&out, &mode);			/* build mode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_onepath:
 *
 * Handles single path filesystem calls.  
 */

static int
ltspfs_onepath(int opcode, const char *path)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_unlink:
 *
 * Handles the unlink filesystem call.  
 */

static int
ltspfs_unlink(const char *path)
{
  return ltspfs_onepath(LTSPFS_UNLINK, path);
}

/*
 * ltspfs_rmdir:
 *
 * Handles the rmdir filesystem call.  
 */

static int
ltspfs_rmdir(const char *path)
{
  return ltspfs_onepath(LTSPFS_RMDIR, path);
}

/*
 * ltspfs_twopath:
 *
 * Handles generic path filesystem calls.  
 */

static int
ltspfs_twopath(int opcode, const char *from, const char *to)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  char *ptr;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  ptr = (char *)from;
  xdr_string(&out, &ptr, PATH_MAX);		/* build from */
  ptr = (char *)to;
  xdr_string(&out, &ptr, PATH_MAX);		/* build to */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_symlink:
 *
 * Handles the symlink filesystem call.  
 */

static int
ltspfs_symlink(const char *from, const char *to)
{
  return ltspfs_twopath(LTSPFS_SYMLINK, from, to);
}

/*
 * ltspfs_rename:
 *
 * Handles the rename filesystem call.  
 */

static int
ltspfs_rename(const char *from, const char *to)
{
  return ltspfs_twopath(LTSPFS_RENAME, from, to);
}

/*
 * ltspfs_link:
 *
 * Handles the link filesystem call.  
 */

static int
ltspfs_link(const char *from, const char *to)
{
  return ltspfs_twopath(LTSPFS_LINK, from, to);
}

/*
 * ltspfs_chmod:
 *
 * Handles the chmod filesystem call.  
 */

static int
ltspfs_chmod(const char *path, mode_t mode)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_CHMOD;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_u_int(&out, &mode);			/* build mode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_chown:
 *
 * Handles the chown filesystem call.  
 */

static int
ltspfs_chown(const char *path, uid_t uid, gid_t gid)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_CHOWN;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_u_int(&out, &uid);			/* build uid */
  xdr_u_int(&out, &gid);			/* build gid */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_truncate:
 *
 * Handles the truncate filesystem call.  
 */

static int
ltspfs_truncate(const char *path, off_t size)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_TRUNCATE;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_longlong_t(&out, &size);			/* build size */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_utime:
 *
 * Handles the utime filesystem call.  
 */

static int
ltspfs_utime(const char *path, struct utimbuf *buf)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_UTIME;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_long(&out, &buf->actime);			/* build accesstime */
  xdr_long(&out, &buf->modtime);		/* build modtime */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_open:
 *
 * Handles the open filesystem call.  
 */

static int
ltspfs_open(const char *path, struct fuse_file_info *fi)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_OPEN;
  char *ptr = (char *)path;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_int(&out, &fi->flags);			/* build open flags */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  return parse_return(&in);
}

/*
 * ltspfs_read:
 *
 * Handles the read filesystem call.  
 */

static int
ltspfs_read(const char *path, char *buf, size_t size, off_t offset,
	    struct fuse_file_info *fi )
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_READ;
  char *ptr = (char *)path;
  int  res, returned;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_u_int(&out, &size);			/* build packet size */
  xdr_longlong_t(&out, &offset);		/* build file offset size */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  pthread_mutex_lock(&lock);			/* Lock mutex */
  writepacket(&out, outbuf);
  readpacket(&in, inbuf);			/* Read response */

  /*
   * Parse the return and populate the read buffer passed to us.
   */

  if (!xdr_int(&in, &res))
    return -EACCES;
  if (!xdr_int(&in, &returned))
    return -EACCES;

  xdr_destroy(&in);

  if (res)					/* Error, return error code */
    return -returned;

  readn(sockfd, buf, returned);			/* read data payload */
  pthread_mutex_unlock(&lock);			/* Unlock mutex */
  
  return returned;				/* Return bytes read */
}

/*
 * ltspfs_write:
 *
 * Handles the write filesystem call.  
 */

static int
ltspfs_write(const char *path, const char *buf, size_t size,
	     off_t offset, struct fuse_file_info *fi)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_WRITE;
  char *ptr = (char *)path;
  int  res, returned;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_u_int(&out, &size);			/* build packet size */
  xdr_longlong_t(&out, &offset);		/* build file offset */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  pthread_mutex_lock(&lock);			/* Lock mutex */
  writepacket(&out, outbuf);
  writen(sockfd, (char *)buf, size);		/* Send data buffer */
  readpacket(&in, inbuf);			/* Read response */
  pthread_mutex_unlock(&lock);			/* Unlock mutex */

  /*
   * Parse the return.
   */

  if (!xdr_int(&in, &res))
    return -EACCES;
  if (!xdr_int(&in, &returned))
    return -EACCES;

  xdr_destroy(&in);

  if (res)					/* Error, return error code */
    return -returned;

  return returned;				/* Return bytes written */
}

/*
 * ltspfs_statfs:
 *
 * Handles the statfs filesystem call.  
 */

static int
ltspfs_statfs(const char *path, struct statfs *stbuf)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_STATFS;
  char *ptr = (char *)path;
  int  ret;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  send_recv(&in, &out, inbuf, outbuf);		/* send output, recv response */

  /*
   * Parse the return and populate the stbuf structure
   */

  if (!xdr_int(&in, &ret))
    return -EACCES;

  if (ret)
    return parse_return(&in);

  xdr_int(&in, &stbuf->f_type);                 /* type of fs */
  xdr_int(&in, &stbuf->f_bsize);                /* optimal transfer block sz */
  xdr_u_longlong_t(&in, &stbuf->f_blocks);      /* total data blocks in fs */
  xdr_u_longlong_t(&in, &stbuf->f_bfree);       /* free blks in fs */
  xdr_u_longlong_t(&in, &stbuf->f_bavail);      /* free blks avail to non-su */
  xdr_u_longlong_t(&in, &stbuf->f_files);       /* total file nodes in fs */
  xdr_u_longlong_t(&in, &stbuf->f_ffree);       /* free file nodes in fs */
  xdr_int(&in, &stbuf->f_namelen);            

  return OK;
}

/*
 * ltspfs_release:
 *
 * Handles the release filesystem call.  
 * Since filesystem is stateless, this is a null function.
 */

static int
ltspfs_release (const char *path __attribute__((unused)),
  struct fuse_file_info *fi __attribute__((unused)))
{
  return OK;
}

/*
 * ltspfs_fsync:
 *
 * Handles the fsync filesystem call.  
 * Since filesystem is stateless, this is a null function.
 */

static int
ltspfs_fsync (const char *path __attribute__((unused)),
  int isdatasync, struct fuse_file_info *fi __attribute__((unused)))
{
  return OK;
}

#if FUSE_MINOR_VERSION >= 3
/*
 * ltspfs_init:
 *
 * Called after the mainline's forked and daemonized.  Create a pinger 
 * thread that will sit in the background and ping the server every
 * PING_INTERVAL seconds.  In the event that the LTSP terminal is shut off
 * without a proper fusermount -u being executed, this will unmount
 * the filesystem automatically, so that dead mounts aren't hanging around.
 */

static void *
ltspfs_init (void)
{
  pthread_t ping_thread;

  /*
   * Kick off our pinger thread.
   */

  if (pthread_create(&ping_thread, NULL, (void *)&ping_timeout,
                    (void *)NULL) < 0) {
    close(sockfd);
    exit(1);
  }

  pthread_detach(ping_thread);

  return NULL;
}
#endif

void
handle_mount(char *mp)
{
  XDR  in, out;
  char inbuf[LTSP_MAXBUF];
  char outbuf[LTSP_MAXBUF];
  int  opcode = LTSPFS_MOUNT;
  char *ptr = mp;
  int  res;

  init_pkt(&in, &out, inbuf, outbuf);		/* Initialize packets */

  xdr_int(&out, &opcode);			/* build opcode */
  xdr_string(&out, &ptr, PATH_MAX);		/* build path */

  writepacket(&out, outbuf);
  readpacket(&in, inbuf);			/* Read response */

  xdr_int(&in, &res);
  if (res) {
    fprintf(stderr, "Couldn't mount %s\n", mp);
    close(sockfd);
    exit(1);
  }
}

/* 
 * Populate our FUSE function callout table.
 */

static struct fuse_operations ltspfs_oper = {
  .getattr    = ltspfs_getattr,
  .readlink   = ltspfs_readlink,
#if FUSE_MINOR_VERSION < 3
  .getdir     = ltspfs_getdir ,			/* older getdir() interface */
#else
  .readdir    = ltspfs_readdir,			/* newer readdir() interface */
#endif
  .mknod      = ltspfs_mknod,
  .mkdir      = ltspfs_mkdir,
  .symlink    = ltspfs_symlink,
  .unlink     = ltspfs_unlink,
  .rmdir      = ltspfs_rmdir,
  .rename     = ltspfs_rename,
  .link       = ltspfs_link,
  .chmod      = ltspfs_chmod,
  .chown      = ltspfs_chown,
  .truncate   = ltspfs_truncate,
  .utime      = ltspfs_utime,
  .open       = ltspfs_open,
  .read       = ltspfs_read,
  .write      = ltspfs_write,
  .statfs     = ltspfs_statfs,
  .release    = ltspfs_release,
  .fsync      = ltspfs_fsync,
#if FUSE_MINOR_VERSION >= 3
  .init       = ltspfs_init,			/* no init pre 2.3 */
#endif
};

/*
 * MAINLINE
 */

int
main (int argc, char *argv[])
{
  int  i, myargc = 0;
  char *host = NULL, *mountpoint = NULL, *hostmount = NULL;
  char **myargv;

  /*
   * Argument handling.
   *
   * This seems really perverse.  Fuse wants certain arguments.  My program
   * wants certain arguments.  Fuse doesn't want my arguments.  Solution?
   * build a new argc and argv (appropriately titled "myargc" and "myargv"),
   * passing along only the arguments that fuse wants.  In this case, that's
   * pretty much just the mount directory where the fuse mount will be.
   *
   * In keeping with the ever-so-popular nfs mount style, the overall command
   * line will look like:
   *
   * ltspfs host:/dir/to/mount /mountpoint <fuse options>
   *
   * We'll strip off the command line option that looks like: 
   * somehost:somedir, and construct a new argc and argv that looks like:
   * ltspfs /mountpoint <fuse-options>
   *
   * We also want to grab a copy of the local mountpoint, as we need it for
   * handling timeouts.   When we get a timeout, we want to execute a 
   * fuse_unmount command, so we'll assume that something that looks like 
   * "/..." is the local directory mount point.
   */

    if (argc < 3) {
      fprintf(stderr, 
	      "Usage: %s host:/dir/to/mount /mountpoint <fuse options>\n",
	      argv[0]);
      exit(1);
    }

  myargv = calloc(argc, sizeof(char *)); 	/* allocate same size */

  if (!myargv) {
    fprintf(stderr, "calloc() failed to allocate memory\n");
    exit(1);
  }

  myargv[myargc++] = argv[0];			/* program name */
 
  for (i = 1; i < argc; i++)			/* rest of arguments */
    if (strchr(argv[i], ':'))
      hostmount = strdup(argv[i]);		/* duplicate our parameter */
    else {
      if (*argv[i] == '/')
        fuse_mount_point = argv[i];		/* point to the local mount */
      myargv[myargc++] = argv[i];		/* copy the rest */
    }
    
  /*
   * Now hostmount contains the string for the host, and the directory.
   */

  if (!hostmount) {
    fprintf(stderr, "Remote dir must be specified as host:/dir.\n");
    exit(0);
  }
    
  host = hostmount;
  mountpoint = strchr(hostmount, ':');
  *mountpoint = '\0';
  mountpoint++;

  if (!host) {
    fprintf(stderr, "No host specified!\n");
    exit(0);
  }

  if (*mountpoint != '/') {
    fprintf(stderr, "No mountpoint specified!\n");
    exit(0);
  }

  /*
   * Open up our socket
   */

  sockfd = opensocket(host, PORT);

  /*
   * Initialize our mutex.
   */

  pthread_mutex_init(&lock, NULL);

  /*
   * The connection's plumbed.  Issue our mount command.
   */

  if (ltspfs_sendauth() != 0) {
    fprintf(stderr, "Authentication failed.\n");
    exit(1);
  }

  handle_mount(mountpoint);

  /*
   * We're mounted.  Fire up fuse.
   */

  return fuse_main (myargc, myargv, &ltspfs_oper);
}
