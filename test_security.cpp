#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
using namespace std;
int main() {
    cout << "[1] Testing STDOUT: This should work!" << endl;

    // create a dummy file to try and delete
    ofstream tmp("jail_test_file.txt");
    tmp << "If you see this, the file was created before the jail.";
    tmp.close();

    cout << "[2] Attempting to DELETE a file (unlink)..." << endl;
    if (unlink("jail_test_file.txt") == 0) {
        cout << "SUCCESS: File deleted. (FAIL: Jail is leaky)" << endl;
    } else {
        perror("BLOCKED: Could not delete file");
    }

    cout << "[3] Attempting to open a NETWORK socket..." << endl;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s != -1) {
        cout << "SUCCESS: Socket opened. (FAIL: Jail is leaky)" << endl;
        close(s);
    } else {
        perror("BLOCKED: Could not open socket");
    }

    return 0;
}