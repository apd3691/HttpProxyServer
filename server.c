#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define MAXLINE 4096
#define MAXLISTEN 10

/* HTTP Types */
#define HTTP1_1 1
#define HTTP1_0 0

/* Method Types */
#define GET 0
#define POST 1
#define HEAD 2

struct request
{
  int method;
  int type;
  char *resource;
  char *full_resource;
  int error;
};

struct headers
{
  char *header;
  struct headers *next;
};

void serveClient(int connfd, char *client_name, char *blacklist[], int blacklistsize );
char *readResponse( int connfd );
void splitHeaders( char *message, struct headers **output );
void freeHeaders( struct headers *input );
int getRequestInfo( char *message, struct request **info );
int establishConnection( int *sock, char *hostname );
char *findValue( struct headers *head, char *key );
int transfer( int clientfd, int serverfd, char *request_line );
int check_blacklist( char *word, char *blacklist[], int size );
void print_client_request( char *client_name, struct request *r );
void freeRequestInfo( struct request *r );

int main( int argc, char *argv[] )
{
  int listenfd, connfd;
  pid_t pid;
  short port;
  socklen_t len;
  struct sockaddr_in servaddr, cliaddr;
  struct sigaction sigact;

  if( argc < 2 )
  {
    fprintf(stderr, "Usage: %s portNumber domainsToFilter\n", argv[0]);
    return EXIT_FAILURE;
  }

  port = atoi(argv[1]);
  if( port < 1024 )
  {
    fprintf(stderr, "ERROR! portNumber should be a number and greater than 1023\n");
    return EXIT_FAILURE;
  }

  sigemptyset( &sigact.sa_mask );
  sigact.sa_flags = 0 | SA_NODEFER | SA_RESETHAND;
  sigact.sa_handler = SIG_IGN;

  sigaction( SIGCHLD, &sigact, NULL); /* To prevent zombie processes */
  //sigaction( SIGINT, &sigact, NULL);  /* To ignore keyboard interupts */

  if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 )
  {
    fprintf(stderr, "ERROR! Could not create listener socket\n");
    return EXIT_FAILURE;
  }

  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Might need to change this IP */
  servaddr.sin_port = htons(port);

  if( bind(listenfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0 )
  {
    fprintf(stderr, "ERROR! Could not bind to port %hd\n", port);
    return EXIT_FAILURE;
  }

  if( listen(listenfd, MAXLISTEN) < 0 )
  {
    fprintf(stderr, "ERROR! Problem in the listen function\n");
    return EXIT_FAILURE;
  }

  /* Create children */

  while( 1 )
  {
    len = sizeof(cliaddr);
    if( (connfd = accept(listenfd, (struct sockaddr *) &cliaddr, &len)) < 0 )
    {
      if( errno == EINTR ) /* May not need this if... */
      {
        continue;
      }
      perror("Accept()");
      return EXIT_FAILURE;
    }

    if( (pid = fork()) == 0 ) /* Child */
    {
      struct hostent *hp;
      hp = gethostbyaddr( (char *) &cliaddr.sin_addr.s_addr, sizeof(cliaddr.sin_addr.s_addr), AF_INET);
      
      serveClient( connfd, hp->h_name, argv+2, argc-2);
      close( connfd );
      exit(0);
    }
    close( connfd );
  }

  return EXIT_SUCCESS;
}

void serveClient(int connfd, char *client_name, char *blacklist[], int blacklistsize )
{
  char badResponse[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 22\r\nContent-Type: text/html\r\n\r\n<h1>403 Forbidden</h1>\r\n";
  char notImplemented[] = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 31\r\nContent-Type: text/html\r\n\r\n<h1>405 Method Not Allowed</h1>\r\n";
  char badRequest[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 24\r\nContent-Type: text/html\r\n\r\n<h1>400 Bad Request</h1>\r\n";
  char *input = NULL;
  struct headers *headers = NULL;
  int sock;
  char *hostname = NULL;
  struct request *r;
  int err = 0;

  if( (input = readResponse(connfd)) == NULL )
  {
    return;
  }

  splitHeaders(input, &headers);

  if( (err = getRequestInfo(headers->header, &r)) != 0 )
  {
    if( err == 2 )
      write(connfd, notImplemented, strlen(notImplemented));
    else
      write(connfd, badRequest, strlen(badRequest));
    freeHeaders(headers);
    free(input);
    return;
  }
 
  if( r->type == HTTP1_1 )
  {
    free(r->resource);
    r->resource = NULL;
    hostname = findValue(headers, "Host");
    if( hostname == NULL )
    {
      write( connfd, badRequest, strlen(badRequest) );
      freeRequestInfo(r);
      free(hostname);
      freeHeaders(headers);
      free(input);
      return;
    }
  }
  else if( r->type == HTTP1_0 )
  {
    hostname = r->resource;
    r->resource = NULL;
  }

  /*else
  {
    freeHeaders(headers);
    free(hostname);
    freeRequestInfo(r);
    free(input);
    write(connfd, notImplemented, strlen(notImplemented));
    return;
  }*/
  print_client_request( client_name, r );
  freeRequestInfo(r);
  
  if( check_blacklist( hostname, blacklist, blacklistsize ) )
  {
    printf("[FILTERED]\n");
    freeHeaders(headers); 
    free(input);
    free(hostname);
    write(connfd, badResponse, strlen(badResponse));
    return;
  }
  printf("\n");

  if( establishConnection( &sock, hostname ) < 0 )
  {
    write( connfd, badRequest, strlen(badRequest));
    freeHeaders(headers); 
    free(input);
    free(hostname);
    return;
  }
  free(hostname);

  transfer(connfd, sock, input);
  freeHeaders(headers);
  free(input);

  return;
}

int getRequestInfo( char *message, struct request **info )
{
  char *start = strdup(message);
  char *piece = NULL;
  char *temp = NULL;
  while( *start == ' ' ) /* Skip any leading whitespace */
    ++start;

  *info = (struct request *) malloc( sizeof(struct request));
  (*info)->resource = NULL;
  (*info)->full_resource = NULL;

  if( (piece = strtok(start, " ")) == NULL )
  {
    free(start);
    freeRequestInfo(*info);
    *info = NULL;
    return -1;
  }
  
  if( strncmp(piece, "GET", 3) == 0 )
    (*info)->method = GET;
  else if( strncmp(piece, "POST", 4) == 0 )
    (*info)->method = POST;
  else if( strncmp(piece, "HEAD", 4) == 0 )
    (*info)->method = HEAD;
  else
  {
    free(start);
    freeRequestInfo(*info);
    *info = NULL;
    return -2;
  }

  if( (piece = strtok(NULL, " ")) == NULL )
  {
    free(start);
    freeRequestInfo(*info);
    *info = NULL;
    return -1;
  }

  (*info)->full_resource = strdup(piece);
  if( (temp = strstr(piece, "://")) != NULL )
  {
    piece = temp+3;
  }
  if( (temp = strstr(piece, "/")) != NULL )
  {
    piece[temp-piece] = '\0';
  }
  (*info)->resource = strdup(piece);

  if( (piece = strtok(NULL, " ")) == NULL )
  {
    free(start);
    freeRequestInfo(*info);
    *info = NULL;
    return -1;
  }

  if( strncmp(piece, "HTTP/1.1", 8) == 0 )
  {
    (*info)->type = HTTP1_1;
  }
  else if( strncmp(piece, "HTTP/1.0", 8) == 0 )
  {
    (*info)->type = HTTP1_0;
  }
  else
  {
    free(start);
    free((*info)->full_resource);
    free((*info)->resource);
    free(*info);
    *info = NULL;
    return -1;
  }
  free(start);
  
  return 0;
}

void splitHeaders( char *message, struct headers **output )
{
  struct headers *head;
  char *messageCopy = strdup(message);
  char *header = strtok(messageCopy, "\r\n");

  (*output) = (struct headers *) malloc(sizeof(struct headers));
  (*output)->header = strdup(header);
  (*output)->next = NULL;

  head = *output;

  while( (header = strtok(NULL, "\r\n")) != NULL )
  {
    (*output)->next = (struct headers *) malloc( sizeof(struct headers) );
    *output = (*output)->next;
    (*output)->header = strdup(header);
    (*output)->next = NULL;
  }
  free(messageCopy);
  *output = head;
}

void freeHeaders( struct headers *input )
{
  struct headers *p;
  while( input != NULL )
  {
    p = input;
    input = input->next;
    free(p->header);
    free(p);
  }
  return;
}

int establishConnection( int *sock, char *hostname )
{
  struct sockaddr_in server;
  struct hostent *hp;
  int port = 80;
  if( (*sock = socket( PF_INET, SOCK_STREAM, 0 )) < 0 )
  {
    return -1;
  }

  server.sin_family = PF_INET;
  hp = gethostbyname(hostname);
  if( hp == NULL )
  {
    return -1;
  }

  bcopy( (char *)hp->h_addr, (char *) &server.sin_addr, hp->h_length );
  server.sin_port = htons(port);
  if( connect( *sock, (struct sockaddr *) &server, sizeof( server ))< 0)
  {
    return -1;
  }
  return 0; /* Successful Connection */
}

char *findValue( struct headers *head, char *key )
{
  struct headers *p = head;
  while( p != NULL )
  {
    if( strncmp(key, p->header, strlen(key)) == 0 )
    {
      char *start = strstr(p->header, ":");
      if( start == NULL )
        return NULL;
      
      ++start;
      while( *start == ' ' )
        ++start;

      return strdup(start);
    }
    p = p->next;
  }
  return NULL;
}

char *readResponse( int connfd )
{
  char *response = NULL;
  int read_count = 0;
  char buffer[MAXLINE];
  struct headers *p = NULL;

  response = (char *) malloc(1);
  response[0] = '\0';
  while( (read_count = read(connfd, buffer, MAXLINE-1)) > 0 )
  {
    /* Build new string */
    int old_length = strlen(response);
    char *temp = (char *) malloc(old_length+read_count+1);
    memcpy(temp,response,old_length);
    memcpy(temp+old_length, buffer, read_count);
    temp[old_length+read_count] = '\0';
    free(response);
    response = temp;

    /* Check for all the headers read */
    temp = strstr(response, "\r\n\r\n");
    if( temp != NULL )
    {
      break;
    }
  }
  freeHeaders(p);
  return response;
}

int transfer( int clientfd, int serverfd, char *request_line )
{
  char buffer[MAXLINE];
  int count;

  write( serverfd, request_line, strlen(request_line) );
  while( (count = recv(serverfd, buffer, MAXLINE, MSG_WAITALL)) > 0 )
  {
    write(clientfd, buffer, count);
    if(count < MAXLINE)
    {
      break;
    }
  }
  return 1;
}

int check_blacklist( char *word, char *blacklist[], int size )
{
  int i;
  for( i = 0; i < size; ++i )
  {
    char *temp = NULL;
    if(strncmp(word, blacklist[i], strlen(blacklist[i])) == 0)
    {
      return 1;
    }
    if( (temp = strstr(word,blacklist[i])) != NULL && strcmp(temp, blacklist[i]) == 0 )
    {
      return 1;
    }
  }
  return 0;
}

void print_client_request( char *client_name, struct request *r )
{
  printf("%s: ", client_name);
  if( r->method == GET )
    printf("GET ");
  else if( r->method == POST )
    printf("POST ");
  else if( r->method == HEAD )
    printf("HEAD ");
  printf("%s ", r->full_resource);
  return;
}

void freeRequestInfo( struct request *r )
{
  if( r->full_resource != NULL )
  {
    free(r->full_resource);
  }
  if( r->resource != NULL )
  {
    free(r->resource);
  }
  free(r);
  return;
}
