#include <iostream>

int main() {
    const int SIZE = 1024 / sizeof(int); // 1KB array
    int arr[SIZE];
    long long sum = 0;

    // Sequential write
    for (int i = 0; i < SIZE; i++) {
        arr[i] = i;
    }

    // Sequential read
    for (int i = 0; i < SIZE; i++) {
        sum += arr[i];
    }

    std::cout << "Sum: " << sum << std::endl;
    return 0;
}
