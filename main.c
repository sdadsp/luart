/**
 * Serial Port Communication Application
 *
 * This application connects to a specified serial port, receives data,
 * displays it on screen (with special characters visible), and echoes it back
 * after a timeout period or when a CR character is received.
 *
 * Compile with GCC: gcc -Wall -Wextra -std=c11 -o luart main.c -lm
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
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <sys/select.h>

/* Default settings as macros */
#define DEFAULT_BAUDRATE 9600
#define DEFAULT_PARITY 'N'
#define DEFAULT_DATABITS 8
#define DEFAULT_STOPBITS 1
#define DEFAULT_TIMEOUT_MS 1000
#define DEFAULT_ADD_CRLF 1
#define MAX_BUFFER_SIZE 32
#define MAX_KEYBOARD_BUFFER 64

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_GREY    "\033[90m"

/* Global flags */
bool g_use_color = true;
bool g_show_timestamp = true;
bool g_show_prompt = false;
bool g_show_crlf = false;

/* Global variable for serial port fd to allow closing in signal handler */
int g_fd_serial = -1;

/* Helper: print color escape if colors are enabled */
static inline void set_color(const char *color) {
    if (g_use_color) printf("%s", color);
}

/* Helper: print timestamp [HH:MM:SS.mmm] in grey */
static void print_timestamp(void) {
    if (!g_show_timestamp) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    set_color(COLOR_GREY);
    printf("[%02d:%02d:%02d.%03ld] ",
           tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    set_color(COLOR_RESET);
}

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

/* Helper: display a single received character with special char formatting */
static void print_serial_char(char c) {
    switch (c) {
        case '\r':
            set_color(COLOR_YELLOW); printf("<CR>"); set_color(COLOR_GREEN);
            break;
        case '\n':
            set_color(COLOR_YELLOW); printf("<LF>"); set_color(COLOR_GREEN);
            break;
        case '\t':
            set_color(COLOR_YELLOW); printf("<TAB>"); set_color(COLOR_GREEN);
            break;
        case '\0':
            set_color(COLOR_YELLOW); printf("<NUL>"); set_color(COLOR_GREEN);
            break;
        default:
            if (c < 32 || c > 126) {
                set_color(COLOR_MAGENTA);
                printf("<0x%02X>", (unsigned char)c);
                set_color(COLOR_GREEN);
            } else {
                printf("%c", c);
            }
            break;
    }
}

/* Function prototypes */
void print_usage(char *program_name);
bool configure_port(int fd, int baudrate, char parity, int databits, int stopbits);
void close_port_and_exit(int sig);

int main(int argc, char *argv[]) {
    /* Serial port parameters */
    char *port_name = NULL;
    int baudrate = DEFAULT_BAUDRATE;
    char parity = DEFAULT_PARITY;
    int databits = DEFAULT_DATABITS;
    int stopbits = DEFAULT_STOPBITS;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    bool add_crlf = DEFAULT_ADD_CRLF;

    /* Set up signal handlers for clean exit */
    signal(SIGINT, close_port_and_exit);
    signal(SIGTERM, close_port_and_exit);

    /* Parse command line arguments */
    int opt;
    while ((opt = getopt(argc, argv, "p:b:a:d:S:t:l:c:s:i:f:h")) != -1) {
        switch (opt) {
            case 'p': port_name = optarg; break;
            case 'b': baudrate = atoi(optarg); break;
            case 'a': parity = optarg[0]; break;
            case 'd': databits = atoi(optarg); break;
            case 'S': stopbits = atoi(optarg); break;
            case 't': timeout_ms = atoi(optarg); break;
            case 'l': add_crlf = atoi(optarg); break;
            case 'c': g_use_color = atoi(optarg); break;
            case 's': g_show_timestamp = atoi(optarg); break;
            case 'i': g_show_prompt = atoi(optarg); break;
            case 'f': g_show_crlf = atoi(optarg); break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    /* Check if port is specified */
    if (port_name == NULL) {
        print_timestamp();
        set_color(COLOR_RED);
        printf("Error: Serial port must be specified with -p option\n");
        set_color(COLOR_RESET);
        print_usage(argv[0]);
        return 1;
    }

    /* Open the serial port */
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        print_timestamp();
        set_color(COLOR_RED);
        printf("Error opening port %s: %s\n", port_name, strerror(errno));
        set_color(COLOR_RESET);
        return 1;
    }

    /* Clear the O_NDELAY flag so reads block according to VMIN/VTIME */
    fcntl(fd, F_SETFL, 0);

    /* Save handle in global variable for signal handler */
    g_fd_serial = fd;

    /* Configure port parameters */
    if (!configure_port(fd, baudrate, parity, databits, stopbits)) {
        print_timestamp();
        set_color(COLOR_RED);
        printf("Failed to configure the port. Closing and exiting.\n");
        set_color(COLOR_RESET);
        close(fd);
        g_fd_serial = -1;
        return 1;
    }

    /* Startup messages */
    print_timestamp(); set_color(COLOR_YELLOW);
    printf("Connected to %s\n", port_name); set_color(COLOR_RESET);
    print_timestamp(); set_color(COLOR_YELLOW);
    printf("Baudrate: %d, Parity: %c, Data bits: %d, Stop bits: %d\n",
           baudrate, parity, databits, stopbits); set_color(COLOR_RESET);
    print_timestamp(); set_color(COLOR_YELLOW);
    printf("Timeout: %d ms, Add CR/LF: %s\n",
           timeout_ms, add_crlf ? "Yes" : "No"); set_color(COLOR_RESET);
    print_timestamp(); set_color(COLOR_YELLOW);
    printf("Waiting for data...\n"); set_color(COLOR_RESET);
    print_timestamp(); set_color(COLOR_YELLOW);
    printf("Type data and press Enter to send. Max %d bytes.\n", MAX_KEYBOARD_BUFFER - 1);
    set_color(COLOR_RESET);

    /* Main loop state */
    char rx_buf[MAX_BUFFER_SIZE + 1] = {0};
    unsigned long total_rx = 0;
    bool data_pending = false;
    unsigned long last_char_time = 0;
    bool prompt_active = false;
    bool skip_lf = false;
    bool running = true;

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; /* 50ms */
        int maxfd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            print_timestamp(); set_color(COLOR_RED);
            printf("select() error: %s\n", strerror(errno));
            set_color(COLOR_RESET);
            break;
        }

        /* === Serial port data === */
        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            ssize_t n = read(fd, rx_buf + total_rx, MAX_BUFFER_SIZE - total_rx);

            if (n > 0) {
                /* Skip lone LF after CR-triggered echo */
                if (skip_lf) {
                    skip_lf = false;
                    if (rx_buf[total_rx] == '\n') {
                        if (n > 1) {
                            memmove(rx_buf + total_rx, rx_buf + total_rx + 1, n - 1);
                        }
                        n--;
                    }
                    if (n <= 0) continue;
                }

                /* Check for CR */
                bool cr = false;
                for (ssize_t i = 0; i < n; i++) {
                    if (rx_buf[total_rx + i] == '\r') { cr = true; break; }
                }

                total_rx += (unsigned long)n;
                data_pending = true;
                last_char_time = get_tick_ms();

                /* On CR or buffer full: display + echo immediately */
                if (cr || total_rx >= MAX_BUFFER_SIZE) {
                    /* Move to new line if prompt was showing */
                    if (prompt_active) { printf("\n"); prompt_active = false; }

                    /* Determine display range (filter trailing CR/LF if needed) */
                    unsigned long display_len = total_rx;
                    if (!g_show_crlf && display_len > 0) {
                        if (rx_buf[display_len - 1] == '\n') display_len--;
                        if (display_len > 0 && rx_buf[display_len - 1] == '\r') display_len--;
                    }

                    /* Display received data */
                    print_timestamp();
                    set_color(COLOR_GREEN);
                    for (unsigned long i = 0; i < display_len; i++) {
                        print_serial_char(rx_buf[i]);
                    }
                    set_color(COLOR_RESET);
                    printf("\n");

                    /* Echo back: strip trailing CR/LF, then append CR+LF */
                    unsigned long echo_len = total_rx;
                    if (echo_len > 0 && rx_buf[echo_len - 1] == '\n') echo_len--;
                    if (echo_len > 0 && rx_buf[echo_len - 1] == '\r') echo_len--;
                    rx_buf[echo_len] = '\r';
                    rx_buf[echo_len + 1] = '\n';
                    echo_len += 2;
                    ssize_t w = write(fd, rx_buf, echo_len);
                    if (w < 0) {
                        print_timestamp(); set_color(COLOR_RED);
                        printf("Error writing to port\n"); set_color(COLOR_RESET);
                    } else if (w > 0) {
                        print_timestamp(); set_color(COLOR_YELLOW);
                        printf("Sent back %ld bytes\n", w); set_color(COLOR_RESET);
                    }

                    skip_lf = cr && !g_show_crlf;
                    total_rx = 0;
                    data_pending = false;
                    memset(rx_buf, 0, sizeof(rx_buf));
                    prompt_active = false;
                    fflush(stdout);
                }

            } else if (n < 0 && errno != EAGAIN) {
                print_timestamp(); set_color(COLOR_RED);
                printf("Error reading from port: %s\n", strerror(errno));
                set_color(COLOR_RESET);
                running = false;
            }
        }

        /* === Keyboard input === */
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char kb_buf[MAX_KEYBOARD_BUFFER];
            ssize_t n = read(STDIN_FILENO, kb_buf, MAX_KEYBOARD_BUFFER - 3);

            if (n > 0) {
                prompt_active = false;
                set_color(COLOR_RESET);

                /* Remove trailing newline */
                if (kb_buf[n - 1] == '\n') n--;

                if (n > 0) {
                    /* Add CR+LF */
                    kb_buf[n] = '\r';
                    kb_buf[n + 1] = '\n';
                    n += 2;

                    ssize_t w = write(fd, kb_buf, n);
                    if (w < 0) {
                        print_timestamp(); set_color(COLOR_RED);
                        printf("Error writing to port\n"); set_color(COLOR_RESET);
                    } else {
                        print_timestamp(); set_color(COLOR_YELLOW);
                        printf("Sent\n"); set_color(COLOR_RESET);
                    }
                }
                fflush(stdout);
            } else if (n == 0) {
                running = false;
            }
        }

        /* === Timeout echo === */
        if (data_pending &&
            (get_tick_ms() - last_char_time > (unsigned long)timeout_ms)) {
            /* Move to new line if prompt was showing */
            if (prompt_active) { printf("\n"); prompt_active = false; }

            /* Determine display range */
            unsigned long display_len = total_rx;
            if (!g_show_crlf && display_len > 0) {
                if (rx_buf[display_len - 1] == '\n') display_len--;
                if (display_len > 0 && rx_buf[display_len - 1] == '\r') display_len--;
            }

            /* Display received data */
            print_timestamp();
            set_color(COLOR_GREEN);
            for (unsigned long i = 0; i < display_len; i++) {
                print_serial_char(rx_buf[i]);
            }
            set_color(COLOR_RESET);
            printf("\n");

            /* Echo back: strip trailing CR/LF, then append CR+LF */
            unsigned long echo_len = total_rx;
            if (echo_len > 0 && rx_buf[echo_len - 1] == '\n') echo_len--;
            if (echo_len > 0 && rx_buf[echo_len - 1] == '\r') echo_len--;
            rx_buf[echo_len] = '\r';
            rx_buf[echo_len + 1] = '\n';
            echo_len += 2;
            ssize_t w = write(fd, rx_buf, echo_len);
            if (w < 0) {
                print_timestamp(); set_color(COLOR_RED);
                printf("Error writing to port\n"); set_color(COLOR_RESET);
            } else if (w > 0) {
                print_timestamp(); set_color(COLOR_YELLOW);
                printf("Sent back %ld bytes\n", w); set_color(COLOR_RESET);
            }

            total_rx = 0;
            data_pending = false;
            memset(rx_buf, 0, sizeof(rx_buf));
            prompt_active = false;
            fflush(stdout);
        }

        /* === Show prompt when idle === */
        if (!data_pending && !prompt_active) {
            if (g_show_prompt) {
                print_timestamp(); set_color(COLOR_YELLOW);
                printf("> ");
            }
            set_color(COLOR_CYAN);
            fflush(stdout);
            prompt_active = true;
        }
    }

    /* Cleanup */
    set_color(COLOR_RESET);
    printf("\n");
    print_timestamp(); set_color(COLOR_YELLOW);
    printf("Closing serial port and exiting...\n"); set_color(COLOR_RESET);
    if (fd >= 0) {
        close(fd);
        g_fd_serial = -1;
    }

    return 0;
}

/**
 * Signal handler for clean exit
 */
void close_port_and_exit(int sig) {
    printf("\n");
    print_timestamp();
    set_color(COLOR_YELLOW);
    printf("Received termination signal (%d). Closing serial port and exiting...\n", sig);
    set_color(COLOR_RESET);

    if (g_fd_serial >= 0) {
        close(g_fd_serial);
        g_fd_serial = -1;
    }

    exit(0);
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
    printf("  -S STOPBITS Stop bits (1 or 2, default: %d)\n", DEFAULT_STOPBITS);
    printf("  -t TIMEOUT  Timeout before echo in milliseconds (default: %d)\n", DEFAULT_TIMEOUT_MS);
    printf("  -l CRLF     Add CR/LF to output (0=no, 1=yes, default: %d)\n", DEFAULT_ADD_CRLF);
    printf("  -c COLOR    Colored output (0=off, 1=on, default: 1)\n");
    printf("  -s STAMP    Timestamp on lines (0=off, 1=on, default: 1)\n");
    printf("  -i INPUT    Show input prompt > (0=off, 1=on, default: 0)\n");
    printf("  -f CRLF     Show trailing <CR><LF> in received data (0=off, 1=on, default: 0)\n");
    printf("  -h          Display this help message\n");
}

/**
 * Configures the serial port parameters
 */
bool configure_port(int fd, int baudrate, char parity, int databits, int stopbits) {
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        printf("Error getting current serial parameters: %s\n", strerror(errno));
        return false;
    }

    /* Set raw mode as baseline */
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
        case 'N': case 'n': break;
        case 'E': case 'e': tty.c_cflag |= PARENB; break;
        case 'O': case 'o': tty.c_cflag |= (PARENB | PARODD); break;
        case 'M': case 'm': tty.c_cflag |= (PARENB | PARODD | CMSPAR); break;
        case 'S': case 's': tty.c_cflag |= (PARENB | CMSPAR); break;
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
    if (stopbits == 2) {
        tty.c_cflag |= CSTOPB;
    } else if (stopbits != 1) {
        printf("Invalid stop bits: %d (must be 1 or 2)\n", stopbits);
        return false;
    }

    /* Disable flow control */
    tty.c_cflag &= ~CRTSCTS;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    /* Set read timeout: VMIN=0, VTIME=1 -> return after 100ms if no data */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        printf("Error setting serial parameters: %s\n", strerror(errno));
        return false;
    }

    return true;
}
