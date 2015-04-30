#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#define BUFSIZE 1024*8

// 서버 소켓 생성 
int create_server_socket(int port)
{
 	struct sockaddr_in serv_addr;
	int sockfd;

	bzero((char*)&serv_addr, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	// create listening socket
	sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sockfd < 0)
	{
		perror("***Failed to initialize socket");
		exit(1);
	}

	int bf = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &bf, sizeof(bf));

	// bind port
	if(bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
	{
		perror("***Failed to bind socket");
		exit(1);
	}

	// start listening
	listen(sockfd, 50);

	return sockfd;
}

// 클라이언트 요청 스트링에서 host, port 를 파싱
char* parse_host(char* buf, int* port)
{
	char* saveptr;
	char* tok;
	tok = strtok_r(buf, " \r\n", &saveptr);
	while (tok != NULL)
	{
		if (strcmp(tok, "Host:")==0)
		{
			tok = strtok_r(NULL, " \r\n", &saveptr);
			//printf("%s\n", tok);

			/* Extract the port number */
			*port = 80; /* default */
			char* hostend = strpbrk(tok, " :/\r\n\0");
			if (hostend != NULL && *hostend == ':')
			{
				*port = atoi(hostend + 1);
				*hostend = '\0';
			}
			return tok;
		}
		tok = strtok_r(NULL, " \r\n", &saveptr);
	}
	return NULL;
}

void* do_proxy(void* thr_param)
{
	int sockfd_client = *((int*)thr_param);
	free(thr_param);

	// obtain header
	char buf[BUFSIZE];
	char req[BUFSIZE];
	char req_cp[BUFSIZE];

	recv(sockfd_client, req, BUFSIZE, 0);
	printf("***Reading client request\n");
	printf("<<<\n");
	printf("%s", req);
	printf("<<<\n\n");

	// find host and port
	strcpy(req_cp, req);
	int port;
	char* host_name = parse_host(req_cp, &port);
	if(host_name == NULL)
	{
		fprintf(stderr, "***No host field\n");
		close(sockfd_client);
		return NULL;
	}
	printf("***Host name : %s\n", host_name);
	printf("***Port : %d\n", port);
	printf("***DNS retrieve...");

	struct hostent *server;
	server = gethostbyname(host_name);
	if (server == NULL)
	{
		fflush(stdout);
		fprintf(stderr, "\n***No such host : %s\n", host_name);
		close(sockfd_client);
		return NULL;
	}
	printf("ok\n");

	struct sockaddr_in serv_addr;
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	struct in_addr *pptr = (struct in_addr  *)server->h_addr;
	bcopy((char *)pptr, (char *)&serv_addr.sin_addr, server->h_length);
	serv_addr.sin_port = htons(port);

	// Connect to server
	char server_ip[100] = {0,};
	inet_ntop(AF_INET, &serv_addr.sin_addr, server_ip, 100); 
	printf("***Connecting to server : %s...", server_ip);
	fflush(stdout);
	int sockfd_server = socket(AF_INET, SOCK_STREAM, 0);
	if(connect(sockfd_server, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0)
	{
		perror("***Error in connecting to server\n");
		close(sockfd_client);
		return NULL;
	}
	
	printf("ok\n");

	/* Send message to server */
	int n = write(sockfd_server, req, strlen(req));
	if(n < 0)
	{
		perror("***Error writing to socket\n");
		close(sockfd_client);
		return NULL;
	}

	/* Read server response */
	int flag = 1;
	printf("***reading server response\n");
	printf(">>>\n");
	while(1)
	{
		char buffer[BUFSIZE] = {0,};
		int nread = read(sockfd_server, buffer, BUFSIZE);
		if(nread <= 0) break;
		
		if(flag)
		{
			printf("%s", buffer);
			flag = 1;
		}
		else 
		{
			printf(".");
		}
		int i = write(sockfd_client, buffer, nread);
	}

	close(sockfd_server);
	close(sockfd_client);
	printf("\n>>>\n");
	return NULL;
}

void main_loop(int sockfd_proxy)
{
	struct sockaddr_in client_addr;
	bzero((char*)&client_addr, sizeof(client_addr));
	int clilen = sizeof(client_addr);

	printf("***Now accept wait...\n");

	while(1)
	{
		int sockfd_client = accept(sockfd_proxy, (struct sockaddr*)&client_addr, (socklen_t *)&clilen);

		printf("***Accepted...ok\n");

		pthread_t p_thread;
		int* thr_param = malloc(sizeof(int));
		*thr_param = sockfd_client;
		int thr_id = pthread_create(&p_thread, NULL, do_proxy, (void *)thr_param);

		pthread_detach(p_thread);
	}
}

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		printf("***Usage: %s <port to bind>\n", argv[0]);
		return -1;
	}

	int port = atoi(argv[1]);
	char hostname[100] = {0,};
	gethostname(hostname, 100);

	printf("***Proxy server(%s:%d) starting...\n", hostname, port);

	int sockfd_proxy = create_server_socket(port);

	main_loop(sockfd_proxy);

	return 0;
}










