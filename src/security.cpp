#include "security.h"
#include <seccomp.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sched.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h> 
#include <unistd.h> 
#include <sys/resource.h>
#include <sys/syscall.h>
#include <filesystem>
#include <string>
#include <linux/capability.h>
#include <iostream>
#include <cerrno>
#include <signal.h>

namespace fs = std::filesystem;

namespace {

void sigsys_handler(int signum, siginfo_t *info, void *context) {
    if (info) {
        std::cerr << "\n>>> SECCOMP TRAP: Syscall number " << info->si_syscall << " was blocked! <<<\n" << std::endl;
    }
    _exit(128 + SIGSYS);
}

bool write_text_file(const std::string& path, const std::string& text) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    ssize_t written = write(fd, text.c_str(), text.size());
    close(fd);
    return written == static_cast<ssize_t>(text.size());
}

void ensure_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

void ensure_parent_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
}

int read_last_capability() {
    int last_cap = 63; // root capability
    int fd = open("/proc/sys/kernel/cap_last_cap", O_RDONLY);
    if (fd < 0) return last_cap;

    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return last_cap;

    char* end = nullptr;
    long parsed = std::strtol(buf, &end, 10);
    if (end == buf || parsed < 0) return last_cap;
    return static_cast<int>(parsed);
}

bool drop_all_capabilities() {
    __user_cap_header_struct hdr{};
    __user_cap_data_struct data[2]{};

    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    if (syscall(SYS_capset, &hdr, data) < 0) {
        return false;
    }

#ifdef PR_CAP_AMBIENT
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0 && errno != EINVAL) {
        return false;
    }
#endif

    int last_cap = read_last_capability();
    for (int cap = 0; cap <= last_cap; ++cap) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0) {
            if (errno == EINVAL || errno == EPERM) {
                continue;
            }
            return false;
        }
    }
    return true;
}

bool bind_mount(const std::string& src, const std::string& dst, bool read_only) {
    std::error_code ec;
    if (!fs::exists(src, ec)) return true;

    if (fs::is_directory(src, ec)) {
        ensure_dir(dst);
    } else {
        ensure_parent_dir(dst);
        int fd = open(dst.c_str(), O_CREAT | O_RDONLY, 0644);
        if (fd >= 0) close(fd);
    }

    unsigned long bind_flags = MS_BIND;
    if (fs::is_directory(src, ec)) bind_flags |= MS_REC;
    if (mount(src.c_str(), dst.c_str(), nullptr, bind_flags, nullptr) < 0) {
        return false;
    }

    if (read_only) {
        unsigned long remount_flags = MS_BIND | MS_REMOUNT | MS_RDONLY;
        if (fs::is_directory(src, ec)) remount_flags |= MS_REC;
        if (mount(nullptr, dst.c_str(), nullptr, remount_flags, nullptr) < 0) {
            return false;
        }
    }
    return true;
}

bool setup_user_namespace_mapping(pid_t child_pid) {
    std::string setgroups_path = "/proc/" + std::to_string(child_pid) + "/setgroups";
    std::string uid_map_path = "/proc/" + std::to_string(child_pid) + "/uid_map";
    std::string gid_map_path = "/proc/" + std::to_string(child_pid) + "/gid_map";

    if (!write_text_file(setgroups_path, "deny")) {
        // some kernels may not allow writing setgroups, continue to uid/gid maps
    }
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (!write_text_file(uid_map_path, "0 " + std::to_string(uid) + " 1\n")) {
        return false;
    }
    if (!write_text_file(gid_map_path, "0 " + std::to_string(gid) + " 1\n")) {
        return false;
    }
    return true;
}

bool setup_restricted_root(const std::string& exec_path, const std::string& jail_root) {
    // make the jail root a private mount point (required for chroot)
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
        perror("DEBUG: mount MS_PRIVATE failed");
        return false;
    }

    // mount essentials (UsrMerge)
    const char* base_mounts[] = {"/usr", "/etc", "/var"};
    for (const char* p : base_mounts) {
        std::string dst = jail_root + p;
        if (!bind_mount(p, dst, true)) {
            std::cerr << "DEBUG: bind_mount failed for " << p << std::endl;
            return false;
        }
    }

    // recreate Fedora symlinks
    symlink("usr/bin", (jail_root + "/bin").c_str());
    symlink("usr/sbin", (jail_root + "/sbin").c_str());
    symlink("usr/lib", (jail_root + "/lib").c_str());
    symlink("usr/lib64", (jail_root + "/lib64").c_str());

    // setup Workspace - copy the binary instead of bind mounting CWD
    std::string workspace_path = jail_root + "/workspace";
    std::error_code ec;
    fs::create_directories(workspace_path, ec);

    // copy just the executable into /workspace
    fs::path src_exec(exec_path);
    std::string dst_exec = workspace_path + "/" + src_exec.filename().string();

    try {
        fs::copy_file(src_exec, dst_exec, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "DEBUG: copy_file failed: " << ec.message() << std::endl;
            return false;
        }
        // Make it executable
        chmod(dst_exec.c_str(), 0755);
    } catch (...) {
        std::cerr << "DEBUG: exception during copy" << std::endl;
        return false;
    }

    // mount /proc
    std::string proc_dst = jail_root + "/proc";
    ensure_dir(proc_dst);
    if (mount("proc", proc_dst.c_str(), "proc", 0, nullptr) < 0) {
        perror("DEBUG: mount proc failed");
        return false;
    }

    // enter the Jail
    if (chroot(jail_root.c_str()) < 0) {
        perror("DEBUG: chroot failed");
        return false;
    }

    if (chdir("/workspace") < 0) {
        perror("DEBUG: chdir to /workspace failed");
        return false;
    }

    return true;
}

bool setup_cgroup(pid_t child_pid, const JailOptions& options) {
    // define a unique path for this specific jail instance
    std::string cg_path = "/sys/fs/cgroup/myshell-" + std::to_string(child_pid);
    
    // create the directory (Kernel will populate it)
    std::error_code ec;
    fs::create_directories(cg_path, ec);
    if (ec) {
        perror("Failed to create cgroup directory");
        return false;
    }

    // apply Limits (e.g., Memory)
    // we limiting physical RAM (RSS), allowing shared virtual memory to map fine
    std::string mem_limit = std::to_string(options.memory_limit_bytes);
    if (!write_text_file(cg_path + "/memory.max", mem_limit)) {
        std::cerr << "ERROR: Could not write to " << cg_path << "/memory.max\n";
        return false;
    }

    // trap the child process in the cgroup
    if (!write_text_file(cg_path + "/cgroup.procs", std::to_string(child_pid))) {
        std::cerr << "ERROR: Could not write to " << cg_path << "/cgroup.procs\n";
        return false;
    }

    return true;
}

} 

int enter_jail_environment(const std::string& exec_path,
                           const JailOptions& options,
                           std::string& jailed_exec_path) {
    char template_path[] = "/tmp/myshell-jail-XXXXXX";
    char* jail_root_tmp = mkdtemp(template_path);
    if (!jail_root_tmp) {
        perror("DEBUG: mkdtemp failed");
        return -1;
    }
    std::string jail_root(jail_root_tmp);

    int p2c[2]; // parent to child sync
    int c2p[2]; // child to parent sync
    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        return -1;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid > 0) { // parent process
        close(p2c[0]);
        close(c2p[1]);

        char c;
        if (read(c2p[0], &c, 1) != 1) { // wait for child to unshare
            _exit(1);
        }

        // put the child in the cgroup before mapping UID
        if (!setup_cgroup(child_pid, options)) {
            perror("ERROR: setup_cgroup failed");
            _exit(1);
        }

        if (!setup_user_namespace_mapping(child_pid)) {
            perror("ERROR: setup_user_namespace_mapping failed");
            _exit(1);
        }

        if (write(p2c[1], "A", 1) != 1) { // wake up child
            perror("ERROR: Failed to wake up child");
            _exit(1);
        }

        close(p2c[1]);
        close(c2p[0]);

        int status = 0;
        if (waitpid(child_pid, &status, 0) < 0) {
            _exit(1);
        }

        // clean up the cgroup after the child dies
        std::error_code ec;
        fs::remove_all("/sys/fs/cgroup/myshell-" + std::to_string(child_pid), ec);

        // clean up the jail tmpdir
        fs::remove_all(jail_root, ec);

        if (WIFEXITED(status)) {
            _exit(WEXITSTATUS(status));
        }
        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGSYS) {
                std::cerr << "DEBUG: Child killed by SECCOMP (SIGSYS)!" << std::endl;
            }
            _exit(128 + WTERMSIG(status));
        }
        _exit(1);
    }

    // child process
    close(p2c[1]);
    close(c2p[0]);

    int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID;
    if (options.disable_network) {
        flags |= CLONE_NEWNET;
    }

    if (unshare(flags) < 0) {
        std::cerr << "DEBUG: unshare failed!" << std::endl;
        _exit(1);
    }

    // tell parent we have unshared
    if (write(c2p[1], "A", 1) != 1) {
        std::cerr << "DEBUG: write to parent failed!" << std::endl;
        _exit(1);
    }

    // wait for parent to set up UID/GID map
    char c;
    if (read(p2c[0], &c, 1) != 1) {
        std::cerr << "DEBUG: read from parent failed!" << std::endl;
        _exit(1);
    }

    close(c2p[1]);
    close(p2c[0]);

    // now wee must fork again to become PID 1 in the new PID namespace
    pid_t grandchild_pid = fork();
    if (grandchild_pid < 0) {
        std::cerr << "DEBUG: second fork failed!" << std::endl;
        _exit(1);
    }

    if (grandchild_pid > 0) {
        int status = 0;
        if (waitpid(grandchild_pid, &status, 0) < 0) {
            _exit(1);
        }
        if (WIFEXITED(status)) {
            _exit(WEXITSTATUS(status));
        }
        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGSYS) {
                std::cerr << "DEBUG: Grandchild killed by SECCOMP (SIGSYS)!" << std::endl;
            }
            _exit(128 + WTERMSIG(status));
        }
        _exit(1);
    }

    // grandchild continues as PID 1 in the new pid namespace.
    std::string src_cwd = fs::current_path().string(); // captured before chroot

    if (!setup_restricted_root(exec_path, jail_root)) {
        std::cerr << "DEBUG: setup_restricted_root failed!" << std::endl;
        _exit(1);
    }

    if (exec_path.rfind(src_cwd, 0) == 0) {
        jailed_exec_path = "/workspace" + exec_path.substr(src_cwd.size());
    } else {
        jailed_exec_path = "/workspace/" + fs::path(exec_path).filename().string();
    }
    return 0;
}

void apply_jail_policy(const JailOptions& options) {

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        perror("prctl"); 
        _exit(1);
    }

    if (!drop_all_capabilities()) {
        perror("drop capabilities");
        _exit(1);
    }

    struct rlimit rl;

    // CPU limit: configurable with small buffer for teardown
    rl.rlim_cur = options.cpu_limit_seconds;
    rl.rlim_max = options.cpu_limit_seconds + 3;
    setrlimit(RLIMIT_CPU, &rl);

    // NPROC limit
    rl.rlim_cur = 3;
    rl.rlim_max = 4;
    setrlimit(RLIMIT_NPROC, &rl);

    // setup an aggressive signal handler specifically to log SECCOMP traps
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigsys_handler;
    sigaction(SIGSYS, &sa, nullptr);

    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_TRAP);

    // essentials
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(execve), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(munmap), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(exit_group), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigreturn), 0);

    // file I/O and startup 
    // only allow openat if the flags (arg 2) do not include write permissions
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(openat), 1,
                     SCMP_A2(SCMP_CMP_MASKED_EQ, O_WRONLY | O_RDWR, 0));
    // allow reading directory contents 
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getdents64), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstatfs), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(statfs), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getcwd), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlink), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlinkat), 0);
    

    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 0);

    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(close), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(dup), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(dup2), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(dup3), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(access), 0);
    
    // diff versions of stat
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat), 0);      
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(newfstatat), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(statx), 0);      

    // system context
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(arch_prctl), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(set_tid_address), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(set_robust_list), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(prlimit64), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getrandom), 0);


    // mem and thread management
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(madvise), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(gettid), 0);

    // identity checks
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getuid), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(geteuid), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getgid), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getegid), 0);

    // extended I/O
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(pread64), 0);   
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);

    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rseq), 0);

    // threading and locking
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex), 0);

    // memory advise
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(madvise), 0);

    // entropy and randomness, used by glibc for security checks
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getrandom), 0);

    // thread identity
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(gettid), 0);

    if (seccomp_load(ctx) < 0) {
        perror("seccomp_load"); 
        _exit(1);
    }
    seccomp_release(ctx);
}