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
#include "thread_pool.h"
#include <iostream>
using namespace std;

int main(int argc, char* argv[]) {
    ThreadPool pool(4, 10);
    vector<future<int>> vec;
    int count = 16;
    vector<int> params(16, 0);
    for (int i = 0; i < count; i++) {
        auto& param = params[i];
        auto fut = pool.spawn([&param, i] {
            this_thread::sleep_for(2s);
            param = i*i;
            return param;
        });
        if (fut.has_value()) {
            vec.push_back(std::move(fut.value()));
        } else {
            cout << "task " << i << " is rejected" << endl;
        }
    }

    for (auto& f : vec) {
        try {
            cout << "future get " << f.get() << endl;
        } catch (const exception& e) {
            cout << "future get " << e.what() << endl;
        }
    }

    cout << "----------------------------\n";

    cout << "[";
    for (int size = count; auto& param : params) {
        cout << param << (--size ? ", " : "]\n");
    }

    return 0;
}
