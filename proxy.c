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
#include <arpa/inet.h>

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

	struct addrinfo *result;
	int error = getaddrinfo(host_name, NULL, NULL, &result);
	if(error != 0)
	{   
		fflush(stdout);
		fprintf(stderr, "\n***No such host : %s\n", host_name);
		close(sockfd_client);
		return NULL;
	}   

	printf("ok\n");

	struct sockaddr_in *serv_addr = (struct sockaddr_in  *)result->ai_addr;
	serv_addr->sin_port = htons(port);

	// Connect to server
	char server_ip[100] = {0,};
	inet_ntop(AF_INET, &serv_addr->sin_addr, server_ip, 100); 
	printf("***Connecting to server : %s...", server_ip);
	fflush(stdout);
	int sockfd_server = socket(AF_INET, SOCK_STREAM, 0); 
	if(connect(sockfd_server, (struct sockaddr *)serv_addr, sizeof(struct sockaddr_in))<0)
	{   
		perror("***Error in connecting to server\n");
		close(sockfd_server);
		close(sockfd_client);
		freeaddrinfo(result);
		return NULL;
	}   

	freeaddrinfo(result);
	printf("ok\n");

	/* Send message to server */
	int n = write(sockfd_server, req, strlen(req));
	if(n < 0)
	{
		perror("***Error writing to socket\n");
		close(sockfd_server);
		close(sockfd_client);
		return NULL;
	}

	/* Read server response */
	char buffer[1024 * 32] = {0,};
	int content_len = 0;
	int count_recv = 0;

	int header_data_size = 1024*1024;
	char* header_data = malloc(header_data_size);
	memset(header_data, 0, header_data_size);
	int nheader_data = 0;

	int body_data_size = 1024*1024*10;
	char* body_data = malloc(body_data_size);
	memset(body_data, 0, body_data_size); 
	int nbody_data = 0;

	int exist_contents_len = 0;

	do
	{
		// recieve from remote host
		memset((char*)buffer, 0, sizeof(buffer));
		n = recv(sockfd_server, buffer, sizeof(buffer), 0);

		if(n > 0)
		{
			char* body = buffer;

			// if this is the first time we are here
			// meaning: we are reading the http response header
			if(count_recv++ == 0)
			{
				// read Content-Length
				const char* p = strstr(buffer, "Content-Length:");
				if(p!= NULL)
				{
					sscanf(p, "Content-Length: %d", &content_len);
					printf("***Content-Length: %d\n", content_len);
					exist_contents_len = 1;
				}

				body = strstr(buffer, "\r\n\r\n") + 4;
				
				nheader_data = body-buffer;
				memcpy(header_data, buffer, nheader_data);

//				printf("%s\n\n\n\n", header_data);
			}

			//printf("%s", buffer);
			//printf("%s", body);
			if(content_len == 0)
			{
				memcpy(body_data + nbody_data, body, strlen(body));
				nbody_data += strlen(body);
			}
			else // Content-Length 가 헤더에 있을 경우
			{
				int header_size = body - buffer;
				int nbody = n - header_size;

				int nwrite = nbody;
				memcpy(body_data + nbody_data, body, nbody);
				nbody_data += nbody;
				content_len -= nwrite;

				if(content_len <= 0) break;
			}
		}
	} while(n > 0);

	if(!exist_contents_len) 
	{ 
		// insert Content-Lengt field 
		char* p = strstr(header_data, "\r\n\r\n");
		sprintf(p, "\nContent-Length: %d\r\n\r\n", nbody_data);
		nheader_data = strlen(header_data);
		printf("***Insert Content-Length Field...ok\n");
	}

	printf(">>>\n");
	printf("%s", header_data);
//	printf("---------------------------\n\n");
//	printf("%s", body_data);

//	printf("***** nheader_data : %d\n", nheader_data);
//	printf("***** nbody_data : %d\n", nbody_data);
	write(sockfd_client, header_data, nheader_data);
	write(sockfd_client, body_data, nbody_data);

	free(header_data);
	free(body_data);

	close(sockfd_server);
	close(sockfd_client);
	printf(">>>\n\n");
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
		pthread_create(&p_thread, NULL, do_proxy, (void *)thr_param);

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










