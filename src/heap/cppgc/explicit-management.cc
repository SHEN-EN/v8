// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/cppgc/explicit-management.h"

#include <algorithm>
#include <tuple>

#include "src/heap/cppgc/heap-base.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/heap-page.h"
#include "src/heap/cppgc/memory.h"

namespace cppgc {
namespace internal {

namespace {

bool InGC(HeapHandle& heap_handle) {
  const auto& heap = HeapBase::From(heap_handle);
  // Whenever the GC is active, avoid modifying the object as it may mess with
  // state that the GC needs.
  return heap.in_atomic_pause() || heap.marker() ||
         heap.sweeper().IsSweepingInProgress();
}

void InvalidateRememberedSlots(HeapBase& heap, void* begin, void* end) {
#if defined(CPPGC_YOUNG_GENERATION)
  // Invalidate slots that reside within |object|.
  auto& remembered_slots = heap.remembered_slots();
  // TODO(bikineev): The 2 binary walks can be optimized with a custom
  // algorithm.
  auto from = remembered_slots.lower_bound(begin),
       to = remembered_slots.lower_bound(end);
  remembered_slots.erase(from, to);
#ifdef ENABLE_SLOW_DCHECKS
  // Check that no remembered slots are referring to the freed area.
  DCHECK(std::none_of(remembered_slots.begin(), remembered_slots.end(),
                      [begin, end](void* slot) {
                        void* value = *reinterpret_cast<void**>(slot);
                        return begin <= value && value < end;
                      }));
#endif  // ENABLE_SLOW_DCHECKS
#endif  // !defined(CPPGC_YOUNG_GENERATION)
}

}  // namespace

void ExplicitManagementImpl::FreeUnreferencedObject(HeapHandle& heap_handle,
                                                    void* object) {
  if (InGC(heap_handle)) {
    return;
  }

  auto& header = HeapObjectHeader::FromObject(object);
  header.Finalize();

  size_t object_size = 0;
  USE(object_size);

  // `object` is guaranteed to be of type GarbageCollected, so getting the
  // BasePage is okay for regular and large objects.
  BasePage* base_page = BasePage::FromPayload(object);
  if (base_page->is_large()) {  // Large object.
    object_size = LargePage::From(base_page)->ObjectSize();
    base_page->space().RemovePage(base_page);
    base_page->heap().stats_collector()->NotifyExplicitFree(
        LargePage::From(base_page)->PayloadSize());
    LargePage::Destroy(LargePage::From(base_page));
  } else {  // Regular object.
    const size_t header_size = header.AllocatedSize();
    object_size = header.ObjectSize();
    auto* normal_page = NormalPage::From(base_page);
    auto& normal_space = *static_cast<NormalPageSpace*>(&base_page->space());
    auto& lab = normal_space.linear_allocation_buffer();
    ConstAddress payload_end = header.ObjectEnd();
    SetMemoryInaccessible(&header, header_size);
    if (payload_end == lab.start()) {  // Returning to LAB.
      lab.Set(reinterpret_cast<Address>(&header), lab.size() + header_size);
      normal_page->object_start_bitmap().ClearBit(lab.start());
    } else {  // Returning to free list.
      base_page->heap().stats_collector()->NotifyExplicitFree(header_size);
      normal_space.free_list().Add({&header, header_size});
      // No need to update the bitmap as the same bit is reused for the free
      // list entry.
    }
  }
  InvalidateRememberedSlots(HeapBase::From(heap_handle), object,
                            reinterpret_cast<uint8_t*>(object) + object_size);
}

namespace {

bool Grow(HeapObjectHeader& header, BasePage& base_page, size_t new_size,
          size_t size_delta) {
  DCHECK_GE(new_size, header.AllocatedSize() + kAllocationGranularity);
  DCHECK_GE(size_delta, kAllocationGranularity);
  DCHECK(!base_page.is_large());

  auto& normal_space = *static_cast<NormalPageSpace*>(&base_page.space());
  auto& lab = normal_space.linear_allocation_buffer();
  if (lab.start() == header.ObjectEnd() && lab.size() >= size_delta) {
    // LABs are considered used memory which means that no allocated size
    // adjustments are needed.
    Address delta_start = lab.Allocate(size_delta);
    SetMemoryAccessible(delta_start, size_delta);
    header.SetAllocatedSize(new_size);
    return true;
  }
  return false;
}

bool Shrink(HeapObjectHeader& header, BasePage& base_page, size_t new_size,
            size_t size_delta) {
  DCHECK_GE(header.AllocatedSize(), new_size + kAllocationGranularity);
  DCHECK_GE(size_delta, kAllocationGranularity);
  DCHECK(!base_page.is_large());

  auto& normal_space = *static_cast<NormalPageSpace*>(&base_page.space());
  auto& lab = normal_space.linear_allocation_buffer();
  Address free_start = header.ObjectEnd() - size_delta;
  if (lab.start() == header.ObjectEnd()) {
    DCHECK_EQ(free_start, lab.start() - size_delta);
    // LABs are considered used memory which means that no allocated size
    // adjustments are needed.
    lab.Set(free_start, lab.size() + size_delta);
    SetMemoryInaccessible(lab.start(), size_delta);
    header.SetAllocatedSize(new_size);
  } else if (size_delta >= ObjectAllocator::kSmallestSpaceSize) {
    // Heuristic: Only return memory to the free list if the block is larger
    // than the smallest size class.
    SetMemoryInaccessible(free_start, size_delta);
    base_page.heap().stats_collector()->NotifyExplicitFree(size_delta);
    normal_space.free_list().Add({free_start, size_delta});
    NormalPage::From(&base_page)->object_start_bitmap().SetBit(free_start);
    header.SetAllocatedSize(new_size);
  }
  InvalidateRememberedSlots(base_page.heap(), free_start,
                            free_start + size_delta);
  // Return success in any case, as we want to avoid that embedders start
  // copying memory because of small deltas.
  return true;
}

}  // namespace

bool ExplicitManagementImpl::Resize(void* object, size_t new_object_size) {
  // `object` is guaranteed to be of type GarbageCollected, so getting the
  // BasePage is okay for regular and large objects.
  BasePage* base_page = BasePage::FromPayload(object);

  if (InGC(base_page->heap())) {
    return false;
  }

  // TODO(chromium:1056170): Consider supporting large objects within certain
  // restrictions.
  if (base_page->is_large()) {
    return false;
  }

  const size_t new_size = RoundUp<kAllocationGranularity>(
      sizeof(HeapObjectHeader) + new_object_size);
  auto& header = HeapObjectHeader::FromObject(object);
  const size_t old_size = header.AllocatedSize();

  if (new_size > old_size) {
    return Grow(header, *base_page, new_size, new_size - old_size);
  } else if (old_size > new_size) {
    return Shrink(header, *base_page, new_size, old_size - new_size);
  }
  // Same size considering internal restrictions, e.g. alignment.
  return true;
}

}  // namespace internal
}  // namespace cppgc
