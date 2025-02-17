// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/bytecode-liveness-map.h"

namespace v8 {
namespace internal {
namespace compiler {

BytecodeLivenessMap::BytecodeLivenessMap(int bytecode_size, Zone* zone)
    : liveness_map_(base::bits::RoundUpToPowerOfTwo32(bytecode_size / 4 + 1),
                    base::KeyEqualityMatcher<int>(),
                    ZoneAllocationPolicy(zone)) {}

uint32_t OffsetHash(int offset) { return offset; }

BytecodeLiveness& BytecodeLivenessMap::InsertNewLiveness(int offset) {
  return liveness_map_.LookupOrInsert(offset, OffsetHash(offset))->value;
}

BytecodeLiveness& BytecodeLivenessMap::GetLiveness(int offset) {
  return liveness_map_.Lookup(offset, OffsetHash(offset))->value;
}

const BytecodeLiveness& BytecodeLivenessMap::GetLiveness(int offset) const {
  return liveness_map_.Lookup(offset, OffsetHash(offset))->value;
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
