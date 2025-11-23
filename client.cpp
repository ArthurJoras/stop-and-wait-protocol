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
#define DATA_SIZE (BUFLEN - sizeof(packet_header_t))

typedef struct {
	unsigned char seq_num;  // 0 ou 1
	unsigned short checksum;
} packet_header_t;

typedef struct {
	int socket;
	struct sockaddr_in server_addr;
	FILE* file;
	char* filename;
	unsigned char current_seq;
} transfer_info_t;

void die(const char* s) {
	perror(s);
	exit(1);
}

unsigned short calculate_checksum(const char* data, size_t length) {
	unsigned int sum = 0;
	for (size_t i = 0; i < length; i++) {
		sum += (unsigned char)data[i];
	}
	// Fold 32-bit sum to 16 bits
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (unsigned short)~sum;
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

int sendWithRetry(int socket, const char* data, size_t length, struct sockaddr_in* dest, int dest_len, unsigned char expected_seq) {
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
		} else if (recvLen > 0 && (unsigned char)ackBuf[0] == expected_seq) {
			acknowledged = 1;
		} else {
			printf("Received wrong ACK (got 0x%02X, expected 0x%02X), retrying...\n",
			       (unsigned char)ackBuf[0], expected_seq);
			retries++;
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

	if (!sendWithRetry(socket, filenameBuf, packet_len, server_addr, slen, 0xFF)) {
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

	info->current_seq = 0;

	while (!finishedFile) {
		packet_header_t* header = (packet_header_t*)buf;
		char* data_ptr = buf + sizeof(packet_header_t);

		size_t bytesRead = fread(data_ptr, 1, DATA_SIZE, info->file);
		if (bytesRead < DATA_SIZE) {
			if (feof(info->file)) {
				finishedFile = 1;
			} else {
				die("Error reading from file");
			}
		}

		header->seq_num = info->current_seq;
		header->checksum = calculate_checksum(data_ptr, bytesRead);

		packetCount++;
		printf("Sending packet #%d (seq=%d, %zu bytes)\n", packetCount, info->current_seq, bytesRead);

		size_t packet_len = sizeof(packet_header_t) + bytesRead;
		if (!sendWithRetry(info->socket, buf, packet_len, &info->server_addr, slen, info->current_seq)) {
			fprintf(stderr, "Failed to receive acknowledgment after maximum retries\n");
			pthread_exit(NULL);
		}

		info->current_seq = 1 - info->current_seq;  // Alterna entre 0 e 1
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
	info->current_seq = 0;

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
