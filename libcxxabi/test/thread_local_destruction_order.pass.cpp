//===-------------- thread_local_destruction_order.pass.cpp ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// 2021.7.19 Add OHOS target support for libcxxabi
//     Copyright (c) 2021 Huawei Device Co., Ltd. All rights reserved.

// Darwin TLV finalization routines fail when creating a thread-local variable
// in the destructor for another thread-local variable:
// http://lists.llvm.org/pipermail/cfe-dev/2016-November/051376.html
// UNSUPPORTED: c++98, c++03
// UNSUPPORTED: libcxxabi-no-threads

#include <cassert>
#include <thread>

int seq = 0;

class OrderChecker {
public:
  explicit OrderChecker(int n) : n_{n} { }

  ~OrderChecker() {
    assert(seq++ == n_);
  }

private:
  int n_;
};

template <int ID>
class CreatesThreadLocalInDestructor {
public:
  ~CreatesThreadLocalInDestructor() {
    thread_local OrderChecker checker{ID};
  }
};

OrderChecker global{7};

void thread_fn() {
  static OrderChecker fn_static{5};
  thread_local CreatesThreadLocalInDestructor<2> creates_tl2;
  thread_local OrderChecker fn_thread_local{1};
  thread_local CreatesThreadLocalInDestructor<0> creates_tl0;
}

int main() {
  static OrderChecker fn_static{6};

  std::thread{thread_fn}.join();
  assert(seq == 3);

  thread_local OrderChecker fn_thread_local{4};
  thread_local CreatesThreadLocalInDestructor<3> creates_tl;

  return 0;
}
