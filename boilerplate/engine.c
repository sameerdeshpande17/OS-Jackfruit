/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


#include <sys/select.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;


    void      *clone_stack;
    int        log_read_fd;
    pthread_t  log_reader_thd;
    int        run_client_fd;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;


typedef struct {
    int              fd;                       /* read-end of the log pipe */
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} log_reader_args_t;


static volatile sig_atomic_t g_sigchld  = 0;
static volatile sig_atomic_t g_shutdown = 0;


static void handle_sigchld(int sig)  { (void)sig; g_sigchld  = 1; }
static void handle_shutdown(int sig) { (void)sig; g_shutdown = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO: (FILLED IN)
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    
    pthread_mutex_lock(&buffer->mutex);

    
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO: (FILLED IN)
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    
    pthread_mutex_lock(&buffer->mutex);

    
    while (buffer->count == 0) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO: (FILLED IN)
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;

    
    struct { char id[CONTAINER_ID_LEN]; int fd; } cache[64];
    int ncache = 0, i;
    memset(cache, 0, sizeof(cache));
    for (i = 0; i < 64; i++) cache[i].fd = -1;

    
    while (bounded_buffer_pop(buffer, &item) == 0) {
        int fd = -1;

        
        for (i = 0; i < ncache; i++) {
            if (strncmp(cache[i].id, item.container_id, CONTAINER_ID_LEN) == 0) {
                fd = cache[i].fd;
                break;
            }
        }

        
        if (fd < 0 && ncache < 64) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
            fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                strncpy(cache[ncache].id, item.container_id, CONTAINER_ID_LEN - 1);
                cache[ncache].fd = fd;
                ncache++;
            }
        }

        
        if (fd >= 0) {
            ssize_t off = 0, total = (ssize_t)item.length;
            while (off < total) {
                ssize_t n = write(fd, item.data + off, (size_t)(total - off));
                if (n <= 0) break;
                off += n;
            }
        }
    }

    
    for (i = 0; i < ncache; i++)
        if (cache[i].fd >= 0) close(cache[i].fd);

    return NULL;
}


static void *log_reader_thread(void *arg)
{
    log_reader_args_t *a = (log_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, a->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(a->fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        if (bounded_buffer_push(a->buffer, &item) != 0)
            break; /* shutdown began – stop pushing */
    }

    close(a->fd);
    free(a);
    return NULL;
}

/*
 * TODO: (FILLED IN)
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char cmd_copy[CHILD_COMMAND_LEN];
    char *argv_arr[64];
    char *tok, *saveptr;
    int   argc = 0;

    
    sethostname(cfg->id, strlen(cfg->id));

    
    {
        char proc_path[PATH_MAX];
        snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
        mkdir(proc_path, 0555); /* silently ignores EEXIST */
        mount("proc", proc_path, "proc",
              MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);
    }

    
    if (chroot(cfg->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/")          < 0) { perror("chdir");  return 1; }

    
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    if (cfg->log_write_fd > STDERR_FILENO)
        close(cfg->log_write_fd);

    
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    
    strncpy(cmd_copy, cfg->command, CHILD_COMMAND_LEN - 1);
    cmd_copy[CHILD_COMMAND_LEN - 1] = '\0';
    tok = strtok_r(cmd_copy, " ", &saveptr);
    while (tok && argc < 63) {
        argv_arr[argc++] = tok;
        tok = strtok_r(NULL, " ", &saveptr);
    }
    argv_arr[argc] = NULL;

    if (argc == 0) { fprintf(stderr, "empty command\n"); return 1; }

    execvp(argv_arr[0], argv_arr);
    perror("execvp");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}


static container_record_t *find_by_id(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}


static container_record_t *find_by_pid(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (c->host_pid == pid) return c;
        c = c->next;
    }
    return NULL;
}


static void send_response(int fd, int status, const char *msg)
{
    control_response_t resp;
    resp.status = status;
    snprintf(resp.message, sizeof(resp.message), "%s", msg ? msg : "");
    write(fd, &resp, sizeof(resp));
}


static void reap_children(supervisor_ctx_t *ctx)
{
    pid_t pid;
    int   status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *c;

        pthread_mutex_lock(&ctx->metadata_lock);
        c = find_by_pid(ctx, pid);
        if (c) {
            if (WIFEXITED(status)) {
                c->state     = CONTAINER_EXITED;
                c->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                /* SIGKILL = hard-limit kill by the kernel module */
                c->state = (WTERMSIG(status) == SIGKILL)
                           ? CONTAINER_KILLED : CONTAINER_STOPPED;
            }
            if (c->clone_stack) { free(c->clone_stack); c->clone_stack = NULL; }
            /* respond to a blocked 'run' client if one is pending */
            if (c->run_client_fd >= 0) {
                char msg[CONTROL_MESSAGE_LEN];
                snprintf(msg, sizeof(msg),
                         "container '%s' finished (code=%d signal=%d)\n",
                         c->id, c->exit_code, c->exit_signal);
                send_response(c->run_client_fd, 0, msg);
                close(c->run_client_fd);
                c->run_client_fd = -1;
            }
            fprintf(stderr, "[supervisor] reaped '%s' pid=%d state=%s\n",
                    c->id, pid, state_to_string(c->state));
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (ctx->monitor_fd >= 0 && c)
            unregister_from_monitor(ctx->monitor_fd, c->id, pid);
    }
}


static void handle_start_run(supervisor_ctx_t *ctx,
                              const control_request_t *req,
                              int client_fd, int is_run)
{
    container_record_t *c;
    child_config_t     *cfg;
    log_reader_args_t  *rdr;
    char               *stack, *stack_top;
    int                 logpipe[2];
    pid_t               child_pid;

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_by_id(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_response(client_fd, -1, "container id already exists");
        if (!is_run) close(client_fd);
        return;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(logpipe) < 0) {
        perror("pipe");
        send_response(client_fd, -1, "pipe() failed");
        if (!is_run) close(client_fd);
        return;
    }

    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(logpipe[0]); close(logpipe[1]);
        send_response(client_fd, -1, "malloc stack failed");
        if (!is_run) close(client_fd);
        return;
    }
    stack_top = stack + STACK_SIZE; /* stack grows downward on x86 */

    cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        free(stack); close(logpipe[0]); close(logpipe[1]);
        send_response(client_fd, -1, "malloc cfg failed");
        if (!is_run) close(client_fd);
        return;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = logpipe[1];

    /* clone with new PID, UTS, and mount namespaces */
    child_pid = clone(child_fn, stack_top,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      (void *)cfg);
    if (child_pid < 0) {
        perror("clone");
        free(cfg); free(stack); close(logpipe[0]); close(logpipe[1]);
        send_response(client_fd, -1, "clone() failed");
        if (!is_run) close(client_fd);
        return;
    }

    close(logpipe[1]); /* parent keeps only the read end */
    free(cfg);

    c = calloc(1, sizeof(*c));
    if (!c) {
        kill(child_pid, SIGKILL); waitpid(child_pid, NULL, 0);
        free(stack); close(logpipe[0]);
        send_response(client_fd, -1, "calloc record failed");
        if (!is_run) close(client_fd);
        return;
    }
    strncpy(c->id, req->container_id, CONTAINER_ID_LEN - 1);
    snprintf(c->log_path, sizeof(c->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);
    c->host_pid         = child_pid;
    c->started_at       = time(NULL);
    c->state            = CONTAINER_RUNNING;
    c->soft_limit_bytes = req->soft_limit_bytes;
    c->hard_limit_bytes = req->hard_limit_bytes;
    c->exit_code        = -1;
    c->clone_stack      = stack;
    c->log_read_fd      = logpipe[0];
    c->run_client_fd    = is_run ? client_fd : -1;

    /* start the per-container log-reader (producer) thread */
    rdr = malloc(sizeof(*rdr));
    if (rdr) {
        rdr->fd     = logpipe[0];
        rdr->buffer = &ctx->log_buffer;
        strncpy(rdr->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        if (pthread_create(&c->log_reader_thd, NULL, log_reader_thread, rdr) != 0) {
            free(rdr);
            close(logpipe[0]);
        }
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    c->next = ctx->containers;
    ctx->containers = c;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, c->id, child_pid,
                               c->soft_limit_bytes, c->hard_limit_bytes);

    fprintf(stderr, "[supervisor] started '%s' pid=%d\n", c->id, child_pid);

    if (!is_run) {
        char msg[CONTROL_MESSAGE_LEN];
        snprintf(msg, sizeof(msg), "started '%s' pid=%d\n", c->id, child_pid);
        send_response(client_fd, 0, msg);
        close(client_fd);
    }
}


static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    container_record_t *c;
    char line[512];

    send_response(client_fd, 0, "");
    snprintf(line, sizeof(line), "%-16s %8s  %-10s  %9s  %9s  %s\n",
             "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "STARTED");
    write(client_fd, line, strlen(line));
    snprintf(line, sizeof(line),
             "---------------------------------------------------------------------\n");
    write(client_fd, line, strlen(line));

    pthread_mutex_lock(&ctx->metadata_lock);
    for (c = ctx->containers; c; c = c->next) {
        char tsbuf[32];
        strftime(tsbuf, sizeof(tsbuf), "%H:%M:%S", localtime(&c->started_at));
        snprintf(line, sizeof(line), "%-16s %8d  %-10s  %9lu  %9lu  %s\n",
                 c->id, c->host_pid, state_to_string(c->state),
                 c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20, tsbuf);
        write(client_fd, line, strlen(line));
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    close(client_fd);
}


static void handle_logs(supervisor_ctx_t *ctx,
                         const control_request_t *req, int client_fd)
{
    container_record_t *c;
    char log_path[PATH_MAX];
    int  log_fd;
    char buf[4096];
    ssize_t n;

    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_by_id(ctx, req->container_id);
    if (c) strncpy(log_path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) { send_response(client_fd, -1, "no such container"); close(client_fd); return; }

    send_response(client_fd, 0, "");
    log_fd = open(log_path, O_RDONLY);
    if (log_fd < 0) {
        const char *msg = "(log not yet available)\n";
        write(client_fd, msg, strlen(msg));
    } else {
        while ((n = read(log_fd, buf, sizeof(buf))) > 0)
            write(client_fd, buf, (size_t)n);
        close(log_fd);
    }
    close(client_fd);
}


static void handle_stop(supervisor_ctx_t *ctx,
                         const control_request_t *req, int client_fd)
{
    container_record_t *c;

    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_by_id(ctx, req->container_id);
    if (c && c->state == CONTAINER_RUNNING) {
        kill(c->host_pid, SIGTERM);
        c->state = CONTAINER_STOPPED;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (c) {
        char msg[CONTROL_MESSAGE_LEN];
        snprintf(msg, sizeof(msg), "SIGTERM sent to '%s' pid=%d\n",
                 req->container_id, c->host_pid);
        send_response(client_fd, 0, msg);
    } else {
        send_response(client_fd, -1, "no such running container");
    }
    close(client_fd);
}


static void handle_client(supervisor_ctx_t *ctx)
{
    int client_fd = accept(ctx->server_fd, NULL, NULL);
    control_request_t req;
    ssize_t n;

    if (client_fd < 0) return;
    n = read(client_fd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req)) { close(client_fd); return; }

    switch (req.kind) {
    case CMD_START: handle_start_run(ctx, &req, client_fd, 0); break;
    case CMD_RUN:   handle_start_run(ctx, &req, client_fd, 1); break;
    case CMD_PS:    handle_ps(ctx, client_fd);                  break;
    case CMD_LOGS:  handle_logs(ctx, &req, client_fd);          break;
    case CMD_STOP:  handle_stop(ctx, &req, client_fd);          break;
    default:
        send_response(client_fd, -1, "unknown command");
        close(client_fd);
    }
}

/*
 * TODO: (FILLED IN)
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO: (FILLED IN)
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */

    
    mkdir(LOG_DIR, 0755);
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] /dev/container_monitor not found "
                        "(memory monitoring disabled)\n");

    
    {
        struct sockaddr_un addr;
        ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

        unlink(CONTROL_PATH); /* remove stale socket file if present */
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); goto cleanup;
        }
        if (listen(ctx.server_fd, 16) < 0) { perror("listen"); goto cleanup; }
    }

    
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_flags   = SA_RESTART;
        sa.sa_handler = handle_sigchld;
        sigaction(SIGCHLD, &sa, NULL);
        sa.sa_handler = handle_shutdown;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("pthread_create logger"); goto cleanup; }

    fprintf(stderr, "[supervisor] ready  socket=%s  rootfs=%s\n",
            CONTROL_PATH, rootfs);

    
    while (!g_shutdown) {
        fd_set rfds;
        struct timeval tv = {0, 100000}; /* 100 ms */
        int ret;

        if (g_sigchld) { g_sigchld = 0; reap_children(&ctx); }

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        ret = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0 && errno != EINTR) { perror("select"); break; }
        if (ret > 0 && FD_ISSET(ctx.server_fd, &rfds))
            handle_client(&ctx);
    }

    
    fprintf(stderr, "[supervisor] shutting down...\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c;
        for (c = ctx.containers; c; c = c->next) {
            if (c->run_client_fd >= 0) {
                send_response(c->run_client_fd, -1, "supervisor shutting down");
                close(c->run_client_fd);
                c->run_client_fd = -1;
            }
            if (c->state == CONTAINER_RUNNING) {
                kill(c->host_pid, SIGTERM);
                c->state = CONTAINER_STOPPED;
            }
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(1);
    reap_children(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c;
        for (c = ctx.containers; c; c = c->next)
            if (c->log_reader_thd) pthread_join(c->log_reader_thd, NULL);
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers, *nxt;
        while (c) { nxt = c->next; if (c->clone_stack) free(c->clone_stack); free(c); c = nxt; }
        ctx.containers = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    fprintf(stderr, "[supervisor] clean exit.\n");
    return 0;

cleanup:
    
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.server_fd >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    return 1;
}

/*
 * TODO: (FILLED IN)
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sockfd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;
    char buf[4096];


    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sockfd);
        return 1;
    }

  
    n = write(sockfd, req, sizeof(*req));
    if (n != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(sockfd);
        return 1;
    }

  
    n = read(sockfd, &resp, sizeof(resp));
    if (n != (ssize_t)sizeof(resp)) {
        perror("read response");
        close(sockfd);
        return 1;
    }

    if (resp.status != 0) {
        fprintf(stderr, "error: %s\n", resp.message);
        close(sockfd);
        return 1;
    }

  
    if (resp.message[0])
        printf("%s", resp.message);

    
    while ((n = read(sockfd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);

    close(sockfd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO: (FILLED IN)
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     *
     * CHANGED: removed the placeholder printf – the supervisor now streams
     * a real formatted table via handle_ps(); send_control_request() prints it.
     */
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
