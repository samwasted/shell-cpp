#include <unistd.h>
#include <iostream>
int main() {
    if (access("./the_hi", X_OK) == 0) {
        std::cout << "the_hi is accessible" << std::endl;
    } else {
        std::cout << "the_hi is NOT accessible" << std::endl;
    }
}
