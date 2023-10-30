CC = gcc
CFLAGS = -Wall

TARGET = amoeba
INSTALL_DIR = /bin

$(TARGET): Amoeba.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	sudo cp $(TARGET) $(INSTALL_DIR)

clean:
	rm -f $(TARGET)
