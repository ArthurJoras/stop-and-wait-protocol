#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFLEN 512
#define PORT 8888
#define FILENAME_PACKET 0xFF
#define RECEIVED_DIR "received_files"
#define MAX_CLIENTS 10
#define QUEUE_SIZE 100
#define DATA_SIZE (BUFLEN - sizeof(packet_header_t))

typedef struct {
	unsigned char seq_num;  // 0 ou 1
	unsigned short checksum;
} packet_header_t;
typedef struct {
	char data[BUFLEN];
	int length;
	struct sockaddr_in from_addr;
} packet_t;
typedef struct {
	packet_t queue[QUEUE_SIZE];
	int head;
	int tail;
	int count;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} packet_queue_t;
typedef struct {
	struct sockaddr_in client_addr;
	char filename[256];
	packet_queue_t* queue;
	int socket;
	bool active;
	unsigned char expected_seq;
} client_info_t;
typedef struct {
	client_info_t clients[MAX_CLIENTS];
	int num_clients;
	pthread_mutex_t mutex;
} client_manager_t;

client_manager_t client_manager = {.num_clients = 0};

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
void initQueue(packet_queue_t* queue) {
	queue->head = 0;
	queue->tail = 0;
	queue->count = 0;
	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->cond, NULL);
}
void enqueue(packet_queue_t* queue, packet_t* packet) {
	pthread_mutex_lock(&queue->mutex);

	if (queue->count < QUEUE_SIZE) {
		queue->queue[queue->tail] = *packet;
		queue->tail = (queue->tail + 1) % QUEUE_SIZE;
		queue->count++;
		pthread_cond_signal(&queue->cond);
	}

	pthread_mutex_unlock(&queue->mutex);
}
int dequeue(packet_queue_t* queue, packet_t* packet) {
	pthread_mutex_lock(&queue->mutex);

	while (queue->count == 0) {
		pthread_cond_wait(&queue->cond, &queue->mutex);
	}

	*packet = queue->queue[queue->head];
	queue->head = (queue->head + 1) % QUEUE_SIZE;
	queue->count--;

	pthread_mutex_unlock(&queue->mutex);
	return 1;
}
bool sameClient(struct sockaddr_in* addr1, struct sockaddr_in* addr2) {
	return (addr1->sin_addr.s_addr == addr2->sin_addr.s_addr &&
	        addr1->sin_port == addr2->sin_port);
}
client_info_t* findClient(struct sockaddr_in* addr) {
	pthread_mutex_lock(&client_manager.mutex);

	for (int i = 0; i < client_manager.num_clients; i++) {
		if (client_manager.clients[i].active &&
		    sameClient(&client_manager.clients[i].client_addr, addr)) {
			pthread_mutex_unlock(&client_manager.mutex);
			return &client_manager.clients[i];
		}
	}

	pthread_mutex_unlock(&client_manager.mutex);
	return NULL;
}

FILE* createAndOpenFile(const char* filename) {
	mkdir(RECEIVED_DIR, 0755);

	char filepath[512];
	char base_name[256];
	char extension[64] = "";

	const char* dot = strrchr(filename, '.');
	if (dot != NULL) {
		size_t base_len = dot - filename;
		strncpy(base_name, filename, base_len);
		base_name[base_len] = '\0';
		strncpy(extension, dot, sizeof(extension) - 1);
	} else {
		strncpy(base_name, filename, sizeof(base_name) - 1);
	}

	snprintf(filepath, sizeof(filepath), "%s/%s%s", RECEIVED_DIR, base_name, extension);

	// Verifica se arquivo existe
	int counter = 0;
	FILE* test = fopen(filepath, "r");

	while (test != NULL) {
		fclose(test);
		counter++;
		snprintf(filepath, sizeof(filepath), "%s/%s_%d%s", RECEIVED_DIR, base_name, counter, extension);
		test = fopen(filepath, "r");

		if (counter > 1000) {
			die("Too many file versions");
		}
	}

	// Cria o arquivo
	FILE* file = fopen(filepath, "wb");
	if (file == NULL) {
		perror("Error creating file");
		die("Error creating file");
	}

	printf("Created file: %s\n", filepath);
	fflush(stdout);
	return file;
}

int createSocket() {
	int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == -1) {
		die("Error creating socket");
	}
	return s;
}

void setupServerAddress(struct sockaddr_in* si_me) {
	memset((char*)si_me, 0, sizeof(*si_me));
	si_me->sin_family = AF_INET;
	si_me->sin_addr.s_addr = htonl(INADDR_ANY);
	si_me->sin_port = htons(PORT);
}

void bindSocket(int socket, struct sockaddr_in* si_me) {
	if (bind(socket, (struct sockaddr*)si_me, sizeof(*si_me)) == -1) {
		die("Error binding socket");
	}
}
void* handle_client(void* arg) {
	client_info_t* info = (client_info_t*)arg;
	packet_t packet;

	printf("[Thread %lu] Handling client %s:%d, file: %s\n",
	       pthread_self(), inet_ntoa(info->client_addr.sin_addr),
	       ntohs(info->client_addr.sin_port), info->filename);

	FILE* file = createAndOpenFile(info->filename);
	size_t totalBytesWritten = 0;
	int packetCount = 0;
	info->expected_seq = 0;

	while (true) {
		dequeue(info->queue, &packet);

		if ((unsigned char)packet.data[0] == FILENAME_PACKET) {
			continue;
		}

		packet_header_t* header = (packet_header_t*)packet.data;
		char* data_ptr = packet.data + sizeof(packet_header_t);
		size_t data_length = packet.length - sizeof(packet_header_t);

		int slen = sizeof(info->client_addr);

		// Validar checksum
		unsigned short calculated_checksum = calculate_checksum(data_ptr, data_length);
		if (calculated_checksum != header->checksum) {
			printf("[Thread %lu] Checksum error! Discarding packet\n", pthread_self());
			continue;  // Não envia ACK para forçar retransmissão
		}

		// Verificar número de sequência
		if (header->seq_num == info->expected_seq) {
			// Pacote correto e na ordem
			packetCount++;
			printf("[Thread %lu] Packet #%d (seq=%d, %zu bytes)\n",
			       pthread_self(), packetCount, header->seq_num, data_length);

			size_t bytesWritten = fwrite(data_ptr, 1, data_length, file);
			if (bytesWritten != data_length) {
				perror("Error writing to file");
				break;
			}
			totalBytesWritten += bytesWritten;

			// Envia ACK e alterna sequência esperada
			sendAck(info->socket, &info->client_addr, slen, info->expected_seq);
			info->expected_seq = 1 - info->expected_seq;  // Alterna entre 0 e 1

			if (data_length < DATA_SIZE) {
				printf("[Thread %lu] Transfer complete. Total: %zu bytes\n",
				       pthread_self(), totalBytesWritten);
				break;
			}
		} else {
			// Pacote duplicado - envia ACK mas não escreve no arquivo
			printf("[Thread %lu] Duplicate packet (seq=%d, expected=%d). Sending ACK anyway\n",
			       pthread_self(), header->seq_num, info->expected_seq);
			sendAck(info->socket, &info->client_addr, slen, header->seq_num);
		}
	}

	fclose(file);

	pthread_mutex_lock(&client_manager.mutex);
	info->active = false;
	pthread_mutex_unlock(&client_manager.mutex);

	pthread_exit(NULL);
}

void sendAck(int socket, struct sockaddr_in* dest, int dest_len, unsigned char seq_num) {
	char ack[2];
	ack[0] = seq_num;
	if (sendto(socket, ack, 1, 0, (struct sockaddr*)dest, dest_len) == -1) {
		perror("Error sending ACK for filename");
	}
}

void printClientInfo(struct sockaddr_in* client_addr, const char* filename) {
	printf("\n=== New client connected ===\n");
	printf("Client: %s:%d\n", inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
	printf("Filename: %s\n", filename);
}
client_info_t* createClient(int socket, struct sockaddr_in* addr, const char* filename) {
	pthread_mutex_lock(&client_manager.mutex);

	if (client_manager.num_clients >= MAX_CLIENTS) {
		pthread_mutex_unlock(&client_manager.mutex);
		return NULL;
	}

	client_info_t* client = &client_manager.clients[client_manager.num_clients];
	client_manager.num_clients++;

	client->client_addr = *addr;
	strncpy(client->filename, filename, sizeof(client->filename) - 1);
	client->filename[sizeof(client->filename) - 1] = '\0';
	client->socket = socket;
	client->active = true;

	client->queue = (packet_queue_t*)malloc(sizeof(packet_queue_t));
	initQueue(client->queue);

	pthread_mutex_unlock(&client_manager.mutex);

	pthread_t thread_id;
	if (pthread_create(&thread_id, NULL, handle_client, client) != 0) {
		perror("Error creating thread");
		client->active = false;
		return NULL;
	}

	pthread_detach(thread_id);
	printf("Thread created to handle client\n");
	printf("============================\n\n");

	return client;
}
void* packetDispatcher(void* arg) {
	int socket = *((int*)arg);
	char buf[BUFLEN];
	struct sockaddr_in from_addr;
	int fromlen = sizeof(from_addr);

	printf("Packet dispatcher started (socket: %d)\n", socket);
	fflush(stdout);

	while (true) {
		printf("[Dispatcher] Waiting for packet...\n");
		fflush(stdout);

		int recv_len = recvfrom(socket, buf, BUFLEN, 0,
		                        (struct sockaddr*)&from_addr, (socklen_t*)&fromlen);

		printf("[Dispatcher] recvfrom returned: %d\n", recv_len);
		fflush(stdout);

		if (recv_len == -1) {
			perror("Error receiving packet in dispatcher");
			continue;
		}

		printf("[Dispatcher] Received %d bytes from %s:%d\n",
		       recv_len, inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
		printf("[Dispatcher] First byte: 0x%02X (expected 0x%02X for filename)\n",
		       (unsigned char)buf[0], FILENAME_PACKET);
		fflush(stdout);

		if ((unsigned char)buf[0] == FILENAME_PACKET) {
			char filename[256];
			strncpy(filename, buf + 1, recv_len - 1);
			filename[recv_len - 1] = '\0';

			printClientInfo(&from_addr, filename);

			sendAck(socket, &from_addr, fromlen, 0xFF);

			createClient(socket, &from_addr, filename);
		} else {
			client_info_t* client = findClient(&from_addr);
			if (client != NULL) {
				packet_t packet;
				memcpy(packet.data, buf, recv_len);
				packet.length = recv_len;
				packet.from_addr = from_addr;
				enqueue(client->queue, &packet);
			}
		}
	}

	return NULL;
}

int main() {
	pthread_mutex_init(&client_manager.mutex, NULL);

	int s = createSocket();

	struct sockaddr_in si_me;
	setupServerAddress(&si_me);

	bindSocket(s, &si_me);

	printf("Server listening on port %d...\n", PORT);
	printf("Waiting for clients to connect...\n");
	fflush(stdout);

	// Cria thread dispatcher para receber e distribuir pacotes
	pthread_t dispatcher_thread;
	if (pthread_create(&dispatcher_thread, NULL, packetDispatcher, (void*)&s) != 0) {
		die("Error creating dispatcher thread");
	}

	// Aguarda o dispatcher (loop infinito)
	pthread_join(dispatcher_thread, NULL);

	close(s);
	return 0;
}