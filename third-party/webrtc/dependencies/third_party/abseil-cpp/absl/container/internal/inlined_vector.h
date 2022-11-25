// Copyright 2019 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_CONTAINER_INTERNAL_INLINED_VECTOR_INTERNAL_H_
#define ABSL_CONTAINER_INTERNAL_INLINED_VECTOR_INTERNAL_H_

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

#include "absl/base/macros.h"
#include "absl/container/internal/compressed_tuple.h"
#include "absl/memory/memory.h"
#include "absl/meta/type_traits.h"
#include "absl/types/span.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace inlined_vector_internal {

// GCC does not deal very well with the below code
#if !defined(__clang__) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

template <typename A>
using AllocatorTraits = std::allocator_traits<A>;
template <typename A>
using ValueType = typename AllocatorTraits<A>::value_type;
template <typename A>
using SizeType = typename AllocatorTraits<A>::size_type;
template <typename A>
using Pointer = typename AllocatorTraits<A>::pointer;
template <typename A>
using ConstPointer = typename AllocatorTraits<A>::const_pointer;
template <typename A>
using SizeType = typename AllocatorTraits<A>::size_type;
template <typename A>
using DifferenceType = typename AllocatorTraits<A>::difference_type;
template <typename A>
using Reference = ValueType<A>&;
template <typename A>
using ConstReference = const ValueType<A>&;
template <typename A>
using Iterator = Pointer<A>;
template <typename A>
using ConstIterator = ConstPointer<A>;
template <typename A>
using ReverseIterator = typename std::reverse_iterator<Iterator<A>>;
template <typename A>
using ConstReverseIterator = typename std::reverse_iterator<ConstIterator<A>>;
template <typename A>
using MoveIterator = typename std::move_iterator<Iterator<A>>;

template <typename Iterator>
using IsAtLeastForwardIterator = std::is_convertible<
    typename std::iterator_traits<Iterator>::iterator_category,
    std::forward_iterator_tag>;

template <typename A>
using IsMemcpyOk =
    absl::conjunction<std::is_same<A, std::allocator<ValueType<A>>>,
                      absl::is_trivially_copy_constructible<ValueType<A>>,
                      absl::is_trivially_copy_assignable<ValueType<A>>,
                      absl::is_trivially_destructible<ValueType<A>>>;

template <typename T>
struct TypeIdentity {
  using type = T;
};

// Used for function arguments in template functions to prevent ADL by forcing
// callers to explicitly specify the template parameter.
template <typename T>
using NoTypeDeduction = typename TypeIdentity<T>::type;

template <typename A>
void DestroyElements(NoTypeDeduction<A>& allocator, Pointer<A> destroy_first,
                     SizeType<A> destroy_size) {
  if (destroy_first != nullptr) {
    for (auto i = destroy_size; i != 0;) {
      --i;
      AllocatorTraits<A>::destroy(allocator, destroy_first + i);
    }
  }
}

// If kUseMemcpy is true, memcpy(dst, src, n); else do nothing.
// Useful to avoid compiler warnings when memcpy() is used for T values
// that are not trivially copyable in non-reachable code.
template <bool kUseMemcpy>
inline void MemcpyIfAllowed(void* dst, const void* src, size_t n);

// memcpy when allowed.
template <>
inline void MemcpyIfAllowed<true>(void* dst, const void* src, size_t n) {
  memcpy(dst, src, n);
}

// Do nothing for types that are not memcpy-able. This function is only
// called from non-reachable branches.
template <>
inline void MemcpyIfAllowed<false>(void*, const void*, size_t) {}

template <typename A, typename ValueAdapter>
void ConstructElements(NoTypeDeduction<A>& allocator,
                       Pointer<A> construct_first, ValueAdapter& values,
                       SizeType<A> construct_size) {
  for (SizeType<A> i = 0; i < construct_size; ++i) {
    ABSL_INTERNAL_TRY { values.ConstructNext(allocator, construct_first + i); }
    ABSL_INTERNAL_CATCH_ANY {
      DestroyElements<A>(allocator, construct_first, i);
      ABSL_INTERNAL_RETHROW;
    }
  }
}

template <typename A, typename ValueAdapter>
void AssignElements(Pointer<A> assign_first, ValueAdapter& values,
                    SizeType<A> assign_size) {
  for (SizeType<A> i = 0; i < assign_size; ++i) {
    values.AssignNext(assign_first + i);
  }
}

template <typename A>
struct StorageView {
  Pointer<A> data;
  SizeType<A> size;
  SizeType<A> capacity;
};

template <typename A, typename Iterator>
class IteratorValueAdapter {
 public:
  explicit IteratorValueAdapter(const Iterator& it) : it_(it) {}

  void ConstructNext(A& allocator, Pointer<A> construct_at) {
    AllocatorTraits<A>::construct(allocator, construct_at, *it_);
    ++it_;
  }

  void AssignNext(Pointer<A> assign_at) {
    *assign_at = *it_;
    ++it_;
  }

 private:
  Iterator it_;
};

template <typename A>
class CopyValueAdapter {
 public:
  explicit CopyValueAdapter(ConstPointer<A> p) : ptr_(p) {}

  void ConstructNext(A& allocator, Pointer<A> construct_at) {
    AllocatorTraits<A>::construct(allocator, construct_at, *ptr_);
  }

  void AssignNext(Pointer<A> assign_at) { *assign_at = *ptr_; }

 private:
  ConstPointer<A> ptr_;
};

template <typename A>
class DefaultValueAdapter {
 public:
  explicit DefaultValueAdapter() {}

  void ConstructNext(A& allocator, Pointer<A> construct_at) {
    AllocatorTraits<A>::construct(allocator, construct_at);
  }

  void AssignNext(Pointer<A> assign_at) { *assign_at = ValueType<A>(); }
};

template <typename A>
class AllocationTransaction {
 public:
  explicit AllocationTransaction(A& allocator)
      : allocator_data_(allocator, nullptr), capacity_(0) {}

  ~AllocationTransaction() {
    if (DidAllocate()) {
      AllocatorTraits<A>::deallocate(GetAllocator(), GetData(), GetCapacity());
    }
  }

  AllocationTransaction(const AllocationTransaction&) = delete;
  void operator=(const AllocationTransaction&) = delete;

  A& GetAllocator() { return allocator_data_.template get<0>(); }
  Pointer<A>& GetData() { return allocator_data_.template get<1>(); }
  SizeType<A>& GetCapacity() { return capacity_; }

  bool DidAllocate() { return GetData() != nullptr; }
  Pointer<A> Allocate(SizeType<A> capacity) {
    GetData() = AllocatorTraits<A>::allocate(GetAllocator(), capacity);
    GetCapacity() = capacity;
    return GetData();
  }

  void Reset() {
    GetData() = nullptr;
    GetCapacity() = 0;
  }

 private:
  container_internal::CompressedTuple<A, Pointer<A>> allocator_data_;
  SizeType<A> capacity_;
};

template <typename A>
class ConstructionTransaction {
 public:
  explicit ConstructionTransaction(A& allocator)
      : allocator_data_(allocator, nullptr), size_(0) {}

  ~ConstructionTransaction() {
    if (DidConstruct()) {
      DestroyElements<A>(GetAllocator(), GetData(), GetSize());
    }
  }

  ConstructionTransaction(const ConstructionTransaction&) = delete;
  void operator=(const ConstructionTransaction&) = delete;

  A& GetAllocator() { return allocator_data_.template get<0>(); }
  Pointer<A>& GetData() { return allocator_data_.template get<1>(); }
  SizeType<A>& GetSize() { return size_; }

  bool DidConstruct() { return GetData() != nullptr; }
  template <typename ValueAdapter>
  void Construct(Pointer<A> data, ValueAdapter& values, SizeType<A> size) {
    ConstructElements<A>(GetAllocator(), data, values, size);
    GetData() = data;
    GetSize() = size;
  }
  void Commit() {
    GetData() = nullptr;
    GetSize() = 0;
  }

 private:
  container_internal::CompressedTuple<A, Pointer<A>> allocator_data_;
  SizeType<A> size_;
};

template <typename T, size_t N, typename A>
class Storage {
 public:
  static SizeType<A> NextCapacity(SizeType<A> current_capacity) {
    return current_capacity * 2;
  }

  static SizeType<A> ComputeCapacity(SizeType<A> current_capacity,
                                     SizeType<A> requested_capacity) {
    return (std::max)(NextCapacity(current_capacity), requested_capacity);
  }

  // ---------------------------------------------------------------------------
  // Storage Constructors and Destructor
  // ---------------------------------------------------------------------------

  Storage() : metadata_(A(), /* size and is_allocated */ 0) {}

  explicit Storage(const A& allocator)
      : metadata_(allocator, /* size and is_allocated */ 0) {}

  ~Storage() {
    if (GetSizeAndIsAllocated() == 0) {
      // Empty and not allocated; nothing to do.
    } else if (IsMemcpyOk<A>::value) {
      // No destructors need to be run; just deallocate if necessary.
      DeallocateIfAllocated();
    } else {
      DestroyContents();
    }
  }

  // ---------------------------------------------------------------------------
  // Storage Member Accessors
  // ---------------------------------------------------------------------------

  SizeType<A>& GetSizeAndIsAllocated() { return metadata_.template get<1>(); }

  const SizeType<A>& GetSizeAndIsAllocated() const {
    return metadata_.template get<1>();
  }

  SizeType<A> GetSize() const { return GetSizeAndIsAllocated() >> 1; }

  bool GetIsAllocated() const { return GetSizeAndIsAllocated() & 1; }

  Pointer<A> GetAllocatedData() { return data_.allocated.allocated_data; }

  ConstPointer<A> GetAllocatedData() const {
    return data_.allocated.allocated_data;
  }

  Pointer<A> GetInlinedData() {
    return reinterpret_cast<Pointer<A>>(
        std::addressof(data_.inlined.inlined_data[0]));
  }

  ConstPointer<A> GetInlinedData() const {
    return reinterpret_cast<ConstPointer<A>>(
        std::addressof(data_.inlined.inlined_data[0]));
  }

  SizeType<A> GetAllocatedCapacity() const {
    return data_.allocated.allocated_capacity;
  }

  SizeType<A> GetInlinedCapacity() const { return static_cast<SizeType<A>>(N); }

  StorageView<A> MakeStorageView() {
    return GetIsAllocated() ? StorageView<A>{GetAllocatedData(), GetSize(),
                                             GetAllocatedCapacity()}
                            : StorageView<A>{GetInlinedData(), GetSize(),
                                             GetInlinedCapacity()};
  }

  A& GetAllocator() { return metadata_.template get<0>(); }

  const A& GetAllocator() const { return metadata_.template get<0>(); }

  // ---------------------------------------------------------------------------
  // Storage Member Mutators
  // ---------------------------------------------------------------------------

  ABSL_ATTRIBUTE_NOINLINE void InitFrom(const Storage& other);

  template <typename ValueAdapter>
  void Initialize(ValueAdapter values, SizeType<A> new_size);

  template <typename ValueAdapter>
  void Assign(ValueAdapter values, SizeType<A> new_size);

  template <typename ValueAdapter>
  void Resize(ValueAdapter values, SizeType<A> new_size);

  template <typename ValueAdapter>
  Iterator<A> Insert(ConstIterator<A> pos, ValueAdapter values,
                     SizeType<A> insert_count);

  template <typename... Args>
  Reference<A> EmplaceBack(Args&&... args);

  Iterator<A> Erase(ConstIterator<A> from, ConstIterator<A> to);

  void Reserve(SizeType<A> requested_capacity);

  void ShrinkToFit();

  void Swap(Storage* other_storage_ptr);

  void SetIsAllocated() {
    GetSizeAndIsAllocated() |= static_cast<SizeType<A>>(1);
  }

  void UnsetIsAllocated() {
    GetSizeAndIsAllocated() &= ((std::numeric_limits<SizeType<A>>::max)() - 1);
  }

  void SetSize(SizeType<A> size) {
    GetSizeAndIsAllocated() =
        (size << 1) | static_cast<SizeType<A>>(GetIsAllocated());
  }

  void SetAllocatedSize(SizeType<A> size) {
    GetSizeAndIsAllocated() = (size << 1) | static_cast<SizeType<A>>(1);
  }

  void SetInlinedSize(SizeType<A> size) {
    GetSizeAndIsAllocated() = size << static_cast<SizeType<A>>(1);
  }

  void AddSize(SizeType<A> count) {
    GetSizeAndIsAllocated() += count << static_cast<SizeType<A>>(1);
  }

  void SubtractSize(SizeType<A> count) {
    assert(count <= GetSize());

    GetSizeAndIsAllocated() -= count << static_cast<SizeType<A>>(1);
  }

  void SetAllocatedData(Pointer<A> data, SizeType<A> capacity) {
    data_.allocated.allocated_data = data;
    data_.allocated.allocated_capacity = capacity;
  }

  void AcquireAllocatedData(AllocationTransaction<A>& allocation_tx) {
    SetAllocatedData(allocation_tx.GetData(), allocation_tx.GetCapacity());

    allocation_tx.Reset();
  }

  void MemcpyFrom(const Storage& other_storage) {
    assert(IsMemcpyOk<A>::value || other_storage.GetIsAllocated());

    GetSizeAndIsAllocated() = other_storage.GetSizeAndIsAllocated();
    data_ = other_storage.data_;
  }

  void DeallocateIfAllocated() {
    if (GetIsAllocated()) {
      AllocatorTraits<A>::deallocate(GetAllocator(), GetAllocatedData(),
                                     GetAllocatedCapacity());
    }
  }

 private:
  ABSL_ATTRIBUTE_NOINLINE void DestroyContents();

  using Metadata = container_internal::CompressedTuple<A, SizeType<A>>;

  struct Allocated {
    Pointer<A> allocated_data;
    SizeType<A> allocated_capacity;
  };

  struct Inlined {
    alignas(ValueType<A>) char inlined_data[sizeof(ValueType<A>[N])];
  };

  union Data {
    Allocated allocated;
    Inlined inlined;
  };

  template <typename... Args>
  ABSL_ATTRIBUTE_NOINLINE Reference<A> EmplaceBackSlow(Args&&... args);

  Metadata metadata_;
  Data data_;
};

template <typename T, size_t N, typename A>
void Storage<T, N, A>::DestroyContents() {
  Pointer<A> data = GetIsAllocated() ? GetAllocatedData() : GetInlinedData();
  DestroyElements<A>(GetAllocator(), data, GetSize());
  DeallocateIfAllocated();
}

template <typename T, size_t N, typename A>
void Storage<T, N, A>::InitFrom(const Storage& other) {
  const auto n = other.GetSize();
  assert(n > 0);  // Empty sources handled handled in caller.
  ConstPointer<A> src;
  Pointer<A> dst;
  if (!other.GetIsAllocated()) {
    dst = GetInlinedData();
    src = other.GetInlinedData();
  } else {
    // Because this is only called from the `InlinedVector` constructors, it's
    // safe to take on the allocation with size `0`. If `ConstructElements(...)`
    // throws, deallocation will be automatically handled by `~Storage()`.
    SizeType<A> new_capacity = ComputeCapacity(GetInlinedCapacity(), n);
    dst = AllocatorTraits<A>::allocate(GetAllocator(), new_capacity);
    SetAllocatedData(dst, new_capacity);
    src = other.GetAllocatedData();
  }
  if (IsMemcpyOk<A>::value) {
    MemcpyIfAllowed<IsMemcpyOk<A>::value>(dst, src, sizeof(dst[0]) * n);
  } else {
    auto values = IteratorValueAdapter<A, ConstPointer<A>>(src);
    ConstructElements<A>(GetAllocator(), dst, values, n);
  }
  GetSizeAndIsAllocated() = other.GetSizeAndIsAllocated();
}

template <typename T, size_t N, typename A>
template <typename ValueAdapter>
auto Storage<T, N, A>::Initialize(ValueAdapter values, SizeType<A> new_size)
    -> void {
  // Only callable from constructors!
  assert(!GetIsAllocated());
  assert(GetSize() == 0);

  Pointer<A> construct_data;
  if (new_size > GetInlinedCapacity()) {
    // Because this is only called from the `InlinedVector` constructors, it's
    // safe to take on the allocation with size `0`. If `ConstructElements(...)`
    // throws, deallocation will be automatically handled by `~Storage()`.
    SizeType<A> new_capacity = ComputeCapacity(GetInlinedCapacity(), new_size);
    construct_data = AllocatorTraits<A>::allocate(GetAllocator(), new_capacity);
    SetAllocatedData(construct_data, new_capacity);
    SetIsAllocated();
  } else {
    construct_data = GetInlinedData();
  }

  ConstructElements<A>(GetAllocator(), construct_data, values, new_size);

  // Since the initial size was guaranteed to be `0` and the allocated bit is
  // already correct for either case, *adding* `new_size` gives us the correct
  // result faster than setting it directly.
  AddSize(new_size);
}

template <typename T, size_t N, typename A>
template <typename ValueAdapter>
auto Storage<T, N, A>::Assign(ValueAdapter values, SizeType<A> new_size)
    -> void {
  StorageView<A> storage_view = MakeStorageView();

  AllocationTransaction<A> allocation_tx(GetAllocator());

  absl::Span<ValueType<A>> assign_loop;
  absl::Span<ValueType<A>> construct_loop;
  absl::Span<ValueType<A>> destroy_loop;

  if (new_size > storage_view.capacity) {
    SizeType<A> new_capacity = ComputeCapacity(storage_view.capacity, new_size);
    construct_loop = {allocation_tx.Allocate(new_capacity), new_size};
    destroy_loop = {storage_view.data, storage_view.size};
  } else if (new_size > storage_view.size) {
    assign_loop = {storage_view.data, storage_view.size};
    construct_loop = {storage_view.data + storage_view.size,
                      new_size - storage_view.size};
  } else {
    assign_loop = {storage_view.data, new_size};
    destroy_loop = {storage_view.data + new_size, storage_view.size - new_size};
  }

  AssignElements<A>(assign_loop.data(), values, assign_loop.size());

  ConstructElements<A>(GetAllocator(), construct_loop.data(), values,
                       construct_loop.size());

  DestroyElements<A>(GetAllocator(), destroy_loop.data(), destroy_loop.size());

  if (allocation_tx.DidAllocate()) {
    DeallocateIfAllocated();
    AcquireAllocatedData(allocation_tx);
    SetIsAllocated();
  }

  SetSize(new_size);
}

template <typename T, size_t N, typename A>
template <typename ValueAdapter>
auto Storage<T, N, A>::Resize(ValueAdapter values, SizeType<A> new_size)
    -> void {
  StorageView<A> storage_view = MakeStorageView();
  auto* const base = storage_view.data;
  const SizeType<A> size = storage_view.size;
  auto& alloc = GetAllocator();
  if (new_size <= size) {
    // Destroy extra old elements.
    DestroyElements<A>(alloc, base + new_size, size - new_size);
  } else if (new_size <= storage_view.capacity) {
    // Construct new elements in place.
    ConstructElements<A>(alloc, base + size, values, new_size - size);
  } else {
    // Steps:
    //  a. Allocate new backing store.
    //  b. Construct new elements in new backing store.
    //  c. Move existing elements from old backing store to now.
    //  d. Destroy all elements in old backing store.
    // Use transactional wrappers for the first two steps so we can roll
    // back if necessary due to exceptions.
    AllocationTransaction<A> allocation_tx(alloc);
    SizeType<A> new_capacity = ComputeCapacity(storage_view.capacity, new_size);
    Pointer<A> new_data = allocation_tx.Allocate(new_capacity);

    ConstructionTransaction<A> construction_tx(alloc);
    construction_tx.Construct(new_data + size, values, new_size - size);

    IteratorValueAdapter<A, MoveIterator<A>> move_values(
        (MoveIterator<A>(base)));
    ConstructElements<A>(alloc, new_data, move_values, size);

    DestroyElements<A>(alloc, base, size);
    construction_tx.Commit();
    DeallocateIfAllocated();
    AcquireAllocatedData(allocation_tx);
    SetIsAllocated();
  }
  SetSize(new_size);
}

template <typename T, size_t N, typename A>
template <typename ValueAdapter>
auto Storage<T, N, A>::Insert(ConstIterator<A> pos, ValueAdapter values,
                              SizeType<A> insert_count) -> Iterator<A> {
  StorageView<A> storage_view = MakeStorageView();

  SizeType<A> insert_index =
      std::distance(ConstIterator<A>(storage_view.data), pos);
  SizeType<A> insert_end_index = insert_index + insert_count;
  SizeType<A> new_size = storage_view.size + insert_count;

  if (new_size > storage_view.capacity) {
    AllocationTransaction<A> allocation_tx(GetAllocator());
    ConstructionTransaction<A> construction_tx(GetAllocator());
    ConstructionTransaction<A> move_construction_tx(GetAllocator());

    IteratorValueAdapter<A, MoveIterator<A>> move_values(
        MoveIterator<A>(storage_view.data));

    SizeType<A> new_capacity = ComputeCapacity(storage_view.capacity, new_size);
    Pointer<A> new_data = allocation_tx.Allocate(new_capacity);

    construction_tx.Construct(new_data + insert_index, values, insert_count);

    move_construction_tx.Construct(new_data, move_values, insert_index);

    ConstructElements<A>(GetAllocator(), new_data + insert_end_index,
                         move_values, storage_view.size - insert_index);

    DestroyElements<A>(GetAllocator(), storage_view.data, storage_view.size);

    construction_tx.Commit();
    move_construction_tx.Commit();
    DeallocateIfAllocated();
    AcquireAllocatedData(allocation_tx);

    SetAllocatedSize(new_size);
    return Iterator<A>(new_data + insert_index);
  } else {
    SizeType<A> move_construction_destination_index =
        (std::max)(insert_end_index, storage_view.size);

    ConstructionTransaction<A> move_construction_tx(GetAllocator());

    IteratorValueAdapter<A, MoveIterator<A>> move_construction_values(
        MoveIterator<A>(storage_view.data +
                        (move_construction_destination_index - insert_count)));
    absl::Span<ValueType<A>> move_construction = {
        storage_view.data + move_construction_destination_index,
        new_size - move_construction_destination_index};

    Pointer<A> move_assignment_values = storage_view.data + insert_index;
    absl::Span<ValueType<A>> move_assignment = {
        storage_view.data + insert_end_index,
        move_construction_destination_index - insert_end_index};

    absl::Span<ValueType<A>> insert_assignment = {move_assignment_values,
                                                  move_construction.size()};

    absl::Span<ValueType<A>> insert_construction = {
        insert_assignment.data() + insert_assignment.size(),
        insert_count - insert_assignment.size()};

    move_construction_tx.Construct(move_construction.data(),
                                   move_construction_values,
                                   move_construction.size());

    for (Pointer<A>
             destination = move_assignment.data() + move_assignment.size(),
             last_destination = move_assignment.data(),
             source = move_assignment_values + move_assignment.size();
         ;) {
      --destination;
      --source;
      if (destination < last_destination) break;
      *destination = std::move(*source);
    }

    AssignElements<A>(insert_assignment.data(), values,
                      insert_assignment.size());

    ConstructElements<A>(GetAllocator(), insert_construction.data(), values,
                         insert_construction.size());

    move_construction_tx.Commit();

    AddSize(insert_count);
    return Iterator<A>(storage_view.data + insert_index);
  }
}

template <typename T, size_t N, typename A>
template <typename... Args>
auto Storage<T, N, A>::EmplaceBack(Args&&... args) -> Reference<A> {
  StorageView<A> storage_view = MakeStorageView();
  const auto n = storage_view.size;
  if (ABSL_PREDICT_TRUE(n != storage_view.capacity)) {
    // Fast path; new element fits.
    Pointer<A> last_ptr = storage_view.data + n;
    AllocatorTraits<A>::construct(GetAllocator(), last_ptr,
                                  std::forward<Args>(args)...);
    AddSize(1);
    return *last_ptr;
  }
  // TODO(b/173712035): Annotate with musttail attribute to prevent regression.
  return EmplaceBackSlow(std::forward<Args>(args)...);
}

template <typename T, size_t N, typename A>
template <typename... Args>
auto Storage<T, N, A>::EmplaceBackSlow(Args&&... args) -> Reference<A> {
  StorageView<A> storage_view = MakeStorageView();
  AllocationTransaction<A> allocation_tx(GetAllocator());
  IteratorValueAdapter<A, MoveIterator<A>> move_values(
      MoveIterator<A>(storage_view.data));
  SizeType<A> new_capacity = NextCapacity(storage_view.capacity);
  Pointer<A> construct_data = allocation_tx.Allocate(new_capacity);
  Pointer<A> last_ptr = construct_data + storage_view.size;

  // Construct new element.
  AllocatorTraits<A>::construct(GetAllocator(), last_ptr,
                                std::forward<Args>(args)...);
  // Move elements from old backing store to new backing store.
  ABSL_INTERNAL_TRY {
    ConstructElements<A>(GetAllocator(), allocation_tx.GetData(), move_values,
                         storage_view.size);
  }
  ABSL_INTERNAL_CATCH_ANY {
    AllocatorTraits<A>::destroy(GetAllocator(), last_ptr);
    ABSL_INTERNAL_RETHROW;
  }
  // Destroy elements in old backing store.
  DestroyElements<A>(GetAllocator(), storage_view.data, storage_view.size);

  DeallocateIfAllocated();
  AcquireAllocatedData(allocation_tx);
  SetIsAllocated();
  AddSize(1);
  return *last_ptr;
}

template <typename T, size_t N, typename A>
auto Storage<T, N, A>::Erase(ConstIterator<A> from, ConstIterator<A> to)
    -> Iterator<A> {
  StorageView<A> storage_view = MakeStorageView();

  SizeType<A> erase_size = std::distance(from, to);
  SizeType<A> erase_index =
      std::distance(ConstIterator<A>(storage_view.data), from);
  SizeType<A> erase_end_index = erase_index + erase_size;

  IteratorValueAdapter<A, MoveIterator<A>> move_values(
      MoveIterator<A>(storage_view.data + erase_end_index));

  AssignElements<A>(storage_view.data + erase_index, move_values,
                    storage_view.size - erase_end_index);

  DestroyElements<A>(GetAllocator(),
                     storage_view.data + (storage_view.size - erase_size),
                     erase_size);

  SubtractSize(erase_size);
  return Iterator<A>(storage_view.data + erase_index);
}

template <typename T, size_t N, typename A>
auto Storage<T, N, A>::Reserve(SizeType<A> requested_capacity) -> void {
  StorageView<A> storage_view = MakeStorageView();

  if (ABSL_PREDICT_FALSE(requested_capacity <= storage_view.capacity)) return;

  AllocationTransaction<A> allocation_tx(GetAllocator());

  IteratorValueAdapter<A, MoveIterator<A>> move_values(
      MoveIterator<A>(storage_view.data));

  SizeType<A> new_capacity =
      ComputeCapacity(storage_view.capacity, requested_capacity);
  Pointer<A> new_data = allocation_tx.Allocate(new_capacity);

  ConstructElements<A>(GetAllocator(), new_data, move_values,
                       storage_view.size);

  DestroyElements<A>(GetAllocator(), storage_view.data, storage_view.size);

  DeallocateIfAllocated();
  AcquireAllocatedData(allocation_tx);
  SetIsAllocated();
}

template <typename T, size_t N, typename A>
auto Storage<T, N, A>::ShrinkToFit() -> void {
  // May only be called on allocated instances!
  assert(GetIsAllocated());

  StorageView<A> storage_view{GetAllocatedData(), GetSize(),
                              GetAllocatedCapacity()};

  if (ABSL_PREDICT_FALSE(storage_view.size == storage_view.capacity)) return;

  AllocationTransaction<A> allocation_tx(GetAllocator());

  IteratorValueAdapter<A, MoveIterator<A>> move_values(
      MoveIterator<A>(storage_view.data));

  Pointer<A> construct_data;
  if (storage_view.size > GetInlinedCapacity()) {
    SizeType<A> new_capacity = storage_view.size;
    construct_data = allocation_tx.Allocate(new_capacity);
  } else {
    construct_data = GetInlinedData();
  }

  ABSL_INTERNAL_TRY {
    ConstructElements<A>(GetAllocator(), construct_data, move_values,
                         storage_view.size);
  }
  ABSL_INTERNAL_CATCH_ANY {
    SetAllocatedData(storage_view.data, storage_view.capacity);
    ABSL_INTERNAL_RETHROW;
  }

  DestroyElements<A>(GetAllocator(), storage_view.data, storage_view.size);

  AllocatorTraits<A>::deallocate(GetAllocator(), storage_view.data,
                                 storage_view.capacity);

  if (allocation_tx.DidAllocate()) {
    AcquireAllocatedData(allocation_tx);
  } else {
    UnsetIsAllocated();
  }
}

template <typename T, size_t N, typename A>
auto Storage<T, N, A>::Swap(Storage* other_storage_ptr) -> void {
  using std::swap;
  assert(this != other_storage_ptr);

  if (GetIsAllocated() && other_storage_ptr->GetIsAllocated()) {
    swap(data_.allocated, other_storage_ptr->data_.allocated);
  } else if (!GetIsAllocated() && !other_storage_ptr->GetIsAllocated()) {
    Storage* small_ptr = this;
    Storage* large_ptr = other_storage_ptr;
    if (small_ptr->GetSize() > large_ptr->GetSize()) swap(small_ptr, large_ptr);

    for (SizeType<A> i = 0; i < small_ptr->GetSize(); ++i) {
      swap(small_ptr->GetInlinedData()[i], large_ptr->GetInlinedData()[i]);
    }

    IteratorValueAdapter<A, MoveIterator<A>> move_values(
        MoveIterator<A>(large_ptr->GetInlinedData() + small_ptr->GetSize()));

    ConstructElements<A>(large_ptr->GetAllocator(),
                         small_ptr->GetInlinedData() + small_ptr->GetSize(),
                         move_values,
                         large_ptr->GetSize() - small_ptr->GetSize());

    DestroyElements<A>(large_ptr->GetAllocator(),
                       large_ptr->GetInlinedData() + small_ptr->GetSize(),
                       large_ptr->GetSize() - small_ptr->GetSize());
  } else {
    Storage* allocated_ptr = this;
    Storage* inlined_ptr = other_storage_ptr;
    if (!allocated_ptr->GetIsAllocated()) swap(allocated_ptr, inlined_ptr);

    StorageView<A> allocated_storage_view{
        allocated_ptr->GetAllocatedData(), allocated_ptr->GetSize(),
        allocated_ptr->GetAllocatedCapacity()};

    IteratorValueAdapter<A, MoveIterator<A>> move_values(
        MoveIterator<A>(inlined_ptr->GetInlinedData()));

    ABSL_INTERNAL_TRY {
      ConstructElements<A>(inlined_ptr->GetAllocator(),
                           allocated_ptr->GetInlinedData(), move_values,
                           inlined_ptr->GetSize());
    }
    ABSL_INTERNAL_CATCH_ANY {
      allocated_ptr->SetAllocatedData(allocated_storage_view.data,
                                      allocated_storage_view.capacity);
      ABSL_INTERNAL_RETHROW;
    }

    DestroyElements<A>(inlined_ptr->GetAllocator(),
                       inlined_ptr->GetInlinedData(), inlined_ptr->GetSize());

    inlined_ptr->SetAllocatedData(allocated_storage_view.data,
                                  allocated_storage_view.capacity);
  }

  swap(GetSizeAndIsAllocated(), other_storage_ptr->GetSizeAndIsAllocated());
  swap(GetAllocator(), other_storage_ptr->GetAllocator());
}

// End ignore "array-bounds" and "maybe-uninitialized"
#if !defined(__clang__) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace inlined_vector_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_CONTAINER_INTERNAL_INLINED_VECTOR_INTERNAL_H_