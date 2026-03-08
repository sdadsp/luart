/**
 * Serial Port Communication Application
 *
 * This application connects to a specified serial port, receives data,
 * displays it on screen (with special characters visible), and echoes it back
 * after a timeout period or when a CR character is received.
 *
 * Compile with GCC: gcc -Wall -Wextra -std=c11 -o luart main.c -lpthread -lm
 *
 * @author sdadsp
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>

/* Default settings as macros */
#define DEFAULT_BAUDRATE 9600
#define DEFAULT_PARITY 'N'
#define DEFAULT_DATABITS 8
#define DEFAULT_STOPBITS 1
#define DEFAULT_TIMEOUT_MS 1000
#define DEFAULT_ADD_CRLF 0
#define MAX_BUFFER_SIZE 32
#define MAX_KEYBOARD_BUFFER 64

/* Global variable for serial port fd to allow closing in signal handler */
int g_fd_serial = -1;

/* Function prototypes */
void print_usage(char *program_name);
bool configure_port(int fd, int baudrate, char parity, int databits, int stopbits);
void close_port_and_exit(int sig);
void *keyboard_thread(void *arg);

/* Helper: get monotonic time in milliseconds */
static unsigned long get_tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Helper: map integer baudrate to speed_t constant */
static speed_t baudrate_to_speed(int baudrate) {
    switch (baudrate) {
        case 1200:    return B1200;
        case 2400:    return B2400;
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 500000:  return B500000;
        case 576000:  return B576000;
        case 921600:  return B921600;
        case 1000000: return B1000000;
        default:      return B0; /* invalid */
    }
}

/* Structure to pass data to the keyboard thread */
typedef struct {
    int fd;
    bool *running;
} ThreadParams;

int main(int argc, char *argv[]) {
    /* Serial port parameters */
    char *port_name = NULL;
    int baudrate = DEFAULT_BAUDRATE;
    char parity = DEFAULT_PARITY;
    int databits = DEFAULT_DATABITS;
    int stopbits = DEFAULT_STOPBITS;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    bool add_crlf = DEFAULT_ADD_CRLF;

    /* Variables for serial port handling */
    int fd;

    /* Buffer for data */
    char buffer[MAX_BUFFER_SIZE + 1] = {0}; /* +1 for null terminator */
    ssize_t bytes_read = 0;

    /* Command line options */
    int opt;

    /* Set up signal handlers for clean exit */
    signal(SIGINT, close_port_and_exit);   /* Ctrl+C */
    signal(SIGTERM, close_port_and_exit);  /* Termination request */

    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "p:b:a:d:s:t:c:h")) != -1) {
        switch (opt) {
            case 'p': /* Port */
                port_name = optarg;
                break;
            case 'b': /* Baudrate */
                baudrate = atoi(optarg);
                break;
            case 'a': /* Parity */
                parity = optarg[0];
                break;
            case 'd': /* Data bits */
                databits = atoi(optarg);
                break;
            case 's': /* Stop bits */
                stopbits = atoi(optarg);
                break;
            case 't': /* Timeout */
                timeout_ms = atoi(optarg);
                break;
            case 'c': /* Add CR/LF */
                add_crlf = atoi(optarg);
                break;
            case 'h': /* Help */
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    /* Check if port is specified */
    if (port_name == NULL) {
        printf("Error: Serial port must be specified with -p option\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Open the serial port */
    fd = open(port_name, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        printf("Error opening port %s: %s\n", port_name, strerror(errno));
        return 1;
    }

    /* Clear the O_NDELAY flag so reads block according to VMIN/VTIME */
    fcntl(fd, F_SETFL, 0);

    /* Save handle in global variable for signal handler */
    g_fd_serial = fd;

    /* Configure port parameters */
    if (!configure_port(fd, baudrate, parity, databits, stopbits)) {
        printf("Failed to configure the port. Closing and exiting.\n");
        close(fd);
        g_fd_serial = -1;
        return 1;
    }

    printf("Connected to %s\n", port_name);
    printf("Baudrate: %d, Parity: %c, Data bits: %d, Stop bits: %d\n",
           baudrate, parity, databits, stopbits);
    printf("Timeout: %d ms, Add CR/LF: %s\n",
           timeout_ms, add_crlf ? "Yes" : "No");
    printf("Waiting for data...\n");
    printf("Type data and press Enter to send. Max %d bytes.\n", MAX_KEYBOARD_BUFFER - 1);

    /* Flag to control threads */
    bool running = true;

    /* Create thread for keyboard input */
    ThreadParams params = { fd, &running };
    pthread_t kb_thread;
    if (pthread_create(&kb_thread, NULL, keyboard_thread, &params) != 0) {
        printf("Error creating keyboard thread\n");
        close(fd);
        g_fd_serial = -1;
        return 1;
    }

    /* Main loop */
    bool data_pending = false;
    unsigned long total_bytes_read = 0;
    unsigned long last_char_time = 0;

    while (running) {
        /* Read data from serial port */
        bytes_read = read(fd, buffer + total_bytes_read, MAX_BUFFER_SIZE - total_bytes_read);

        if (bytes_read > 0) {
            /* We received some data */
            data_pending = true;
            last_char_time = get_tick_ms();

            /* Check for CR character */
            bool cr_received = false;
            for (unsigned long i = total_bytes_read; i < total_bytes_read + (unsigned long)bytes_read; i++) {
                if (buffer[i] == '\r') {
                    cr_received = true;
                    break;
                }
            }

            /* Update total bytes received */
            total_bytes_read += (unsigned long)bytes_read;

            /* Null terminate the string */
            buffer[total_bytes_read] = '\0';

            /* Display received data with special characters visible */
            for (unsigned long i = total_bytes_read - (unsigned long)bytes_read; i < total_bytes_read; i++) {
                char c = buffer[i];
                switch (c) {
                    case '\r':
                        printf("<CR>");
                        break;
                    case '\n':
                        printf("<LF>");
                        break;
                    case '\t':
                        printf("<TAB>");
                        break;
                    case '\0':
                        printf("<NUL>");
                        break;
                    default:
                        if (c < 32 || c > 126) {
                            /* Non-printable character */
                            printf("<0x%02X>", (unsigned char)c);
                        } else {
                            /* Regular printable character */
                            printf("%c", c);
                        }
                        break;
                }
            }
            fflush(stdout);

            /* If CR was received, send immediately */
            if (cr_received) {
                /* Add CR LF to screen output */
                printf("\r\n");

                ssize_t bytes_written = write(fd, buffer, total_bytes_read);
                if (bytes_written < 0) {
                    printf("\nError writing to port\n");
                } else if (bytes_written > 0) {
                    printf("Sent back %ld bytes\r\n", bytes_written);
                }

                /* Reset for the next data */
                data_pending = false;
                total_bytes_read = 0;
                memset(buffer, 0, sizeof(buffer));
                continue;
            }
        } else if (bytes_read < 0 && errno != EAGAIN) {
            printf("Error reading from port: %s\n", strerror(errno));
            running = false;
            break;
        }

        /* Check if we need to send data back */
        if (data_pending &&
            (get_tick_ms() - last_char_time > (unsigned long)timeout_ms || total_bytes_read >= MAX_BUFFER_SIZE)) {
            /* Always add CR LF after timeout */
            printf("\r\n");

            /* Timeout occurred or buffer is full - send the data back */
            ssize_t bytes_written = write(fd, buffer, total_bytes_read);
            if (bytes_written < 0) {
                printf("\nError writing to port\n");
            } else if (bytes_written > 0) {
                printf("Sent back %ld bytes\r\n", bytes_written);
            }

            /* Reset for the next data */
            data_pending = false;
            total_bytes_read = 0;
            memset(buffer, 0, sizeof(buffer));
        }

        /* Small delay to prevent CPU hogging */
        usleep(10000);
    }

    /* Signal thread to stop and wait for it */
    running = false;
    pthread_join(kb_thread, NULL);

    /* Close the serial port */
    printf("Closing serial port and exiting...\n");
    if (fd >= 0) {
        close(fd);
        g_fd_serial = -1;
    }

    return 0;
}

/**
 * Signal handler for clean exit
 * This function will be called when the program receives termination signals
 */
void close_port_and_exit(int sig) {
    printf("\nReceived termination signal (%d). Closing serial port and exiting...\n", sig);

    /* Close serial port if it's open */
    if (g_fd_serial >= 0) {
        close(g_fd_serial);
        g_fd_serial = -1;
    }

    /* Exit program */
    exit(0);
}

/**
 * Thread function for handling keyboard input
 */
void *keyboard_thread(void *arg) {
    ThreadParams *params = (ThreadParams *)arg;
    int fd = params->fd;
    bool *running = params->running;

    char input_buffer[MAX_KEYBOARD_BUFFER];

    while (*running) {
        /* Print prompt */
        printf("Send> ");
        fflush(stdout);

        /* Read a line from keyboard */
        if (fgets(input_buffer, MAX_KEYBOARD_BUFFER, stdin) == NULL) {
            /* Error or EOF */
            break;
        }

        /* Remove trailing newline if present */
        size_t len = strlen(input_buffer);
        if (len > 0 && input_buffer[len-1] == '\n') {
            input_buffer[len-1] = '\0';
            len--;
        }

        /* Skip if empty */
        if (len == 0) {
            continue;
        }

        /* Add CR+LF to the end of the line */
        if (len < MAX_KEYBOARD_BUFFER - 2) {
            input_buffer[len] = '\r';
            input_buffer[len+1] = '\n';
            len += 2;
        } else {
            /* Buffer is too full for both CR and LF, add just CR */
            input_buffer[MAX_KEYBOARD_BUFFER-2] = '\r';
            len = MAX_KEYBOARD_BUFFER-1;
        }

        /* Send data to serial port */
        ssize_t bytes_written = write(fd, input_buffer, len);
        if (bytes_written < 0) {
            printf("\nError writing to port\n");
            continue;
        }

        printf("Sent %ld bytes\r\n", bytes_written);
    }

    return NULL;
}

/**
 * Displays usage information
 */
void print_usage(char *program_name) {
    printf("Usage: %s -p PORT [options]\n", program_name);
    printf("Options:\n");
    printf("  -p PORT     Serial port (e.g., /dev/ttyUSB0, /dev/ttyACM0)\n");
    printf("  -b RATE     Baudrate (default: %d)\n", DEFAULT_BAUDRATE);
    printf("  -a PARITY   Parity (N=none, E=even, O=odd, default: %c)\n", DEFAULT_PARITY);
    printf("  -d DATABITS Data bits (7 or 8, default: %d)\n", DEFAULT_DATABITS);
    printf("  -s STOPBITS Stop bits (1 or 2, default: %d)\n", DEFAULT_STOPBITS);
    printf("  -t TIMEOUT  Timeout before echo in milliseconds (default: %d)\n", DEFAULT_TIMEOUT_MS);
    printf("  -c CRLF     Add CR/LF to output (0=no, 1=yes, default: %d)\n", DEFAULT_ADD_CRLF);
    printf("  -h          Display this help message\n");
}

/**
 * Configures the serial port parameters
 */
bool configure_port(int fd, int baudrate, char parity, int databits, int stopbits) {
    struct termios tty;

    /* Get current settings */
    if (tcgetattr(fd, &tty) != 0) {
        printf("Error getting current serial parameters: %s\n", strerror(errno));
        return false;
    }

    /* Set raw mode as baseline (no echo, no canonical processing, no signals) */
    cfmakeraw(&tty);

    /* Set baudrate */
    speed_t speed = baudrate_to_speed(baudrate);
    if (speed == B0) {
        printf("Unsupported baudrate: %d\n", baudrate);
        return false;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    /* Enable receiver, local mode */
    tty.c_cflag |= (CLOCAL | CREAD);

    /* Set parity */
    tty.c_cflag &= ~(PARENB | PARODD);
    switch (parity) {
        case 'N':
        case 'n':
            /* No parity - already cleared above */
            break;
        case 'E':
        case 'e':
            tty.c_cflag |= PARENB;
            break;
        case 'O':
        case 'o':
            tty.c_cflag |= (PARENB | PARODD);
            break;
        case 'M':
        case 'm':
            tty.c_cflag |= (PARENB | PARODD | CMSPAR);
            break;
        case 'S':
        case 's':
            tty.c_cflag |= (PARENB | CMSPAR);
            break;
        default:
            printf("Invalid parity setting: %c\n", parity);
            return false;
    }

    /* Set data bits */
    tty.c_cflag &= ~CSIZE;
    switch (databits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        case 8: tty.c_cflag |= CS8; break;
        default:
            printf("Invalid data bits: %d (must be 5-8)\n", databits);
            return false;
    }

    /* Set stop bits */
    tty.c_cflag &= ~CSTOPB;
    switch (stopbits) {
        case 1:
            /* 1 stop bit - CSTOPB already cleared */
            break;
        case 2:
            tty.c_cflag |= CSTOPB;
            break;
        default:
            printf("Invalid stop bits: %d (must be 1 or 2)\n", stopbits);
            return false;
    }

    /* Disable flow control */
    tty.c_cflag &= ~CRTSCTS;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    /* Set read timeout: VMIN=0, VTIME=1 → return after 100ms if no data */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    /* Apply settings */
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        printf("Error setting serial parameters: %s\n", strerror(errno));
        return false;
    }

    return true;
}

/**
 * Process and display received data
 *
 * Note: This function is no longer used in the main loop due to the improved
 * timeout logic, but is kept for potential future use.
 */
void process_received_data(char *buffer, int bytes_read, bool add_crlf) {
    printf("Received %d bytes: ", bytes_read);

    /* Display data */
    for (int i = 0; i < bytes_read; i++) {
        printf("%c", buffer[i]);
    }

    /* Add CR/LF if required */
    if (add_crlf) {
        printf("\r\n");
    }
}
