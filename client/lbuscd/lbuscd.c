/*
 * lbuscd.c - Copyright 2005 by James A. McQuillan (jam@Ltsp.org)
 *
 * This routine is for the Linux Terminal Server Project (LTSP.org)
 * It's purpose is to run on the workstation and listen on a port
 * for incoming connections from servers.  Once a server connects, it
 * will 'Register' itself to this program.  From that point on, any
 * time a device is added to the system, lbuscd will send out messages
 * to all registered servers, to tell them of the new device.
 *
 * The idea is that those remote servers can do something, like create
 * a desktop Icon for the user, and mount the device using nfs, smb or
 * sshfs, to give the user access from their session running on the 
 * server, to the device attached to their local workstation.
 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <netdb.h>
#include <syslog.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <popt.h>
#include <sys/ioctl.h>
#include <linux/iso_fs.h>
#include <linux/cdrom.h>

static char version[] = "lbuscd Version 0.6";
char *fpath = "/tmp/lbus.fifo";
char *generic_cdrom = "CDrom";

#define    FALSE      0
#define    TRUE       1
#define    MAXARGS    20
#define    MAXCLIENTS FD_SETSIZE
#define    LOGOPTS    (LOG_PERROR|LOG_PID|LOG_DAEMON|LOG_ERR)

#define    MAXDEV     20
#define    BLOCKDEV   1

static int nPort      = 9202;
static int fDebug     = FALSE;
static int fNoDaemon  = FALSE;

struct {
  char    *buf;
  ssize_t bufsize;
  int     registered;
  int     userid;
  char    *username;
} Clients[MAXCLIENTS];

struct {
  char    *sharename;
  char    *desc;
  int     removeable;
  int     devnum;
  int     size;
  int     type;            // 1=block
  int     cdrom;           // Do we need to poll the device for media?

  /* CDRom info */

  char    *blockdev;
  char    *volumeid;
  int     media_present;
  int     disctype;
} Devices[MAXDEV];

int device_id    = 0;
int message_id   = 0;

/*
 * Function Prototypes
 */

void DecodeCommandLine(int, char**);
void server(void);

int  process_server_msg( int );
int  process_fifo_event( int );
void dispatch_fifo_request( char * );
void dispatch_server_request( char *, int );
void enumerate_devices( int, int, char ** );
void register_client( int, int, char ** );

void new_block_device( int, char **, char * );
void remove_device( int, char ** );
void dump_devices( void );

void send_AddBlockDevice_msg( int, int, int );
void send_RemoveDevice_msg( int, int, int );
int  find_device_by_sharename( char * );

void clear_dev_entry( int );
void clear_client_entry( int );

void new_cdrom_drive( int, char **, char * );
void check_cdrom_status( int );
void cdrom_inserted( int, int );
void cdrom_removed( int, int );
void add_cdrom_data( int, int );

int split( char **, int asize, char *, char *);

//-----------------------------------------------------------------------------

int main( int argc, char **argv ) {
  DecodeCommandLine(argc,argv);
  if(fDebug)
    fprintf(stderr,"%s\n",version);
  server();
  return(0);
}

//-----------------------------------------------------------------------------

void usage() {
  fprintf(stderr,"\nUsage: lbuscd [{-p|--port} <portnum>]   default=9202\n");
  fprintf(stderr,"              [{-d|--debug}]            Turn on debugging\n");
  fprintf(stderr,"              [{-n|--nodaemon}]         Do not daemonize\n");
  fprintf(stderr,"              [{-v|--version}]          Display version\n");
  fprintf(stderr,"\n");
  exit(1);
}

//-----------------------------------------------------------------------------

int split( char **array, int asize, char *delim, char *str ) {
  int i;
  char *s;

  memset( array, 0x00, ( sizeof(array) * asize ) );   // Zero out the array
  i = 0;
  while( ( s = strsep( &str, delim ) ) != NULL  && i < asize ){
    if(strlen(s))
      array[i++] = s;
  }
  return( i );
}

//-----------------------------------------------------------------------------

void DecodeCommandLine(int argc, char **argv) {
  char c;
  poptContext optCon;        // context for parsing command-line options

  struct poptOption optionsTable[] = {
    { "port",           'p', POPT_ARG_INT,    &nPort,        0,   NULL, NULL },
    { "help",           'h', 0,               0,             'h', NULL, NULL },
    { "debug",          'd', 0,               0,             'd', NULL, NULL },
    { "nodaemon",       'n', 0,               0,             'n', NULL, NULL },
    { "version",        'v', 0,               0,             'v', NULL, NULL },
    { NULL,              0,  0,               NULL,          0,   NULL, NULL }
  };

  optCon = poptGetContext( NULL, argc, (const char **)argv, optionsTable, 0 );

  while( ( c = poptGetNextOpt(optCon) ) >= 0 ){
    if(c < 0){
      fprintf(stderr,"Error occurred!\n");
    }
    switch(c){
      case 'h':   usage();
                  break;
      case 'd':   fDebug = TRUE;
                  break;
      case 'n':   fNoDaemon = TRUE;
                  break;
      case 'v':   fprintf(stderr,"%s\n",version);
                  exit(1);
    }
  }

  if( c < -1 ){
    fprintf( stderr,
             "%s, option = %s\n",
             poptStrerror(c),
             poptBadOption(optCon,0) );
    exit(1);
  }

  poptFreeContext(optCon);
}

//-----------------------------------------------------------------------------

void server() {
  struct rlimit       resourcelimit;
  struct protoent     *proto;
  struct sockaddr_in  netaddr;

  int    i;
  int    netfd, fd, fifo, one = 1;

  fd_set readable_handles, active_handles;

  struct sockaddr_in clientname;
  socklen_t size;

  if(fDebug){
    fprintf(stderr,"Inside of server(), \n");
  }
  else if( !fNoDaemon ){
    /*
    ** Throw the routine into background by calling fork()
    */

    switch (fork()) {
      case -1: syslog(LOGOPTS, "fork: %m\n");
               exit(1);

      case 0:  /* child */
               break;

      default: /* parent */
               exit(0);
    }

    /* Now in child process */

    /*
    ** Get the maximum number of files that can be opened
    ** by this process so we can close them all.
    */
    resourcelimit.rlim_max = 0;
    if ( getrlimit(RLIMIT_NOFILE, &resourcelimit) < 0) {
      syslog(LOGOPTS, "getrlimit: %m\n");
      exit(1);
    }

    /*
    ** Loop through and close all file handles
    */
    for (fd = 0; fd < resourcelimit.rlim_max; ++fd)
      (void)close(fd);

    if( setsid() < 0 ){
      syslog( LOGOPTS, "setsid: %m\n" );
      exit(1);
    }

    (void)chdir("/");
    (void)umask(022);
    fd = open("/dev/null", O_RDWR);    /* stdin */
    (void)dup(fd);                     /* stdout */
    (void)dup(fd);                     /* stderr */

  } /* ! fDebug */

  //
  // Clear the dev table
  //
  for( i=0; i < MAXDEV; i++ ){
    clear_dev_entry(i);
  }

  //
  // If there's already a fifo, remove it.
  // Then, create a new fifo
  //
  if( access( fpath, F_OK ) == 0 ){
    unlink(fpath);
  }

  if( mkfifo(fpath, 0666 ) ){
    perror("mkfifo");
    exit(EXIT_FAILURE);
  }

  if( ( fifo = open( fpath, O_RDONLY | O_NONBLOCK ) ) < 0 ){
    perror("open fifo");
    exit(EXIT_FAILURE);
  }

  //
  // Setup the buffer to hold the data being read
  //
  Clients[fifo].buf     = NULL;
  Clients[fifo].bufsize = 0;

  /*
   * Setup all the stuff we need to create a socket
   */

  if( ( proto = getprotobyname("tcp") ) == NULL ){
    syslog(LOGOPTS, "Cannot find protocol for TCP!\n");
    if(fDebug)
      fprintf( stderr, "Error calling getprotobyname() %s\n",
               strerror(errno));
    exit(1);
  }

  if( ( netfd = socket(AF_INET, SOCK_STREAM, proto->p_proto) ) < 0 ) {
    syslog(LOGOPTS, "socket: %m\n");
    if(fDebug)
      fprintf(stderr,"Error calling socket() %s\n",strerror(errno));
    exit(1);
  }

  if( setsockopt(netfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one) ) < 0 ){
    syslog(LOGOPTS, "setsockopt: %m\n");
    if(fDebug)
      fprintf(stderr,"Error calling setsockopt() %s\n",strerror(errno));
    exit(1);
  }

  netaddr.sin_port        = htons(nPort);
  netaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  memset(netaddr.sin_zero, 0, sizeof(netaddr.sin_zero));

  if( bind( netfd, (struct sockaddr *)&netaddr, sizeof(netaddr) ) < 0 ){
    syslog(LOGOPTS, "bind: %m\n");
    if(fDebug)
      fprintf(stderr,"Error calling bind() %s\n",strerror(errno));
    exit(1);
  }

  if( listen( netfd, 10 ) < 0 ){
    syslog(LOGOPTS, "listen: %m\n");
    if(fDebug)
      fprintf(stderr,"Error calling listen() %s\n",strerror(errno));
    exit(1);
  }

  /****************************************************************
   * Setup the set of filehandles that we'll be "select()'ing"
   */

  if(fDebug)
    fprintf(stderr,"Setup the filehandle vector for the select()\n");

  FD_ZERO( &active_handles );
  FD_SET( netfd, &active_handles );
  FD_SET( fifo, &active_handles );

  //
  // Loop forever
  //
  while(1){
    int    sock;
    int    drive;
    struct timeval waittime;

    waittime.tv_sec  = 1L;
    waittime.tv_usec = 0L;

    readable_handles = active_handles;

    //
    // We wait for upto 1 second for any data to be available.  Then, if
    // no data was available, we check the status of the cdrom, to
    // see if media has been inserted/removed
    //
    if( select( FD_SETSIZE, &readable_handles, NULL, NULL, &waittime ) < 0) {
      perror("select");
      exit(EXIT_FAILURE);
    }

    //
    // Loop through all available file descriptors
    //
    for( sock = 0; sock < FD_SETSIZE; sock++ ){
      //
      // See if this filehandle has data ready
      //
      if( FD_ISSET( sock, &readable_handles) ){
        if(fDebug)
          fprintf(stderr,"sock %d is rumored to have something for us\n", sock);
        if( sock == netfd ){
          //
          // New connection coming in
          //
          int newsock;
          size = sizeof(clientname);
          newsock  = accept( sock, (struct sockaddr *) &clientname, &size );
          if( newsock < 0 ){
            perror("accept");
            exit(EXIT_FAILURE);
          }
          if(fDebug){
            fprintf(stderr,"Server: connect from host %s, port %hd.\n",
                           inet_ntoa(clientname.sin_addr),
                           ntohs(clientname.sin_port));
          }
          //
          // Add this new socket to our list of fd's to select() on
          //
          FD_SET( newsock, &active_handles );

          fcntl( newsock, F_SETFL, O_NONBLOCK );

          //
          // Setup the buffer to hold the data being read
          //
          Clients[newsock].buf     = NULL;
          Clients[newsock].bufsize = 0;
        }
        else if( sock == fifo ){
          //
          // Data coming in from the fifo. Prolly the hotplug script
          //
          if( process_fifo_event( sock ) < 0 ){
            FD_CLR( sock, &active_handles );
            close(sock);
            if( ( fifo = open( fpath, O_RDONLY | O_NONBLOCK ) ) < 0 ){
              perror("open fifo");
              exit(EXIT_FAILURE);
            }

            FD_SET( fifo, &active_handles );
            fcntl( fifo, F_SETFL, O_NONBLOCK );
            //
            // Setup the buffer to hold the data being read
            //
            Clients[sock].buf     = NULL;
            Clients[sock].bufsize = 0;
          }
        }
        else{
          //
          // Data is available from one of the network sockets
          //
          if( process_server_msg( sock ) < 0 ){
            //
            // If there's a problem reading the socket, lets close it and
            // remove it from the list of handles we're interested in
            //
            if(fDebug)
              fprintf(stderr,"Lost connection from %d\n", sock );
            close( sock );
            FD_CLR( sock, &active_handles );
            if( Clients[sock].registered ){
              if( Clients[sock].bufsize > 0 ){
                free( Clients[sock].buf );
              }
              free( Clients[sock].username );
              clear_client_entry( sock );
            }
          }
        }
      }
    }

    //
    // Now, check for any change in the cdrom status
    //
    for( drive = 0; drive < MAXDEV; drive++ ){
      if( Devices[drive].devnum > 0 && Devices[drive].cdrom )
        check_cdrom_status( drive );
    }
  }
  exit(1);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//
//  Messages that come in on the fifo
//
//  These are typically messages from the hotplug system telling us that
//  there's a new device detected.
//

int process_fifo_event( int sock ) {

  ssize_t bytesread, size;
  char buffer[100];

  if(fDebug)
    fprintf(stderr,"Something coming in from the fifo: %d\n", sock);

  bytesread = read( sock, buffer, sizeof(buffer) );

  if(fDebug)
    fprintf( stderr, "bytesread from fifo: %d\n", bytesread );

  if( bytesread > 0 ){
    while( bytesread > 0 ){
      //
      // Allocate some space on the client buffer, and throw the newly
      // read data on the end
      //

      size = Clients[sock].bufsize + bytesread;
      if( Clients[sock].bufsize ){
        if( ( Clients[sock].buf
              = ( char *) realloc((void *)Clients[sock].buf,size) ) == NULL ){
          perror("realloc");
          exit(EXIT_FAILURE);
        }
      }
      else{
        if( ( Clients[sock].buf = malloc( size ) ) == NULL ){
          perror("malloc");
          exit(EXIT_FAILURE);
        }
      }
      memcpy( ( Clients[sock].buf + Clients[sock].bufsize ),
              buffer, bytesread );
      Clients[sock].bufsize += bytesread;

      //
      // Keep on reading, until there ain't no more
      //
      bytesread = read( sock, buffer, sizeof(buffer) );
    }
   
    size = Clients[sock].bufsize; 
    if( Clients[sock].buf[size-1] == '\n' ){
      char *p, *ptr, *line;
      char *s, *d;
      int i;

      if( ( p = malloc(size+1) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }

      memset( p, '\0', size+1 );

      //
      // Copy the data to a tmp buffer, skipping carriage returns
      //
      for( i=0, s=Clients[sock].buf, d=p ; i < size; i++, s++){
        if( *s != '\r' ) *d++ = *s;
      }

      ptr = p;

      while( ( line = strsep( &ptr, "\n" ) ) != NULL ){
        if( strlen(line) ){
          char *save_p = ptr;     // Save the ptr, so we can use strsep later
          dispatch_fifo_request( line );
          ptr = save_p;
        }
      }

      free( p );                  // Free our temporary space
      free( Clients[sock].buf );
      Clients[sock].bufsize = 0;
    }
    else{
      if(fDebug){
        fprintf(stderr,
                "There wasn't a newline, so lets go back to the select()\n");
      }
    }
  }
  else{
    return(-1);    // No data to read
  }
  return(0);
}

//-----------------------------------------------------------------------------

void dispatch_fifo_request( char *p ) {
  char *argv[20];
  int  argc;

  argc = split( argv, 20, "|", p );

  if(fDebug)
    fprintf(stderr,"Processing local request of: %s\n", argv[0]);

  if( strcasecmp( argv[0], "AddBlockDevice" ) == 0 ){
    new_block_device( argc-1, &argv[1], p );
  }
  else if( strcasecmp(argv[0], "AddCDRomDrive" ) == 0 ){
    new_cdrom_drive( argc-1, &argv[1], p );
  }
  else if( strcasecmp(argv[0], "RemoveDevice" ) == 0 ){
    remove_device( argc-1, &argv[1] );
  }
  else if( strcasecmp(argv[0], "DumpDevices" ) == 0 ){
    dump_devices( );
  }
}

//-----------------------------------------------------------------------------

void new_block_device( int argc, char **argv, char *recvd_line ) {

  int len;
  int idx;
  int newdev;
  int fFound;

  idx    = 0;
  fFound = FALSE;

  if( argc == 5 ){
    while( ! fFound && idx < MAXDEV ){
      if( Devices[idx].devnum == 0 ){
        newdev = idx;
        fFound = TRUE;
      }
      idx++;
    }

    if( fFound ){
      device_id++;

      //
      // Sharename
      //
      len = strlen(argv[0]);

      if( ( Devices[newdev].sharename = malloc( len+1 ) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      strcpy( Devices[newdev].sharename, argv[0] );

      //
      // Block Device Name
      //
      len = strlen(argv[1]);

      if( ( Devices[newdev].blockdev = malloc( len+1 ) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      strcpy( Devices[newdev].blockdev, argv[1] );

      //
      // Device Description
      //
      len = strlen(argv[4]);
      if( ( Devices[newdev].desc = malloc(len+1) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      strcpy( Devices[newdev].desc, argv[4] );

      //
      // Fill in the Devices entry
      //

      Devices[newdev].removeable = atoi(argv[2]);
      Devices[newdev].devnum     = device_id;
      Devices[newdev].size       = atoi(argv[3]);
      Devices[newdev].type       = BLOCKDEV;

      for( idx = 0; idx < MAXCLIENTS; idx++ ){
        if( Clients[idx].registered )
          send_AddBlockDevice_msg( newdev, message_id++, idx );
      }
    }
    else{
      if(fDebug)
        fprintf(stderr,"Couldn't find a slot to insert our new device into!\n");
    }
  }
  else{
    syslog(LOGOPTS,
           "new_block_device: Invalid number of args\n");
    syslog(LOGOPTS,
           "Expecting: 'AddBlockDevice|sharename|blockdev|Removeable|size|desc'\n");
    syslog(LOGOPTS,
           " Received: '%s'\n", recvd_line );

    if( fDebug ){
      fprintf(stderr,
              "Invalid number of arguments in new_block_device\r\n");
      fprintf(stderr,
            "expecting: 'AddBlockDevice|sharename|blockdev|Removeable|size|desc'\r\n");
      fprintf(stderr,
            " received: '%s'\r\n", recvd_line );
    }
  }
}

//-----------------------------------------------------------------------------

void new_cdrom_drive( int argc, char **argv, char *recvd_line ) {

  int len;
  int idx;
  int newdev;
  int fFound;

  idx    = 0;
  fFound = FALSE;

  if( argc == 3 ){
    while( ! fFound && idx < MAXDEV ){
      if( Devices[idx].devnum == 0 ){
        newdev = idx;
        fFound = TRUE;
      }
      idx++;
    }

    if( fFound ){
      device_id++;

      //
      // Sharename
      //
      len = strlen(argv[0]);

      if( ( Devices[newdev].sharename = malloc( len+1 ) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      strcpy( Devices[newdev].sharename, argv[0] );

      //
      // Block Device Name
      //
      len = strlen(argv[1]);

      if( ( Devices[newdev].blockdev = malloc( len+1 ) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      strcpy( Devices[newdev].blockdev, argv[1] );

      //
      // Device Description
      //
      len = strlen(argv[2]);
      if( ( Devices[newdev].desc = malloc( len+1 ) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      strcpy( Devices[newdev].desc, argv[2] );

      Devices[newdev].devnum        = device_id;
      Devices[newdev].type          = BLOCKDEV;
      Devices[newdev].cdrom         = 1;
      Devices[newdev].removeable    = 1;
      Devices[newdev].media_present = 0;    // Start off assuming no media
    }
    else{
      if(fDebug)
        fprintf(stderr,"Couldn't find a slot to insert our new cdrom into!\n");
    }
  }
  else{
    syslog(LOGOPTS,
           "new_cdrom_drive: Invalid number of args\n");
    syslog(LOGOPTS,
           "Expecting: 'AddCDRomDrive|sharename|blockdev|desc'\n");
    syslog(LOGOPTS,
           " Received: '%s'\n", recvd_line );

    if( fDebug ){
      fprintf(stderr,
              "Invalid number of arguments in new_cdrom_drive\r\n");
      fprintf(stderr,
            "expecting: 'AddCDRomDrive|sharename|blockdev|desc'\r\n");
      fprintf(stderr,
            " received: '%s'\r\n", recvd_line );
    }
  }
}

//-----------------------------------------------------------------------------

void remove_device( int argc, char **argv ) {

  int idx;
  int devidx;

  if( ( devidx = find_device_by_sharename( argv[0] ) ) >= 0 ){

    if(fDebug)
      fprintf(stderr,"Remove device: %s\n", Devices[devidx].sharename);

    for( idx = 0; idx < MAXCLIENTS; idx++ ){
      if( Clients[idx].registered ){
        send_RemoveDevice_msg( devidx, message_id++, idx );
      }
    }

    free( Devices[devidx].sharename );
    free( Devices[devidx].desc );

    if( Devices[devidx].blockdev ){
      free( Devices[devidx].blockdev );
      Devices[devidx].blockdev = 0;
    }

    if( Devices[devidx].volumeid ){
      free( Devices[devidx].volumeid );
      Devices[devidx].volumeid = 0;
    }

    clear_dev_entry( devidx );
  }
}

void clear_client_entry( int idx ) {
  memset( &Clients[idx], 0x00, sizeof( Clients[idx] ) );
}

void clear_dev_entry( int idx ) {
  memset( &Devices[idx], 0x00, sizeof( Devices[idx] ) );
}

//-----------------------------------------------------------------------------

void dump_devices(  ) {
  int i;

  if( fDebug ){
    fprintf(stderr,"\ndump_devices:\n");
    for( i = 0; i < MAXDEV; i++ ){
      if( Devices[i].devnum ){
        fprintf(stderr,"  %d) [%d] [%s] [%s] [%d] [%d] [%d]\n", i,
                                            Devices[i].devnum,
                                            Devices[i].sharename,
                                            Devices[i].desc,
                                            Devices[i].removeable,
                                            Devices[i].size,
                                            Devices[i].type );
      }
    }
  }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//
//  Messages that come in over the network connection.
//
//  These are messages from the server daemon (lbussd), or from someone
//  telnetting into us, to run a debug session
//

int process_server_msg( int sock ) {
  ssize_t bytesread, size;
  char buffer[100];

  if(fDebug)
    fprintf(stderr,"Something coming in from sock: %d\n", sock);

  bytesread = read( sock, buffer, sizeof(buffer) );
  if( bytesread > 0 ){
    while( bytesread > 0 ){
      //
      // Allocate some space on the client buffer, and throw the newly
      // read data on the end
      //

      size = Clients[sock].bufsize + bytesread;
      if( Clients[sock].bufsize ){
        if( ( Clients[sock].buf
              = realloc((void *)Clients[sock].buf,size) ) == NULL ){
          perror("realloc");
          exit(EXIT_FAILURE);
        }
      }
      else{
        if( ( Clients[sock].buf = malloc( size ) ) == NULL ){
          perror("malloc");
          exit(EXIT_FAILURE);
        }
      }
      memcpy( ( Clients[sock].buf + Clients[sock].bufsize ),
              buffer, bytesread );
      Clients[sock].bufsize += bytesread;

      bytesread = read( sock, buffer, sizeof(buffer) );
    }
   
    size = Clients[sock].bufsize; 
    if( Clients[sock].buf[size-1] == '\n' ){
      char *p, *ptr, *line;
      char *s, *d;
      int i;

      if( ( p = malloc(size+1) ) == NULL ){
        perror("malloc");
        exit(EXIT_FAILURE);
      }

      memset( p, '\0', size+1 );

      //
      // Copy the data to a tmp buffer, skipping carriage returns
      //
      for( i=0, s=Clients[sock].buf, d=p ; i < size; i++, s++ ){
        if( *s != '\r' ) *d++ = *s;
      }

      ptr = p;

      while( ( line = strsep( &ptr, "\n" ) ) != NULL ){
        if( strlen( line ) ){
          char *save_p = ptr;     // Save the ptr, so we can use strsep later
          if( fDebug )
            fprintf(stderr,"about to process request: [%s] from sock: %d\n",
                       line, sock );
          dispatch_server_request( line, sock );
          ptr = save_p;
        }
      }

      free( p );                  // Free our temporary space
      free( Clients[sock].buf );
      Clients[sock].bufsize = 0;
    }
  }
  else{
    return(-1);    // No data to read
  }
  return(0);
}

//-----------------------------------------------------------------------------
//
// dispatch_server_request() is where the server gets to tell the
// client daemon to do someting, such as it wants to Register with the
// client, or it wants to EnumerateDevices() (Get a list of devices)
//
void dispatch_server_request( char *p, int sock ) {
  char *argv[20];
  int  argc;

  argc = split( argv, 20, "|", p );

  if(fDebug)
    fprintf(stderr,"dispatching: %s\n", argv[0]);

  if( strcasecmp( argv[0], "Register" ) == 0 ){
    register_client( sock, argc-1, &argv[1] );
  }
  else if( strcasecmp( argv[0], "EnumerateDevices" ) == 0 ){
    enumerate_devices( sock, argc-1, &argv[1] );
  }
  else{
    if(fDebug)
      fprintf(stderr,"Unrecognized!!\n");
  }
}

//-----------------------------------------------------------------------------
//
// register_client()
//
//  Basically, just tell the client that there is a server session out there
//  that is interested in getting events from the workstation.
//
void register_client( int sock, int argc, char **argv ){

  int len;
  int nbytes;

  if( argc == 3 ){
    Clients[sock].registered = 1;
    Clients[sock].userid     = atoi(argv[1]);

    len = strlen( argv[2] );
    if( ( Clients[sock].username = malloc( len+1 ) ) == NULL ){
      perror("malloc");
      exit(EXIT_FAILURE);
    }
    strcpy( Clients[sock].username, argv[2] );

    if(fDebug)
      fprintf(stderr,"Registering new client, userid: %d, username: %s\n",
                     Clients[sock].userid,
                     Clients[sock].username);
  }
  else{
    char *msg = "expecting: 'Register|msgid|userid|username'\r\n";
    syslog( LOGOPTS, msg );
    nbytes = write( sock, msg, strlen(msg));
    if( nbytes < 0 ){
      char buf[100];
      sprintf(buf,"write to sock %d failed",sock);
      perror(buf);
      exit(EXIT_FAILURE);
    }
  }
}

//-----------------------------------------------------------------------------

void enumerate_devices( int sock, int argc, char **argv ) {

  int i;
  int groupid = message_id++;

  if(fDebug)
    fprintf(stderr,"Enumerate the devices back to sock: %d\n", sock);

  for( i = 0; i < MAXDEV; i++ ){
    if( Devices[i].devnum ){
      if( Devices[i].cdrom ){
        if( Devices[i].media_present ){
          send_AddBlockDevice_msg( i, groupid, sock );
        }
      }
      else{
        send_AddBlockDevice_msg( i, groupid, sock );
      }
    }
  }
}

//-----------------------------------------------------------------------------

int find_device_by_sharename( char *s ) {
  int idx;
  int devidx;
  int fFound;

  fFound = FALSE;
  idx    = 0;

  while( ! fFound && idx < MAXDEV ){
    if( Devices[idx].devnum ){
      if( strcasecmp( Devices[idx].sharename, s ) == 0 ){
        devidx = idx;
        fFound = TRUE;
      }
    }
    idx++;
  }
  return( ( fFound ) ? devidx : -1 );
}

//-----------------------------------------------------------------------------

void send_AddBlockDevice_msg( int devidx, int msgid, int sock ) {

  char buffer[512];
  int  nbytes;

  sprintf(buffer,"AddBlockDevice|%d|%d|%s|%d|%d|%s\r\n",
                 msgid,
                 Devices[devidx].devnum,
                 Devices[devidx].sharename,
                 Devices[devidx].removeable,
                 Devices[devidx].size,
                 ( ( Devices[devidx].cdrom ) ? Devices[devidx].volumeid
                                           : Devices[devidx].desc ) );

#ifdef COMMENT
  if( Devices[devidx].cdrom ){
    sprintf(buffer,"AddBlockDevice|%d|%d|%s|%d|%d|%s\r\n",
                   msgid,
                   Devices[devidx].devnum,
                   Devices[devidx].sharename,
                   Devices[devidx].removeable,
                   Devices[devidx].size,
                   Devices[devidx].volumeid );
  }
  else{
    sprintf(buffer,"AddBlockDevice|%d|%d|%s|%d|%d|%s\r\n",
                   msgid,
                   Devices[devidx].devnum,
                   Devices[devidx].sharename,
                   Devices[devidx].removeable,
                   Devices[devidx].size,
                   Devices[devidx].desc );
  }
#endif

  if(fDebug)
    fprintf(stderr, "Sending %s to %d\n", buffer, sock );

  nbytes = write( sock, buffer, strlen(buffer) );
  if( nbytes < 0 ){
    char buf[100];
    sprintf(buf,"write to sock %d failed",sock);
    perror(buf);
    exit(EXIT_FAILURE);
  }
}

//-----------------------------------------------------------------------------

void send_RemoveDevice_msg( int devidx, int msgid, int sock ) {

  char buffer[512];
  int  nbytes;

  sprintf(buffer,"RemoveDevice|%d|%d\r\n",
                 msgid,
                 Devices[devidx].devnum );

  nbytes = write( sock, buffer, strlen(buffer) );
  if( nbytes < 0 ){
    char buf[100];
    sprintf(buf,"write to sock %d failed",sock);
    perror(buf);
    exit(EXIT_FAILURE);
  }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//
//  Routines for monitoring the CDROM Drive status
//

void check_cdrom_status(int drive) {
  int cdromfd = open( Devices[drive].blockdev, O_RDONLY | O_NONBLOCK );
  if( cdromfd >= 0 ){
    int ret = ioctl(cdromfd, CDROM_DRIVE_STATUS, 0);
    switch(ret){
      case CDS_DISC_OK:
              //
              // If the state changed, then we need to do something
              //
              if( ! Devices[drive].media_present ){
                cdrom_inserted( drive, cdromfd );
              }
              break;
      case CDS_TRAY_OPEN:
              if( Devices[drive].media_present ){
                cdrom_removed( drive, cdromfd );
              }
              break;
      case CDS_NO_INFO:
              if( Devices[drive].media_present ){
                cdrom_removed( drive, cdromfd );
              }
              break;
      case CDS_DRIVE_NOT_READY:
              break;
      default:
              break;
    }
    close(cdromfd);
  }
}

//-----------------------------------------------------------------------------
//
// A disc has been detected in the drive.
// We need to make sure it is a DATA disc
// Then, we need to read the volume label, and the volume size,
// so we can report it up to the server
//

void cdrom_inserted( int drive, int cdromfd ) {
  int  ret;
  char buf[256];
  int  idx;

  sprintf( buf, "A new CDRom has been inserted in %s\n",
                Devices[drive].blockdev );
  printf( buf );
  syslog(LOGOPTS, buf );

  ret = ioctl( cdromfd, CDROM_DISC_STATUS);
  switch(ret){
    case CDS_DATA_1:
    case CDS_DATA_2:
             add_cdrom_data( drive, cdromfd );
             break;
  }

  Devices[drive].disctype      = ret;
  Devices[drive].media_present = TRUE;

  if( Devices[drive].disctype == CDS_DATA_1
  ||  Devices[drive].disctype == CDS_DATA_2 ){
    if(fDebug)
      fprintf(stderr,"CD Inserted, volumeid: [%s]\n", Devices[drive].volumeid);
    for( idx = 0; idx < MAXCLIENTS; idx++ ){
      if( Clients[idx].registered )
        send_AddBlockDevice_msg( drive, message_id++, idx );
    }
  }
}

//-----------------------------------------------------------------------------

void cdrom_removed( int drive, int cdromfd ) {

  char buf[256];
  int idx;
  sprintf( buf, "The CDRom has been removed from %s\n",
                Devices[drive].blockdev );
  syslog(LOGOPTS, buf );
  printf( "The CDRom has been removed from %s\n", Devices[drive].blockdev );

  Devices[drive].media_present = FALSE;

  if( Devices[drive].disctype == CDS_DATA_1
  ||  Devices[drive].disctype == CDS_DATA_2 ){
    for( idx = 0; idx < MAXCLIENTS; idx++ ){
      if( Clients[idx].registered ){
        send_RemoveDevice_msg( drive, message_id++, idx );
      }
    }
    free( Devices[drive].volumeid );
    Devices[drive].volumeid = 0;

    Devices[drive].disctype = 0;
  }
}

//-----------------------------------------------------------------------------

void add_cdrom_data( int drive, int cdromfd ) {

  char buf[80];
  int sector;
  char *p, *volid = NULL;
  struct iso_primary_descriptor ipd;
  int fd;

  //
  // We need to read the 'iso_primary_descriptor' to get some info
  // about the filesystem
  //

  //
  // For some completely strange reason, we can't use cdromfd.  Using it
  // gives us cdrom block errors!  So, open a new one for the volid read.
  //
  
  fd = open(Devices[drive].blockdev, O_RDONLY | O_NONBLOCK);

  //
  // cdrom descriptor might not be right at sector 16.  
  //
  
  for (sector = 16; sector < 100; sector++) {
    if ( lseek( fd, sector * ISOFS_BLOCK_SIZE, SEEK_SET) == (off_t) -1 ) {
      sprintf(buf, "lbuscd: seek error on %s, errno=%d\n",
                   Devices[drive].blockdev, errno );
      syslog( LOGOPTS, buf );
      return;
    }

    if (read(fd, &ipd, sizeof(ipd)) < 0) {
      sprintf(buf, "lbuscd: read error on %s, errno=%d\n",
                   Devices[drive].blockdev, errno );
      syslog(LOGOPTS, buf );

      //
      // If the read failed, set a generic cd value
      //
      
      volid = strdup(generic_cdrom);

      if (!volid) {
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      Devices[drive].volumeid = volid;

      close(fd);

      return;
    }


    // Get the CDRom Volume ID

    //
    // Allocate a piece of memory 1 byte larger than the volume_id, so that
    // we can terminate it with a NULL.
    //
    
    if (!strncmp(ipd.id, ISO_STANDARD_ID, strlen(ISO_STANDARD_ID))) {
      if ((volid = calloc(sizeof(ipd.volume_id) + 1, sizeof(char))) == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
      }

      strncpy(volid, ipd.volume_id, sizeof(ipd.volume_id));
      break;
    }
  }

  close(fd);

  if (!volid) {
    volid = strdup(generic_cdrom);
    if (!volid) {
      perror("malloc");
      exit(EXIT_FAILURE);
    }
  }

  // Get the position of the last byte

  p = volid + (strlen(volid) - 1);

  // Get rid of the trailing spaces
  while( p >= volid && *p == ' ' ){
    *p = '\0';
    p--;
  }

  if(fDebug)
    fprintf(stderr,"volid = [%s]\n", volid );

  Devices[drive].volumeid = volid;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
