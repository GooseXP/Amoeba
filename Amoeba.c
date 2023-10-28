#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

// Define constants for database and buffer sizes
#define DBBUFFER 1000000
#define WRDBUFFER 100
#define CMDMAX 10 //set the maximum number of commands to enter
#define TIMEOUT 5 // Set the timeout in seconds
#define NORMTHLD 5 //Set the threshold (in tenths of a percent) of items greater than the overall average before normalizing
#define REWARD 10 //set reward for learning new data
#define PENALTY 1 //Set penalty for recieving redundant data
#define WRITEIVL 10 // Set the interval for writing the database to files


// Define arrays for storing command strings and their usage info
char wordarray[DBBUFFER][WRDBUFFER];
long int wordinfo[DBBUFFER][CMDMAX];
int dbloc = 0;
int kill_attempts = 0;
time_t t;
pid_t child_pid = 0;

// Initialize the database with common Linux commands
void init() {
    int chridx = 0;
    char word[WRDBUFFER];
    FILE *cmdfile = popen("ls /bin && ls /sbin", "r");
    char cmdchr = '\0';

    while (cmdchr != EOF) {
        cmdchr = fgetc(cmdfile);

        // Read characters from the command output
        if (cmdchr != ' ' && cmdchr != '\n') {
            word[chridx] = cmdchr;
            chridx++;
        } else {
            // When a word is complete, handle it
            word[chridx] = ' ';

            int redundant = 0;

            // Check if the word is already in the database
            for (int i = 0; i < dbloc; i++) {
                if (strcmp(wordarray[i], word) == 0) {
                    redundant = 1;
                }
            }

            if (redundant == 0) {
                // If not redundant, add the word to the database
                strcpy(wordarray[dbloc], word);

                // Initialize the command usage info
                for (int i = 0; i < CMDMAX; i++) {
                    wordinfo[dbloc][i] = 0;
                }

                // Increment the database location
                dbloc++;
            }

            // Clear the word and index for the next word
            int i = 0;
            while (i <= chridx) {
                word[i] = '\0';
                i++;
            }
            chridx = 0;
        }
    }
    pclose(cmdfile);
}

// Write the database to files
void writeDB() {
FILE *wordFile = fopen("words.txt", "w");
    FILE *dataFile = fopen("worddata.csv", "w");

    if (wordFile && dataFile) {
        for (int i = 0; i < dbloc; i++) {
            fprintf(wordFile, "%s\n", wordarray[i]);
            for (int j = 0; j < CMDMAX; j++) {
                fprintf(dataFile, "%ld,", wordinfo[i][j]);
            }
            fprintf(dataFile, "\n");
        }

        fclose(wordFile);
        fclose(dataFile);
    } else {
        perror("Failed to open files for writing.");
    }
}

// Load the database from files if available, otherwise initialize it
void loadDB() {
  FILE *wordFile = NULL;
    FILE *dataFile = NULL;
    char line[WRDBUFFER];
    
    // Open the files for reading
    wordFile = fopen("words.txt", "r");
    dataFile = fopen("worddata.csv", "r");

    if (wordFile && dataFile) {
        while (fgets(line, sizeof(line), wordFile) != NULL) {
            // Trim trailing newline character.
            line[strcspn(line, "\n")] = '\0';
            strcpy(wordarray[dbloc], line);
         	int position = 0;
			while (position < CMDMAX) {
					fscanf(dataFile, "%ld,", &wordinfo[dbloc][position]);
		        	position++;
			}
            dbloc++;
        }
		fclose(wordFile);
        fclose(dataFile);
    } else {
        // Handle the case when "words.txt" or "worddata.csv" doesn't exist
        init();
    }
}

void timeout_handler() {
    if (kill_attempts == 0) {
        // First attempt - send SIGTERM
        kill(child_pid, SIGTERM);
    } else if (kill_attempts == 1) {
		// Second attempt - send SIGQUIT
		kill(child_pid, SIGQUIT);
    } else if (kill_attempts == 2) {
		// Third attempt - send SIGKILL
		kill(child_pid, SIGKILL);
    } else if (kill_attempts == 3) {
        // Third attempt - give up
        fprintf(stderr, "Failed to terminate the child process.\n");
        exit(1);
    }
	kill_attempts++;
}
    
// Function to check the status of the child process and handle timeouts
void check_child_status() {
    int status;
    int timer = 0;
    pid_t result;
    while(1){
    result = waitpid(child_pid, &status, WNOHANG);
    if (result == -1) {
        // Print the error message to stderr
        perror("waitpid error");
        exit(1); // Exit the program due to the error
    } else if (result > 0) {
        // Child process has terminated, take appropriate action
        child_pid = 0;  // Reset child_pid
        kill_attempts = 0; // Reset kill_attempts
        return;
    } else {
    	timer++;
		usleep(1);
		if(timer >= (TIMEOUT* 1000000)){
			timeout_handler();
			timer = 0;
			}
		}
	}
}
//Normalize the data
void normalize() {
    int overall_scores = 0;
    for (int i = 0; i < dbloc; i++) {
        for (int j = 0; j < CMDMAX; j++) {
            overall_scores += wordinfo[i][j];
        }
    }
    
    int overall_average = overall_scores / (dbloc * CMDMAX);  // Calculate the overall average score
    int select_items = 0;
    int select_scores = 0;
    for (int i = 0; i < dbloc; i++) {
        for (int j = 0; j < CMDMAX; j++) {
            if (wordinfo[i][j] > overall_average) {
                select_scores += wordinfo[i][j];
                select_items++;
            }
        }
    }
	int selectpct = (select_items * 1000) / (dbloc * (CMDMAX - 1)); //tenth of a percent of items with above average scores
    if (selectpct >= NORMTHLD) {
        int select_average = select_scores / select_items; // Calculate the average of scores greater than the overall average
        for (int i = 0; i < dbloc; i++) {
            for (int j = 0; j < CMDMAX; j++) {
                if (wordinfo[i][j] < select_average) {
                    wordinfo[i][j] = select_average;  // Normalize the score to the average of select_scores
                }
            }
        }
    }
}

// Learn from commands and update the database
int learn(int cmdlen) {
    int chridx = 0;
    int lrnval = 0; // Initialize lrnval to 0
    int srchitr = rand() % dbloc + 1;
    int select = 0;
    int cmdint[CMDMAX];
    char word[WRDBUFFER];
    char cmd[WRDBUFFER * CMDMAX];
    char output_buffer[DBBUFFER]; // Character buffer to store the output
    strcpy(cmd, "\0");

    // Randomly select commands for learning
    for (int i = 0; i <= srchitr; i++) {
        for (int j = 0; j <= cmdlen; j++) {
            select = rand() % dbloc + 1;
            if (i == 0)
                cmdint[j] = select;
            else {
                // Choose commands with higher usage count
                if (wordinfo[select][j] > wordinfo[cmdint[j]][j]) {
                    cmdint[j] = select;
                }
            }
        }
    }

    cmdint[cmdlen + 1] = -1;

    // Construct the command to execute
    for (int i = 0; cmdint[i] != -1; i++) {
        strcat(cmd, wordarray[cmdint[i]]);
    }

    printf("\n$ %s\n", cmd);

    // Create a pipe to capture stdout
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }

    // Fork a child process to execute the command
    if ((child_pid = fork()) == 0) {
        // This is the child process
        close(pipefd[0]); // Close the read end of the pipe
        dup2(pipefd[1], 1); // Redirect stdout to the write end of the pipe
        dup2(pipefd[1], 2); // Redirect stderr to the same pipe as stdout
        close(pipefd[1]); // Close the duplicated file descriptors

        execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)0);
        // Exec failed
        perror("execl failue");
        exit(1);
    } else if (child_pid < 0) {
        // Handle fork failure
        perror("fork failure");
        exit(1);
    }

    // Close the write end of the pipe in the parent
    close(pipefd[1]);
    
    // Check child process status before reading data
    check_child_status();

    // Read the contents of the pipe (stdout and stderr) directly into the output_buffer
    int output_index = 0;
    char current_char;
    while (read(pipefd[0], &current_char, 1) > 0) {
        output_buffer[output_index] = current_char;
        output_index++;

        // Check for word boundaries and increment lrnval when a new word is added
        if ((current_char == ' ' || current_char == '\n') && chridx > 0) {
            word[chridx] = '\0';

            int redundant = 0;
            for (int i = 0; i < dbloc; i++) {
                if (strcmp(wordarray[i], word) == 0) {
                    redundant = 1;
                    break; // Word already in the database
                }
            }

            if (!redundant) {
                // Add the new word to the database
                strcpy(wordarray[dbloc], word);
                for (int i = 0; i < CMDMAX; i++) {
                    wordinfo[dbloc][i] = 0;
                }
                dbloc++;
                lrnval += REWARD;
            } else{
				lrnval -= PENALTY;			
			}

            chridx = 0;
        } else {
            word[chridx] = current_char;
            chridx++;
        }
    }
    close(pipefd[0]);

    output_buffer[output_index] = '\0'; // Null-terminate the output
    printf("%s", output_buffer); // Print the captured output (stdout and stderr)

    // Update usage count for learned commands
    for (int i = 0; i <= cmdlen; i++) {
        wordinfo[cmdint[i]][i] += lrnval;
    }
	normalize();
    return lrnval;
}

int main() {
    loadDB();
    srand((unsigned)time(&t));
    int write = 0;
    int length = 1;
    int score = 0;
    int prevscore = 0;
    while (1) {
        // Learn and adapt command length based on performance
        score = learn(length);

        if (score >= prevscore) {
            if (length < CMDMAX) {
                length += rand() % 2;
            }
        } else if (length > 1) {
            length -= rand() % 2;
        }
        prevscore = score;
        write++;

        // Write the database to files at regular intervals
        if (write == WRITEIVL) {
            writeDB();
            write = 0;
        }
    }

    return 0;
}
