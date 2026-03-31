#include <iostream>
#include <fstream>
#include <filesystem>
#include <unistd.h>
namespace fs = std::filesystem;

int main() {
    std::cout << "access ./the_hi: " << access("./the_hi", F_OK) << "\n";
    std::cout << "access /lib64/ld-linux-x86-64.so.2: " << access("/lib64/ld-linux-x86-64.so.2", F_OK) << "\n";
    std::cout << "access /bin/sh: " << access("/bin/sh", F_OK) << "\n";
    return 0;
}
