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
#include <thread>
using namespace std;

struct Sqtr {
    int data;
    int result;

    Sqtr(int data) : data(data) {}

    void operator()() {
        result = data * data;
    }
};

int main(int argc, char* argv[]) {
    ThreadPool pool(2, 10);
    vector<future<shared_ptr<Sqtr>>> vec;
    int count = 16;

    for (int i = 0; i < count; i++) {
        auto sqtr = make_shared<Sqtr>(i);
        auto fut = pool.spawn([sqtr] {
            this_thread::sleep_for(2s);
            (*sqtr)();
            return sqtr;
        });
        if (fut.has_value()) {
            vec.push_back(std::move(fut.value()));
        } else {
            cout << "task " << i << " is rejected" << endl;
        }
    }

    for (auto& f : vec) {
        try {
            auto one = f.get();
            cout << one->data << " sqrt is " << one->result << endl;
        } catch (const exception& e) {
            cout << "future get " << e.what() << endl;
        }
    }

    return 0;
}
