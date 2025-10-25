#include <arpa/inet.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SERVER "127.0.0.1"
#define PORT 8888
#define BUFLEN 512
#define TIMEOUT 5000
#define MAX_RETRIES 5
#define FILENAME_PACKET 0xFF

typedef struct {
	int socket;
	struct sockaddr_in server_addr;
	FILE* file;
	char* filename;
} transfer_info_t;

void die(const char* s) {
	perror(s);
	exit(1);
}

FILE* openFile(const char* filename) {
	FILE* file = fopen(filename, "rb");
	if (file == NULL) {
		die("Error opening file");
	}
	return file;
}

int createSocket() {
	int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == -1) {
		die("Error creating socket");
	}
	return s;
}

void setupServerAddress(struct sockaddr_in* si_other) {
	memset((char*)si_other, 0, sizeof(*si_other));
	si_other->sin_family = AF_INET;
	si_other->sin_port = htons(PORT);

	if (inet_aton(SERVER, &si_other->sin_addr) == 0) {
		fprintf(stderr, "Invalid server address: %s\n", SERVER);
		exit(1);
	}
}

void setSocketTimeout(int socket) {
	struct timeval tv;
	tv.tv_sec = TIMEOUT / 1000;
	tv.tv_usec = (TIMEOUT % 1000) * 1000;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
}

int sendWithRetry(int socket, const char* data, size_t length, struct sockaddr_in* dest, int dest_len) {
	int retries = 0;
	int acknowledged = 0;

	while (!acknowledged && retries < MAX_RETRIES) {
		if (sendto(socket, data, length, 0, (struct sockaddr*)dest, dest_len) == -1) {
			die("Error sending packet");
		}

		setSocketTimeout(socket);

		char ackBuf[10];
		int recvLen = recvfrom(socket, ackBuf, sizeof(ackBuf), 0, (struct sockaddr*)dest, (socklen_t*)&dest_len);
		if (recvLen == -1) {
			retries++;
			printf("Timeout, retrying... (%d/%d)\n", retries, MAX_RETRIES);
		} else {
			acknowledged = 1;
		}
	}

	return acknowledged;
}

int sendFilename(int socket, const char* filename, struct sockaddr_in* server_addr, int slen) {
	char filenameBuf[BUFLEN];
	filenameBuf[0] = FILENAME_PACKET;
	strncpy(filenameBuf + 1, filename, BUFLEN - 1);

	size_t packet_len = strlen(filename) + 2;
	printf("Sending filename: %s\n", filename);

	if (!sendWithRetry(socket, filenameBuf, packet_len, server_addr, slen)) {
		fprintf(stderr, "Failed to send filename after maximum retries\n");
		return 0;
	}

	printf("Filename acknowledged\n");
	return 1;
}

void* sendFileThread(void* arg) {
	transfer_info_t* info = (transfer_info_t*)arg;
	char buf[BUFLEN];
	int slen = sizeof(info->server_addr);
	int finishedFile = 0;
	int packetCount = 0;

	if (!sendFilename(info->socket, info->filename, &info->server_addr, slen)) {
		pthread_exit(NULL);
	}

	while (!finishedFile) {
		size_t bytesRead = fread(buf, 1, BUFLEN, info->file);
		if (bytesRead < BUFLEN) {
			if (feof(info->file)) {
				finishedFile = 1;
			} else {
				die("Error reading from file");
			}
		}

		packetCount++;
		printf("Sending packet #%d (%zu bytes)\n", packetCount, bytesRead);

		if (!sendWithRetry(info->socket, buf, bytesRead, &info->server_addr, slen)) {
			fprintf(stderr, "Failed to receive acknowledgment after maximum retries\n");
			pthread_exit(NULL);
		}
	}

	printf("File sent successfully (%d packets)\n", packetCount);
	pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		fprintf(stderr, "Received %d arguments\n", argc - 1);
		fprintf(stderr, "Expected 1 argument: filename\n");
		exit(1);
	}

	FILE* file = openFile(argv[1]);

	int s = createSocket();

	struct sockaddr_in si_other;
	setupServerAddress(&si_other);

	transfer_info_t* info = (transfer_info_t*)malloc(sizeof(transfer_info_t));
	if (info == NULL) {
		die("Error allocating memory");
	}

	info->socket = s;
	info->server_addr = si_other;
	info->file = file;
	info->filename = basename(argv[1]);

	pthread_t thread_id;
	printf("Starting file transfer thread...\n");
	if (pthread_create(&thread_id, NULL, sendFileThread, info) != 0) {
		die("Error creating thread");
	}

	pthread_join(thread_id, NULL);

	fclose(file);
	close(s);
	free(info);

	printf("Transfer complete\n");
	return 0;
}
