CC 	= gcc
CFLAGS	= -Wall -Wextra 
TARGET  = sayq
SRC	= main.c


all:
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm $(TARGET)
