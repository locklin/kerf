#pragma once
namespace KERF_NAMESPACE {

// 2026.02.20 I think this is correct but it's been a long break
// this describes how to do real pooling (beyond always-malloc() always-free()) without "leaking" cross-thread
// Threaded global memory model. Either
// i. Leak: You leak via passing between thread (thread-to-thread) via the global tree non-released atomish size memory (frags accumulate and never release - they're passing in one direction from one thread's pool to another thread's pool instead of being "reused" by the originating thread, and so overflowing the lanes in the second pool). (Unless you pause the world and garbage collect - no)
// ii. Malloc/free: You disable the pooler mostly and replace small items with malloc/free -os handles. Some perf penalty.
// iii. Page: The minimum pool size becomes a 4096 page which is no longer subdivided for atoms: then releasing it is trivial (unmap page when thread's repository lane becomes too large, either in batch (half) or one at a time)
// iv. Convert: Everything that hits the tree gets converted: enlarged past the pool max, in the kona terminology, or really just marked as releasable and converted so that they aren't repoolable (mapped disk objects are also releasable in addition to big in-memory lists). This sounds like a pain to track.
// v. Use pool but small pool items are all malloc'd so you can free() lanes when you detect the lanes get too full/overflow (half in batch or one at a time, probably one at a time is better) because another thread is feeding it/it's consuming from another thread off the global-tree. No subdividing. This avoids the fragment-reassembly problem, or more accurately, hands it to the OS. It also solves the cross thread leak problem. More efficient than pages. Fails gracefully. Big items can still be automatically freed when returned.
//
// the best soln is v. (of the first v)
// malloc'ing each atom will cause some bubbles because it has more overhead than inlining them on a single cache-efficient slab we manage
// for solns which malloc() atoms, we can counter with better presented-kerf-object-types which manage atoms inside of lists better, to avoid the malloc, but the problem never truly disappears I don't think, anyway it's small and bounded
// (in a single-threaded mode you could avoid soln v and just use the same pool strategy from kona which packs items tighter than malloc)


I PAGE_SIZE_BYTES = sysconf(_SC_PAGE_SIZE);

struct MEMORY_POOL
{
  void* pool_alloc_struct(I bytes_requested)
  {
    assert(POOL_LANE_STRUCT_MIN >= LOG_SLAB_WIDTH);

    void* v = pool_alloc_with_min_lane(bytes_requested, POOL_LANE_STRUCT_MIN);

#if TEST_TRACK_ALLOCATIONS
  if(test_alloc_tracker.contains(v)) {
    die(Test alloc tracker somehow already had this memory pointer element.)
  }
  test_alloc_tracker.insert(v);
#endif

    return v;
  }

  void pool_dealloc(SLAB* z);

#if TEST_TRACK_ALLOCATIONS
  std::set<void*> test_alloc_tracker;
#endif

protected:
  virtual void* pool_depool(char lane) = 0;
  virtual void* pool_anonymous_system_memory(int64_t requested_bytes, bool shared = 0) = 0; //share with forked children?
  virtual void pool_repool(void* v, char lane) = 0;

  char min_lane_for_bytes(I bytes_requested, char min_lane)
  {
    char lane = ceiling_log_2(bytes_requested);
    lane = MAX(lane, min_lane);
    return lane;
  }

  void* pool_alloc_with_min_lane(I bytes_requested, char min_lane)
  {
    void *z;
    char lane = min_lane_for_bytes(bytes_requested, min_lane);
    z = pool_depool(lane);

    assert(min_lane >= LOG_SLAB_WIDTH);
    char refs = 0;
    *(SLAB*)z = (SLAB){.m_memory_expansion_size=(UC)lane, .reference_management_arena=REFERENCE_MANAGEMENT_ARENA_CPP_WORKSTACK, .r_slab_reference_count=(UC)refs};

    return z;
  }

public:
  virtual ~MEMORY_POOL();

};

struct THREAD_SAFE_MALLOC_POOL : MEMORY_POOL
{
protected:
  virtual void* pool_depool(char lane)
  {
    return malloc(POW2(lane));
  }

  virtual void pool_repool(void* v, char lane)
  {
    free(v);
  }

  virtual void* pool_anonymous_system_memory(int64_t requested_bytes, bool shared = 0)
  {
    if(shared) {std::cerr << "cannot share malloc memory\n"; kerf_exit(-1);}
    return malloc(requested_bytes);
  }
};

struct THREAD_UNSAFE_MMAP_LANE_SLAB_POOL : MEMORY_POOL
{
  // POTENTIAL_OPTIMIZATION_POINT: a similar class which has lanes by BOTH {slab_width} X {object-type}
  // TODO see break.c for a long list of useful assert()'s we should implement

  // TODO have a #define for the minmum number of bytes we'll allocate via mmap ANON for slicing up for pool structs or for returning normally. This needs to be "high" relative to sysctl vm.max_map_count ~ 655300. On kerf1 we subdivided 1 page, the mmap minimum, and never actually ran into this problem.
};



}
