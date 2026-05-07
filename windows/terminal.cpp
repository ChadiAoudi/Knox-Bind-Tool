#include "terminal.h"
#include "framework.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <vector>
#include <cstdio>

struct process {
    int id;
    std::string tool;
    std::string tab_name;
    HANDLE proc_handle;
    HANDLE thread_handle;
    HANDLE stdin_write;
    HANDLE stdout_read;
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

static std::string quote_arg(const std::string& arg){
    std::string quoted = "\"";
    for(size_t i = 0; i < arg.size(); i++){
        if(arg[i] == '\\'){
            size_t j = i;
            while(j < arg.size() && arg[j] == '\\') j++;
            size_t num_bs = j - i;
            if(j == arg.size() || arg[j] == '"')
                quoted.append(num_bs * 2, '\\');
            else
                quoted.append(num_bs, '\\');
            i = j - 1;
        } else if(arg[i] == '"'){
            quoted += "\\\"";
        } else {
            quoted += arg[i];
        }
    }
    quoted += '"';
    return quoted;
}

static std::string build_cmdline(const std::string& tool, const std::vector<std::string>& argv){
    std::string cmd = quote_arg(tool);
    for(const auto& a : argv) cmd += " " + quote_arg(a);
    return cmd;
}

static void reader_thread(process* p){
    char buf[4096];
    DWORD n;
    while(ReadFile(p->stdout_read, buf, sizeof(buf), &n, nullptr) && n > 0){
        std::string clean = strip_ansi(buf, (size_t)n);
        std::lock_guard<std::mutex> lock(p->output_mutex);
        p->output += clean;
    }
    WaitForSingleObject(p->proc_handle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p->proc_handle, &code);
    p->exit_code = (int)code;
    p->running.store(false);
    CloseHandle(p->stdout_read);
    p->stdout_read = INVALID_HANDLE_VALUE;
    CloseHandle(p->stdin_write);
    p->stdin_write = INVALID_HANDLE_VALUE;
    CloseHandle(p->proc_handle);
    p->proc_handle = INVALID_HANDLE_VALUE;
    CloseHandle(p->thread_handle);
    p->thread_handle = INVALID_HANDLE_VALUE;
}

void launch_process(const std::string& tool, const std::vector<std::string>& argv){
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;

    if(!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) return;
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    if(!CreatePipe(&stdin_read, &stdin_write, &sa, 0)){
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    std::string cmdline = build_cmdline(tool, argv);
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if(!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)){
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        return;
    }

    CloseHandle(stdout_write);
    CloseHandle(stdin_read);

    auto p = std::make_unique<process>();
    p->id = next_id.fetch_add(1);
    p->tool = tool;
    p->tab_name = tool + " #" + std::to_string(p->id);
    p->proc_handle = pi.hProcess;
    p->thread_handle = pi.hThread;
    p->stdin_write = stdin_write;
    p->stdout_read = stdout_read;

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
            DWORD left = (DWORD)data.size();
            while(left > 0){
                DWORD written = 0;
                if(!WriteFile(p->stdin_write, buf, left, &written, nullptr)) break;
                buf += written;
                left -= written;
            }
            return;
        }
    }
}

void kill_process(int id){
    std::lock_guard<std::mutex> lock(processes_mutex);
    for(auto& p : processes){
        if(p->id == id && p->running.load()){
            TerminateProcess(p->proc_handle, 1);
            return;
        }
    }
}

void shutdown_processes(){
    {
        std::lock_guard<std::mutex> lock(processes_mutex);
        for(auto& p : processes){
            if(p->running.load()) TerminateProcess(p->proc_handle, 1);
        }
    }
    std::lock_guard<std::mutex> lock(processes_mutex);
    for(auto& p : processes){
        if(p->reader.joinable()) p->reader.join();
    }
    processes.clear();
}
