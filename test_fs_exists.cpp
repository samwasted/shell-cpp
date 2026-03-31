#include <iostream>
#include <filesystem>
int main() {
    std::error_code ec;
    std::cout << "/lib64 exists: " << std::filesystem::exists("/lib64", ec) << "\n";
    std::cout << "/lib exists: " << std::filesystem::exists("/lib", ec) << "\n";
}
