#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>



#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2


typedef struct{
    board_t *board;
    int notif_fd;
}send_updates_arg_t;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    char* server_pipe_path;
    int max_games;
    char* level_path; 

} anfitria_thread_arg_t;

typedef struct { 
int notif_fd;
int req_fd;
char* req_path;
int id; 
int client_slot;
board_t *board; 
} pacman_thread_arg_t;

typedef struct{
    char* level_path;
    char notif_path[40];
    char req_path[40];
    int notif_fd;
    int req_fd;
    int client_id;
    int client_slot;

}sessao_thread_arg_t;

sem_t num_jogos;
int sessoes = 0;

volatile sig_atomic_t sigusr1_received = 0;

typedef struct {
    int active;
    int client_id;
    int points;
    int index;
} client_info_t;

client_info_t *clients = NULL;
pthread_mutex_t sessoes_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

int compare_clients(const void *a, const void *b) { 
    const client_info_t *c1 = a;
    const client_info_t *c2 = b;

    return c2->points - c1->points; // ordem decrescente
}

void sigusr1_handler(int sig) {
    (void) sig; 
    sigusr1_received = 1;
}


static int write_all(int fd, const void *buf, size_t size) {
    size_t written = 0;

    while (written < size) {
        ssize_t w = write(fd, buf + written, size - written);
        if (w <= 0)
            return -1;
        written += w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t size) {
  size_t readb = 0;
  char *p = buf;

  while (readb < size) {
        ssize_t r = read(fd, p + readb, size - readb);

        if (sigusr1_received) return -1;
        
        if (r < 0){
            if (errno == EINTR) continue;
            return -1;
        }
        readb += r;
  }
  return 0;
}


int receive_board_updates(board_t* board,
                           int victory, int endgame, int accumulated_points)
{
    if (board->notif_fd == -1) return -1;
    ssize_t size = sizeof(char) + 6*sizeof(int)
                 + (board->height * board->width * sizeof(char));

    char* message = malloc(size);
    ssize_t i = 0;

    message[i++] = 4;

    memcpy(&message[i], &board->width, sizeof(int));   i += sizeof(int);
    memcpy(&message[i], &board->height, sizeof(int));  i += sizeof(int);
    memcpy(&message[i], &board->tempo, sizeof(int));   i += sizeof(int);
    memcpy(&message[i], &victory, sizeof(int));        i += sizeof(int);
    memcpy(&message[i], &endgame, sizeof(int));        i += sizeof(int);
    memcpy(&message[i], &accumulated_points, sizeof(int));
    i += sizeof(int);

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            board_pos_t *pos = &board->board[idx];
            pthread_mutex_lock(&pos->lock);
            char c = ' ';

            if (pos->content == 'W') c = '#';
            else if (pos->content == 'M') c = 'M';
            else if (pos->content == 'P') c = 'C';
            else if (pos->has_dot) c = '.';
            else if (pos->has_portal) c = '@';

            pthread_mutex_unlock(&pos->lock);
            message[i++] = c;
        }
    }

    if (write_all(board->notif_fd, message, size) < 0) {
        //close(board->notif_fd);
        free(message);
        //perror("write notif");
        return -1;
    }

    free(message);
    return 0;
}

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void* send_updates_thread(void *arg){
    send_updates_arg_t* update_arg = (send_updates_arg_t*) arg;
    board_t *board = update_arg->board;
    while(1){
        int shutdown, victory, end_game, points;
        pthread_rwlock_rdlock(&board->state_lock);
        shutdown = board->thread_shutdown;
        victory = board->victory;
        end_game = board->end_game;
        points = board->points;
        pthread_rwlock_unlock(&board->state_lock);
        if (shutdown) return 0;
        if(receive_board_updates(board, victory, end_game, points) == -1) return 0;
        sleep_ms(15);
               
    }
    return 0;
}

void handle_top (int max_games){
    sigusr1_received = 0;
    pthread_mutex_lock(&clients_mutex);

    client_info_t temp[max_games];
    int count = 0;

    for (int i = 0; i < max_games; i++) {
        if (clients[i].active) {
            temp[count++] = clients[i];
        }
    }

    // Ordenar por pontuação (decrescente)
    qsort(temp, count, sizeof(client_info_t), compare_clients);

    int fd = open("top5_clients.txt",
                    O_WRONLY | O_CREAT | O_TRUNC,
                    0644);
    
    if (fd != -1) {
        char buffer[64];
        int len;
        int limit = (count < 5) ? count : 5;
        for (int i = 0; i < limit; i++) {
            len = snprintf(buffer, sizeof(buffer),
            "Player: %d | Points: %d\n",
            temp[i].client_id,
            temp[i].points);
            write(fd, buffer, len);
        }
    
        close(fd);
    }

    
    pthread_mutex_unlock(&clients_mutex);
    return ;
}

void* pacman_thread(void *arg)
{
    /* Bloquear SIGUSR1 nesta thread */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pacman_thread_arg_t* pac_arg = (pacman_thread_arg_t*) arg;
    board_t *board = pac_arg->board;
    pacman_t* pacman = &board->pacmans[0];

    pacman->points = board->points;

    int *retval = malloc(sizeof(int));
    if (!retval) pthread_exit(NULL);

    command_t cmd;
    cmd.command = '\0';

    
    if (board->req_fd == -1) {
        perror("open req fifo");
        pthread_rwlock_wrlock(&board->state_lock);
        board->end_game = 1;
        board->victory = 0;
        pthread_rwlock_unlock(&board->state_lock);
        *retval = QUIT_GAME;
        return retval;
    }

    while (true) {
        pthread_rwlock_rdlock(&board->state_lock);
        int end_game_flag = board->end_game;
        int pacman_alive = pacman->alive;
        pthread_rwlock_unlock(&board->state_lock);

        if (!pacman_alive || end_game_flag) {
            pthread_rwlock_wrlock(&board->state_lock);
            board->victory = 0;
            board->end_game = 1;
            pthread_rwlock_unlock(&board->state_lock);
            *retval = QUIT_GAME;
            break;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        int got_input = 0;
        char reply;
        ssize_t r = read(board->req_fd, &reply, 1);

        if (r == 1) {
            if (reply == 2 || reply == 'Q') {
                pthread_rwlock_wrlock(&board->state_lock);
                board->end_game = 1;
                board->victory = 0;
                pthread_rwlock_unlock(&board->state_lock);
                *retval = QUIT_GAME;
                break;
            }
            cmd.command = reply;
            got_input = 1;
        } else if (r == 0) {
            pthread_rwlock_wrlock(&board->state_lock);
            board->end_game = 1;
            board->victory = 0;
            pthread_rwlock_unlock(&board->state_lock);
            *retval = QUIT_GAME;
            break;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                perror("read fifo");
                *retval = QUIT_GAME;
                break;
            }
        }

        if (got_input) {
            int result = move_pacman(board, 0, &cmd);  

            if (result == REACHED_PORTAL) {
                pthread_rwlock_wrlock(&board->state_lock);
                *retval = NEXT_LEVEL;
                board->points = pacman->points;
                pthread_rwlock_unlock(&board->state_lock);
                break;
            }

            if (result == DEAD_PACMAN) {
                pthread_rwlock_wrlock(&board->state_lock);
                *retval = QUIT_GAME;
                board->points = pacman->points;
                board->victory = 0;
                board->end_game = 1;
                pthread_rwlock_unlock(&board->state_lock);
                break;
            }
        }

        pthread_rwlock_wrlock(&board->state_lock);
        board->points = pacman->points;
        board->victory = 0;
        board->end_game = 0;
        pthread_rwlock_unlock(&board->state_lock);

        pthread_mutex_lock(&clients_mutex);
        clients[pac_arg->client_slot].points = board->points;
        pthread_mutex_unlock(&clients_mutex);

        cmd.command = '\0';
    }

    pthread_rwlock_wrlock(&board->state_lock);
    board->thread_shutdown = 1;
    pthread_rwlock_unlock(&board->state_lock);
    return retval;
}



void* ghost_thread(void *arg) {

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (board->thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        pthread_rwlock_unlock(&board->state_lock);
        if(move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]) == DEAD_PACMAN){
            pthread_rwlock_wrlock(&board->state_lock);
            board->victory = 0;
            board->end_game = 1;
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
    }
}



void* sessao_thread(void *arg){

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sessao_thread_arg_t* sessao_arg = (sessao_thread_arg_t*) arg;

    srand((unsigned int)time(NULL));
    DIR* level_dir = opendir(sessao_arg->level_path);
    if (!level_dir) {
        perror("opendir level_path");
        sem_post(&num_jogos);
        pthread_exit(NULL);
    }
    int accumulated_points = 0;
    bool end_game = false;
    int victory, end = 0;
    board_t game_board; 
    game_board.req_fd = open(sessao_arg->req_path, O_RDONLY);
    game_board.notif_fd = sessao_arg->notif_fd;
    game_board.points = 0;
    game_board.victory = 0;
    game_board.end_game = 0;
    int primeiro = 0;
    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;
        if (strcmp(dot, ".lvl") == 0) {
            if (primeiro == 0){
                load_level(&game_board, entry->d_name, sessao_arg->level_path, accumulated_points);

                primeiro = 1;
            } else{
                unload_level(&game_board);
                load_level(&game_board, entry->d_name, sessao_arg->level_path, accumulated_points);
            }
            while(true) {
                end = 0;
                victory = 0;
                pthread_t /*ncurses_tid,*/ send_updates_tid;
                pthread_t pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
                game_board.thread_shutdown = 0;

                debug("Creating threads\n");
                pacman_thread_arg_t* pac_aux = malloc(sizeof(pacman_thread_arg_t));
                pac_aux->board = &game_board;
                pac_aux->req_path = malloc(strlen(sessao_arg->req_path) + 1);
                strcpy(pac_aux->req_path, sessao_arg->req_path);
                pac_aux->notif_fd = sessao_arg->notif_fd; 
                pac_aux->req_fd = sessao_arg->req_fd;
                size_t tamanho = strlen(sessao_arg->req_path);
                pac_aux->req_path[tamanho] = '\0';
                pac_aux->client_slot = sessao_arg->client_slot;
                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) pac_aux);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                send_updates_arg_t* update_aux = malloc(sizeof(send_updates_arg_t));
                update_aux->notif_fd = sessao_arg->notif_fd; 
                update_aux->board = &game_board;
                pthread_create(&send_updates_tid, NULL, send_updates_thread, (void*) update_aux);
                int *retval;
                pthread_join(pacman_tid, (void**)&retval);
                pthread_rwlock_wrlock(&game_board.state_lock);
                game_board.thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);
                pthread_join(send_updates_tid, NULL);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }
                free(pac_aux->req_path);
                free(pac_aux);
                free(update_aux);
                free(ghost_tids);
                int result = *retval;
                free(retval);
                if(result == NEXT_LEVEL) {
                    victory = 1;
                    break;
                }
                if(result == QUIT_GAME) {
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    end = 1;
                    break;
                }
                pthread_rwlock_wrlock(&game_board.state_lock);
            }
            accumulated_points = game_board.points;
            
        }
    }    
    end_game = true;
    end = 1;
    if (end == 1) printf("\n");
    receive_board_updates(&game_board, victory, end, accumulated_points); 
    pthread_rwlock_wrlock(&game_board.state_lock);
    game_board.notif_fd = -1;
    game_board.req_fd = -1;
    pthread_rwlock_unlock(&game_board.state_lock);
    unload_level(&game_board);

    


    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }
    sem_post(&num_jogos);
    pthread_mutex_lock(&sessoes_mutex);
    sessoes--;
    pthread_mutex_unlock(&sessoes_mutex);
    pthread_mutex_lock(&clients_mutex);
    clients[sessao_arg->client_slot].active = 0;
    pthread_mutex_unlock(&clients_mutex);
    free(sessao_arg->level_path);

    close(game_board.notif_fd);
    close(game_board.req_fd);
    free(sessao_arg);

    return 0;

}


void* anfitria_thread(void *arg){
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);


    anfitria_thread_arg_t* anfitria_arg = (anfitria_thread_arg_t*) arg;
    size_t path_len = 81;
    char buf[path_len];
    pthread_t sessao_tid[anfitria_arg->max_games];
    sem_init(&num_jogos, 0, anfitria_arg->max_games);
    int server_pipe = open(anfitria_arg->server_pipe_path, O_RDONLY);
    while(1){
        if (sigusr1_received) {
            handle_top(anfitria_arg->max_games);
        }
        int bytes_read = read_all(server_pipe, buf, path_len);

        if(bytes_read >= 0){
            if(buf[0] == 1){

                while (sem_wait(&num_jogos) == -1 && (sigusr1_received || errno == EINTR)) { 
                    if (sigusr1_received) {
                        handle_top(anfitria_arg->max_games);
                    }
                }
                char* client_id;
                sessao_thread_arg_t* aux = malloc(sizeof(sessao_thread_arg_t));
                memcpy(aux->req_path,  buf + 1, 39);
                memcpy(aux->notif_path, buf + 41, 39);
                size_t tmp = strlen(aux->req_path) - 13;
                client_id = malloc((tmp+1) * sizeof(char));
                for(size_t i = 0, t = 5; i < tmp; i++,t++){
                    client_id[i] = (aux->req_path)[t];
                }
                client_id[tmp] = '\0';
                aux->client_id = atoi(client_id);
                free(client_id);
                aux->level_path = malloc(strlen(anfitria_arg->level_path) + 1);
                strcpy(aux->level_path,anfitria_arg->level_path);
                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < anfitria_arg->max_games; i++){
                    if(clients[i].active == 0){
                        clients[i].active = 1;
                        clients[i].client_id = aux->client_id;
                        clients[i].points = 0;
                        clients[i].index = i;
                        aux->client_slot = i;
                        break;
                    }
                }
                int notif = open(aux->notif_path, O_WRONLY);
                aux->notif_fd = notif;
                if(notif == -1) return NULL;
                pthread_mutex_unlock(&clients_mutex);
                pthread_create(&sessao_tid[sessoes], NULL, sessao_thread, (void*) aux);
                pthread_mutex_lock(&sessoes_mutex);
                sessoes++;
                pthread_mutex_unlock(&sessoes_mutex);
                char reply[2];
                reply[0] = 1;
                reply[1] = 0;
                write_all(notif, reply, 2);
                
                
            }
        } else if(sigusr1_received) {
            handle_top(anfitria_arg->max_games);
        }
    }
}



int main(int argc, char** argv) {

    signal(SIGPIPE, SIG_IGN);
    if (argc != 4){
        printf("Argumentos insuficientes");
        return -1;
    }
    char* named_pipe = malloc(strlen(argv[3]) + 1);
    int size = strlen (argv[3]);
    strcpy(named_pipe, argv[3]);
    named_pipe[size] = '\0';
    size_t tamanho = strlen(argv[2]);
    int max_games = 0;
    if (tamanho == 1){ max_games = argv[2][0] - '0';} 
    else{ 
        max_games = ((argv[2][0] - '0') * 10) + (argv[2][1] - '0');
    }
    clients = calloc(max_games, sizeof(client_info_t));


    if (unlink(named_pipe) != 0 && errno != ENOENT) {
        perror("[ERR]: unlink(%s) failed");
        return 1;
    }
    if (mkfifo(named_pipe, 0666) != 0) {
        perror("[ERR]: mkfifo failed");
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    struct sigaction sa;
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; 
    sigaction(SIGUSR1, &sa, NULL);

    pthread_t anfitria_tid;
    anfitria_thread_arg_t* anfiarg = malloc (sizeof(anfitria_thread_arg_t));
    anfiarg->level_path = malloc(strlen(argv[1])+ 1);
    strcpy(anfiarg->level_path, argv[1]);
    size = strlen(argv[1]);
    anfiarg->level_path[size] = '\0';
    anfiarg->server_pipe_path = malloc(strlen(named_pipe) + 1);
    strcpy(anfiarg->server_pipe_path, named_pipe);
    size = strlen(named_pipe);
    anfiarg->server_pipe_path[size] = '\0';
    anfiarg->max_games = max_games;
    open_debug_file("debug.log");
    

    pthread_create(&anfitria_tid, NULL, anfitria_thread, (void*) anfiarg);
    pthread_join(anfitria_tid, NULL);

    sem_destroy(&num_jogos);

    close_debug_file();
    return 0;

}
