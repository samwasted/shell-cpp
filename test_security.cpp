#include <iostream>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <cstring>

using namespace std;

void test_network() {
    cout << "[TEST] Attempting to create a socket..." << endl;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) perror("  Result (Success)");
    else { cout << "  Result (FAILURE: Network is open!)" << endl; close(sock); }
}

void test_fork_bomb() {
    cout << "[TEST] Attempting to fork (Fork Bomb test)..." << endl;
    for(int i = 0; i < 10; i++) {
        pid_t p = fork();
        if (p == 0) _exit(0); 
        if (p < 0) {
            cout << "  Result (Success): Fork blocked after " << i << " attempts." << endl;
            return;
        }
    }
}

void test_memory() {
    cout << "[TEST] Attempting to allocate 500MB (Memory Bomb test)..." << endl;
    try {
        // Try to allocate more than the 128MB limit
        vector<char> v(500 * 1024 * 1024);
        cout << "  Result (FAILURE: Memory not restricted!)" << endl;
    } catch (...) {
        cout << "  Result (Success): Allocation failed." << endl;
    }
}

void test_cpu() {
    cout << "[TEST] Starting infinite loop (CPU limit test)..." << endl;
    cout << "  (You should see the shell kill this in ~2 seconds)" << endl;
    while(true); 
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: jail ./test_sandbox [net|fork|mem|cpu]" << endl;
        return 0;
    }

    string arg = argv[1];
    if (arg == "net") test_network();
    else if (arg == "fork") test_fork_bomb();
    else if (arg == "mem") test_memory();
    else if (arg == "cpu") test_cpu();

    return 0;
}