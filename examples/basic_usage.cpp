#include "taskforge/scheduler.hpp"

#include <iostream>
#include <string>

int main() {
    taskforge::Scheduler scheduler(2, 8);

    auto greeting = scheduler.submit([](std::string name) {
        return "Hello, " + name + "!";
    }, std::string("TaskForge"));

    auto sum = scheduler.submit(2, [](int left, int right) {
        return left + right;
    }, 20, 22);

    std::cout << greeting.get() << '\n';
    std::cout << "20 + 22 = " << sum.get() << '\n';

    scheduler.shutdown();
}
