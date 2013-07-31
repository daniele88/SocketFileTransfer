#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>            // inet_aton()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>                // getaddrinfo()

#define BUF_LEN 1024
#define BUF_CMD_LEN 1024

typedef int SOCKET;

// my_recv waits for a message and checks for errors; returns #bytes received
int my_recv(SOCKET s, char *buf, int buf_len) {
	int res = recv(s, buf, buf_len, 0);
	if (res == 0) {
		printf("The connection has been closed\n");
		close(s);
		exit(-1);
	} else if (res == -1) {
		printf("recv failed()\n");
		close(s);
		exit(-1);
	}
	send(s, "ACK", 3, 0);
	return res;
}

int main(int argc, char*argv[]) {
	SOCKET s;
	FILE *fp;
	struct sockaddr_in peer;
	struct addrinfo hints, *resA;
	int res, n, received, cmd_already_read = 0, err;
	unsigned int len;
	uint32_t filesize;
	char buf[BUF_LEN], buf_cmd[BUF_CMD_LEN], *filename, filepath[65];
	fd_set set;

	if (argc == 3) {
		memset(&hints, 0, sizeof(hints)); 
		hints.ai_family = PF_INET;
		hints.ai_socktype = SOCK_STREAM; 
		if ((res = getaddrinfo(argv[1], argv[2], &hints, &resA))) {
			printf("getaddrinfo() failed\n");
			exit(-1);
		}
	} else {
		printf("Wrong parameters; usage: %s address port\n", argv[0]);
		exit(-1);
	}
		
	s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == -1) {
		printf("socket() failed\n");
		exit(-1);
	}
	freeaddrinfo(resA);

	printf("-- Connecting with the server...\n");
	res = connect(s, resA->ai_addr, resA->ai_addrlen);
	if (res == -1) {
		printf("connect() failed\n");
		exit(-1);
	}

	// read "HELLO" if server has accepted the connection; otherwise, recv returns
	// 0 (connection closed by the server)
	res = recv(s, buf, BUF_LEN, 0);
	if (res == 0) {
		printf("The connection has been refused by the server\n");
		close(s);
		exit(-1);
	}
		
	len = sizeof(struct sockaddr_in);
	getpeername(s, (struct sockaddr*)&peer, &len);
	printf("-- Connected to %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

	setbuf(stdout, NULL);
	
	// initialize set to wait for both socket and stdin
	FD_ZERO(&set);
	FD_SET(fileno(stdin), &set);
	FD_SET(s, &set);

	while (1) {
		if (!cmd_already_read) {
			printf("Type \"GET file_name\" or \"QUIT\" to finish (\"ABORT\" "
			       "to interrupt immediately the transfer): ");
			if ((res = select(s+1, &set, NULL, NULL, NULL)) == -1) {
				fprintf(stderr, "select() failed\n");
				continue;
			} else if (FD_ISSET(s, &set)) {      // socket has been closed
				printf("Disconnected by the server due to inactivity\n");
				break;
			}
			// s has been removed from the set by the select
			FD_SET(s, &set);
				
			fgets(buf_cmd, BUF_CMD_LEN, stdin);
			n = strlen(buf_cmd);
			buf_cmd[n-1] = '\r'; buf_cmd[n] = '\n'; buf_cmd[n+1] = '\0';
		}
		cmd_already_read = 0;
		
		if (strcmp(buf_cmd, "QUIT\r\n") == 0) {
			// send QUIT message and exit
			res = send(s, buf_cmd, strlen(buf_cmd), 0);
			if (res != strlen(buf_cmd)) {
				printf("send() failed\n");
				return -1;
			}
			break;
			
		} else {
			// send GET message
			res = send(s, buf_cmd, strlen(buf_cmd), 0);
			if (res != strlen(buf_cmd)) {
				printf("send() failed\n");
				return -1;
			}
			
			strcpy(filepath, buf_cmd+4);
			filepath[n-5] = '\0';
			my_recv(s, buf, BUF_LEN);       // receive +OK or -ERR
			if (strncmp(buf, "+OK\r\n", 5) == 0) {
				my_recv(s, buf, BUF_LEN);   // receive file length
				filesize = ntohl(*(uint32_t*)buf);
				
				printf("Receiving file \"%s\"... \n", filepath);

				// extract filename from filepath
				filename = strrchr(filepath, (int)'/');
				if (filename == NULL) filename = filepath;
				else filename++;
				fp = fopen(filename, "wb");

				err = 0;
				// receive the file and write it on disk; listen also for stdin
				for (received = 0; received < filesize; ) {
					if ((res = select(s+1, &set, NULL, NULL, NULL)) == -1) {
						fprintf(stderr, "select() failed\n");
						err = 1;
						break;

					} else if (FD_ISSET(s, &set)) {
						res = my_recv(s, buf, BUF_LEN);
						fwrite(buf, 1, res, fp);
						FD_SET(fileno(stdin), &set);
						received += res;
						printf("\rBytes received: %d [%d%%]", received, 
						       (int)((float)received / filesize * 100));

					} else if (FD_ISSET(fileno(stdin), &set)) {
						fgets(buf_cmd, BUF_CMD_LEN, stdin);
						n = strlen(buf_cmd);
						buf_cmd[n-1] = '\r'; buf_cmd[n] = '\n'; buf_cmd[n+1] = '\0';

						if (strncmp(buf_cmd, "ABORT\r\n", 7) == 0) {
							err = 1;
							break;
						} else
							cmd_already_read = 1;
					
						FD_SET(s, &set);
					}
				}
				fclose(fp);

				if (err) {
					fprintf(stderr, "File transfer aborted\n");
					break;
				} else {
					printf("\rCompleted. %d bytes received\nFile \"%s\" created successfully\n", 
						   filesize, filename);
				}
				
			} else if (strncmp(buf, "-ERR\r\n", 6) == 0) {
				printf("Illegal command or file not found\n");
			}
		}
	}
	
	close(s);
	printf("-- Connection closed\n");
	return 0;
}

