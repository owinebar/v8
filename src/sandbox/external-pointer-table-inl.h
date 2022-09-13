// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SANDBOX_EXTERNAL_POINTER_TABLE_INL_H_
#define V8_SANDBOX_EXTERNAL_POINTER_TABLE_INL_H_

#include "src/base/atomicops.h"
#include "src/sandbox/external-pointer-table.h"
#include "src/sandbox/external-pointer.h"
#include "src/utils/allocation.h"

#ifdef V8_COMPRESS_POINTERS

namespace v8 {
namespace internal {

Address ExternalPointerTable::Get(ExternalPointerHandle handle,
                                  ExternalPointerTag tag) const {
  uint32_t index = HandleToIndex(handle);
  Entry entry = RelaxedLoad(index);
  DCHECK(entry.IsRegularEntry());
  return entry.Untag(tag);
}

void ExternalPointerTable::Set(ExternalPointerHandle handle, Address value,
                               ExternalPointerTag tag) {
  DCHECK_NE(kNullExternalPointerHandle, handle);
  DCHECK_EQ(0, value & kExternalPointerTagMask);
  DCHECK(tag & kExternalPointerMarkBit);

  uint32_t index = HandleToIndex(handle);
  Entry entry = Entry::MakeRegularEntry(value, tag);
  RelaxedStore(index, entry);
}

Address ExternalPointerTable::Exchange(ExternalPointerHandle handle,
                                       Address value, ExternalPointerTag tag) {
  DCHECK_NE(kNullExternalPointerHandle, handle);
  DCHECK_EQ(0, value & kExternalPointerTagMask);
  DCHECK(tag & kExternalPointerMarkBit);

  uint32_t index = HandleToIndex(handle);
  Entry new_entry = Entry::MakeRegularEntry(value, tag);
  Entry old_entry = RelaxedExchange(index, new_entry);
  DCHECK(old_entry.IsRegularEntry());
  return old_entry.Untag(tag);
}

bool ExternalPointerTable::TryAllocateEntryFromFreelist(
    uint32_t freelist_head) {
  DCHECK(freelist_head);
  DCHECK_LT(freelist_head, capacity());

  Entry entry = RelaxedLoad(freelist_head);
  uint32_t new_freelist_head = entry.ExtractNextFreelistEntry();

  uint32_t old_val = base::Relaxed_CompareAndSwap(
      &freelist_head_, freelist_head, new_freelist_head);
  bool success = old_val == freelist_head;

  // When the CAS succeeded, the entry must've been a freelist entry.
  // Otherwise, this is not guaranteed as another thread may have allocated
  // the same entry in the meantime.
  DCHECK(!success || entry.IsFreelistEntry());
  return success;
}

ExternalPointerHandle ExternalPointerTable::AllocateAndInitializeEntry(
    Isolate* isolate, Address initial_value, ExternalPointerTag tag) {
  DCHECK(is_initialized());

  uint32_t freelist_head;
  bool success = false;
  while (!success) {
    // This is essentially DCLP (see
    // https://preshing.com/20130930/double-checked-locking-is-fixed-in-cpp11/)
    // and so requires an acquire load as well as a release store in Grow() to
    // prevent reordering of memory accesses, which could for example cause one
    // thread to read a freelist entry before it has been properly initialized.
    freelist_head = base::Acquire_Load(&freelist_head_);
    if (!freelist_head) {
      // Freelist is empty. Need to take the lock, then attempt to grow the
      // table if no other thread has done it in the meantime.
      base::MutexGuard guard(mutex_);

      // Reload freelist head in case another thread already grew the table.
      freelist_head = base::Relaxed_Load(&freelist_head_);

      if (!freelist_head) {
        // Freelist is (still) empty so grow the table.
        freelist_head = Grow(isolate);
      }
    }

    success = TryAllocateEntryFromFreelist(freelist_head);
  }

  Entry entry = Entry::MakeRegularEntry(initial_value, tag);
  RelaxedStore(freelist_head, entry);

  return IndexToHandle(freelist_head);
}

ExternalPointerHandle ExternalPointerTable::AllocateEvacuationEntry(
    uint32_t start_of_evacuation_area) {
  DCHECK(is_initialized());

  uint32_t freelist_head;
  bool success = false;
  while (!success) {
    freelist_head = base::Acquire_Load(&freelist_head_);
    // Check that the next free entry is below the start of the evacuation area.
    if (!freelist_head || freelist_head >= start_of_evacuation_area)
      return kNullExternalPointerHandle;

    success = TryAllocateEntryFromFreelist(freelist_head);
  }

  return IndexToHandle(freelist_head);
}

uint32_t ExternalPointerTable::FreelistSize() {
  Entry entry;
  do {
    uint32_t freelist_head = base::Relaxed_Load(&freelist_head_);
    if (!freelist_head) return 0;
    entry = RelaxedLoad(freelist_head);
  } while (!entry.IsFreelistEntry());
  uint32_t freelist_size = entry.ExtractFreelistSize();
  DCHECK_LE(freelist_size, capacity());
  return freelist_size;
}

void ExternalPointerTable::Mark(ExternalPointerHandle handle,
                                Address handle_location) {
  static_assert(sizeof(base::Atomic64) == sizeof(Address));
  DCHECK_EQ(handle, *reinterpret_cast<ExternalPointerHandle*>(handle_location));

  uint32_t index = HandleToIndex(handle);

  // Check if the entry should be evacuated for table compaction.
  // The current value of the start of the evacuation area is cached in a local
  // variable here as it otherwise may be changed by another marking thread
  // while this method runs, causing non-optimal behaviour (for example, the
  // allocation of an evacuation entry _after_ the entry that is evacuated).
  uint32_t current_start_of_evacuation_area = start_of_evacuation_area();
  if (index >= current_start_of_evacuation_area) {
    DCHECK(IsCompacting());
    ExternalPointerHandle new_handle =
        AllocateEvacuationEntry(current_start_of_evacuation_area);
    if (new_handle) {
      DCHECK_LT(HandleToIndex(new_handle), current_start_of_evacuation_area);
      uint32_t index = HandleToIndex(new_handle);
      // No need for an atomic store as the entry will only be accessed during
      // sweeping.
      Store(index, Entry::MakeEvacuationEntry(handle_location));
#ifdef DEBUG
      // Mark the handle as visited in debug builds to detect double
      // initialization of external pointer fields.
      auto handle_ptr = reinterpret_cast<base::Atomic32*>(handle_location);
      base::Relaxed_Store(handle_ptr, handle | kVisitedHandleMarker);
#endif  // DEBUG
    } else {
      // In this case, the application has allocated a sufficiently large
      // number of entries from the freelist so that new entries would now be
      // allocated inside the area that is being compacted. While it would be
      // possible to shrink that area and continue compacting, we probably do
      // not want to put more pressure on the freelist and so instead simply
      // abort compaction here. Entries that have already been visited will
      // still be compacted during Sweep, but there is no guarantee that any
      // blocks at the end of the table will now be completely free.
      uint32_t compaction_aborted_marker =
          current_start_of_evacuation_area | kCompactionAbortedMarker;
      set_start_of_evacuation_area(compaction_aborted_marker);
    }
  }
  // Even if the entry is marked for evacuation, it still needs to be marked as
  // alive as it may be visited during sweeping before being evacuation.

  Entry old_entry = RelaxedLoad(index);
  DCHECK(old_entry.IsRegularEntry());

  Entry new_entry = old_entry;
  new_entry.SetMarkBit();

  // We don't need to perform the CAS in a loop: if the new value is not equal
  // to the old value, then the mutator must've just written a new value into
  // the entry. This in turn must've set the marking bit already (see
  // ExternalPointerTable::Set), so we don't need to do it again.
  Entry entry = RelaxedCompareAndSwap(index, old_entry, new_entry);
  DCHECK((entry == old_entry) || entry.IsMarked());
  USE(entry);
}

bool ExternalPointerTable::IsCompacting() {
  return start_of_evacuation_area() != kNotCompactingMarker;
}

bool ExternalPointerTable::CompactingWasAbortedDuringMarking() {
  return (start_of_evacuation_area() & kCompactionAbortedMarker) ==
         kCompactionAbortedMarker;
}

}  // namespace internal
}  // namespace v8

#endif  // V8_COMPRESS_POINTERS

#endif  // V8_SANDBOX_EXTERNAL_POINTER_TABLE_INL_H_