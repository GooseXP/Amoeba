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

// Define constants for database and buffer sizes
#define LINEBUFFER 100
#define WRDBUFFER 100
#define SRCHMIN 1 //minimum percentage to search database
#define SRCHMAX 100 //maximum percentage to search database
#define CMDMAX 10 //set the maximum number of commands to enter
#define RUNTIME 3 // Set the process runtime in seconds
#define TIMEOUT 10 //Timeout if child process locks waitpid
#define NORMTHLD 250 //Set the threshold for items with values greater than the average before normalizing
#define REWARD 10 //set reward for learning new data
#define PENALTY 1 //Set penalty for recieving redundantWord data
#define INPUTBONUS 1 //Set extra reward value when two of the arguments used together in the command are used together in input
#define WRITEIVL 10 // Set the interval for writing the database to files

pid_t child_pid = 0;
volatile sig_atomic_t termination_requested = 0;

//Define struct to handle learned words
typedef struct {
	char** token;
	int**** value;
	size_t numWords;
	int** observation;
	size_t numObservations;
}DatabaseStruct;

void reallocateWords(DatabaseStruct* database,int wordLength) {
	char*** token = &(database->token);
	int***** value = &(database->value);
	size_t* old_size  = &(database->numWords); 
	size_t new_size = *old_size + 1;
	if (*old_size == 0) {
		*token = malloc(sizeof((*token)[0]));
		(*token)[*old_size] = malloc(wordLength + 1);
		*value = calloc(new_size, sizeof((*value)[0]));
		for(size_t i = 0; i < new_size; i++){
			(*value)[i] = calloc(CMDMAX, sizeof((*value)[0][0]));
			for(int j = 0; j < CMDMAX; j++){
			    (*value)[i][j] = calloc(new_size, sizeof((*value)[0][0][0]));
			    for(size_t k = 0; k < new_size; k++){
			        (*value)[i][j][k] = calloc(CMDMAX, sizeof((*value)[0][0][0][0]));
			    }
			}
		}
	} else {
		*token = realloc(*token, sizeof(char*) * new_size);
		(*token)[new_size] = malloc(wordLength + 1);

		*value = realloc(*value, new_size * sizeof((*value)[0]));
		for(size_t i = 0; i < new_size; i++){
			if (i >= *old_size) {
				(*value)[i] = calloc(CMDMAX, sizeof((*value)[0][0]));
			}
			for(int j = 0; j < CMDMAX; j++){
				if (i >= *old_size) {
					*value[i][j] = calloc(new_size, sizeof((*value)[0][0][0]));
				} else {
					*value[i][j] = realloc((*value)[i][j], new_size * sizeof((*value)[0][0][0]));
				}
				for(size_t k = 0; k < new_size; k++){
					if (i >= *old_size || k >= *old_size) {
						(*value)[i][j][k] = calloc(CMDMAX, sizeof((*value)[0][0][0][0]));
					}
				}
			}
		}
	}
	old_size++;
}
void reallocateObservations(DatabaseStruct* database, int observationLength) {
	int*** observation = &(database->observation);
	size_t* old_size = &(database->numObservations);
	size_t new_size = *old_size + 1; 
	if (*old_size == 0) {
		*observation = malloc(sizeof((*observation)[0]));
		(*observation)[*old_size] = malloc(observationLength + 1);
	} else {
		*observation = realloc(*observation, sizeof(char*) * new_size);
		(*observation)[new_size] = malloc(observationLength + 1);
	}
	old_size++;
}
// Initialize the database with common Linux commands
void init(DatabaseStruct* database) {
	char*** token = &(database->token);
	int***** value = &(database->value);
	size_t* numWords = &(database->numWords);
	int chridx = 0;
	char word[WRDBUFFER];
	FILE* cmdfile = popen("ls /bin && ls /sbin", "r");
	char cmdchr = '\0';
	while (cmdchr != EOF) {
		cmdchr = fgetc(cmdfile);
		// Read characters from the command output
		if (cmdchr != ' ' && cmdchr != '\n') {
			word[chridx] = cmdchr;
			chridx++;
		} else {
			word[chridx] = ' ';
			// When a word is complete, handle it
			word[chridx + 1] = '\0';
			int redundantWord = 0;
			// Check if the word is already in the database
			for (size_t i = 0; i < *numWords; i++) {
				if ((*token)[i] != NULL && strcmp((*token)[i], word) == 0) {
					redundantWord = 1;
					break;
				}
			}
			
			if (!redundantWord) {
				reallocateWords(database, strlen(word));
				//*token = realloc(*token, sizeof(char*) * (numWords + 1));
				//(*token)[numWords] = malloc(strlen(word) + 1);
				
				// If not redundantWord, add the word to the database
				strcpy((*token)[*numWords], word);
                
				// Initialize the command usage info
				for (int i = 0; i < CMDMAX; i++) {
					for (size_t j = 0; j < *numWords; j++) {
						for (int k =0; k < CMDMAX; k++) { 
							value[*numWords][i][j][k] = 0;
						}
					}
				}
				// Increment the database location
				numWords++;
			}
			// Clear the word and index for the next word
			for (int i = 0; i <= chridx; i++) {
				word[i] = '\0';
			}
			chridx = 0;
		}
	}
	pclose(cmdfile);
}

// Write the database to files
void writeDB(DatabaseStruct* database) {
	FILE* tokenFile = fopen("tokens.txt", "w");
	FILE* valueFile = fopen("values.csv", "w");
	FILE* observationFile = fopen("observations.csv", "w");
	char*** token = &(database->token);
	int***** value = &(database->value);
	int*** observation =&(database->observation);
	size_t* numObservations = &(database->numObservations);
	size_t* numWords = &(database->numWords);
	
	if (tokenFile && valueFile && observationFile) {
		for (size_t i = 0; i < *numWords; i++) {
			fprintf(tokenFile, "%s\n", (*token)[i]);
			for (int j = 0; j < CMDMAX; j++) {
				for (size_t k = 0; k < *numWords; k++) {
					for (int l = 0; l < CMDMAX; l++) {
						fprintf(valueFile, "%d,", *value[i][j][k][l]);
					}
					fprintf(valueFile, " "); 
				}
			}
			fprintf(valueFile, "\n");
		}
		for (size_t i = 0; i < *numObservations; i++) {
			for (int j = 0; (*observation)[i][j] != -1; j++) {
				fprintf(observationFile, "%d,", (*observation)[i][j]);
			}
			fprintf(observationFile, "\n");
		}
		fclose(tokenFile);
		fclose(valueFile);
		fclose(observationFile);
	} else {
		fprintf(stderr,"Failed to open files for writing.");
	}
}

// Load the database from files if available, otherwise initialize it
int loadDB(DatabaseStruct* database) {
	FILE* tokenFile = fopen("token.txt", "r");
	FILE* valueFile = fopen("worddata.csv", "r");
	FILE* observationFile = fopen("observations.csv", "r");
	char*** token = &(database->token);
	int***** value = &(database->value);
	int*** observations =&(database->observation);
	size_t* numObservations = &(database->numObservations);
	size_t* numWords = &(database->numWords);

	char word[WRDBUFFER];
	
	if (tokenFile && valueFile) {
		while (fgets(word, sizeof(word), tokenFile) != NULL) {
			// Trim trailing newword character.
			word[strcspn(word, "\n")] = '\0';
			reallocateWords(database, strlen(word));
			strcpy((*token)[*numWords], word);
			for (int i = 0; i < CMDMAX; i++) {
				for (size_t j = 0; j < *numWords; j++) {
					for (int k =0; k <= CMDMAX; k++) {
						fscanf(valueFile, "%d,", value[*numWords][i][j][k]);
					}
				}
			}
		}
		char observationToken;
		int tokenized_line[LINEBUFFER];
		int observationLength = 0;
		while ((observationToken = fgetc(observationFile)) != EOF) {
			if (observationToken != '\n') {
				tokenized_line[observationLength] = observationToken;
				observationLength++;
			} else {
				reallocateObservations(database, observationLength);
				for(int i = 0; i <= observationLength; i++) {
					*observations[*numObservations][i] = tokenized_line[i];
				}
				*observations[*numObservations][observationLength + 1] = -1;
			}
		}
		fclose(tokenFile);
		fclose(valueFile);
		fclose(observationFile);
	} else {
		// Handle the case when "token.txt" or "worddata.csv" doesn't exist
		return 1;
	}
	return 0;
}

//Handle timeout if waitpid locks up
void signal_handler(int signum) {
	if (signum == SIGINT || signum == SIGTERM) {
		termination_requested = 1;
	} else if (signum == SIGALRM){
		child_pid = 0;
	}
}

// Function to check the status of the child process and handle RUNTIME
int check_child_status() {
    int status;
    time_t start_time, current_time;
    time(&start_time);
    int kill_attempts = 0;
    while (1) {
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);
        time(&current_time);
        int proc_time = difftime(current_time, start_time);

        if (proc_time >= RUNTIME) {
            // Child process is still running, and a process time has exceeded maximum.
            if (kill_attempts == 0) {
                // First attempt - send SIGTERM
                if (kill(child_pid, SIGTERM) == 0) {
                } else {
                    fprintf(stderr,"kill error");
                }
            } else if (kill_attempts == 1) {
                // Second attempt - send SIGKILL
                if (kill(child_pid, SIGKILL) == 0) {
                } else {
                	fprintf(stderr,"kill error");
                }
            } else if (kill_attempts == 2) {
                // Third attempt - give up
                fprintf(stderr, "Failed to terminate the child process.");
                child_pid = 0;
		return 1;
            }
            sleep(1);
        	kill_attempts++;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
		// Child process has terminated
		child_pid = 0;  // Reset child_pid
		return 0;
        } else if (wpid < 0) {
            // Handle the case when waitpid returns an error
            fprintf(stderr,"waitpid error");
	    child_pid = 0;
            return 1;
        }
    }
}

//Normalize the data
/*
void normalize() {
	int overall_scores = 0;
	for (int i = 0; i < numWords; i++) {
		for (int j = 0; j < CMDMAX; j++) {
			for (int k = 0; k < numWords; k++){
				for(int l = 0; l < CMDMAX; l++){
					overall_scores += value[i][j][k][l];
				}
			}
		}
	}
	int overall_average = overall_scores / (numWords * CMDMAX);  // Calculate the overall average score
	int select_items = 0;
	int select_scores = 0;
	for (int i = 0; i < numWords; i++) {
		for (int j = 0; j < CMDMAX; j++) {
			for (int k; k < numWords; k++) {
				for (int l; l <= CMDMAX; l++) {
					if (value[i][j][k][l] > overall_average) {
						select_scores += value[i][j][k][l];
						select_items++;
					}
				}
			}
		}
	}
	if (select_items >= NORMTHLD) {
		int select_average = select_scores / select_items; // Calculate the average of scores greater than the overall average
		for (int i = 0; i < numWords; i++) {
			for (int j = 0; j < CMDMAX; j++) {
				for (int k = 0; k < numWords; k++) {
					for (int l = 0; l < CMDMAX; l++) {
						if (value[i][j][k][l] < select_average) {
							value[i][j][k][l] = select_average;  // Normalize the score to the average of select_scores
						}
					}
				}		
			}
		}
	}
}
*/

// Function to construct the command and return it as a char array
int* constructCommand(DatabaseStruct* database,int cmdlen, int srchpct) {
	size_t* numWords = &(database->numWords);
	size_t* numObservations = &(database->numObservations);
	int***** value = &(database->value);
	int*** observations = &(database->observation);
	int srchitr = (*numWords * srchpct) / 100;
	int select = 0;
	int cmdval = 0;
	int prevcmdval = 0;
	int arg1found = 0;
	int arg2found = 0;
	static int* cmdint[CMDMAX + 1];
	// Randomly select commands for learning
	for (int i = 0; i <= srchitr; i++) {
		for (int j = 0; j <= cmdlen; j++) {
			select = rand() % *numWords;
			if (i == 0){
				//build the baseline command and get the total value of it
				*cmdint[j] = select;
				if (j == cmdlen) {
					for (int k = 0; k <= cmdlen; k++) {
						for (int l = 0; l <= cmdlen; l++) {
							prevcmdval += *value[*cmdint[k]][k][*cmdint[l]][l];
							for (size_t m = 0; m <= *numObservations; m++){
								for (int n = 0; *observations[m][n] != -1; n++) {
									if (*observations[m][n] == select) {
										arg1found = 1;
									}
									if (*observations[l][m] == *cmdint[k]) {
										arg2found = 1;
									}
									if (arg1found && arg2found) {
										prevcmdval += INPUTBONUS;
									}
								}
								arg1found = 0;
								arg2found = 0;
							}
						}
					}
				}
			} else {
				//change one argument in the command and check if the overall value is improved in relation to the other arguments in the command
				for (int k = 0; k <= cmdlen; k++) {
					cmdval += *value[select][j][*cmdint[k]][k];
					for (size_t l = 0; l <= *numObservations; l++){
						for (int m = 0; *observations[l][m] != -1; m++) {
							if (*observations[l][m] == select) {
								arg1found = 1;
							}
							if (*observations[l][m] == *cmdint[k]) {
								arg2found = 1;
							}
							if (arg1found && arg2found) {
								cmdval += INPUTBONUS;
							}
						}
						arg1found = 0;
						arg2found = 0;
					}
				}
			}
			// if the value is improved replace the argument at position j with select
			if (cmdval > prevcmdval) {
				*cmdint[j] = select;
				prevcmdval = cmdval;
				cmdval = 0;
			}
		}
	}
	*cmdint[cmdlen + 1] = -1;
	return cmdint[0];
}

// Function to execute the command and capture output
char* executeCommand(char cmd[]) {
	// Create a pipe to capture stdout
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		fprintf(stderr,"pipe");
		exit(1);
	}
	// Fork a child process to execute the command
	if ((child_pid = fork()) == 0) {
		close(pipefd[0]); // Close the read end of the pipe
		dup2(pipefd[1], 1); // Redirect stdout to the write end of the pipe
		dup2(pipefd[1], 2); // Redirect stderr to the same pipe as stdout
		close(pipefd[1]); // Close the duplicated file descriptors
        	if (execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)0) == -1) {
			fprintf(stderr,"execl failure");
			exit(1);
    }
	} else if (child_pid < 0) {
		// Handle fork failure
		fprintf(stderr,"fork failure");
		exit(1);
    }
	
	// Close the write end of the pipe in the parent
	close(pipefd[1]);
	// Check child process status before reading data
	int exit_failure = check_child_status();
	if (!exit_failure) {
		char* output = malloc(sizeof(char));
		// Read the contents of the pipe (stdout and stderr) directly into the output
		int output_index = 0;
		char current_char;
		while (read(pipefd[0], &current_char, 1) > 0) {
			if (current_char == '\n' || isprint(current_char)) {
				output = realloc(output,sizeof(char) * (output_index + 1));
				output[output_index] = current_char;
				output_index++;
			}
		}
		close(pipefd[0]);
		output = realloc(output,sizeof(char) * (output_index + 1));
		output[output_index] = '\0'; // Null-terminate the output
		return output;
	} else {
		return NULL;
	}
}

// Function to update the database with learned commands
int updateDatabase(DatabaseStruct* database, char* output,int* cmdint){
	int***** value = &(database->value);
	char*** token = &(database->token);
	int*** observations = &(database->observation);
	size_t* numObservations = &(database->numObservations);
	size_t* numWords =&(database->numWords);
	int chridx = 0;
	int lrnval = 0;
	int output_index = 0;
	int observationLength = 0;
	size_t srchidx = 0; 
	int tokenized_line[LINEBUFFER]; //it might be a good idea to dynamically alocate this we'll just make it a buffer for now
	char word[WRDBUFFER];

	// Handle word boundaries and update the database
	while (output[output_index] != '\0') {
		if ((output[output_index] == ' ' || output[output_index] == '\n') && (chridx > 0)) {
			word[chridx] = '\0';
			int redundantWord = 0;
			for (size_t i = 0; i < *numWords; i++) {
				if ((*token)[i] != NULL && strcmp((*token)[i], word) == 0) {
					redundantWord = 1;
					break; // Word already in the database
				}
			}
			if (!redundantWord && strlen(word) != 0) {
				*token = realloc(*token, sizeof(char*) * (*numWords + 1));
				(*token)[*numWords] = malloc(strlen(word) + 1);
				// Add the new word to the database
				strcpy((*token)[*numWords], word);
				for (int i = 0; i < CMDMAX; i++) {
					for (size_t j = 0; j < *numWords; j++) {
						for (int k = 0; k < CMDMAX; k++) {
							value[*numWords][i][j][k] = 0;
						}
					}
				}
				numWords++;
			}
			for (size_t i = 0; i < *numWords; i++) {
				if (strcmp((*token)[i], word) == 0){
					tokenized_line[observationLength] = i;
					observationLength++;
				}
			}
			if (output[output_index] == '\n') {
				int redobs = 0;
				while (srchidx <= *numObservations) {
					for (int i = 0; *observations[srchidx][i] != -1 && i <= observationLength; i++) {
						if  (*observations[srchidx][i] != tokenized_line[i]) {
						srchidx++;
						} else if (i == observationLength && *observations[srchidx][i + 1] == -1) {
							redobs = 1;
							break;
						}
					}
					if (redobs) {
						break;
					} else {
						srchidx++;
					}
				}
				if (!redobs) {
					lrnval += REWARD;
					reallocateObservations(database, observationLength);
					for(int i = 0; i <= observationLength; i++) {
						*observations[*numObservations][i] = tokenized_line[i];
					}	
					*observations[*numObservations][observationLength + 1] = -1;
					observationLength = 0;
				} else {
					lrnval -= PENALTY;
				}
			}	
			chridx = 0;
		} else {
			word[chridx] = output[output_index];
			chridx++;
		}
		output_index++;
	}
	for (int i = 0; cmdint[i] != -1; i++){
		for (int j =0; cmdint[j] != -1; j++){
			value[cmdint[i]][i][j][j] += lrnval;
		}
	}
	return lrnval;
}
// Function to free the dynamically alocated arrays on program exit
void cleanup(DatabaseStruct* database) {
	char*** token = &(database->token);
	size_t* numWords = &(database->numWords);
	for (size_t i = 0; i < *numWords; i++){
		free((*token)[i]);
	}
	free(*token);
}

int main() {
	int lrnval = 0;
	int prevlrnval = 0;
	int srchpct = 1;
	int cmdlen = 1;
	int write = 0;
	int *prevCmdint = NULL;
	int redundancy = 0;
	int prevRedundancy = 0;
	char cmd[WRDBUFFER * CMDMAX];
	DatabaseStruct database;
	srand((unsigned)time(NULL));
	//Setup signal handling
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGALRM, signal_handler); 
	int chkinit = loadDB(&database);
	if(chkinit){
		init(&database);
	}
	while (!termination_requested) {
		int* cmdint = constructCommand(&database, cmdlen, srchpct);
		if (prevCmdint == NULL) {
			prevCmdint = cmdint;
		} else {
			for (int i = 0; cmdint[i] != -1 && prevCmdint[i] != -1; i++) {
				if (cmdint[i] == prevCmdint[i]) {
					redundancy++;
				}
			}
			if (redundancy > prevRedundancy && srchpct >= 1) {
				srchpct--;
			} else {
				srchpct++;
			}
			prevRedundancy = redundancy;
			redundancy = 0;
		}
		cmd[0] = '\0'; // Initialize cmd as an empty string
		for (int i = 0; cmdint[i] != -1; i++) {
			strcat(cmd, database.token[cmdint[i]]);
	       	}
		printf("\n$ %s\n", cmd);
		char* output = executeCommand(cmd);
		if (output != NULL){
			printf("%s", output);
			lrnval = updateDatabase(&database, output, cmdint);
			free(output);
			//normalize the data
			//normalize();
			// Adjust the cmdlen and write variables
			if (lrnval >= prevlrnval) {
				if (cmdlen < CMDMAX) {
					cmdlen += rand() % 2;
				}
			} else if (cmdlen > 1) {
			cmdlen -= rand() % 2;
			}
			prevlrnval = lrnval;
			write++;
		}

		// Write the database to files at regular intervals
		if (write == WRITEIVL) {
			writeDB(&database);
			write = 0;
		}
	}
	cleanup(&database);
	printf("\n\nProgram exited successfully!\n");
	return 0;
}
