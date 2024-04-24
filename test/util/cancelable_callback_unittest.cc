// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "melon/utility/cancelable_callback.h"

#include "melon/utility/bind.h"
#include "melon/utility/bind_helpers.h"
#include "melon/utility/memory/ref_counted.h"
#include <gtest/gtest.h>

namespace mutil {
namespace {

class TestRefCounted : public RefCountedThreadSafe<TestRefCounted> {
 private:
  friend class RefCountedThreadSafe<TestRefCounted>;
  ~TestRefCounted() {};
};

void Increment(int* count) { (*count)++; }
void IncrementBy(int* count, int n) { (*count) += n; }
void RefCountedParam(const scoped_refptr<TestRefCounted>& ref_counted) {}

// Cancel().
//  - Callback can be run multiple times.
//  - After Cancel(), Run() completes but has no effect.
TEST(CancelableCallbackTest, Cancel) {
  int count = 0;
  CancelableClosure cancelable(
      mutil::Bind(&Increment, mutil::Unretained(&count)));

  mutil::Closure callback = cancelable.callback();
  callback.Run();
  EXPECT_EQ(1, count);

  callback.Run();
  EXPECT_EQ(2, count);

  cancelable.Cancel();
  callback.Run();
  EXPECT_EQ(2, count);
}

// Cancel() called multiple times.
//  - Cancel() cancels all copies of the wrapped callback.
//  - Calling Cancel() more than once has no effect.
//  - After Cancel(), callback() returns a null callback.
TEST(CancelableCallbackTest, MultipleCancel) {
  int count = 0;
  CancelableClosure cancelable(
      mutil::Bind(&Increment, mutil::Unretained(&count)));

  mutil::Closure callback1 = cancelable.callback();
  mutil::Closure callback2 = cancelable.callback();
  cancelable.Cancel();

  callback1.Run();
  EXPECT_EQ(0, count);

  callback2.Run();
  EXPECT_EQ(0, count);

  // Calling Cancel() again has no effect.
  cancelable.Cancel();

  // callback() of a cancelled callback is null.
  mutil::Closure callback3 = cancelable.callback();
  EXPECT_TRUE(callback3.is_null());
}

// CancelableCallback destroyed before callback is run.
//  - Destruction of CancelableCallback cancels outstanding callbacks.
TEST(CancelableCallbackTest, CallbackCanceledOnDestruction) {
  int count = 0;
  mutil::Closure callback;

  {
    CancelableClosure cancelable(
        mutil::Bind(&Increment, mutil::Unretained(&count)));

    callback = cancelable.callback();
    callback.Run();
    EXPECT_EQ(1, count);
  }

  callback.Run();
  EXPECT_EQ(1, count);
}

// Cancel() called on bound closure with a RefCounted parameter.
//  - Cancel drops wrapped callback (and, implicitly, its bound arguments).
TEST(CancelableCallbackTest, CancelDropsCallback) {
  scoped_refptr<TestRefCounted> ref_counted = new TestRefCounted;
  EXPECT_TRUE(ref_counted->HasOneRef());

  CancelableClosure cancelable(mutil::Bind(RefCountedParam, ref_counted));
  EXPECT_FALSE(cancelable.IsCancelled());
  EXPECT_TRUE(ref_counted.get());
  EXPECT_FALSE(ref_counted->HasOneRef());

  // There is only one reference to |ref_counted| after the Cancel().
  cancelable.Cancel();
  EXPECT_TRUE(cancelable.IsCancelled());
  EXPECT_TRUE(ref_counted.get());
  EXPECT_TRUE(ref_counted->HasOneRef());
}

// Reset().
//  - Reset() replaces the existing wrapped callback with a new callback.
//  - Reset() deactivates outstanding callbacks.
TEST(CancelableCallbackTest, Reset) {
  int count = 0;
  CancelableClosure cancelable(
      mutil::Bind(&Increment, mutil::Unretained(&count)));

  mutil::Closure callback = cancelable.callback();
  callback.Run();
  EXPECT_EQ(1, count);

  callback.Run();
  EXPECT_EQ(2, count);

  cancelable.Reset(
      mutil::Bind(&IncrementBy, mutil::Unretained(&count), 3));
  EXPECT_FALSE(cancelable.IsCancelled());

  // The stale copy of the cancelable callback is non-null.
  ASSERT_FALSE(callback.is_null());

  // The stale copy of the cancelable callback is no longer active.
  callback.Run();
  EXPECT_EQ(2, count);

  mutil::Closure callback2 = cancelable.callback();
  ASSERT_FALSE(callback2.is_null());

  callback2.Run();
  EXPECT_EQ(5, count);
}

// IsCanceled().
//  - Cancel() transforms the CancelableCallback into a cancelled state.
TEST(CancelableCallbackTest, IsNull) {
  CancelableClosure cancelable;
  EXPECT_TRUE(cancelable.IsCancelled());

  int count = 0;
  cancelable.Reset(mutil::Bind(&Increment,
                              mutil::Unretained(&count)));
  EXPECT_FALSE(cancelable.IsCancelled());

  cancelable.Cancel();
  EXPECT_TRUE(cancelable.IsCancelled());
}

}  // namespace
}  // namespace mutil
