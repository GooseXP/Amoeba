#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

// Set various buffer sizes and constants used throughout the program
#define LINEBUFFER 100			// Max number of tokens per observation line
#define WRDBUFFER 100			// Max length of a single word
#define SRCHMIN 1				// Minimum percentage to search database
#define SRCHMAX 100				// Maximum percentage to search database
#define CMDMAX 10				// Maximum number of arguments (words) in a generated command
#define CMDMIN 1				// Minimum number of arguments (words) in a generated command
#define RUNTIME 10				// Child process allowed runtime in seconds
#define REWARD 10				// Reward value when new data is learned
#define PENALTY 1				// Penalty value when redundant data is observed
#define MAX_THREADS 8			// Maximum number of concurrent threads
#define COMMANDS_PER_THREAD 2	// Number of commands which will be executed in each worker thread
#define TREND_WINDOW_SIZE 10	// Number of recent lrnval entries to consider for moving average

// Semaphore to limit the number of concurrent threads
sem_t thread_sem;

// LearningTrendTracker structure to maintain a moving average of lrnval
typedef struct {
	int window_size;		// Number of recent lrnval entries to consider
	int* lrnvals;			// Array to store recent lrnval values
	int index;				// Current index in the circular buffer
	int count;				// Number of lrnval entries added
	double moving_average;	// Current moving average of lrnval
	pthread_mutex_t mutex;	// Mutex to protect access to the structure
} LearningTrendTracker;

// DatabaseStruct holds the learned words, their values, and entries_ptr
typedef struct {
	// token: an array of strings representing all known words
	// Example: token[0] might be "ls", token[1] might be "cat", etc.
	char** token;

	// value: a 4D array used to represent associations and 'value' scores between words in commands
	// value[i][j][k][l] is an int representing the value of relationship between word i at position j and word k at position l.
	// i and k: indices of words in token[]
	// j and l: indices of positions in the command
	int**** value;

	// numWords: how many words are currently in the database
	size_t numWords;
	pthread_mutex_t mutex;
} Words;

typedef struct {
	// entries: a 2D array of observed word indices representing lines of output from executed commands
	// Each entries[i] is an int array representing a line of output tokenized into word indices found in token[].
	// entries[i][j] = index of word in token[]
	// terminated by -1 to indicate end
	int** entries;
	// numObservations: how many lines of output data we have stored
	size_t numObservations;
	pthread_mutex_t mutex;
} Observations;

typedef struct {
	int length;
	int scope;
	pthread_mutex_t mutex;
} CommandSettings;

// Structure to pass data to threads
typedef struct {
	Words* words;
	Observations* observations;
	CommandSettings* settings;
	LearningTrendTracker* tracker; // Added pointer to LearningTrendTracker
} ThreadData;

// Flag to indicate if termination is requested
volatile sig_atomic_t termination_requested = 0;

// Signal handler to set termination_requested flag if SIGINT or SIGTERM is received
void signal_handler(int signum) {
	if (signum == SIGINT || signum == SIGTERM) {
		termination_requested = 1;
	}
}

// Function: reallocate_words
// This function expands the token and value arrays to accommodate one new word.
// It also initializes all the associated value dimensions for that new word.
void reallocate_words(Words* words, int wordLength) {
	char*** token = &(words->token);
	int***** value = &(words->value);
	size_t* old_size_ptr  = &(words->numWords); 
	size_t new_size = *old_size_ptr + 1;

	if (*old_size_ptr == 0) {
		// If this is the first word:
		// Allocate token array with space for 1 word.
		*token = malloc(sizeof(char*));
		if (!*token) {
			fprintf(stderr, "Failed to allocate memory for tokens\n");
			exit(EXIT_FAILURE);
		}

		// Allocate space for that 1 word
		(*token)[0] = malloc(wordLength + 1);
		if (!(*token)[0]) {
			fprintf(stderr, "Failed to allocate memory for first token\n");
			exit(EXIT_FAILURE);
		}

		// Allocate the value structure:
		// value is int****, so we allocate at each dimension:
		// (*value) is an array of size new_size of int*** pointers
		*value = calloc(new_size, sizeof(int***));
		if (!*value) {
			fprintf(stderr, "Failed to allocate memory for value array\n");
			exit(EXIT_FAILURE);
		}

		// Iterate through each newly allocated dimension to initialize
		for(size_t i = 0; i < new_size; i++){
			(*value)[i] = calloc(CMDMAX, sizeof(int**));
			if (!(*value)[i]) {
				fprintf(stderr, "Failed to allocate memory for value[%zu].\n", i);
				exit(EXIT_FAILURE);
			}
			for(int j = 0; j < CMDMAX; j++){
				(*value)[i][j] = calloc(new_size, sizeof(int*));
				if (!(*value)[i][j]) {
					fprintf(stderr, "Failed to allocate memory for value[%zu][%d].\n", i, j);
					exit(EXIT_FAILURE);
				}
				for(size_t k = 0; k < new_size; k++){
					(*value)[i][j][k] = calloc(CMDMAX, sizeof(int));
					if (!(*value)[i][j][k]) {
						fprintf(stderr, "Failed to allocate memory for value[%zu][%d][%zu].\n", i, j, k);
						exit(EXIT_FAILURE);
					}
				}
			}
		}
	} else {
		// If we already have words and we need to add one more:
		// Increase token array size to hold one more word
		char** temp_token = realloc(*token, sizeof(char*) * new_size);
		if (!temp_token) {
			fprintf(stderr, "Failed to realloc memory for tokens\n");
			exit(EXIT_FAILURE);
		}
		*token = temp_token;

		// Place the new word at index new_size-1 (zero-based indexing)
		(*token)[new_size - 1] = malloc(wordLength + 1);
		if (!(*token)[new_size - 1]) {
			fprintf(stderr, "Failed to allocate memory for new token\n");
			exit(EXIT_FAILURE);
		}

		// Reallocate 'value' to hold one more word dimension:
		int**** temp_value = realloc(*value, new_size * sizeof(int***));
		if (!temp_value) {
			fprintf(stderr, "Failed to realloc memory for value array\n");
			exit(EXIT_FAILURE);
		}
		*value = temp_value;

		// Now we must expand the existing arrays to accommodate the new dimensions
		for (size_t i = 0; i < new_size; i++) {
			// If this 'row' is new (i.e., i >= *old_size_ptr), allocate from scratch
			if (i >= *old_size_ptr) {
				(*value)[i] = calloc(CMDMAX, sizeof(int**));
				if (!(*value)[i]) {
					fprintf(stderr, "Failed to allocate memory for new value[%zu].\n", i);
					exit(EXIT_FAILURE);
				}
			}
			for (int j = 0; j < CMDMAX; j++) {
				if (i >= *old_size_ptr) {
					// If new row in dimension i, allocate space for new_size 'k' dimension
					(*value)[i][j] = calloc(new_size, sizeof(int*));
					if (!(*value)[i][j]) {
						fprintf(stderr, "Failed to allocate memory for new value[%zu][%d].\n", i, j);
						exit(EXIT_FAILURE);
					}
				} else {
					// If row existed before, we need to reallocate it
					int** temp_j = realloc((*value)[i][j], new_size * sizeof(int*));
					if (!temp_j) {
						fprintf(stderr, "Failed to realloc memory for value[%zu][%d].\n", i, j);
						exit(EXIT_FAILURE);
					}
					(*value)[i][j] = temp_j;
				}
				for (size_t k = 0; k < new_size; k++) {
					// If 'k' or 'i' is beyond old_size_ptr, we need to calloc a new dimension
					if (i >= *old_size_ptr || k >= *old_size_ptr) {
						(*value)[i][j][k] = calloc(CMDMAX, sizeof(int));
						if (!(*value)[i][j][k]) {
							fprintf(stderr, "Failed to allocate memory for value[%zu][%d][%zu].\n", i, j, k);
							exit(EXIT_FAILURE);
						}
					}
				}
			}
		}
	}

	// Update the word count to reflect the new word
	*old_size_ptr = new_size;
}

// Function: reallocate_observations
// This function expands the observation array to accommodate one more line of entries_ptr
// Each line of entries_ptr is an array of int indices which are word tokens.
void reallocate_observations(Observations* observations, int observationLength) {
	int*** entries_ptr = &(observations->entries);
	size_t* old_size_ptr = &(observations->numObservations);
	size_t new_size = *old_size_ptr + 1; 

	if (*old_size_ptr == 0) {
		// No entries_ptr before, allocate the first line
		*entries_ptr = malloc(sizeof(int*) * new_size);
		if (!*entries_ptr) {
			fprintf(stderr, "Failed to allocate memory for entries_ptr\n");
			exit(EXIT_FAILURE);
		}
		(*entries_ptr)[0] = malloc(sizeof(int)*(observationLength + 2));
		if (!(*entries_ptr)[0]) {
			fprintf(stderr, "Failed to allocate memory for first observation\n");
			exit(EXIT_FAILURE);
		}
		// Initialize to -1
		for(int i = 0; i < observationLength + 2; i++) {
			(*entries_ptr)[0][i] = -1;
		}
	} else {
		// Already have entries_ptr, so reallocate for one more line
		int** temp_observation = realloc(*entries_ptr, sizeof(int*) * new_size);
		if (!temp_observation) {
			fprintf(stderr, "Failed to realloc memory for entries_ptr\n");
			exit(EXIT_FAILURE);
		}
		*entries_ptr = temp_observation;
		(*entries_ptr)[new_size - 1] = malloc(sizeof(int)*(observationLength + 2));
		if (!(*entries_ptr)[new_size - 1]) {
			fprintf(stderr, "Failed to allocate memory for new observation\n");
			exit(EXIT_FAILURE);
		}
		// Initialize to -1
		for(int i = 0; i < observationLength + 2; i++) {
			(*entries_ptr)[new_size - 1][i] = -1;
		}
	}
	// Increment observation count
	*old_size_ptr = new_size;
}

// Function: init
// Initializes the database by scanning common Linux commands from /bin and /sbin
// and adding them as words in the database.
void init(Words* words) {
	words->numWords = 0;
	char*** token = &(words->token);
	int***** value = &(words->value); // Corrected pointer type
	size_t* numWords = &(words->numWords);

	int chridx = 0;
	char word[WRDBUFFER];
	memset(word, 0, sizeof(word));

	// Get a list of commands by running "ls /bin && ls /sbin"
	FILE* cmdfile = popen("ls /bin && ls /sbin", "r");
	if(!cmdfile) {
		fprintf(stderr, "popen failed\n");
		exit(EXIT_FAILURE);
	}

	int c;
	// Read characters from the pipe and split by space/newline
	while ((c = fgetc(cmdfile)) != EOF) {
		if (c != ' ' && c != '\n') {
			// Build up word until space or newline
			if (chridx < WRDBUFFER - 1) {
				word[chridx++] = (char)c;
			} else {
				// Word too long, truncate safely
				word[chridx] = '\0';
				// We'll still treat this truncated word as valid
				chridx = 0;
			}
		} else {
			// We hit a space or newline, which means we have a complete word
			if (chridx > 0) {
				word[chridx] = '\0'; // Null-terminate
				// Check if word is already known
				int redundantWord = 0;
				for (size_t i = 0; i < *numWords; i++) {
					if ((*token)[i] && strcmp((*token)[i], word) == 0) {
						redundantWord = 1;
						break;
					}
				}
				if (!redundantWord) {
					// If new word, reallocate arrays to add this word
					reallocate_words(words, (int)strlen(word));
					// Copy the new word into token array at index numWords-1
					strcpy((*token)[*numWords - 1], word);

					// Initialize values for this new word
					for (int i = 0; i < CMDMAX; i++) {
						for (size_t j = 0; j < *numWords; j++) {
							for (int k =0; k < CMDMAX; k++) {
								if ((*numWords - 1) >= words->numWords || i >= CMDMAX || j >= words->numWords || k >= CMDMAX) {
									fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
									continue;
								}
								(*value)[*numWords - 1][i][j][k] = 0;
							}
						}
					}
				}
				// Reset chridx for next word
				chridx = 0;
			}
		}
	}
	pclose(cmdfile);
}

// Function: write_database
// Writes the current tokens, values, and entries_ptr to disk.
// tokens.txt for words
// values.csv for the 4D values
// observations.csv for the observation lines
void write_database(Words* words, Observations* observations) {
	pthread_mutex_lock(&words->mutex);
	pthread_mutex_lock(&observations->mutex);

	FILE* tokenFile = fopen("tokens.txt", "w");
	FILE* valueFile = fopen("values.csv", "w");
	FILE* observationFile = fopen("observations.csv", "w");
	if (!tokenFile || !valueFile || !observationFile) {
		fprintf(stderr,"Failed to open files for writing.\n");
		if(tokenFile) fclose(tokenFile);
		if(valueFile) fclose(valueFile);
		if(observationFile) fclose(observationFile);
		return;
	}

	char*** token_ptr = &(words->token);
	int***** value_ptr = &(words->value); // Corrected pointer type
	int*** entries_ptr = &(observations->entries);
	size_t* numObservations_ptr = &(observations->numObservations);
	size_t* numWords_ptr = &(words->numWords);

	// Write each token on its own line in tokens.txt
	for (size_t i = 0; i < *numWords_ptr; i++) {
		fprintf(tokenFile, "%s\n", (*token_ptr)[i]);
	}

	// Write values to values.csv
	// For each word i:
	//   For each command position j:
	//	 For each word k:
	//	   For each command position l:
	// Write value[i][j][k][l] separated by commas and spaces
	for (size_t i = 0; i < *numWords_ptr; i++) {
		for (int j = 0; j < CMDMAX; j++) {
			for (size_t k = 0; k < *numWords_ptr; k++) {
				for (int l = 0; l < CMDMAX; l++) {
					fprintf(valueFile, "%d,", (*value_ptr)[i][j][k][l]);
				}
			}
			fprintf(valueFile, " ");
		}
		fprintf(valueFile, "\n");
	}

	// Write entries_ptr to observations.csv
	// Each observation line ends with -1
	for (size_t i = 0; i < *numObservations_ptr; i++) {
		int idx = 0;
		while ((*entries_ptr)[i][idx] != -1) {
			fprintf(observationFile, "%d,", (*entries_ptr)[i][idx]);
			idx++;
		}
		fprintf(observationFile, "\n");
	}

	fclose(tokenFile);
	fclose(valueFile);
	fclose(observationFile);

	pthread_mutex_unlock(&words->mutex);
	pthread_mutex_unlock(&observations->mutex);
}

// Function: load_database
// Attempts to load tokens, values, and entries_ptr from disk.
// If not found, returns 1 to indicate we need to init from scratch.
// Otherwise, loads everything into memory.
int load_database(Words* words, Observations* observations) {
	FILE* tokenFile = fopen("tokens.txt", "r");
	FILE* valueFile = fopen("values.csv", "r");
	FILE* observationFile = fopen("observations.csv", "r");

	char*** token_ptr = &(words->token);
	int***** value_ptr = &(words->value); // Corrected pointer type
	int*** entries_ptr = &(observations->entries);
	size_t* numObservations_ptr = &(observations->numObservations);
	size_t* numWords_ptr = &(words->numWords);

	*numWords_ptr = 0;
	*numObservations_ptr = 0;

	if (!tokenFile || !valueFile) {
		if(tokenFile) fclose(tokenFile);
		if(valueFile) fclose(valueFile);
		if(observationFile) fclose(observationFile);
		return 1;
	}
	printf("Loading Database please wait...\n");

	// First load all tokens to count them
	char word[WRDBUFFER];
	while (fgets(word, sizeof(word), tokenFile)) {
		word[strcspn(word, "\n")] = '\0';
		// Reallocate arrays to add this word
		reallocate_words(words, (int)strlen(word));
		// Copy the new word into token array at index numWords-1
		strcpy((*token_ptr)[*numWords_ptr - 1], word);

		// Initialize values for this new word
		for (int i = 0; i < CMDMAX; i++) {
			for (size_t j = 0; j < *numWords_ptr; j++) {
				for (int k =0; k < CMDMAX; k++) {
					if ((*numWords_ptr - 1) >= words->numWords || i >= CMDMAX || j >= words->numWords || k >= CMDMAX) {
						fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
						continue;
					}
					(*value_ptr)[*numWords_ptr - 1][i][j][k] = 0;
				}
			}
		} 
	}

	// Now, we have loaded all words (numWords known)
	// But we need to reload them carefully to read values correctly.
	// We'll close and re-open tokenFile:
	fclose(tokenFile);
	fseek(valueFile, 0, SEEK_SET); // ensure at start of value file
	tokenFile = fopen("tokens.txt","r");
	if (!tokenFile) {
		fprintf(stderr, "Failed to reopen tokens.txt for loading values.\n");
		fclose(valueFile);
		if(observationFile) fclose(observationFile);
		return 1;
	}
	*numWords_ptr = 0; // reset and reload to align with reading values
	while (fgets(word, sizeof(word), tokenFile)) {
		word[strcspn(word, "\n")] = '\0';
		reallocate_words(words, (int) strlen(word));
		strcpy((*token_ptr)[*numWords_ptr - 1], word);

		// For the newly added word, read corresponding values from valueFile
		for (int j = 0; j < CMDMAX; j++) {
			for (size_t k = 0; k < *numWords_ptr; k++) {
				for (int l = 0; l < CMDMAX; l++) {
					if (fscanf(valueFile, "%d,", &((*value_ptr)[*numWords_ptr - 1][j][k][l])) != 1) {
						fprintf(stderr, "Failed to read value[%zu][%d][%zu][%d].\n", *numWords_ptr - 1, j, k, l);
						fclose(tokenFile);
						fclose(valueFile);
						if(observationFile) fclose(observationFile);
						return 1;
					}
				}
			}
			// After reading CMDMAX values for a word, skip the space
			fscanf(valueFile, " ");
		}
		// After each word's values, skip the newline
		fscanf(valueFile, "\n");
	}

	fclose(tokenFile);
	fclose(valueFile);

	// Load entries_ptr if any
	if (observationFile) {
		char line[LINEBUFFER*4];
		while (fgets(line, sizeof(line), observationFile)) {
			int observation_line[LINEBUFFER];
			int index = 0;
			char* ptr = strtok(line, ",");
			while(ptr && index < LINEBUFFER - 1) {
				observation_line[index++] = atoi(ptr);
				ptr = strtok(NULL, ",");
			}
			observation_line[index] = -1;
			reallocate_observations(observations, index);
			size_t obsIndex = *numObservations_ptr - 1;
			for(int i = 0; i <= index; i++){
				(*entries_ptr)[obsIndex][i] = observation_line[i];
			}
		}
		fclose(observationFile);
	}
	return 0;
}

// Function: check_child_status
// Waits for the child process to finish or kills it after RUNTIME seconds.
// If child is not responding, tries SIGTERM, then SIGKILL, up to 3 attempts.
int check_child_status(pid_t child_pid) {
	int status;
	time_t start_time, current_time;
	time(&start_time);
	int kill_attempts = 0;

	while (1) {
		pid_t wpid = waitpid(child_pid, &status, WNOHANG);
		time(&current_time);
		int proc_time = (int)difftime(current_time, start_time);

		if (proc_time >= RUNTIME) {
			// Child ran too long, try to kill it
			if (kill_attempts == 0) {
				if (kill(child_pid, SIGTERM) != 0) {
					printf("Timeout reached sending SIGTERM\n");
				}
			} else if (kill_attempts == 1) {
				if (kill(child_pid, SIGKILL) != 0) {
					printf("SIGTERM didn't work trying SIGKILL\n");
				}
			} else if (kill_attempts == 2) {
				fprintf(stderr, "Failed to terminate the child process %d.\n", child_pid);
				child_pid = 0;
				return 1;
			}
			sleep(1);
			kill_attempts++;
		} else if (wpid == child_pid) {
			// wpid matches child_pid, so child changed state
			if (WIFEXITED(status)) {
				child_pid = 0;
				return 0;
			}
			if (WIFSIGNALED(status)) {
				child_pid = 0;
				return 0;
			}
		} else if (wpid < 0) {
			fprintf(stderr, "waitpid");
			child_pid = 0;
			return 1;
		}
		// Short sleep to prevent busy waiting
		usleep(100000); // 100 ms
	}
}

void initialize_mutexes(Words* words, Observations* observations, CommandSettings* settings, LearningTrendTracker* tracker) {
	// Initialize DatabaseStruct mutex
	if (pthread_mutex_init(&words->mutex, NULL) != 0) {
		fprintf(stderr, "Failed to initialize Words mutex.\n");
		exit(EXIT_FAILURE);
	}
	if (pthread_mutex_init(&observations->mutex, NULL) != 0) {
		fprintf(stderr, "Failed to initialize Observations mutex.\n");
		exit(EXIT_FAILURE);
	}
	if (pthread_mutex_init(&settings->mutex, NULL) != 0) {
		fprintf(stderr, "Failed to initialize CommandSettings mutex.\n");
		exit(EXIT_FAILURE);
	}
	// Initialize LearningTrendTracker mutex
	if (pthread_mutex_init(&tracker->mutex, NULL) != 0) {
		fprintf(stderr, "Failed to initialize LearningTrendTracker mutex.\n");
		exit(EXIT_FAILURE);
	}
}


// Function: construct_command
// Attempts to construct a command array of length settings->length from the learned words.
// settings->scope indicates the percentage of the word database to search.
// This function tries to find a command combination with improved 'value'.
int* construct_command(Words* words, CommandSettings* settings) {
	pthread_mutex_lock(&words->mutex);
	pthread_mutex_lock(&settings->mutex);
	// Extract shortcuts to data
	size_t numWords = words->numWords;
	int***** value_ptr = &(words->value);

	// srchitr is how many times we attempt random improvements
	int srchitr = (int)((numWords * settings->scope) / 100);
	if (srchitr < 1) srchitr = 1; // Ensure at least one iteration

	// Local variables for searching best command combination
	int select = 0;
	int cmdval = 0;
	int prevcmdval = 0;

	// cmdint will hold the indices of words chosen for the command
	// static to avoid returning pointer to local variable
	static int cmdint[CMDMAX + 1];
	for(int i = 0; i < CMDMAX + 1; i++) cmdint[i] = -1;

	// For simplicity, the code tries multiple iterations (srchitr) 
	// and tries to improve the command value by changing arguments
	for (int i = 0; i < srchitr; i++) {
		for (int j = 0; j < settings->length; j++) {
			// Randomly select a word
			if (numWords == 0) {
				// No words available to construct command
				return cmdint;
			}
			select = (int)(rand() % numWords);
			if (i == 0) {
				// First iteration builds a baseline command
				cmdint[j] = select;
				if (j == settings->length-1) {
					// Once the command is fully chosen, calculate its initial score
					for (int k = 0; k < settings->length; k++) {
						for (int l = 0; l < settings->length; l++) {
							if (cmdint[k] < 0 || cmdint[l] < 0 || (size_t)cmdint[k] >= numWords || (size_t)cmdint[l] >= numWords) {
								continue;
							}
							prevcmdval += (*value_ptr)[cmdint[k]][k][cmdint[l]][l];
						}
					}
				}
			} else {
				// Subsequent iterations: try changing one argument and see if it improves
				cmdval = 0;
				for (int k = 0; k < settings->length; k++) {
					if (select < 0 || cmdint[k] < 0 || (size_t)select >= numWords || (size_t)cmdint[k] >= numWords) {
						continue;
					}
					cmdval += (*value_ptr)[select][j][cmdint[k]][k];
				}
				// If the new cmdval is better than prevcmdval, update the command
				if (cmdval > prevcmdval) {
					cmdint[j] = select;
					prevcmdval = cmdval;
					cmdval = 0;
				}
			}
		}
	}
	cmdint[settings->length] = -1; // Terminate command list
	pthread_mutex_unlock(&words->mutex);
	pthread_mutex_unlock(&settings->mutex);
	return cmdint;
}

// Function: execute_command
// Executes the given command string using /bin/sh, and captures its output (stdout/stderr) into a string.
// Returns the output as a dynamically allocated char*.
char* execute_command(char cmd[]) {
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		fprintf(stderr, "Failed to create pipe: %s\n", strerror(errno));
		return NULL;
	}

	// Fork a child to run the command
	pid_t child_pid = fork();
	if (child_pid == 0) {
		// Child process: redirect stdout & stderr into pipe
		close(pipefd[0]); // Close read end in child
		if (dup2(pipefd[1], STDOUT_FILENO) == -1 || dup2(pipefd[1], STDERR_FILENO) == -1) {
			fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
			close(pipefd[1]);
			exit(EXIT_FAILURE);
		}
		close(pipefd[1]); // Close write end after redirecting
		execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		fprintf(stderr, "execl failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	} else if (child_pid < 0) {
		// Fork failed
		fprintf(stderr, "Fork failed: %s\n", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	// Parent process: close write end, read from pipe
	close(pipefd[1]);
	char* output = malloc(1);
	if (!output) {
		fprintf(stderr, "Failed to allocate memory for command output\n");
		close(pipefd[0]);
		return NULL;
	}

	// Check child process status before reading data
	if (check_child_status(child_pid) != 0) {
		fprintf(stderr, "Child process failed or timed out\n");
		free(output);
		close(pipefd[0]);
		return NULL;
	}

	int output_index = 0;
	char current_char;
	while (read(pipefd[0], &current_char, 1) > 0) {
		if (current_char == '\n' || isprint((unsigned char)current_char)) {
			char* temp = realloc(output, sizeof(char) * (output_index + 2));
			if (!temp) {
				fprintf(stderr, "Failed to realloc memory for command output\n");
				free(output);
				close(pipefd[0]);
				return NULL;
			}
			output = temp;
			output[output_index++] = current_char;
		}
	}
	close(pipefd[0]);

	// Null-terminate the output
	char* temp = realloc(output, sizeof(char) * (output_index + 1));
	if (!temp) {
		fprintf(stderr, "Failed to realloc memory for final command output\n");
		free(output);
		return NULL;
	}
	output = temp;
	output[output_index] = '\0';

	return output;
}

// Function: update_database
// Given the output of a command and the command indices (command_integers),
// updates the database with any new words found in output, and rewards/penalizes accordingly.
int update_database(Words* words, Observations* observations, char* output, int* command_integers) {
	pthread_mutex_lock(&words->mutex);
	pthread_mutex_lock(&observations->mutex);
	int***** value = &(words->value);
	char*** token = &(words->token);
	int*** entries_ptr = &(observations->entries);
	size_t* numObservations = &(observations->numObservations);
	size_t* numWords = &(words->numWords);

	int chridx = 0;
	int lrnval = 0;
	int output_index = 0;
	int observationLength = 0;
	int tokenized_line[LINEBUFFER]; // Buffer to hold token indices of one observation line
	char word[WRDBUFFER];
	memset(word, 0, sizeof(word));
	
	// Command buffer for execution
	char cmd[WRDBUFFER * CMDMAX];
	memset(cmd, 0, sizeof(cmd));

	// Parse the output into words
	while (1) {
		char c = output[output_index];
		if (c == '\0') {
			// End of output, handle any last word
			if (chridx > 0) {
				word[chridx] = '\0';
				chridx = 0;

				// Check if the word is already known
				int redundantWord = 0;
				size_t w;
				for (w = 0; w < *numWords; w++) {
					if (strcmp((*token)[w], word) == 0) {
						redundantWord = 1;
						break;
					}
				}

				// If new word
				if (!redundantWord && strlen(word) != 0) {
					reallocate_words(words, (int)strlen(word));
					// Now *numWords has been incremented inside reallocate_words
					strcpy((*token)[*numWords - 1], word);

					// Initialize value for new word
					for (int ii = 0; ii < CMDMAX; ii++) {
						for (size_t jj = 0; jj < *numWords; jj++) {
							for (int kk = 0; kk < CMDMAX; kk++) {
								if ((*numWords - 1) >= words->numWords || ii >= CMDMAX || jj >= words->numWords || kk >= CMDMAX) {
									fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
									continue;
								}
								(*value)[*numWords - 1][ii][jj][kk] = 0;
							}
						}
					}
				}

				// Add the token index for the last word to tokenized_line
				for (w = 0; w < *numWords; w++) {
					if (strcmp((*token)[w], word) == 0) {
						tokenized_line[observationLength] = (int)w;
						observationLength++;
						break;
					}
				}
			}
			break; // no more chars in output
		} else if (c == ' ' || c == '\n') {
			// End of a word
			if (chridx > 0) {
				word[chridx] = '\0';
				chridx = 0;

				// Check if this word is new
				int redundantWord = 0;
				size_t w;
				for (w = 0; w < *numWords; w++) {
					if (strcmp((*token)[w], word) == 0) {
						redundantWord = 1;
						break;
					}
				}
				if (!redundantWord && strlen(word) != 0) {
					reallocate_words(words, (int)strlen(word));
					strcpy((*token)[*numWords - 1], word);

					for (int ii = 0; ii < CMDMAX; ii++) {
						for (size_t jj = 0; jj < *numWords; jj++) {
							for (int kk = 0; kk < CMDMAX; kk++) {
								if ((*numWords - 1) >= words->numWords || ii >= CMDMAX || jj >= words->numWords || kk >= CMDMAX) {
									fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
									continue;
								}
								(*value)[*numWords - 1][ii][jj][kk] = 0;
							}
						}
					}
				}

				// Add this token's index to tokenized_line
				for (w = 0; w < *numWords; w++) {
					if (strcmp((*token)[w], word) == 0) {
						tokenized_line[observationLength] = (int)w;
						observationLength++;
						break;
					}
				}

				// If we hit a newline, that means we completed one observation line
				if (c == '\n') {
					// Check if this observation line is new
					int redobs = 0;
					for (size_t line = 0; line < *numObservations; line++) {
						int match = 1;
						for (int idx = 0; idx < observationLength; idx++) {
							if ((*entries_ptr)[line][idx] != tokenized_line[idx]) {
								match = 0;
								break;
							}
						}
						// Check if ended exactly with -1
						if (match && ((*entries_ptr)[line][observationLength] == -1)) {
							redobs = 1;
							break;
						}
					}

					if (!redobs) {
						// New observation line
						lrnval += REWARD;
						// Allocate space for a new observation line
						reallocate_observations(observations, observationLength);
						// reallocate_observations increments *numObservations internally

						// Now write the tokenized_line into entries_ptr
						// *numObservations is now incremented, so we use (*numObservations - 1)
						size_t obsIndex = *numObservations - 1;
						for (int i = 0; i < observationLength; i++) {
							(*entries_ptr)[obsIndex][i] = tokenized_line[i];
						}
						// Add the terminator
						(*entries_ptr)[obsIndex][observationLength] = -1;
					} else {
						// Redundant observation line
						lrnval -= PENALTY;
					}

					// Reset for the next line
					observationLength = 0;
				}
			}
		} else {
			// Building a word
			if (chridx < WRDBUFFER - 1) {
				word[chridx++] = c;
			} else {
				// Word too long, truncate safely
				word[chridx] = '\0';
				// We'll still treat this truncated word as valid
				chridx = 0;
			}
		}
		output_index++;
	}

	// After processing all output
	// If any observation line is incomplete (no newline), we ignore it or handle accordingly:
	// If we consider incomplete lines as not valid entries_ptr, do nothing.

	// Update values for the command arguments (if necessary)
	for (int i = 0; command_integers[i] != -1; i++) {
		for (int j = 0; command_integers[j] != -1; j++) {
			// Ensure command_integers[i], command_integers[j] are within range [0, *numWords-1]
			if (command_integers[i] >= 0 && (size_t)command_integers[i] < *numWords && 
				command_integers[j] >= 0 && (size_t)command_integers[j] < *numWords) {
				(*value)[command_integers[i]][i][command_integers[j]][j] += lrnval;
			} else {
				fprintf(stderr, "Invalid command_integers indices: command_integers[%d]=%d, command_integers[%d]=%d\n", 
						i, command_integers[i], j, command_integers[j]);
			}
		}
	}
	pthread_mutex_unlock(&words->mutex);
	pthread_mutex_unlock(&observations->mutex);
	return lrnval;
}

// Frees all dynamically allocated memory to prevent leaks.
void cleanup_database(Words* words, Observations* observations) {
	// Free token arrays
	if (words->token) {
		for (size_t i = 0; i < words->numWords; i++){
			free(words->token[i]);
		}
		free(words->token);
	}

	// Free observation arrays
	if (observations->entries) {
		for (size_t i = 0; i < observations->numObservations; i++){
			free(observations->entries[i]);
		}
		free(observations->entries);
	}

	// Free value arrays (4D structure)
	if (words->value) {
		for (size_t i = 0; i < words->numWords; i++) {
			for (int j = 0; j < CMDMAX; j++) {
				for (size_t k = 0; k < words->numWords; k++) {
					free(words->value[i][j][k]);
				}
				free(words->value[i][j]);
			}
			free(words->value[i]);
		}
		free(words->value);
	}
}

// Function to initialize LearningTrendTracker
void init_trend_tracker(LearningTrendTracker* tracker) {
	tracker->window_size = TREND_WINDOW_SIZE;
	tracker->lrnvals = calloc(tracker->window_size, sizeof(int));
	if (!tracker->lrnvals) {
		fprintf(stderr, "Failed to allocate memory for LearningTrendTracker lrnvals.\n");
		exit(EXIT_FAILURE);
	}
	tracker->index = 0;
	tracker->count = 0;
	tracker->moving_average = 0.0;
	pthread_mutex_init(&tracker->mutex, NULL);
}

// Function to update LearningTrendTracker with new lrnval
void update_trend_tracker(LearningTrendTracker* tracker, int lrnval) {
	pthread_mutex_lock(&tracker->mutex);
	
	// Subtract the oldest value from the moving average
	if (tracker->count >= tracker->window_size) {
		tracker->moving_average -= tracker->lrnvals[tracker->index];
	}
	
	// Add the new lrnval to the buffer and moving average
	tracker->lrnvals[tracker->index] = lrnval;
	tracker->moving_average += lrnval;
	
	// Update the index and count
	tracker->index = (tracker->index + 1) % tracker->window_size;
	if (tracker->count < tracker->window_size) {
		tracker->count++;
	}
	
	// Calculate the new moving average
	tracker->moving_average /= tracker->count;
	
	pthread_mutex_unlock(&tracker->mutex);
}

// Function to get current moving average
double get_moving_average(LearningTrendTracker* tracker) {
	pthread_mutex_lock(&tracker->mutex);
	double avg = tracker->moving_average;
	pthread_mutex_unlock(&tracker->mutex);
	return avg;
}

// Function to analyze trend and decide on settings->length adjustment
// Returns 1 to increase settings->length, -1 to decrease settings->length, 0 to keep it unchanged
int analyze_learning_trend(LearningTrendTracker* tracker, double previous_average) {
	double current_average = get_moving_average(tracker);

	if (current_average > previous_average + 1e-6) { // Threshold to account for floating-point precision
		// Trend is increasing
		return 1; // Indicate settings->length should be increased
	} else if (current_average < previous_average - 1e-6) {
		// Trend is decreasing
		return -1; // Indicate settings->length should be decreased
	} else {
		// Trend is stable
		return 0; // No change
	}
}

// Thread function to execute command and update database
void* worker_thread(void* arg) {
	// Cast the argument to ThreadData*
	ThreadData* thread_data = (ThreadData*) arg;
	Words* words = thread_data->words;
	Observations* observations = thread_data->observations;
	CommandSettings* settings = thread_data->settings;
	
	int commands_executed = 0;
	int prevRedundancy = 0;
	int prevcommand_integers[CMDMAX + 1];
	for (int i = 0; i < CMDMAX + 1; i++) prevcommand_integers[i] = -1;
	
	// Command buffer for execution
	char cmd[WRDBUFFER * CMDMAX];
	memset(cmd, 0, sizeof(cmd));
	
	while (commands_executed < COMMANDS_PER_THREAD && !termination_requested) {
		// Construct the command
		int* command_integers_ptr = construct_command(words, settings);
		printf("generated command\n");
		if (command_integers_ptr == NULL) {
			fprintf(stderr, "Failed to construct command. Skipping iteration.\n");
			continue;
		}

		if (prevcommand_integers[0] == -1) {
			// First run, just copy current command_integers to prevcommand_integers
			for (int i = 0; i < CMDMAX + 1; i++) {
				prevcommand_integers[i] = command_integers_ptr[i];
			}
		} else {
			// Check redundancy with previous command
			int currentRedundancy = 0;
			for (int i = 0; command_integers_ptr[i] != -1 && prevcommand_integers[i] != -1; i++) {
				if (command_integers_ptr[i] == prevcommand_integers[i]) {
					currentRedundancy++;
				}
			}
			pthread_mutex_lock(&settings->mutex);
			// Adjust settings->scope based on redundancy
			if (currentRedundancy > prevRedundancy && settings->scope > SRCHMIN) {
				settings->scope--;
			} else {
				settings->scope++;
				if (settings->scope > SRCHMAX) settings->scope = SRCHMAX;
			}
			pthread_mutex_unlock(&settings->mutex);
			prevRedundancy = currentRedundancy;

			for (int i = 0; i < CMDMAX + 1; i++) {
				prevcommand_integers[i] = command_integers_ptr[i];
			}
		}
		// Build command string from command_integers
		cmd[0] = '\0';
		for (int i = 0; command_integers_ptr[i] != -1; i++) {
			if (command_integers_ptr[i] >= 0 && (size_t)command_integers_ptr[i] < words->numWords) {	
				strcat(cmd, words->token[command_integers_ptr[i]]);
				strcat(cmd, " ");
			} else {
				fprintf(stderr, "Invalid command_integers[%d] = %d. Skipping this word.\n", i, command_integers_ptr[i]);
			}
		}

		// Trim trailing space
		size_t len = strlen(cmd);
		if (len > 0 && cmd[len - 1] == ' ') {
			cmd[len - 1] = '\0';
		}

		// Execute the command
		printf("\n$ %s\n", cmd);
		char* output = execute_command(cmd);
		if (output != NULL){
			printf("%s", output);
			int lrnval = update_database(words, observations, output, command_integers_ptr);

			// Update LearningTrendTracker with new lrnval using the passed pointer
			if (thread_data->tracker != NULL) {
				update_trend_tracker(thread_data->tracker, lrnval);
			} else {
				fprintf(stderr, "LearningTrendTracker pointer is NULL.\n");
			}

			free(output);
		}
		commands_executed++;
	}
	// Free the thread data
	free(thread_data);

	// Release semaphore
	sem_post(&thread_sem);

	return NULL;
}

int main() {
	// Initialize srand with time
	srand((unsigned)time(NULL));
	
	// Initialize Words struct
	Words words;
	words.token = NULL;
	words.value = NULL;
	words.numWords = 0;
	
	//Initialize Observations struct
	Observations observations;
	observations.entries = NULL;
	observations.numObservations = 0;
	
	//Initialize CommandSettings struct
	CommandSettings settings;
	settings.length = 1;
	settings.scope = 1;
	
	// Initialize LearningTrendTracker struct
	LearningTrendTracker tracker;
	init_trend_tracker(&tracker);

	// Setup signal handling for graceful termination
	struct sigaction sa;
	sa.sa_handler = signal_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}
	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	// Attempt to load database from disk. If not present, init from scratch.
	int chkinit = load_database(&words, &observations);
	if(chkinit) {
		printf("Initializing please wait...\n");
		init(&words);
	}
	
	// Initialize semaphore
	if (sem_init(&thread_sem, 0, MAX_THREADS) != 0) {
		fprintf(stderr, "Failed to initialize semaphore.\n");
		exit(EXIT_FAILURE);
	}
	
	// Initialize moving average
	double previous_average = 0.0;

	// Main loop: Continues until termination_requested is set
	while (!termination_requested) {
		sem_wait(&thread_sem);

		// Prepare thread data
		ThreadData* thread_data = malloc(sizeof(ThreadData));
		if (!thread_data) {
			fprintf(stderr, "Failed to allocate memory for thread data.\n");
			continue;
		}
		thread_data->words = &words;
		thread_data->observations = &observations;
		thread_data->settings = &settings;
		thread_data->tracker = &tracker; // Assign the tracker pointer

		// Create thread to execute command and update database
		pthread_t tid;
		if (pthread_create(&tid, NULL, worker_thread, (void*)thread_data) != 0) { // Removed cast
			fprintf(stderr, "Failed to create thread.\n");
			free(thread_data);
			continue;
		}

		// Detach the thread as we don't need to join
		pthread_detach(tid);

		// Analyze learning trend
		int adjustment = analyze_learning_trend(&tracker, previous_average);
		double current_average = get_moving_average(&tracker);

		// Adjust settings->length based on the trend analysis
		if (adjustment > 0) {
			settings.length += 1;
			if (settings.length >= CMDMAX) settings.length = CMDMAX - 1;
		} else if (adjustment < 0) {
			settings.length -= 1;
			if (settings.length < CMDMIN) settings.length = CMDMIN;
		}

			// Update previous_average
			previous_average = current_average;
	}
	printf("\nTermination requested. Saving data, and cleaning up...\n");
	
	// 1. Wait for all existing threads to finish
	for(int i = 0; i < MAX_THREADS; i++) {
		sem_wait(&thread_sem);
		}
	// 2. Write the database before closeing
	write_database(&words, &observations);
	
	// 3. Cleanup all dynamically allocated memory
	cleanup_database(&words, &observations);
	
	// 4. Destroy semaphore and mutex
	sem_destroy(&thread_sem);
	pthread_mutex_destroy(&words.mutex);
	pthread_mutex_destroy(&observations.mutex);
	pthread_mutex_destroy(&settings.mutex);
	// Cleanup LearningTrendTracker
	free(tracker.lrnvals);
	pthread_mutex_destroy(&tracker.mutex);
	
	printf("Cleanup complete. Exiting program.\n");
	return 0;
}
