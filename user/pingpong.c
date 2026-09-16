#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int p2c_pipe[2];
    int c2p_pipe[2];
    pipe(p2c_pipe);
    pipe(c2p_pipe);

    if (fork() == 0) {
        int child_pid = getpid();
        int parent_pid;
        close(0);
        read(p2c_pipe[0], &parent_pid, sizeof(parent_pid));
        printf("%d: received ping from pid %d\n", child_pid, parent_pid);
        close(p2c_pipe[0]);
        close(p2c_pipe[1]);
        close(c2p_pipe[0]);
        write(c2p_pipe[1], &child_pid, sizeof(child_pid));
        close(c2p_pipe[1]);
    } else {
        int parent_pid = getpid();
        int child_pid;
        close(p2c_pipe[0]);
        write(p2c_pipe[1], &parent_pid, sizeof(parent_pid));
        close(p2c_pipe[1]);
        close(0);
        read(c2p_pipe[0], &child_pid, sizeof(child_pid));
        printf("%d: received pong from pid %d\n", parent_pid, child_pid);
        close(c2p_pipe[0]);
        close(c2p_pipe[1]);
    }

    exit(0);
}