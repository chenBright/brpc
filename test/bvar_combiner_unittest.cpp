// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <pthread.h>
#include <unordered_map>
#include <gtest/gtest.h>
#include "bvar/combiner.h"

namespace {

struct Item {
    int key;
    int value;
};

struct Map {
public:
    void Submit(const Item& item) {
        map[item.key] += item.value;
    }

    void Merge(const Map& other) {
        for (const auto& it : other.map) {
            map[it.first] += it.second;
        }
    }

private:
    std::unordered_map<int, int> map;
};

TEST(CombinerTest, sanity) {
    bvar::Combiner<Map, Item> combiner;
    combiner << Item{1, 10} << Item{2, 20} << Item{1, 5};

    combiner.get_value_with_callback([](Map& map) {
        ASSERT_EQ(1, map.map.count(1));
        ASSERT_EQ(15, map.map[1]);
        ASSERT_EQ(1, map.map.count(2));
        ASSERT_EQ(20, map.map[2]);
    });

    Map map = combiner.get_and_reset_value();
    ASSERT_EQ(1, map.map.count(1));
    ASSERT_EQ(15, map.map[1]);
    ASSERT_EQ(1, map.map.count(2));
    ASSERT_EQ(20, map.map[2]);

    map = combiner.get_value();
    ASSERT_EQ(0, map.map.count(1));
    ASSERT_EQ(0, map.map.count(2));
}

bool g_start = false;
int g_n_num = 5;
int g_num_count = 200000;

void* combine_thread(void* arg) {
    while (!g_start) {
        usleep(1000);
    }
    auto combiner = static_cast<bvar::Combiner<Map, Item>*>(arg);
    for (int i = 0; i < g_n_num; ++i) {
        for (int j = 0; j < g_num_count; ++j) {
            (*combiner) << Item{i, 1};
        }
    }
    return NULL;
}

TEST(CombinerTest, multi_thread) {
    bvar::Combiner<Map, Item> combiner;
    const size_t num_thread = 8;
    pthread_t threads[num_thread];
    for (auto& thread : threads) {
        ASSERT_EQ(0, pthread_create(&thread, NULL, &combine_thread, &combiner));
    }
    g_start = true;
    for (auto thread : threads) {
        ASSERT_EQ(0, pthread_join(thread, NULL));
    }

    Map map = combiner.get_value();
    for (int i = 0; i < g_n_num; ++i) {
        ASSERT_EQ(1, map.map.count(i));
        ASSERT_EQ(num_thread * g_num_count, map.map[i]);
    }
}

}