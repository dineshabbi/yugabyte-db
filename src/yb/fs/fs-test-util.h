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
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_FS_FS_TEST_UTIL_H
#define YB_FS_FS_TEST_UTIL_H

#include "yb/fs/block_manager.h"
#include "yb/util/malloc.h"

namespace yb {
namespace fs {

// ReadableBlock that counts the total number of bytes read.
//
// The counter is kept separate from the class itself because
// ReadableBlocks are often wholly owned by other objects, preventing tests
// from easily snooping on the counter's value.
//
// Sample usage:
//
//   gscoped_ptr<ReadableBlock> block;
//   fs_manager->OpenBlock("some block id", &block);
//   size_t bytes_read = 0;
//   gscoped_ptr<ReadableBlock> tr_block(new CountingReadableBlock(block.Pass(), &bytes_read));
//   tr_block->Read(0, 100, ...);
//   tr_block->Read(0, 200, ...);
//   ASSERT_EQ(300, bytes_read);
//
class CountingReadableBlock : public ReadableBlock {
 public:
  CountingReadableBlock(gscoped_ptr<ReadableBlock> block, size_t* bytes_read)
    : block_(block.Pass()),
      bytes_read_(bytes_read) {
  }

  virtual const BlockId& id() const override {
    return block_->id();
  }

  virtual CHECKED_STATUS Close() override {
    return block_->Close();
  }

  virtual CHECKED_STATUS Size(uint64_t* sz) const override {
    return block_->Size(sz);
  }

  virtual CHECKED_STATUS Read(uint64_t offset, size_t length,
                      Slice* result, uint8_t* scratch) const override {
    RETURN_NOT_OK(block_->Read(offset, length, result, scratch));
    *bytes_read_ += length;
    return Status::OK();
  }

  virtual size_t memory_footprint() const override {
    return block_->memory_footprint();
  }

 private:
  gscoped_ptr<ReadableBlock> block_;
  size_t* bytes_read_;
};

} // namespace fs
} // namespace yb

#endif
