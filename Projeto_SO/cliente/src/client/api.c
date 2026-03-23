#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>

struct Session {
    int id;
    int req_pipe;
    int notif_pipe;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

static int write_all(int fd, const void *buf, size_t size) {
    size_t written = 0;
    const char *p = buf;
    while (written < size) {
        ssize_t w = write(fd, p + written, size - written);
        if (w < 0) return -1;
        written += w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t size) {
    size_t readb = 0;
    char *p = buf;
    while (readb < size) {
        ssize_t r = read(fd, p + readb, size - readb);
        if (r <= 0) return -1;
        readb += r;
    }
    return 0;
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    if (!req_pipe_path || !notif_pipe_path || !server_pipe_path) return 1;

    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

    // Remove old pipes if they exist
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);

    if (mkfifo(session.req_pipe_path, 0666) != 0) { perror("mkfifo req"); return 1; }
    if (mkfifo(session.notif_pipe_path, 0666) != 0) { perror("mkfifo notif"); return 1; }

    // Send connection request to server
    int serverfd = open(server_pipe_path, O_WRONLY);
    if (serverfd == -1) { perror("open server pipe"); return 1; }

    char message[2*MAX_PIPE_PATH_LENGTH + 1] = {0};
    message[0] = OP_CODE_CONNECT;
    strncpy(message + 1, session.req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(message + 1 + MAX_PIPE_PATH_LENGTH, session.notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    if (write_all(serverfd, message, sizeof(message)) < 0) {
        perror("write server pipe");
        close(serverfd);
        return 1;
    }
    close(serverfd);

    // Open notification pipe for reading (persistent)
    session.notif_pipe = open(session.notif_pipe_path, O_RDONLY);
    if (session.notif_pipe == -1) { perror("open notif pipe"); return 1; }

    // Open request pipe for writing (persistent)
    session.req_pipe = open(session.req_pipe_path, O_WRONLY);
    if (session.req_pipe == -1) { perror("open req pipe"); close(session.notif_pipe); return 1; }

    // Read server reply
    char reply[2];
    if (read_all(session.notif_pipe, reply, sizeof(reply)) < 0) {
        perror("read server reply");
        close(session.notif_pipe);
        close(session.req_pipe);
        return 1;
    }

    if (reply[0] != OP_CODE_CONNECT || reply[1] != 0) {
        perror("NOTIF");
        close(session.notif_pipe);
        close(session.req_pipe);
        return 1;
    }

    session.id = 1; // session ID placeholder
    return 0;
}

int pacman_play(char command) {
    if (session.id == -1 || session.req_pipe == -1){ return -1;}
    
    char message[2] = { OP_CODE_PLAY, command };
    if (write_all(session.req_pipe, message, sizeof(message)) < 0){    
        return -1;
    }
    
    return 0;
}

int pacman_disconnect() {
    char op = OP_CODE_DISCONNECT;
    write(session.req_pipe, &op, 1);
    close(session.req_pipe);
    close(session.notif_pipe);
    session.notif_pipe = -1;
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);

    session.id = -1;
    return 0;
}

Board receive_board_update(void) {
    Board board;
    memset(&board, 0, sizeof(Board));
    if (session.id == -1 || session.notif_pipe == -1){debug("NOTIF");return board;}

    char opcode;
    ssize_t r = read(session.notif_pipe, &opcode, sizeof(char));
    if (r <= 0){debug("NOTIF");return board;}
    if (opcode != OP_CODE_BOARD) return board;

    if (read_all(session.notif_pipe, &board.width, sizeof(int)) < 0 ||
        read_all(session.notif_pipe, &board.height, sizeof(int)) < 0 ||
        read_all(session.notif_pipe, &board.tempo, sizeof(int)) < 0 ||
        read_all(session.notif_pipe, &board.victory, sizeof(int)) < 0 ||
        read_all(session.notif_pipe, &board.game_over, sizeof(int)) < 0 ||
        read_all(session.notif_pipe, &board.accumulated_points, sizeof(int)) < 0) {
        debug("NOTIF");
        return board;
    }

    size_t size = board.width * board.height * sizeof(char);
    board.data = malloc(size);
    if (!board.data) return board;

    if (read_all(session.notif_pipe, board.data, size) < 0) {
        free(board.data);
        memset(&board, 0, sizeof(Board));
        return board;
    }

    return board;
}
