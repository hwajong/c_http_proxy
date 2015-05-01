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
	int is_chunked_encoding = 0;
	int is_only_1_byte_size = 0;
	char only_1_byte_size;
	int content_len = 0;
	int count_recv = 0;
	int chunked_skip_size = 0;
	int remaind = 0;

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

//		printf("****** n : %d\n", n);

		/*
		printf(">>>>>>>>>>>>>>>>>\n");
		printf("%s", buffer);
		printf(">>>>>>>>>>>>>>>>>\n");
		*/

		if(n > 0)
		{
			// 청크드 인코딩의 사이즈 읽을때 1 바이트 만 남아 1바이트 저장한 경우
			if(is_only_1_byte_size)
			{
				char temp[1024 * 32] = {0,}; // 32K
				temp[0] = only_1_byte_size;
				memcpy(temp+1, buffer, n);
				memcpy(buffer, temp, n+1);
				n++;
				is_only_1_byte_size = 1;
			}

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

				p = strstr(buffer, "Transfer-Encoding: chunked");
				if(p != NULL)
				{
					is_chunked_encoding = 1;
					printf("***chunked encoding !!\n");
				}

				body = strstr(buffer, "\r\n\r\n") + 4;
				
				nheader_data = body-buffer;
				memcpy(header_data, buffer, nheader_data);

//				printf("%s\n\n\n\n", header_data);

				// remove Transfer-Encoding field
				char* ptr = strstr(header_data, "Transfer-Encoding: chunked");
				if(ptr != NULL)
				{
					char* header_cp = malloc(header_data_size);
					memset(header_cp, 0, header_data_size);

					const char* pp = strchr(ptr, '\n');
					int size_front = ptr-header_data;
					int size_rear = nheader_data - (pp+1 - header_data);
					memcpy(header_cp, header_data, size_front);
					memcpy(header_cp+size_front, pp+1, size_rear);

					memset(header_data, 0, header_data_size);
					nheader_data = size_front + size_rear;
					memcpy(header_data, header_cp, nheader_data);

					free(header_cp);

					printf("***Remove Transfer-Encoding: chunked Field...ok\n");
//					printf("%s\n\n\n\n", header_data);
				}
			}

			// ---------------------
			// Body 처리
			// ---------------------

			//printf("%s", buffer);
			//printf("%s", body);
			if(content_len == 0)
			{
				if(is_chunked_encoding) // chunked encoding 처리 
				{
					//printf("\n********************************************************\n");
					//fwrite(buffer, 1, n, stdout);
					//printf("\n********************************************************\n");
					int chunk_size = 0;
					const char* pchunk = body;

					// 직전에 스킵 못한 만큼 버린다.
					if(chunked_skip_size > 0) 
					{
						pchunk += chunked_skip_size;
						chunked_skip_size = 0;
					}

					// 직전에 다 읽지 못한 나머지 읽어 처리
					if(remaind > 0)
					{
						if(remaind > n)
						{
//	printf("@@@@@@@@@@@@ copy 0\n");
							memcpy(body_data + nbody_data, pchunk, n);
							nbody_data += n;
							remaind -= n;
							continue;
						}

//	printf("@@@@@@@@@@@@ copy 1\n");
						memcpy(body_data + nbody_data, pchunk, remaind);
						nbody_data += remaind;
						pchunk += remaind;
						remaind = 0;

						int cur_pos = pchunk - buffer;
						if(cur_pos >= n) continue;

						//pchunk = strstr(pchunk, "\n") + 1;
						pchunk += 2;
					}

					// Loop 돌며 chunk 단위로 처리
					while(1)
					{
						// 1 바이트 만 남았을 경우 (최소 2바이트가 남아 있어야 청크 사이즈를 알수 있다.
						if(n - (pchunk - buffer) == 1)
						{
							// 한바이트 저장하고 다시 recv 받은것과 같이 처리한다.
							is_only_1_byte_size = 1;
							only_1_byte_size = *pchunk;
							break;
						}

						sscanf(pchunk, "%X", &chunk_size);
//						printf("***nrecv : %d pos : %d\n", n, pchunk - buffer);
//						printf("***chunk size : %d ( %x )\n", chunk_size, chunk_size);
						if(chunk_size == 0) break;

						if(chunk_size <= 15) pchunk += 3;
						else if(chunk_size <= 255) pchunk += 4;
						else if(chunk_size <= 4095) pchunk += 5;
						else if(chunk_size <= 65535) pchunk += 6;
						else pchunk += 7;
						//pchunk = strstr(pchunk, "\n") + 1;

						int cur_pos = pchunk - buffer;
						if(cur_pos >= n) // 청크가 짤려있을 경우
						{
							chunked_skip_size = cur_pos - n; 
							remaind = chunk_size;
							break;
						}

						if(cur_pos + chunk_size > n) // 청크가 짤려있을 경우
						{
							remaind = chunk_size;
							chunk_size = n - cur_pos;
							remaind -= chunk_size;
//	printf("@@@@@@@@@@@@ copy 2 copy size : %d\n", chunk_size);
							memcpy(body_data + nbody_data, pchunk, chunk_size);
							nbody_data += chunk_size;
							//printf("%s", pchunk);
							break;
						}
						else // 청크가 버퍼에 온전히 있을 경우
						{
							remaind = 0;
//	printf("@@@@@@@@@@@@ copy 3\n");
							memcpy(body_data + nbody_data, pchunk, chunk_size);
							nbody_data += chunk_size;
							pchunk += chunk_size;
							pchunk += 2;

							cur_pos = pchunk - buffer;
							if(cur_pos >= n)
							{
								chunked_skip_size = cur_pos - n;
								break;
							}
						}
					}
				}
				else // 청크드 인코딩이 아닌경우 
				{
//					printf("%s", body);
//	printf("@@@@@@@@@@@@ copy 4\n");
					memcpy(body_data + nbody_data, body, strlen(body));
					nbody_data += strlen(body);
				}
			}
			else // Content-Length 가 헤더에 있을 경우
			{
				int header_size = body - buffer;
				int nbody = n - header_size;

				int nwrite = nbody;
//	printf("@@@@@@@@@@@@ copy 5\n");
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










