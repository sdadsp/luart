# Makefile for Serial Port Communication Application

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS = -lm

# Target executable
TARGET = luart

# Source files
SRC = main.c

# Object files
OBJ = $(SRC:.c=.o)

# Default rule
all: $(TARGET)

# Compile source files to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link object files to create executable
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

# Clean up build files
clean:
	rm -f $(OBJ) $(TARGET)
