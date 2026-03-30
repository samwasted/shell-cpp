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
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {
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

bool setup_user_namespace_mapping() {
    if (!write_text_file("/proc/self/setgroups", "deny")) {
        // Some kernels may not allow writing setgroups; continue to uid/gid maps.
    }
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (!write_text_file("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1\n")) {
        return false;
    }
    if (!write_text_file("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1\n")) {
        return false;
    }
    return true;
}

bool setup_restricted_root(const std::string& exec_path) {
    char template_path[] = "/tmp/myshell-jail-XXXXXX";
    char* jail_root = mkdtemp(template_path);
    if (!jail_root) return false;

    // Prevent mount propagation outside the jail namespace.
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
        return false;
    }

    // Mount only runtime essentials and explicit command/cwd paths.
    const char* base_mounts[] = {
        "/bin", "/sbin", "/lib", "/lib64", "/usr", "/etc"
    };
    for (const char* p : base_mounts) {
        std::string dst = std::string(jail_root) + p;
        if (!bind_mount(p, dst, true)) return false;
    }

    std::string cwd = fs::current_path().string();
    std::string cwd_dst = std::string(jail_root) + cwd;
    if (!bind_mount(cwd, cwd_dst, true)) return false;

    std::string exec_parent = fs::path(exec_path).parent_path().string();
    if (!exec_parent.empty()) {
        std::string exec_parent_dst = std::string(jail_root) + exec_parent;
        if (!bind_mount(exec_parent, exec_parent_dst, true)) return false;
    }

    std::string proc_dst = std::string(jail_root) + "/proc";
    ensure_dir(proc_dst);
    if (mount("proc", proc_dst.c_str(), "proc", 0, nullptr) < 0) {
        return false;
    }

    if (chroot(jail_root) < 0) return false;
    if (chdir(cwd.c_str()) < 0) {
        // Fall back to root inside jail if cwd is unavailable.
        if (chdir("/") < 0) return false;
    }
    return true;
}
} // namespace

int enter_jail_environment(const std::string& exec_path,
                           const JailOptions& options,
                           std::string& jailed_exec_path) {
    int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID;
    if (options.disable_network) {
        flags |= CLONE_NEWNET;
    }

    if (unshare(flags) < 0) {
        return -1;
    }

    if (!setup_user_namespace_mapping()) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            _exit(1);
        }
        if (WIFEXITED(status)) {
            _exit(WEXITSTATUS(status));
        }
        if (WIFSIGNALED(status)) {
            _exit(128 + WTERMSIG(status));
        }
        _exit(1);
    }

    // Child continues as PID 1 in the new pid namespace.
    if (!setup_restricted_root(exec_path)) {
        return -1;
    }

    jailed_exec_path = exec_path;
    return 0;
}

void apply_jail_policy(const JailOptions& options) {

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        perror("prctl"); 
        _exit(1);
    }

    struct rlimit rl;

    // CPU limit: configurable with small buffer for teardown.
    rl.rlim_cur = options.cpu_limit_seconds;
    rl.rlim_max = options.cpu_limit_seconds + 3;
    setrlimit(RLIMIT_CPU, &rl);

    // memory limit: configurable with 2x hard ceiling.
    rl.rlim_cur = options.memory_limit_bytes;
    rl.rlim_max = options.memory_limit_bytes * 2;
    setrlimit(RLIMIT_AS, &rl);

    // NPROC limit
    rl.rlim_cur = 3;
    rl.rlim_max = 4;
    setrlimit(RLIMIT_NPROC, &rl);

    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL);

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
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(openat), 1,
                     SCMP_A2(SCMP_CMP_MASKED_EQ, O_WRONLY | O_RDWR, 0));
    
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getcwd), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlink), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlinkat), 0);
    
    // only allow writing to STDOUT (1) or STDERR (2)
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 1,
                     SCMP_A0(SCMP_CMP_EQ, STDOUT_FILENO));
    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 1,
                     SCMP_A0(SCMP_CMP_EQ, STDERR_FILENO));

    seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(close), 0);
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