#include "calculator.hpp"

#include <iostream>

int main() {
    if (calculator::add(2, 3) != 5) {
        std::cerr << "add(2, 3) should be 5\n";
        return 1;
    }
    if (calculator::add(-2, 3) != 1) {
        std::cerr << "add(-2, 3) should be 1\n";
        return 2;
    }
    std::cout << "calculator tests passed\n";
    return 0;
}
