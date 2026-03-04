#pragma once

namespace KERF_NAMESPACE {

SUTEX::SUTEX_GUARD::~SUTEX_GUARD()
{
  if(should_unlock)
  {
    sutex_unlock(sutex, exclusive, ignores_thread);
    return;
  }

#if DEBUG
  if(indeterminate)
  {
    er(Recovering a sutex lock which was left in an indeterminate state, likely due to an asynchronous ctrl-c signal)
    assert(sutex);
    sutex->zero_sutex_claimed_subset_inside_container();
    // die(TODO: recover)
    // TODO We want to zero indeterminate locks right away, but I think we need to build a list (std::vector) of them and wait until the end of the ctrl-c handler. (Zeroing the locks right away at least means building robust unlocks, and actually I think is impossible to rehabilitate because can allow writes to proceed when there are readers present.) Currently such locks are assumed not to live inside other thread's stacks (potentially though in our own) and can be assumed to live inside kerf-tree/other parented areas.
    // Question. Can we just signal-safe-wrap the SUTEX_GUARD constructor? 
    // Idea. We could have a "don't pthread_kill me quite yet" per-thread atomic flag (mutex) that main_thread could wait on to acquire. This thread sets it before entering the critical section and unsets it as it leaves. There is no contention except at ctrl-c time. Then we have a [performant] guarantee that the sutexes never become indeterminate via ctrl-c. This flag might replace wrappers of all sorts in the code. You can just call it "indeterminate" flag or "critical_section" flag. Produce "critical_section_wrapper" - downside is will trigger a thread-id lookup.
    // Idea. Alternative. Another option is to toggle off guard unlocks during ctrl-c handling (that is, generic workstack frees of sutex guards don't trigger unlocks (capture the ->should_unlock value and set it to false inside the workstack before destructing), but regular execution elsewhere still does ), then have the main thread ctrl-c thread cleanup handler walk all the "designated areas" where multi-thread accessible objects are stored (kerf tree, etc.) and zero the sutexes, assuming we've disallowed basic cross-thread-workstack sutex locks. This should work for ctrl-c, but not for single-thread-error-jumping, which is OK for us for this purpose. Then you don't need to track indeterminacy at all.
    // Alternative. We could also just die() on this case and not 
    return;
  }
#endif
}

int SUTEX::sutex_lock_exclusive(SUTEX *u, const bool try_only, const I allowed_counter, const bool ignores_thread)
{
  // POP Remark. everywhere is the question of which __ATOMIC_* attribute? Part of the difficulty in answering the question is that the C++11 atomics are designed for atomic `.load()` and `.store()` operations, say on a bool, where they make more sense. When coding a mutex (sutex), you have lock and unlock operations associated with acquiring and releasing the locks. But seemingly, because most of these operations are writes/stores, you actually will use memory_order_release (or stronger) on much of it, and not memory_order_acquire, even when "acquiring" the lock, because they are writes/stores. C++11 committee should have chosen different names. Descriptions are also bad. Use "lexical" and "temporal" ordering, and "compiler" and "CPU" ordering. I think their "release/acquire" refers to "releasing/acquiring" the hypothetical lock on mutable data.
  //  RELAXED < CONSUME < ACQUIRE < RELEASE < ACQ_REL < SEQ_CST
  // Observation. one minor issue with the sutex-protected threading model implementation we're going to use is that __ATOMIC_ACQUIRE and __ATOMIC_RELEASE and friends give a total data ordering for threads A and B, when generally we're just concerned with their ordering for the object data in question (the data protected by the lock, not the entire heap/stack), but we can't gate the memory at that fine a grain. 
  // Remark. Thread A with the sutex exclusive lock on Data D cannot simply allow Thread B to write to D: Thread B must still wrap its writes in __ATOMIC_ACQUIRE and __ATOMIC_RELEASE (which can be __atomic_thread_fence but still need to be paired to a shared atomic action, probably load on the mutex), otherwise B's writes have no order or appearance guarantees with respect to observing Thread C, who must still ACQUIRE for consistency.
  // POP. We can prob use CONSUME in 1+ places here and in the thread/id allocation process but I can't test it because compilers haven't implemented it


  I one_id;

  if(!ignores_thread)
  {
    one_id = 1 + kerf_get_cached_normalized_thread_id();
  }
  else
  {
    one_id = IGNORED_THREAD_PLACEHOLDER_ONE_ID;
  }

  SUTEX old, s;
  BACKOFF b, c;

  if(DEBUG)
  {
    old.counter = -one_id;
    assert(one_id == -old.counter); // value must store correctly in bitfield

    assert(-one_id >= SUTEX_COUNTER_MIN);
  }

  // There's two approaches you can take:
  // A. Writers set the waiting flag like a mutex they own (only one thread can turn it on and hold it)
  // B. Writers set the flag (multiple bitwise OR) and then fight for the counter
  // We do A below. Both are writer-preferring
  // The tradeoffs are:
  // A: subsequent writers can't inherit the flag from a running writer, so a reader may get through (sounds ideal, to not starve them)
  // A (&B?): the "try" version of exclusive locking has two failure points/gates (must get waiter flag and then have no readers)
  // B: writers will all cut the line in front of readers, for writes of decent length
  // B: you have to check and reenable the flag when it goes off while you're waiting your turn to write (and turn it off right after you get it)
  // B: the downgrade needs to atomically write both the flag + counter at same time, I think
  //
  // Question. Why is the writer_waiting flag set while the writer has an exclusive lock? Is this implied by A above?
  // Question. Is there a third case we neglected: A' gets the write lock but then releases the writer_waiting flag? I think this shares a trait with B, in that it causes writes to always preempt reads. Perhaps that's why we didn't investigate, or perhaps there's some other flaw/trait that causes it to reduce to a previous case.

set_writer_waiting_flag:
  {
    // Remark. Linux has __test_and_set_bit, x86 has test_and_set_bit
    // Take 1: old.i = __sync_fetch_and_or(&u->i, ((SUTEX){.writer_waiting=1}).i);
    old.i = __atomic_fetch_or(&u->i, ((SUTEX){.writer_waiting=-1}).i, __ATOMIC_RELAXED);

    if(old.writer_waiting)  
    {
      if(old.counter == -one_id) return EDEADLK;

      if(try_only) return EBUSY;

      b.pause();
      goto set_writer_waiting_flag;
    }
    old.writer_waiting = -1;
  }
  
  // // Take 1
  //wait_for_zeroed_counter:
  //   old = *u;
  //   s = old;
  //   old.counter = 0;
  //   if(!__sync_bool_compare_and_swap(&u->i, old.i, s.i))
  //   {
  //     if(try_only) {...}
  //     c.pause();
  //     goto wait_for_zeroed_counter;
  //   }
  
  // Take 2
  // Using val_compare is two lines longer than bool,
  // but I suspect more performant (fewer checks on u)
wait_for_counter:
  {
    s = old;
    old.counter = allowed_counter;
    s.counter = -one_id;

    //Take 2.1: before.i = __sync_val_compare_and_swap(&u->i, old.i, s.i);
    if(! __atomic_compare_exchange_n(&u->i, &old.i, s.i, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
      if(try_only)
      {
        // this is unsafe: u->writer_waiting = 0;
        // this is invalid/unimplemented C++ currently: __atomic_store_n(&u->writer_waiting, 0, __ATOMIC_RELAXED);
        // so do this:
        {

          static_assert(1==SUTEX_FLAG_BITS);
          static_assert(std::is_signed_v<decltype(SUTEX::writer_waiting)>);
          #pragma clang diagnostic push
          #pragma clang diagnostic ignored "-Wsingle-bit-bitfield-constant-conversion"
          static_assert(((SUTEX){.writer_waiting=1}).writer_waiting == -1);
          #pragma clang diagnostic pop
          __atomic_fetch_and(&u->i, ~((SUTEX){.writer_waiting=-1}).i, __ATOMIC_RELAXED);
        }

        return EBUSY;
      }

      c.pause();
      goto wait_for_counter;
    }
  }

  if(DEBUG)
  {
    SUTEX temp;
    __atomic_load(u, &temp, __ATOMIC_RELAXED);
    assert(temp.writer_waiting);
    assert(temp.counter == s.counter); // valid test for exclusive locks, not for shared
  }

  return SUCCESS;
}

int SUTEX::sutex_try_lock_exclusive(SUTEX *u, const bool ignores_thread)
{
  return sutex_lock_exclusive(u, true, DEFAULT_ALLOWED_COUNTER, ignores_thread);
}

void SUTEX::sutex_unlock_exclusive(SUTEX *u, const bool ignores_thread)
{

  SUTEX temp;
  __atomic_load(u, &temp, __ATOMIC_RELAXED);
  // SUTEX temp = *u; // POP. I think technically this is safe even through tearing, but need to silence san

  if(DEBUG)
  {
    auto one_id = 1 + kerf_get_cached_normalized_thread_id();
    assert(temp.writer_waiting);
    if(!ignores_thread)
    {
      assert(temp.counter == -one_id); // valid test for exclusive locks, not for shared
    }
    else
    {
      assert(temp.counter == -IGNORED_THREAD_PLACEHOLDER_ONE_ID); 
    }
  }

  temp.counter = 0;
  temp.writer_waiting = 0;
  __atomic_store_n(&u->i, temp.i, __ATOMIC_RELEASE);
}

int SUTEX::sutex_lock_shared(SUTEX *u, const bool try_only)
{ 
  BACKOFF b;

  SUTEX stale;
  stale.i = __atomic_load_n(&u->i, __ATOMIC_RELAXED);
  SUTEX renew;

retry:

  // Fail if writer is waiting, otherwise atomic increment

  stale.writer_waiting = 0;
  stale.counter = MAX(0, stale.counter); // ignore writers (negative one-indexed thread ids)
  stale.counter = MIN(stale.counter, SUTEX_MAX_ALLOWED_READERS - 1); // don't overflow max readers. Remark. If one thread has captured all these, it will deadlock.

  renew = stale;
  renew.counter = stale.counter + 1;

  // Alternatively, I think we can capture the write flag, do an atomic addition, unset write flag. This yields a different set of semantics, which may make reader-throughput more stable. What we really want is an atomic add that doesn't process if the write flag is on, that's what we're poorly simulating here with compare_and_swap.

  // Take 1: before.i = __sync_val_compare_and_swap(&u->i, stale.i, renew.i);
  if(! __atomic_compare_exchange_n(&u->i, &stale.i, renew.i, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
  {
    bool saturated = stale.counter >= SUTEX_COUNTER_MAX;
    const bool EAGAIN_FOR_TRY = true;
    const bool EAGAIN_FOR_NOT_TRY = false;

    if(try_only)
    {
      if(EAGAIN_FOR_TRY && saturated) return EAGAIN;
      return EBUSY;
    }

    assert(!EAGAIN_FOR_NOT_TRY); // we assume, in the wrappers, that non_try always succeeds/blocks
    if(EAGAIN_FOR_NOT_TRY && !try_only && saturated) return EAGAIN;

    b.pause();
    goto retry;
  }

  assert(renew.counter > 0);

  return SUCCESS;
}

int SUTEX::sutex_try_lock_shared(SUTEX *u)
{ 
  return sutex_lock_shared(u, true);
}

void SUTEX::sutex_unlock_shared(SUTEX *u)
{
  // We're making this assumption:
  // High order bit
  //   Counter
  //   Waiter Flag
  //   Remainining unused
  // Low order bit
  // So if this breaks due to bit-ordering on some platform that's what to look at
  //
  // Related: I couldn't get this version to work somehow
  // subtrahend.i = 1LL << (SUTEX_FLAG_BITS + SUTEX_BIT_REMAIN);
  // but the {.counter = 1} version works
  // possibly a platform/bit ordering issue, I only spent a few seconds

  if(DEBUG)
  {
    SUTEX temp;
    __atomic_load(u, &temp, __ATOMIC_RELAXED);
    assert(temp.counter > 0);
  }

  SUTEX was; // for debug

  SUTEX subtrahend = {.counter = 1};

  assert(subtrahend.counter == 1);

  // Take 1: old.i = __sync_fetch_and_sub(&u->i, subtrahend.i);
  was.i = __atomic_fetch_sub(&u->i, subtrahend.i, __ATOMIC_RELEASE);
  // POP. Technically, if this is a reader-only lock (and no SLAB-TREE-OR-PARENTED-reference-increments took place or were themselves atomic) then this could be RELAXED.

  assert(was.counter > 0);
}

/////////////////////////////////////////////////////////////////////////////////

int SUTEX::sutex_downgrade_lock(SUTEX *u, const bool ignores_thread)
{ 
  // Remark. You can also autodetect whether to upgrade or downgrade based on
  // the lock state (& eg allowed_counter is known 1 or stack-tracked), but I'm
  // not sure what the use of that is, having a downgrade/upgrade toggle method.

  SUTEX temp;
  __atomic_load(u, &temp, __ATOMIC_RELAXED);
  // SUTEX temp = *u; // POP. I think technically this is safe even through tearing, but preemptively silencing san

  // downgrade from exclusive to shared:
  if(DEBUG)
  {
    auto one_id = 1 + kerf_get_cached_normalized_thread_id();

    if(!ignores_thread)
    {
      assert(temp.counter == -one_id);
    }
    assert(temp.writer_waiting);
  }

  temp.counter = 1;
  temp.writer_waiting = 0;
  __atomic_store_n(&u->i, temp.i, __ATOMIC_RELEASE);

  return SUCCESS;
}

int SUTEX::sutex_try_downgrade_lock(SUTEX *u, const bool ignores_thread)
{
  // always succeeds, alias
  return sutex_downgrade_lock(u, ignores_thread);
}

int SUTEX::sutex_upgrade_lock(SUTEX *u, const bool try_only, const I allowed_counter, const bool ignores_thread)
{ 
  // upgrade from shared to exclusive:

  if(DEBUG)
  {
    SUTEX temp;
    __atomic_load(u, &temp, __ATOMIC_RELAXED);
    assert(temp.counter > 0);
    assert(temp.counter >= allowed_counter);
  }

  // "allowed_counter" is the best we can do [short of stack-tracking]:
  // if thread has 1 read lock, use 1
  // but if you have more, you need to know how many,
  // since the counter doesn't track per-thread
  // otherwise you'll cause a deadlock
  // (this is the 1 distinct upgrading thread with multiple read locks
  // and arbitrary other threads not-upgrading but with arbitrary read locks)

  // NB. Regarding contention among at least two *distinct* threads to both
  // upgrade: The "try" case already supports one winning and arbitrary losers
  // (to later release their read locks). The "non-try" (blocking) case must
  // necessarily produce a deadlock because threads never let go of their read
  // locks after failing to get the write flag. (The "workaround" for that is
  // to have each distinct thread release its read locks and then attempt a
  // non-try write lock: but this is not a true "upgrade" operation, as
  // understood here, because it requires releasing the read locks first.)

  return sutex_lock_exclusive(u, try_only, allowed_counter, ignores_thread);
}

int SUTEX::sutex_try_upgrade_lock(SUTEX *u, const I allowed_counter, const bool ignores_thread)
{
  return sutex_upgrade_lock(u, true, allowed_counter, ignores_thread);
}

int SUTEX::sutex_lock(SUTEX *u, const bool exclusive, const bool ignores_thread)
{
  return exclusive ? sutex_lock_exclusive(u, false, DEFAULT_ALLOWED_COUNTER, ignores_thread): sutex_lock_shared(u);
}

void SUTEX::sutex_unlock(SUTEX *u, const bool exclusive, const bool ignores_thread)
{
  if(exclusive) sutex_unlock_exclusive(u, ignores_thread); 
  else sutex_unlock_shared(u);
}

#pragma mark -



} // namespace
