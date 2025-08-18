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
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace std;

int main(int argc, char* argv[]) {
    ThreadPool pool(1, 32);
    vector<future<tuple<int, int>>> vec;
    int count = 40;
    for (int i = 0; i < count; i++) {
        auto fut = pool.spawn([i] {
            this_thread::sleep_for(2s);
            switch (i % 3) {
                case 0: {
                    throw runtime_error("task error " + to_string(i));
                }
                case 1: {
                    throw logic_error("task error " + to_string(i));
                }
                default:
                    return make_tuple(i, i * i);
            }
        });
        if (fut) {
            vec.push_back(std::move(fut.value()));
        } else {
            cout << "task " << i << " is rejected" << endl;
        }
    }

    pool.set_worker_count(4);

    for (auto& f : vec) {
        try {
            auto [i, j] = f.get();
            cout << "future get " << i << ":" << j
                 << " task size: " << pool.get_task_count() << endl;

        } catch (const runtime_error& e) {
            cout << "future get runtime_error" << " " << e.what() << endl;
        } catch (const logic_error& e) {
            cout << "future get logic_error" << " " << e.what() << endl;
        } catch (const exception& e) {
            cout << "future get exception" << " " << e.what() << endl;
        }
    }

    cout << "----------------------------\n";

    // cout << "[";
    // for (auto size = count; auto& param : params) {
    //     cout << param << (--size ? ", " : "]\n");
    // }
    pool.shutdown();
    return 0;
}
