#define MAX_LABEL_SIZE 100
#define _GNU_SOURCE
#include <stdarg.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <poll.h>

int (*original_close)(int);
int (*original_dup2)(int, int);

// -----structures-----
typedef struct
{
	int source;
	int target;
	char label[MAX_LABEL_SIZE];
} Edge;

typedef struct
{
	Edge *edges;
	int num_edges;
	int num_states;
} Graph;

typedef struct
{
	char request[50];
	char response[50];
} Communication;
// -----end structures-----

// -----global variables-----
int length_request = 50;
int length_response = 50;
bool ENABLE_DEBUG = true;
FILE *debug_file;
char *RESET_COMMAND = "quit";
Communication *trace = NULL;
int size = 0;
int capacity = 0;
char filename_graph[] = "./graph.dot";
char filename_debug[] = "./debug.txt";
char filename_states_hit[] = "./states_hit.txt";
char filename_mess_sent[] = "./messages_sent.txt";
char filename_output[] = "./output.txt";
bool file_is_open = false;
bool first_response = true;
int *_states;
char *sequence_messages = NULL;
char last_response[100];
bool keep_message;
Graph *graph;
struct stat file_info;
typedef ssize_t (*sendFunc)(int, const void *, size_t, int);
typedef ssize_t (*readFunc)(int, void *, size_t);
bool first_message = true;
int current_state = 0; // Start at state 0
long int n_mess = 0;
int id_socket_request;
int id_socket_response;
FILE *errors;
// -----end variables-----

// -----functions-----
typedef int (*scanfFunc)(const char *, ...);
typedef int (*printfFunc)(const char *, ...);
int printf(const char *, ...);
int handle_response(char *);
int handle_request(char *);
Graph *parseDotFile(const char *);
void concatenateAndResize(char **, const char *);
bool reached_new_state();
// -----end functions-----

//****Core functions****

void writeError(const char *format, ...)
{
	errors = fopen("./AFLstar_errors.txt", "a");
	va_list args;
	va_start(args, format);
	vfprintf(errors, format, args);
	va_end(args);
	fclose(errors);
}

Graph *parseDotFile(const char *filename_graph)
{

	FILE *file = fopen(filename_graph, "r");
	if (file == NULL)
	{
		writeError("Error opening %s\n", filename_graph);
		return NULL;
	}
	char line[256];
	int max_state = -1; // To keep track of the maximum state number encountered
	while (fgets(line, sizeof(line), file))
	{
		if (strstr(line, "digraph"))
		{
			graph = (Graph *)malloc(sizeof(Graph));
			graph->num_edges = 0;
			graph->edges = NULL;

			while (fgets(line, sizeof(line), file))
			{
				if (strstr(line, "}"))
					break;
				if (strstr(line, "->"))
				{
					Edge edge;
					sscanf(line, "%d -> %d [label = \"%[^\"]\"]", &edge.source, &edge.target, edge.label);
					graph->edges = (Edge *)realloc(graph->edges, (graph->num_edges + 1) * sizeof(Edge));
					graph->edges[graph->num_edges] = edge;
					graph->num_edges++;
					if (edge.source > max_state)
						max_state = edge.source;
					if (edge.target > max_state)
						max_state = edge.target;
				}
			}
			// I also consider the initial state
			graph->num_states = max_state + 1;
			fclose(file);
			return graph;
		}
	}
	fclose(file);
	return NULL;
}
bool reached_new_state(char *messages)
{
	char *token;
	int messages_length = strlen(messages);
	// Create a new vector to store the copied content
	char *copied_messages = (char *)malloc((messages_length + 1) * sizeof(char));
	// Copy the content of messages into the new vector
	strcpy(copied_messages, messages);
	token = strtok(copied_messages, "\n");
	current_state = 0; // Start at state 0
	while (token != NULL)
	{
		int i;
		int found = 0;
		// Check if the current state has an outgoing edge with the label
		for (i = 0; i < graph->num_edges; i++)
		{
			Edge edge = graph->edges[i];
			if ((edge.source == current_state && strncmp(edge.label, token, strlen(edge.label)) == 0))
			{
				current_state = edge.target;
				found = 1;
				break;
			}
		}
		if (!found)
		{
			// trimming the last message from the sequence since it isn't interesting
			sequence_messages[strlen(sequence_messages) - strlen(token) - 1] = '\0';
			return 0;
		}
		token = strtok(NULL, "\n");
	}
	// check if it's a new state to update the debug file
	if (_states[current_state] == 1)
	{
		fprintf(debug_file, "Trace:\n---\n%s\n---\n\n", sequence_messages);
	}
	return 0;
}
void concatenateAndResize(char **destination, const char *source)
{
	// Check if destination is NULL or not allocated
	if (*destination == NULL)
	{
		*destination = malloc(strlen(source) + 1);
		if (*destination == NULL)
		{
			writeError("Memory allocation failed. Exiting.\n");
			exit(EXIT_FAILURE);
		}
		strcpy(*destination, source);
	}
	else
	{
		size_t dest_len = strlen(*destination);
		size_t source_len = strlen(source);
		size_t total_len = dest_len + source_len + 1; // +1 for the null-terminator
		// Check if enough space in the destination string
		if (total_len > dest_len + 1)
		{
			// Double the size of the destination string
			size_t new_size = 2 * total_len;
			char *temp = (char *)realloc(*destination, new_size);
			if (temp == NULL)
			{
				writeError("Memory allocation failed. Exiting.\n");
				exit(EXIT_FAILURE);
			}
			*destination = temp;
		}
		strcat(*destination, source);
	}
}
int handle_request(char *request)
{

	debug_file = fopen(filename_debug, "a");
	if (debug_file == NULL)
	{
		writeError("Error opening %s\n", filename_debug);
		return 1;
	}
	// here need to read the .dot file and the hit_state for the first time
	if (!file_is_open)
	{
		file_is_open = true;
		// Parse the DOT file
		graph = parseDotFile(filename_graph);
		if (graph == NULL)
		{
			writeError("Error opening %s\n", filename_graph);
			return 1;
		}

		FILE *states_file = fopen(filename_states_hit, "r");
		if (states_file == NULL)
		{
			writeError("Error opening %s\n", filename_states_hit);
			return 1;
		}
		else
		{
			printf("Loading states-hit file...\n");
			int i = 0;
			char buffer[100];
			size_t arraySize = 0;
			while (fgets(buffer, sizeof(buffer), states_file) != NULL)
			{
				arraySize++;
				_states = (int *)realloc(_states, arraySize * sizeof(int));
				_states[i] = atoi(buffer);
				i++;
			}
		}

		FILE *messages_sent_file = fopen(filename_mess_sent, "r");
		if (messages_sent_file == NULL)
		{
			writeError("Error opening %s\n", filename_mess_sent);
			return 1;
		}
		else
		{
			printf("Loading messages sent file...\n");
			fscanf(messages_sent_file, "%ld", &n_mess);
		}
		fclose(states_file);
		fclose(messages_sent_file);
	}

	// Check if the vector is full and resize if needed
	// ATTENTION HERE, size of requests and responses is static (50 char)
	if (size >= capacity)
	{
		capacity = (capacity == 0) ? 1 : capacity * 2;
		trace = (Communication *)realloc(trace, capacity * sizeof(Communication));
		if (trace == NULL)
		{
			writeError("Failed to allocate memory");
			return 1;
		}
	}

	if (strncmp(RESET_COMMAND, request, strlen(RESET_COMMAND)) == 0)
	{
		sequence_messages = NULL;
		first_message = true;
		current_state = 0;
	}

	else
	{
		strncpy(trace[size].request, request, length_request);
	}
	fclose(debug_file);
}
bool check_response(char *response)
{
	if (first_response)
	{
		first_response = false;
		keep_message = true;
		strcpy(last_response, " ");
		return true;
	}
	else
	{
		if (strcmp(last_response, response) != 0 || strcmp(last_response, " ") == 0)
		{
			return true;
		}
		else
		{
			return false;
		}
	}
}
int handle_response(char *response)
{

	n_mess++;
	int len_message;
	// Check if the vector is full and resize if needed
	if (size >= capacity)
	{
		capacity = (capacity == 0) ? 1 : capacity * 2;
		trace = (Communication *)realloc(trace, capacity * sizeof(Communication));
		if (trace == NULL)
		{
			writeError("Failed to allocate memory");
			return 1;
		}
	}
	debug_file = fopen(filename_debug, "a");
	if (debug_file == NULL)
	{
		writeError("Error opening %s\n", filename_debug);
		return 1;
	}
	keep_message = check_response(response);
	if (keep_message == true)
	{
		bool valid = false;
		// This message triggers a new response, still have to see if it triggers a new branch
		for (int i = 0; i < graph->num_edges; i++)
		{
			Edge edge = graph->edges[i];
			if (strncmp(edge.label, trace[size].request, strlen(edge.label)) == 0)
			{
				// the message exists
				valid = true;
				break;
			}
		}
		if (valid)
		{

			valid = false;
			if (sequence_messages == NULL)
			{
				len_message = 0;
				sequence_messages = (char *)realloc(sequence_messages, 2);
				strcpy(sequence_messages, "");
			}
			else
			{
				len_message = strlen(sequence_messages);
			}

			sequence_messages = (char *)realloc(sequence_messages, strlen(trace[size].request) + len_message + 5);

			if (sequence_messages == NULL)
			{
				printf("Failed to allocate new_sequence_messages\n");
			}

			if (trace[size].request[strlen(trace[size].request) - 1] != '\n')
			{
				strcat(trace[size].request, "\n");
			}
			strcat(sequence_messages, trace[size].request);
			// I can finally controll the state model
			reached_new_state(sequence_messages);
		}
	}
	strncpy(trace[size].response, response, length_response);
	strncpy(last_response, trace[size].response, length_response);

	if (ENABLE_DEBUG)
		fprintf(debug_file, "%d:Request(id:%d): %s\n%d:Response(id:%d): %s\nTrace:\n---\n%s\n---\n\n", size, id_socket_request, trace[size].request, size, id_socket_response, trace[size].response, sequence_messages);

	size++;
	fclose(debug_file);
	// first of all I save the hit
	_states[current_state] += 1;
	FILE *states_file = fopen(filename_states_hit, "w");
	if (states_file == NULL)
	{
		writeError("Error opening %s\n", filename_states_hit);
		return 1;
	}
	else
	{
		for (int i = 0; i < graph->num_states; i++)
		{
			fprintf(states_file, "%d\n", _states[i]);
		}
	}
	// I need to update the output file
	int states_covered = 0;
	for (int i = 0; i < graph->num_states; i++)
	{
		if (_states[i] > 0)
		{
			states_covered++;
		}
	}
	double perc = (double)(states_covered) / graph->num_states * 100;
	FILE *output_file = fopen(filename_output, "w");
	if (output_file == NULL)
	{
		writeError("Error opening %s\n", filename_output);
		return 1;
	}
	else
	{
		fprintf(output_file, "Total states discovered: %d \\ %d. State coverage: %.0f%%. Messages sent: %ld\n", states_covered, graph->num_states, perc, n_mess);
		for (int i = 0; i < graph->num_states; i++)
		{
			fprintf(output_file, "State: %d - Hit: %d\n", i, _states[i]);
		}
	}
	FILE *file_mess_sent = fopen(filename_mess_sent, "w");
	if (file_mess_sent == NULL)
	{
		writeError("Error opening %s\n", filename_mess_sent);
		return 1;
	}
	else
	{
		fprintf(file_mess_sent, "%ld\n", n_mess);
	}
	fclose(output_file);
	fclose(states_file);
	fclose(file_mess_sent);
	return 0;
}
//****End core functions****

//****Handlers****
int scanf(const char *, ...);

int scanf(const char *format, ...)
{
	va_list args;
	scanfFunc original_scanf;
	// Load the C library containing the original scanf function
	void *handle = dlopen("libc.so.6", RTLD_LAZY);
	if (handle == NULL)
	{
		// Handle error if the library fails to load
		writeError("Failed to load C library: %s\n", dlerror());
		return -1; // or handle the error in an appropriate manner
	}
	// Get a reference to the original scanf function
	original_scanf = (scanfFunc)dlsym(handle, "scanf");
	// Create a buffer to store the input
	char buffer[100];
	// Read from standard input directly
	fgets(buffer, sizeof(buffer), stdin);
	size_t buffer_len = strlen(buffer);
	if (buffer_len > 0 && buffer[buffer_len - 1] == '\n')
	{
		buffer[buffer_len - 1] = '\0';
		buffer_len--; // Decrease the length by 1 since we removed the newline character
	}
	// Call the original scanf function
	va_start(args, format);
	int result = vsscanf(buffer, format, args);
	va_end(args);

	handle_request(buffer);
	// Close the library handle
	dlclose(handle);

	return result;
}

typedef ssize_t (*recvFunc)(int sockfd, void *buf, size_t len, int flags);

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	static recvFunc original_recv = NULL;
	if (original_recv == NULL)
	{
		original_recv = (recvFunc)dlsym(RTLD_NEXT, "recv");
		if (original_recv == NULL)
		{
			writeError("Failed to get original recv function: %s\n", dlerror());
			return -1;
		}
	}

	ssize_t bytes_received = original_recv(sockfd, buf, len, flags);
	// Handling the requests
	id_socket_request = sockfd;
	handle_request(buf);
	return bytes_received;
}

typedef char *(*orig_gets_t)(char *);

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	static sendFunc original_send = NULL;
	if (original_send == NULL)
	{
		// Load the C library containing the original send function
		void *handle = dlopen("libc.so.6", RTLD_LAZY);
		if (handle == NULL)
		{
			writeError("Failed to load C library: %s\n", dlerror());
			return -1;
		}

		// Get a reference to the original send function
		original_send = (sendFunc)dlsym(handle, "send");

		if (original_send == NULL)
		{
			writeError("Failed to find original send function: %s\n", dlerror());
			dlclose(handle);
			return -1;
		}
	}

	int result = original_send(sockfd, buf, len, flags);

	if (result < 10)
	{
		result = length_response - 5;
	}
	if (!first_message)
	{
		char *buffer = malloc(result + 1); // Allocate enough space for null-terminator
		strncpy(buffer, buf, result);
		buffer[result] = '\0'; // Add null-terminator
		id_socket_response = sockfd;
		handle_response(buffer);
		free(buffer);
	}
	first_message = false;

	return result;
}

//****End Handlers****
