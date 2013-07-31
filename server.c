#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>            // inet_aton()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define BUF_LEN 1024
#define BACK_LOG 5                // length of pending requests queue
#define MAX_CHILDREN 10           // max number of concurrent children (i.e. connections)
#define TIMEOUT 60                // max waiting time for requests from client

//#define TEST_ABORT                // sleep for 1s after each send to test ABORT command

typedef int SOCKET;

char buf[BUF_LEN];
int children;
int *pid;                         // pid of all children, to kill them all at the end

/** return -1 in case of transmission error
 *  return -2 in case of reception error (i.e. connection closed by the client)
 */
int sendFile(SOCKET s, char *name) {
	FILE *fp;
	uint32_t size, sizebuf;
	int res, byte, pid = getpid();

	fp = fopen(name, "rb");
	if (fp == NULL) return -1;

	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	
	res = send(s, "+OK\r\n", 5, 0);
	if (res != 5) {
		fprintf(stderr, "[%d] send(+OK) failed\n", pid);
		fclose(fp);
		return -1;
	}
	if (!recv(s, buf, 3, 0)) { fclose(fp); return -2; }         // ACK

	sizebuf = htonl(size);
	res = send(s, (char*)&sizebuf, 4, 0);
	if (res != 4) {
		fprintf(stderr, "[%d] send(size) failed\n", pid);
		fclose(fp);
		return -1;
	}
	if (!recv(s, buf, 3, 0)) { fclose(fp); return -2; }         // ACK
	
	fseek(fp, 0, SEEK_SET);
	
	printf("[%d] Sending file \"%s\"...\n", pid, name);
	while ((byte = fread(buf, 1, sizeof(buf), fp)) > 0) {
#ifdef TEST_ABORT
		sleep(1);	
#endif
		res = send(s, buf, byte, 0);
		if (res != byte) {
			fprintf(stderr, "[%d] send(file) failed\n", pid);
			fclose(fp);
			return -1;
		}
		if (!recv(s, buf, 3, 0)) { fclose(fp); return -2; }     // ACK
	}
	printf("[%d] Completed. %d bytes sended\n", pid, size);
	
	fclose(fp);
	return 0;
}

void serve_req(SOCKET s_conn, struct sockaddr_in caddr) {
	int res, pid = getpid();
	char command[50];
	fd_set set;
	struct timeval timeout;

	// notify the client that its request of connection has been accepted
	res = send(s_conn, "HELLO", 5, 0);
	if (res != 5) {
		fprintf(stderr, "[%d] send(HELLO) failed\n", pid);
		close(s_conn);
		return;
	}

	printf("-- [%d] Connected with %s:%d\n", pid, inet_ntoa(caddr.sin_addr), 
	       ntohs(caddr.sin_port));		

	FD_ZERO(&set);
	FD_SET(s_conn, &set);

	while (1) {
		pid = getpid();
		// waiting for a request from the connected client
		printf("-- [%d] Waiting for requests...\n", pid);		
		
		timeout.tv_sec = TIMEOUT; timeout.tv_usec = 0;
		if ((res = select(s_conn+1, &set, NULL, NULL, &timeout)) == -1) {
			fprintf(stderr, "[%d] select() failed\n", pid);
			continue;
		} else if (res == 0) {         // timeout expired
			printf("[%d] Timeout expired\n", pid);
			break;
		}
		
		res = recv(s_conn, buf, BUF_LEN, 0);
		if (res == 0) {
			printf("-- [%d] The connection has been closed\n", pid);
			break;
		} else if (res == -1) {
			fprintf(stderr, "[%d] recv(request) failed\n", pid);
			break;
		}

		// elimination of \r\n at the end of the received string
		buf[res-2] = '\0';
		sscanf(buf, "%s", command);
		if (strcmp(command, "QUIT") == 0) {
			break;
		}

		else if (strcmp(command, "GET") == 0) {
			res = sendFile(s_conn, buf+4);
			if (res == -1) {
				res = send(s_conn, "-ERR\r\n", 6, 0);
				if (res != 6) fprintf(stderr, "[%d] send(-ERR) failed\n", pid);
				if (!recv(s_conn, buf, 3, 0)) break;            // ACK
			} else if (res == -2) break;
			
		} else {
			res = send(s_conn, "-ERR\r\n", 6, 0);
			if (res != 6) fprintf(stderr, "[%d] send(-ERR) failed\n", pid);
			if (!recv(s_conn, buf, 3, 0)) break;                // ACK
		}
	}

	printf("-- [%d] Disconnected from %s:%d\n", pid, inet_ntoa(caddr.sin_addr), 
	       ntohs(caddr.sin_port));
	close(s_conn);
}

void serve_conn(SOCKET s) {
	SOCKET s_conn;
	struct sockaddr_in caddr;
	unsigned int addrlen;
	int pid = getpid();
	
	while (1) {
		// waiting for a incoming request of connection
		printf("-- [%d] Waiting for connections...\n", pid);		
		addrlen = sizeof(struct sockaddr_in);
		s_conn = accept(s, (struct sockaddr*)&caddr, &addrlen);
		if (s_conn == -1) {
			fprintf(stderr, "[%d] accept() failed\n", pid);
			continue;
		}
	
		serve_req(s_conn, caddr);
	}
}

/**
 * child_handler handles signal SIGCHLD (child terminated)
 * the "while" handles the case when a child terminates during the execution of
 * the handler itself
 */
void child_handler() {
	while(waitpid(-1, NULL, WNOHANG) > 0);
	signal(SIGCHLD, child_handler);
}

/**
 * sigint_handler handles signal SIGINT (CTRL-C)
 * send SIGTERM signal to each child
 */
void sigint_handler () {
	int i;
	
	for (i = 0; i < children; i++)
		kill(pid[i], SIGTERM);
	
	while(waitpid(-1, NULL, WNOHANG) > 0);
	exit(0);
}

int main(int argc, char*argv[]) {
	SOCKET s;
	struct sockaddr_in saddr;
	int port, res, i;

	if (argc == 3) {
		port = atoi(argv[1]);
		children = atoi(argv[2]);
		if (children > MAX_CHILDREN) {
			printf("Max %d children are allowed\n", MAX_CHILDREN);
			exit(-1);
		}
		pid = (int*)malloc(sizeof(int) * children);
	} else {
		printf("Wrong parameters; usage: %s port #children\n", argv[0]);
		exit(-1);
	}

	s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == -1) {
		fprintf(stderr, "socket() failed\n");
		exit(-1);
	}

	// server address and port
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(port);
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);

	res = bind(s, (struct sockaddr*)&saddr, sizeof(saddr));
	if (res == -1) {
		fprintf(stderr, "bind() failed\n");
		exit(-1);
	}

	res = listen(s, BACK_LOG);
	if (res == -1) {
		fprintf(stderr, "listen() failed\n");
		exit(-1);
	}
	
	signal(SIGCHLD, child_handler);
	signal(SIGINT, sigint_handler);
	setbuf(stdout, NULL);

	for (i = 0; i < children; i++) {
		pid[i] = fork();
		if (pid[i] == 0) serve_conn(s);
	}
	
	wait(NULL);	
	return 0;
}

