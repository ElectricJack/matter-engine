#include "internal.h"
#include "mesh_worker.h"
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace matter::geometry::detail {
namespace {
using Clock=std::chrono::steady_clock;
struct Files {
    std::string directory, input, output;
    Files() {
        auto pattern=(std::filesystem::temp_directory_path()/"matter-mesh-XXXXXX").string();
        char *p=mkdtemp(pattern.data());
        if(!p) throw std::runtime_error("cannot create mesh worker directory");
        // Construct path strings before ownership transfers; clean on allocation failure.
        try { directory=p; input=directory+"/input"; output=directory+"/output"; }
        catch(...) { rmdir(p); throw; }
    }
    ~Files() { unlink(input.c_str()); unlink(output.c_str()); rmdir(directory.c_str()); }
};
struct Child {
    pid_t pid=-1;
    Child(const std::string &path,const char *mode,const Files &files,uint64_t memory,uint64_t bytes) {
        const char *args[]={path.c_str(),mode,files.input.c_str(),files.output.c_str(),nullptr};
        const rlimit mem{rlim_t(memory),rlim_t(memory)}, file{rlim_t(bytes),rlim_t(bytes)}, core{0,0};
        const pid_t parent=getpid();
        int nullfd=open("/dev/null",O_RDWR|O_CLOEXEC);
        if(nullfd<0) throw std::runtime_error("cannot open mesh worker streams");
        pid=fork();
        if(pid==0) {
            // No allocation or C++ library work between fork and exec: the
            // service can run on a host thread while other threads allocate.
            if(setpgid(0,0)!=0 || setrlimit(RLIMIT_AS,&mem)!=0 || setrlimit(RLIMIT_FSIZE,&file)!=0 ||
                setrlimit(RLIMIT_CORE,&core)!=0) _exit(126);
#ifdef __linux__
            if(prctl(PR_SET_PDEATHSIG,SIGKILL)!=0 || getppid()!=parent) _exit(126);
#else
            (void)parent;
#endif
            if(dup2(nullfd,0)<0 || dup2(nullfd,1)<0 || dup2(nullfd,2)<0) _exit(126);
            // Do not leak a host's open files/sockets into the backend. This
            // optional Linux backend requires close_range (Linux >= 5.9).
            if(syscall(SYS_close_range,3u,~0u,0u)!=0) _exit(126);
            execv(path.c_str(),const_cast<char *const *>(args));
            _exit(127);
        }
        close(nullfd);
        if(pid<0) throw std::runtime_error("cannot start mesh worker");
    }
    ~Child() {
        if(pid>0) {
            kill(-pid,SIGKILL); kill(pid,SIGKILL);
            int status; while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
        }
    }
    bool done() {
        int status=0; auto r=waitpid(pid,&status,WNOHANG);
        if(r==0 || (r<0 && errno==EINTR)) return false;
        if(r<0) {
            // A host SIGCHLD handler may have reaped it. Never signal a PID
            // which might now belong to another process.
            if(errno==ECHILD) pid=-1;
            throw std::runtime_error("cannot collect mesh worker");
        }
        // Reap the direct worker and terminate any descendants before releasing
        // its process group. A successful file alone never proves job success.
        kill(-pid,SIGKILL); pid=-1;
        if(WIFSIGNALED(status))
            throw std::runtime_error("mesh worker terminated by signal "+std::to_string(WTERMSIG(status)));
        if(WIFEXITED(status) && WEXITSTATUS(status)==wire::resource_exit)
            reject(data::ErrorCode::resource_limit,"mesh worker exhausted memory or output budget");
        if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)
            throw std::runtime_error("mesh worker failed with exit status "+std::to_string(WEXITSTATUS(status)));
        return true;
    }
};
}

bool mesh_worker_available(const std::string &path) {
    if(path.empty() || path.front()!='/' || access(path.c_str(),X_OK)!=0) return false;
    try {
        Files files;
        Child child(path,"--probe",files,256ull*1024*1024,4096);
        const auto until=Clock::now()+std::chrono::milliseconds(300);
        while(!child.done()) {
            if(Clock::now()>=until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(Clock::now()>=until) return false;
        std::ifstream in(files.output,std::ios::binary);
        return wire::get(in)==wire::magic && in.peek()==EOF;
    } catch(...) { return false; }
}

MeshIndexed run_mesh_worker(const std::string &path,const MeshIndexed &mesh,const wire::Options &options,
    const jobs::ResourceBudget &budget,jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) {
    checked(monitor.checkpoint(usage));
    const uint64_t available=budget.working_bytes>usage.working_bytes ? budget.working_bytes-usage.working_bytes : 0;
    if(available<1024*1024 || wire::size(mesh.positions.size(),mesh.indices.size())>wire::hard_bytes)
        reject(data::ErrorCode::resource_limit,"mesh worker input or memory limit exceeded");
    // Native decoded storage, vectors and reprojection scratch require more
    // memory than the wire bytes. Admit both transfer files and the decoded copy.
    wire::Limits limits{budget.vertices,budget.indices,std::min({budget.output_bytes,available/4,wire::hard_bytes})};
    if(limits.bytes<wire::size(3,3))
        reject(data::ErrorCode::resource_limit,"mesh worker output budget cannot hold one triangle");
    Files files;
    {
        std::ofstream out(files.input,std::ios::binary);
        wire::write_options(out,options,limits);
        wire::write(out,mesh,{UINT32_MAX,UINT32_MAX,wire::hard_bytes});
        out.close(); if(!out) throw std::runtime_error("cannot write mesh worker input");
    }
    checked(monitor.checkpoint(usage));
    Child child(path,"--run",files,available,limits.bytes);
    const auto started=Clock::now();
    while(!child.done()) {
        checked(monitor.checkpoint(usage));
        if(options.timeout_seconds && Clock::now()-started>=std::chrono::seconds(options.timeout_seconds))
            reject(data::ErrorCode::resource_limit,"mesh worker timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    checked(monitor.checkpoint(usage));
    if(options.timeout_seconds && Clock::now()-started>=std::chrono::seconds(options.timeout_seconds))
        reject(data::ErrorCode::resource_limit,"mesh worker timeout");
    const auto bytes=std::filesystem::file_size(files.output);
    if(bytes>limits.bytes) throw std::runtime_error("mesh worker output exceeds transfer limit");
    std::ifstream in(files.output,std::ios::binary);
    // A lying count header is bounded by the actual file size before allocating.
    limits.bytes=bytes;
    MeshIndexed result=wire::read(in,limits);
    if(in.peek()!=EOF) throw std::runtime_error("mesh worker output has trailing bytes");
    checked(monitor.checkpoint(usage));
    return result;
}
} // namespace matter::geometry::detail
