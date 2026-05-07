#include "terminal.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>

struct process {
    int id;
    std::string tool;
    std::string tab_name;
    pid_t pid;
    int master_fd;
    std::thread reader;
    std::mutex output_mutex;
    std::string output;
    std::atomic<bool> running{true};
    int exit_code = -1;
};

static std::mutex processes_mutex;
static std::vector<std::unique_ptr<process>> processes;
static std::atomic<int> next_id{1};

static std::string strip_ansi(const char* in, size_t n){
    std::string out;
    out.reserve(n);
    for(size_t i = 0; i < n; i++){
        if(in[i] == '\x1b' && i + 1 < n && in[i+1] == '['){
            i += 2;
            while(i < n && !(in[i] >= '@' && in[i] <= '~')) i++;
        } else if(in[i] == '\r' && (i + 1 >= n || in[i+1] != '\n')){
        } else {
            out += in[i];
        }
    }
    return out;
}

static void reader_thread(process* p){
    char buf[4096];
    while(true){
        ssize_t n = read(p->master_fd, buf, sizeof(buf));
        if(n > 0){
            std::string clean = strip_ansi(buf, (size_t)n);
            std::lock_guard<std::mutex> lock(p->output_mutex);
            p->output += clean;
        } else {
            break;
        }
    }
    int status = 0;
    waitpid(p->pid, &status, 0);
    if(WIFEXITED(status)) p->exit_code = WEXITSTATUS(status);
    else if(WIFSIGNALED(status)) p->exit_code = -WTERMSIG(status);
    p->running.store(false);
    close(p->master_fd);
}

void launch_process(const std::string& tool, const std::vector<std::string>& argv){
    std::vector<char*> args_c;
    args_c.reserve(argv.size() + 2);
    args_c.push_back(const_cast<char*>(tool.c_str()));
    for(const auto& a : argv) args_c.push_back(const_cast<char*>(a.c_str()));
    args_c.push_back(nullptr);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if(master < 0){ fprintf(stderr, "posix_openpt: %s\n", strerror(errno)); return; }
    if(grantpt(master) < 0 || unlockpt(master) < 0){
        fprintf(stderr, "grantpt/unlockpt: %s\n", strerror(errno));
        close(master); return;
    }
    char slave_name[256];
    if(ptsname_r(master, slave_name, sizeof(slave_name)) != 0){
        fprintf(stderr, "ptsname_r: %s\n", strerror(errno));
        close(master); return;
    }

    pid_t pid = fork();
    if(pid < 0){
        fprintf(stderr, "fork: %s\n", strerror(errno));
        close(master); return;
    }

    if(pid == 0){
        setsid();
        int slave = open(slave_name, O_RDWR);
        if(slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if(slave > 2) close(slave);
        close(master);
        execvp(tool.c_str(), args_c.data());
        _exit(127);
    }

    auto p = std::make_unique<process>();
    p->id = next_id.fetch_add(1);
    p->tool = tool;
    p->tab_name = tool + " #" + std::to_string(p->id);
    p->pid = pid;
    p->master_fd = master;

    process* ptr = p.get();
    {
        std::lock_guard<std::mutex> lock(processes_mutex);
        processes.push_back(std::move(p));
    }
    ptr->reader = std::thread(reader_thread, ptr);
}

std::vector<process_info> list_processes(){
    std::lock_guard<std::mutex> lock(processes_mutex);
    std::vector<process_info> out;
    out.reserve(processes.size());
    for(auto& p : processes){
        out.push_back({p->id, p->tab_name, p->running.load(), p->exit_code});
    }
    return out;
}

std::string get_process_output(int id){
    std::lock_guard<std::mutex> lock(processes_mutex);
    for(auto& p : processes){
        if(p->id == id){
            std::lock_guard<std::mutex> l2(p->output_mutex);
            return p->output;
        }
    }
    return {};
}

void send_to_process(int id, const std::string& data){
    std::lock_guard<std::mutex> lock(processes_mutex);
    for(auto& p : processes){
        if(p->id == id && p->running.load()){
            const char* buf = data.data();
            size_t left = data.size();
            while(left > 0){
                ssize_t n = write(p->master_fd, buf, left);
                if(n < 0){
                    if(errno == EINTR) continue;
                    break;
                }
                buf += n;
                left -= n;
            }
            return;
        }
    }
}

void kill_process(int id){
    std::lock_guard<std::mutex> lock(processes_mutex);
    for(auto& p : processes){
        if(p->id == id && p->running.load()){
            kill(p->pid, SIGTERM);
            return;
        }
    }
}

void shutdown_processes(){
    {
        std::lock_guard<std::mutex> lock(processes_mutex);
        for(auto& p : processes){
            if(p->running.load()) kill(p->pid, SIGTERM);
        }
    }
    std::lock_guard<std::mutex> lock(processes_mutex);
    for(auto& p : processes){
        if(p->reader.joinable()) p->reader.join();
    }
    processes.clear();
}
