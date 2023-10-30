Amoeba - Linux Command Line Learning Program

Amoeba is a program designed to explore the Linux command line by learning strings and their usages. It keeps track of commands executed and their frequencies to adapt and improve its knowledge over time. The learned data is stored in a database and periodically saved to files for persistence. If the program encounters a string it has not seen before, it adds it to its database and adds a reward value to the command used to generate it.

WARNING
- It cannot be overstated that by nature this program makes a mess, use at your own risk and please sandbox it in some way.

Features
- Learns strings and their usages by capturing stdout and stderr of executed commands.
- Generates commands based on their previous usefullness in a given position within the executed command.
- Adapts string length based on performance.
- Periodically saves the learned data to files.
- Normalizes the learned data to prevent overfitting.

Prerequisites
To run Amoeba, you need the following:
- A Linux-based operating system (e.g., Ubuntu, CentOS).
- GCC (GNU Compiler Collection) for compiling C programs.
- Basic knowledge of the Linux command line.

Sandboxing Requirements
For security reasons, it is mandatory to sandbox the program using one of the following methods:
- [Docker](https://www.docker.com/) containerization
- [chroot](https://man7.org/linux/man-pages/man2/chroot.2.html) isolation
- [SELinux](https://selinuxproject.org/) policies
- Running in a virtual machine (VM)

Sandboxing helps isolate the program from the host system and restricts its access to critical resources, reducing potential security risks.

Installation
Amoeba comes with a Makefile for easy installation. Here's how to install it:
1. Clone the repository or download the source code.
   git clone (https://github.com/Code-Cortex/Amoeba)
   cd Amoeba
2. Compile and install the program using make.
   make
   make install

Usage
Run Amoeba by simply entering its name in the terminal:
amoeba
The program will start and continuously learn and adapt its knowledge of Linux strings. It will execute commands, capture their output, and adjust its string length based on performance. To stop the program, use Ctrl+C.

Data Storage
Amoeba stores learned data in two files:
- strings.txt: Contains a list of learned Linux strings.
- stringdata.csv: Contains the usage information for each string.
You can review these files to see the learned strings and their associated usage data. Periodically, the program will save the database to these files.

Running in a Virtual Machine (Optional)
- To best isolate the program from sensitive data it may be best to run it within a virtualmachine.

Contributing
Contributions are welcome! If you have ideas for improving Amoeba or would like to report issues, please open an issue or submit a pull request on the GitHub repository.

