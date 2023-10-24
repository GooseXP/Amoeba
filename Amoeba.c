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
#define CMDMAX 10
#define TIMEOUT 2 // Set the timeout in seconds

// Set the interval for writing the database to files
const int WRITEIVL = 10;

// Define arrays for storing command strings and their usage info
char wordarray[DBBUFFER][WRDBUFFER];
long int wordinfo[DBBUFFER][CMDMAX];
int dbloc = 0;
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
    FILE *wordFile;
    FILE *dataFile;
    wordFile = fopen("words.csv", "w");
    dataFile = fopen("worddata.csv", "w");

    for (int i = 0; i < dbloc; i++) {
        fprintf(wordFile, "%s,", wordarray[i]);
    }
    fclose(wordFile);

    for (int i = 0; i < dbloc; i++) {
        for (int j = 0; j < CMDMAX; j++) {
            if (j > 0) {
                fprintf(dataFile, ",");
            }
            fprintf(dataFile, "%ld", wordinfo[i][j]);
        }
        fprintf(dataFile, "\n");
    }
    fclose(dataFile);
}

// Load the database from files if available, otherwise initialize it
void loadDB() {
    int infoloc = 0;
    int position = 0;
    FILE *wordFile;
    FILE *dataFile;

 if ((wordFile = fopen("words.csv", "r"))) {
        char line[WRDBUFFER];
        while (fgets(line, sizeof(line), wordFile) != NULL) {
            line[strcspn(line, "\n")] = '\0'; // Remove the trailing newline character
            // Replace commas with spaces
            for (size_t i = 0; i < strlen(line); i++) {
                if (line[i] == ',') {
                    line[i] = '\0';
                }
            }
            strcpy(wordarray[dbloc], line);
            dbloc++;
        }
        fclose(wordFile);
    } else {
        // Handle the case when "words.csv" doesn't exist
        init();
        return;
    }

    if ((dataFile = fopen("worddata.csv", "r"))){
        while (infoloc < dbloc && fscanf(dataFile, "%ld", &wordinfo[dbloc][infoloc]) != EOF) {
            position++;
            if (position >= CMDMAX) {
                position = 0;
                infoloc++;
            }
        }
        fclose(dataFile);
    } else {
	init();
	}
}

void timeout_handler(int signum) {
    int kill_attempts = 0; // Not static

    if (child_pid > 0) {
        if (kill_attempts == 0) {
            // First attempt - send SIGKILL
            kill(child_pid, SIGKILL);
            kill_attempts++;
        } else if (kill_attempts == 1) {
            // Second attempt - send another SIGKILL
            kill(child_pid, SIGKILL);
            kill_attempts++;
        } else {
            // Third attempt - give up
            fprintf(stderr, "Failed to terminate the child process.\n");
            exit(1); // Or take any other necessary action
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
        perror("execl");
        exit(1);
    } else if (child_pid < 0) {
        // Handle fork failure
        perror("fork");
        exit(1);
    }

    // Close the write end of the pipe in the parent
    close(pipefd[1]);

    // Set a timeout for the command execution
    signal(SIGALRM, timeout_handler);
    alarm(TIMEOUT); // Set the alarm

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
                lrnval++;
            }

            chridx = 0;
        } else {
            word[chridx] = current_char;
            chridx++;
        }
    }
    close(pipefd[0]);

    // Reset the alarm
    alarm(0);

	output_buffer[output_index] = '\0'; // Null-terminate the output
	printf("%s", output_buffer);// Print the captured output (stdout and stderr)

    // Update usage count for learned commands
    for (int i = 0; i <= cmdlen; i++) {
        wordinfo[cmdint[i]][i] += lrnval;
    }
    int status;
    
    waitpid(child_pid, &status, 0);

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
            if (length <= CMDMAX) {
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
