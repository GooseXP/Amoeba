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
int wordinfo[DBBUFFER][CMDMAX];
int dbloc = 0;
time_t t;
pid_t child_pid = 0;

// Initialize the database with common Linux commands
void init() {
    int chridx = 0;
    char word[WRDBUFFER];
    FILE *cmdfile = popen("ls /bin", "r");
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

// Load the database from files if available, otherwise initialize it
void loaddb() {
    FILE *readwrd;
    FILE *readinfo;
    char line[WRDBUFFER];
    char word[WRDBUFFER];

    if ((readwrd = fopen("words.txt", "r")) && (readinfo = fopen("wordinfo.txt", "r")) ) {
        int infoloc = 0;

        while (fgets(line, sizeof(line), readwrd) != NULL) {
            // Trim trailing newline character, if any
            line[strcspn(line, "\n")] = '\0';
            // Tokenize the line to extract words
            int position = 0;
            char *token = strtok(line, " \t\n");

            while (token != NULL) {
                // Add the word to the database
                strcpy(wordarray[dbloc], token);
                strcat(wordarray[dbloc], " ");
                dbloc++;

                // Read command usage info from the file
                if (fscanf(readinfo, "%d", &wordinfo[infoloc][position]) != 1) {
                    break;
                }

                token = strtok(NULL, " \t\n");
                position++;
            }

            infoloc++;
        }

        fclose(readwrd);
        fclose(readinfo);
    } else {
        // Database files don't exist, initialize the database
        init();
    }
}

void timeout_handler(int signum) {
    // Handle the timeout here
    // You can choose to kill the child process or perform other actions
    // In this example, we will kill the child process
    if (child_pid > 0) {
        kill(child_pid, SIGKILL);
        child_pid = 0;
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
        close(pipefd[1]); // Close the duplicated file descriptor

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

    // Read the contents of the pipe (stdout) directly into the output_buffer
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
    // Update usage count for learned commands
    for (int i = 0; cmdint[i] != -1; i++) {
        wordinfo[cmdint[i]][i] += lrnval;
    }

    int status;
    
    waitpid(child_pid, &status, 0);

    return lrnval;
}

// Write the database to files
void writedb() {
    FILE *writewrd;
    FILE *writeinfo;
    writewrd = fopen("words.txt", "w");
    writeinfo = fopen("wordinfo.txt", "w");

    // Write commands and usage info to files
    for (int i = 0; i < dbloc; i++) {
        fprintf(writewrd, "%s\n", wordarray[i]);
        for (int j = 0; j < CMDMAX; j++) {
            fprintf(writeinfo, "%d", wordinfo[i][j]);
        }
        fprintf(writeinfo, "\n");
    }

    fclose(writewrd);
    fclose(writeinfo);
}

int main() {
    loaddb();
	srand((unsigned)time(&t));
    int write = 0;
    int length = 5;
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
            writedb();
            write = 0;
        }
    }
    
    return 0;
}
