/* ***************************************************************
 * Copyright(c) 2023, Peter Pan. All rights reserverd.
 * Name          : example.cpp
 *
 * Author        : Peter Pan
 * Description   :
 * Created       : 2025-03-03 15:49:46
 * Version       : 1.0
 * Last Modified :
 * ***************************************************************/
#include "ThreadPool.h"
#include <print>
using namespace std;

int main(int argc, char* argv[]) {
    ThreadPool pool(4, 10);
    vector<future<int>> vec;
    for (int i = 0; i < 16; i++) {
        auto fut = pool.spawn([i] {
            this_thread::sleep_for(1s);
            return i * i;
        });
        if (fut.has_value()) {
            vec.push_back(std::move(fut.value()));
        } else {
            println("task {} is rejected", i);
        }
    }

    for (auto& f : vec) {
        print("{}\n", f.get());
    }

    return 0;
}
