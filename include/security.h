#ifndef SECURITY_H
#define SECURITY_H

#include <cstddef>
#include <string>

struct JailOptions {
	int cpu_limit_seconds = 2;
	std::size_t memory_limit_bytes = 128ULL * 1024ULL * 1024ULL;
	bool disable_network = false;
};

// Enters user/mount/pid namespaces and configures a restricted filesystem view.
// On success, jailed_exec_path points to the executable path inside the jail.
// This function may _exit() in the intermediate parent when CLONE_NEWPID is used.
int enter_jail_environment(const std::string& exec_path,
						   const JailOptions& options,
						   std::string& jailed_exec_path);

void apply_jail_policy(const JailOptions& options);

#endif