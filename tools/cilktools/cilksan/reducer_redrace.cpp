// XXX Punt reducer related stuff here for now

// A stack keeping track of the current context (i.e., USER, UPDATE, or
// REDUCE).  We need a stack of context, because they can nest.  For example, 
// we can invoke update operation in the user code, which in turn invokes 
// the identity function.  Both are UPDATE, but we need to stay in UPDATE 
// when the inner UPDATE context ends.
static Stack_t<enum AccContextType_t> context_stack;


/*************************************************************************/
/**  Helper functions for Events
/*************************************************************************/
static inline void swap(uint32_t *x, uint32_t *y) {
  uint32_t tmp = *x;
  *x = *y;
  *y = tmp;
}

// max_sync_block_size == the max number of continuations within a sync block 
// randomly choose 3 numbers, i1, i2 and i3 between [0-max_sb] to indicate the
// intervals that we want to check reduce op on.  
// we will be checking [i1, i2-1] op [i2, i3-1] (inclusive both ends)
// to check it, it boils down to simulating steal at continuation points where
// current_sync_block_size == i1, i2, and i3.
static void randomize_steal_points() {

  FrameData_t *f = frame_stack.head();
  f->steal_index = 0;
  f->Pbag_index = 0;

  DBG_TRACE_CALLBACK(REDUCE_CHECK, "Randomize steals for frame %ld: ",
                     f->Sbag->get_node()->get_func_id());
  if(max_sync_block_size < 2) { // special case
    f->steal_points[0] = NOP_STEAL; // NOP steal points will be ignored 
    f->steal_points[1] = NOP_STEAL;  
    f->steal_points[2] = (rand() & 0x1); // the cont may or may not be stolen
    f->steal_index = 2;
    // we are skipping the first interval, so keep the Pbag index in sync
    f->Pbag_index = 1; 
    DBG_TRACE_CALLBACK(REDUCE_CHECK, "0, 0, %d.\n", f->steal_points[2]);
    return;
  }

  uint32_t *arr = f->steal_points; // just for convenience 
  // +1 for picking a value between [0, max_sb] and 
  // +1 to allow last interval to be picked
  uint32_t intervals = max_sync_block_size + 2;

  arr[0] = rand() % intervals; 
  // need to pick unique numbers
  do { arr[1] = rand() % intervals; } while(arr[0] == arr[1]);
  if(arr[0] > arr[1]) { swap(&arr[0], &arr[1]); } // keep sorted 
  do { arr[2] = rand() % intervals; } while(arr[0] == arr[2] 
                                         || arr[1] == arr[2]);
  if(arr[1] > arr[2]) { // keep sorted 
    swap(&arr[1], &arr[2]);
    if(arr[0] > arr[1]) { swap(&arr[0], &arr[1]); }
  } 
  DBG_TRACE_CALLBACK(REDUCE_CHECK, "%d, %d, %d.\n", f->steal_points[0],
                     f->steal_points[1], f->steal_points[2]);
  // assert that the steal points are unique and sorted in increasing order
  racedetector_assert(f->steal_points[0] < f->steal_points[1] 
                   && f->steal_points[1] < f->steal_points[2]); 

  // we are skipping the first interval, so keep the Pbag index in sync
  if(f->steal_points[0] == NOP_STEAL) {
    f->Pbag_index = 1;
  }
}

// return true if the next continuation within the spwaning funciton
// ought to be stolen to check for reducer races; otherwise, return false.
static bool should_steal_next_continuation() {
  FrameData_t *f = frame_stack.head();
  bool should_steal = false;

  if( cont_depth_to_check ) {
    racedetector_assert(!check_reduce);
    uint64_t curr_cont_depth = f->init_cont_depth + f->current_sync_block_size; 
    if( curr_cont_depth == cont_depth_to_check ) {
      DBG_TRACE_CALLBACK(REDUCE_CHECK, 
              "Should steal frame %ld, sb size %d, with cont depth %d.\n",
              f->Sbag->get_node()->get_func_id(), f->current_sync_block_size,
              curr_cont_depth);
      should_steal = true;
    }

  } else if(check_reduce) {
    if(f->current_sync_block_size == 1) { 
      // about to enter first spawn child; now we need those steal points
      randomize_steal_points();
      // skip over Nop steals
      while(f->steal_index < MAX_NUM_STEALS && 
          f->steal_points[f->steal_index] < f->current_sync_block_size) {
        f->steal_index++;
      }
    }
    
    // check if the next steal point matches
    if(f->steal_index < MAX_NUM_STEALS &&
       f->steal_points[f->steal_index] == f->current_sync_block_size) {
      f->steal_index++;
      DBG_TRACE_CALLBACK(REDUCE_CHECK, 
        "Should steal frame %ld, cont %d.\n",
        f->Sbag->get_node()->get_func_id(), f->current_sync_block_size);
      should_steal = true;
    }
    racedetector_assert(f->steal_index == MAX_NUM_STEALS ||
       f->steal_points[f->steal_index] > f->current_sync_block_size);

  } else if(simulate_all_steals) {
    should_steal = true;
  }

  return should_steal;
}

// return true if function f's next continuation was stolen
static inline bool next_continuation_was_stolen(FrameData_t *f) {
  
  bool ret = false;

  if(cont_depth_to_check &&
     cont_depth_to_check == (f->init_cont_depth + f->current_sync_block_size)) {
    ret = true;

  } else if(check_reduce) {
    if(f->steal_index == 0) {
      racedetector_assert( 
          f->steal_points[f->steal_index] > f->current_sync_block_size); 
      ret = false;
    } else if(f->steal_points[f->steal_index-1] == f->current_sync_block_size) {
      racedetector_assert( (f->steal_index == MAX_NUM_STEALS || 
          f->steal_points[f->steal_index] > f->current_sync_block_size) &&
          f->steal_points[f->steal_index-1] <= f->current_sync_block_size ); 
      ret = true;
    }

  } else if(simulate_all_steals) {
    ret = true;
  }

  return ret;
}

// This function gets called when the runtime is performing:
//
// A. the return protocol for a spawned child whose parent (call it f) has 
// been stolen; and 
// B. a non-trivial cilk_sync
//
// Call the steal points i1, i2, and i3, as chosen by
// randomize_steal_points() function (and stored in f->steal_points[]).
// interval 1 will be [i1, i2-1] (inclusive both ends), and 
// interval 2 will be [i2, i3-1] (inclusive both ends).
// interval 0 will be what comes before i1 and interval 3 will be what 
// comes after i3; these two intervals could be empty.
//
// In case A., at the point of invocation, we have seen leave_frame_begin 
// but not leave_frame_end for the spawn helper (and we won't, because 
// __cilkrts_leave_frame won't return in this case), so the helper has been 
// popped off frame_stack.  Function f (the spawner)'s current_sync_block_size 
// is the index of the continuation strand after the spawn statement (assume 
// 0 based indexing).  
//
// In case B., at the point of invocation, we have seen cilk_sync_begin
// but not cilk_sync_end, and the spawner frame has not reset its
// current_sync_block_size to 0 yet (that happens in cilk_sync_end). 
//
// This function returns (current interval << 1) & end_of_interval
static unsigned int get_current_reduce_interval(bool spawn_ret) {
  // should be invoked only when we are simulating steals
  racedetector_assert(cont_depth_to_check || check_reduce || 
                      simulate_all_steals);

#define INTERVAL_MASK(interval, end) (((interval) << 1) | end)
#define MASK_TO_INTERVAL(mask) ((mask) >> 1)
#define MASK_TO_INTERVAL_END(mask) ((mask) & 0x1)
  unsigned int ret = 0;
  // f is the function that contains the spwan / sync statement
  FrameData_t *f = frame_stack.head();
  uint32_t sb_size = f->current_sync_block_size;
  DBG_TRACE_CALLBACK(REDUCE_CHECK, 
    "Get interval for frame %ld, spawn return? %d",
    f->Sbag->get_node()->get_func_id(), spawn_ret);
  DBG_TRACE_CALLBACK(REDUCE_CHECK, 
    ", sb size %d, steal index %d, pindex: %u.\n",
    sb_size, f->steal_index, f->Pbag_index);

  if(cont_depth_to_check) {
    // we are simulating steals to check updates, so there should be only a
    // single steal point per sync block.  Let's call the interval before the
    // steal point as 0 and after as 1, and performs the reduction at sync. 
    uint64_t curr_cont_depth = f->init_cont_depth + sb_size; 
    if(curr_cont_depth == cont_depth_to_check) {
      ret = INTERVAL_MASK(0, 0x1); // end of interval 0
    } else if(curr_cont_depth > cont_depth_to_check) {
      ret = INTERVAL_MASK(1, 0x0); // middle of interval 1 (not ended yet)
    }
  
  } else if(check_reduce) { // we are simulating steals to check reduce ops
    // we are in interval 0 if we haven't passed any steal points
    if(f->steal_index > 0) {
      // special case: we are returning from a spawn where the next
      // continuation is a steal point that has already been processed
      if(f->steal_points[f->steal_index - 1] == sb_size) {
        ret = INTERVAL_MASK( f->steal_index-1, 0x1 );
      } else {
        // otherwise, we are within an interval indicated by the steal index`
        ret = INTERVAL_MASK( f->steal_index, 0x0 );
      }
    }

  } else if(simulate_all_steals) {
    // we are simulating all steals and performing eager reduce ops, so treat
    // the first spawn child returning as end of interval 1, and the rest as
    // end of interval 2; this way, we will always perform one single reduce
    // op everything this function is invoked.
    if(f->current_sync_block_size == 1) {
      ret = INTERVAL_MASK(0, 0x1); // end of interval 0
    } else {
      // special encoding: ene of interval 3; usually we never reach the end
      // of interval 3, but simulate_all_steals is encoded specially
      ret = INTERVAL_MASK(3, 0x1); 
    }
  }

  DBG_TRACE_CALLBACK(REDUCE_CHECK, 
      "Check interval returns interval %u, end: %u.\n", 
      MASK_TO_INTERVAL(ret), MASK_TO_INTERVAL_END(ret));
  // the PBag index should be the same as the number of intervals at this point
  // or, PBag index == 1 and we are at the end of first interval
#if CILKSAN_DEBUG
  // The PBag index should be the same 
  if(spawn_ret) {
    // at spawn return, the PBag index should be the same as the interval
    // index, or PBag index == 1 if we are in interval 3. 
    racedetector_assert( f->Pbag_index == MASK_TO_INTERVAL(ret) || 
        (f->Pbag_index == 1 && MASK_TO_INTERVAL(ret) == MAX_NUM_STEALS) );
  } else {
    // at sync, the PBag index should be the same as the interval
    // index, or PBag index == 1 if we are in end of interval 2 or 3. 
    racedetector_assert( f->Pbag_index == 
                         (MASK_TO_INTERVAL(ret) + MASK_TO_INTERVAL_END(ret)) || 
        (f->Pbag_index == 1 && 
         MASK_TO_INTERVAL(ret) + MASK_TO_INTERVAL_END(ret) >= MAX_NUM_STEALS) );
  }
#endif

  return ret;
}

// This is invoked right before the runtime performs a merge, merging the
// right-most reducer map into the reducer map to its left.  The tool
// correspoindingly update the disjointset data structure to reflect that ---
// any memory access performed during the merge operation is now logically
// in-series with the memory accesses stored in the top two PBags.
//
// This is called when either the runtime performs the return protocol 
// for a stolen spawned child or when the runtime performs cilk_sync.
static void update_disjointsets() {

  // f is the function that contains the spwan / sync statement
  FrameData_t *f = frame_stack.head();
  DBG_TRACE_CALLBACK(REDUCE_CHECK, 
      "XXX: frame %ld update disjoint set, merging PBags in index %d and %d.\n",
      f->Sbag->get_node()->get_func_id(), f->Pbag_index-1, f->Pbag_index);

  racedetector_assert(f->Pbag_index > 0 && f->Pbag_index < MAX_NUM_STEALS);
  // pop the top-most Pbag
  DisjointSet_t<SPBagInterface *> *top_pbag = f->Pbags[f->Pbag_index];
  f->Pbags[f->Pbag_index] = NULL;
  f->Pbag_index--;
  if(top_pbag) {
    DisjointSet_t<SPBagInterface *> *next_pbag = f->Pbags[f->Pbag_index];
    if(next_pbag) { // merge with the next Pbag if there is one
      racedetector_assert( top_pbag->get_set_node()->is_PBag() );
      next_pbag->combine(top_pbag);
      racedetector_assert( next_pbag->get_set_node()->is_PBag() );
      racedetector_assert( next_pbag->get_node()->get_func_id() == 
                           next_pbag->get_set_node()->get_func_id() );
    } else {
      f->Pbags[f->Pbag_index] = top_pbag;
    }
  }
}

// Check that the entry_stack indeed mirrors the worker's runtime deque
#if CILKSAN_DEBUG
static void check_deque_invariants() {

  DBG_TRACE_CALLBACK(DEQUE_OP, 
    "%ld: check deque invariants, range: [%d, %d), entry stack size: %u.\n", 
    frame_stack.head()->Sbag->get_node()->get_func_id(), 
    rts_deque_begin, rts_deque_end, entry_stack.size());

  // when this function is invoked, we must be inside a helper function
  racedetector_assert(entry_stack.ancestor(0)->entry_type == HELPER);
  racedetector_assert(rts_deque_begin <= rts_deque_end);
  if(rts_deque_begin == rts_deque_end) { // nothing to check except levels
    racedetector_assert( worker->head == worker->tail );
    return;
  }

  // check that everything above the first frame in the deque is FULL
  for(uint32_t i=1; i < rts_deque_begin; i++) {
    racedetector_assert(entry_stack.at(i)->frame_type == FULL_FRAME); 
  }

  // Check deque content: rts_deque_end must be pointing to a HELPER frame
  racedetector_assert( entry_stack.size() > rts_deque_end && 
                       entry_stack.at(rts_deque_end)->entry_type == HELPER);

  uint32_t levels = 0;
  // The frame type at the top of the deque could be anything.
  if(entry_stack.at(rts_deque_begin)->entry_type != HELPER) {
    Entry_t *f = entry_stack.at(rts_deque_begin);
    racedetector_assert( rts_deque_begin == 1 || f->frame_type == FULL_FRAME ); 
    if(f->frame_type == FULL_FRAME) {
      // Angelina: not true; if a Cilk function calls another Cilk function,
      // the head will be pointing to the callee; only the oldest Cilk
      // function at the beginning of the stacklet would be a FULL frame
      // racedetector_assert((*worker->head)->flags & CILK_FRAME_STOLEN);
      racedetector_assert(f->entry_type != HELPER);
    }
    levels++; // if it's not a HELPER frame, it counds as its own stacklet 
  }
  for(uint32_t i=rts_deque_begin; i < rts_deque_end; i++) {
    Entry_t *f = entry_stack.at(i);
    racedetector_assert(i==rts_deque_begin || f->frame_type == SHADOW_FRAME);
    // each HELPER frame forms a new stacklet
    if(f->entry_type == HELPER) { levels++; }
  }
  // check that the levels we counted in entry_stack matches that of the runtime
  racedetector_assert(levels == (worker->tail - worker->head));

  // check that everything not on deque is shadow and not marked as LAZY
  for(uint32_t i=rts_deque_end; i < entry_stack.size(); i++) {
    racedetector_assert(entry_stack.at(i)->frame_type == SHADOW_FRAME); 
  }

  /*
   * We can't actually check if the number of shadow frames match; in the
   * cilk-for implementation, the tool sees an "enter_frame," but the shadow
   * frame is not actually pushed onto the runtime deque if that particular
   * recursive call reaches the base case (i.e., invoking loop body).
  // check the number of frame in entry_stack matches that of the runtime deque
  uint32_t num_frames = 0;
  // tail-1 is valid only if the deque is not empty
  __cilkrts_stack_frame *sf = *(worker->tail-1);
  while( sf && (sf->flags & CILK_FRAME_STOLEN) == 0 ) { 
    // count number of frames in the deque, including the top FULL frame
    num_frames++; 
    sf = sf->call_parent;
  } 

  // the last FULL frame may or may not be on the deque; can't tell from 
  // runtime deque, because its child has not been unlinked from it. 
  // Even if it's stolen, let's just count it if entry_stack says it's on 
  // the deque (there can be at most one such FULL frame on the deque)
  if(sf && entry_stack.at(rts_deque_begin)->frame_type == FULL_FRAME) {
    num_frames++;
  }*/

  DBG_TRACE_CALLBACK(DEQUE_OP, 
    "Done checking deque invariants, levels: %d.\n", levels);
}

static void update_deque_for_simulating_steals() {
  // promote everything except for the last stacklet; 
  for(uint32_t i=rts_deque_begin; i < rts_deque_end; i++) {
    entry_stack.at(i)->frame_type = FULL_FRAME; // mark everything as FULL
  }
  rts_deque_begin = rts_deque_end;
  racedetector_assert(rts_deque_begin == entry_stack.size()-1 );

  DBG_TRACE_CALLBACK(DEQUE_OP, 
    "After simulating steal, new deque range: entry_stack[%d, %d).\n", 
    rts_deque_begin, rts_deque_end);
}

static void update_deque_for_entering_helper() {
  // rts_deque_end points to the slot where this HELPER is inserted
  rts_deque_end = entry_stack.size() - 1; 
  entry_stack.head()->prev_helper = youngest_active_helper;
  youngest_active_helper = rts_deque_end;
  // entry_stack always gets pushed slightly before frame_id gets incremented
  // entry_stack.head()->frame_id = frame_id+1;

  DBG_TRACE_CALLBACK(DEQUE_OP, "Enter helper %ld, deque range [%d, %d).\n", 
    entry_stack.head()->frame_id, rts_deque_begin, rts_deque_end);
}

static void update_deque_for_leaving_spawn_helper() {

  uint64_t exiting_frame = entry_stack.head()->frame_id;
  uint32_t prev_helper = entry_stack.head()->prev_helper;
  // the new size after we pop the entry stack
  uint32_t new_estack_size = entry_stack.size() - 1;

  // update rts_deque_begin if we are about to pop off the last frame on deque
  if(new_estack_size == rts_deque_begin) {
    rts_deque_begin--;
    racedetector_assert(
        entry_stack.at(rts_deque_begin)->frame_type == FULL_FRAME);
  }
  racedetector_assert(new_estack_size == rts_deque_end);
  racedetector_assert(rts_deque_end > rts_deque_begin && 
                      rts_deque_end-prev_helper >= 2);

  // move the rts_deque_end to either the previous helper frame or where
  // the deque begin if the previous helper has been stolen.
  uint32_t new_rts_deque_end = prev_helper >= rts_deque_begin ?
                             prev_helper : rts_deque_begin;
  // check that indeed there is not helper in between
  for(uint32_t i=rts_deque_end-1; i > new_rts_deque_end; i--) {
    racedetector_assert(entry_stack.at(i)->entry_type != HELPER);
  }

  // update the youngest helper
  racedetector_assert(rts_deque_end == youngest_active_helper);
  youngest_active_helper = prev_helper;
  rts_deque_end = new_rts_deque_end;

  racedetector_assert(rts_deque_end >= rts_deque_begin);
  DBG_TRACE_CALLBACK(DEQUE_OP, "Leave helper frame %ld, deque range [%d-%d).\n",
      exiting_frame, rts_deque_begin, rts_deque_end); 
}

static void update_deque_for_leaving_cilk_function() {

  uint64_t exiting_frame = entry_stack.head()->frame_id;
  enum FrameType_t exiting_frame_type = entry_stack.head()->frame_type;
  // the new size after we pop the entry stack
  uint32_t new_estack_size = entry_stack.size() - 1;

  if(new_estack_size == rts_deque_begin) { 
    // Either the worker has one FULL frame (not on deque), and we are 
    // popping that FULL frame off, or we returning from the last Cilk frame.
    racedetector_assert( rts_deque_end-rts_deque_begin == 0 && 
        (exiting_frame_type == FULL_FRAME || rts_deque_begin == 1) &&
        entry_stack.ancestor(1)->frame_type == FULL_FRAME ); 

    // we are about to pop off the last frame on deque; update deque_begin/end
    rts_deque_begin--;
    rts_deque_end--;
  }

  racedetector_assert(rts_deque_end >= rts_deque_begin);
  DBG_TRACE_CALLBACK(DEQUE_OP, "Leave Cilk frame %ld, deque range [%d-%d).\n",
    exiting_frame, rts_deque_begin, rts_deque_end); 
}
#endif

static void simulate_steal(CONTEXT *context) {
   
  DBG_TRACE_CALLBACK(DEQUE_OP, "Simulate steal; promote entry_stack[%d, %d).\n",
      rts_deque_begin, rts_deque_end);
  WHEN_RACEDETECTOR_ASSERT( check_deque_invariants(); )
  
  // simulate steals in runtime deque; call __cilkrts_promote_own_deque
  ADDRINT func_addr = RTN_Address( rts_promote_deque_rtn ); 
  PIN_CallApplicationFunction( context, PIN_ThreadId(), 
                               CALLINGSTD_DEFAULT, 
                               AFUNPTR(func_addr),  
                               PIN_PARG(void),
                               PIN_PARG(__cilkrts_worker *), worker,
                               PIN_PARG_END() );
  // update the debugging bookkeeping info to reflect steals
  WHEN_RACEDETECTOR_ASSERT( update_deque_for_simulating_steals(); )
}

// Check if the spawner that we are about to return back to has its upcoming
// continuation stolen.  If so, we need to update the PBag_index (i.e., push
// an "empty" PBag onto the stack of PBags) and the view_id for the spawner.
// At this point, both entry_stack and frame has both popped off the HELPER,
// so what's on top is basically the spawner we are checking.  
static void update_reducer_view() {

  FrameData_t *spawner = frame_stack.head();
  if( next_continuation_was_stolen(spawner) ) {
    // update the current view id if a steal occurred at this continuation
    spawner->curr_view_id = view_id++;
    spawner->Pbag_index++;
    racedetector_assert(spawner->Pbag_index < MAX_NUM_STEALS);

    DBG_TRACE_CALLBACK(REDUCE_CHECK, 
        "Increment frame %ld to Pbag index %u and view id %lu.\n",
        spawner->Sbag->get_set_node()->get_func_id(),
        spawner->Pbag_index, spawner->curr_view_id);
  }
}

static inline void 
begin_view_aware_strand(VOID *data_in, enum AccContextType_t type) {

  enum AccContextType_t curr_context = *(context_stack.head());
  DBG_TRACE_CALLBACK(REDUCE_CHECK, "Change context: %u -> %u.\n", 
                     curr_context, type);
  context_stack.push();
  *(context_stack.head()) = type;

/*
  racedetector_assert(current_context == USER);
  switch(type) {
    case VA_REDUCE:
      current_context = REDUCE; 
      break;
    case VA_UPDATE:
      current_context = UPDATE;
      break;
    default:
      racedetector_assert(0);
  }
*/
}

static inline void end_view_aware_strand(VOID *data_in, 
                                         enum AccContextType_t type) {
  enum AccContextType_t curr_context = *(context_stack.head());
  racedetector_assert(curr_context == (enum AccContextType_t)type);
  context_stack.pop();
  DBG_TRACE_CALLBACK(REDUCE_CHECK, "Change context: %u -> %u.\n", 
                     curr_context, *context_stack.head());
/*
#if CILKSAN_DEBUG
  AccContextType_t ending_context;
  switch(type) {
    case VA_REDUCE:
      ending_context = REDUCE; 
      break;
    case VA_UPDATE:
      ending_context = UPDATE;
      break;
    default:
      racedetector_assert(0);
  }
  racedetector_assert(current_context == ending_context);
#endif
*/
}

// Cilk fake lock related stuff
typedef struct LockData_t {
    LockData_t():lock(NULL) {}
    void *lock;
} LockData_t;
// a stack the locks we get from cilkscreen_acquire/release_lock
static Stack_t<LockData_t> lock_stack;


static inline void acquire_lock(void *lock) {
  racedetector_assert(lock_stack.head()->lock == NULL);
  lock_stack.head()->lock = lock;
  lock_stack.push();
  disable_checking();
}

static inline void release_lock(void *lock) {
  lock_stack.pop();
  racedetector_assert(lock_stack.head()->lock == lock);
  lock_stack.head()->lock = NULL;
  enable_checking();
  racedetector_assert(lock_stack.size() > 1 || checking_disabled == 0);
}


// XXX Stuff that we no longer need 

/* There will be 2 ignore range total, one for worker state and one for
 * worker's deque.  Any additional worker causes an error, since this tool is
 * meant to be used with single-thread execution. */
#define EXPECTED_IGNORES 1
ADDRINT ignore_begin[EXPECTED_IGNORES] = { static_cast<ADDRINT>(0ULL) };
ADDRINT ignore_end[EXPECTED_IGNORES] = { static_cast<ADDRINT>(0ULL) };

static inline void 
get_info_on_inst_addr(ADDRINT inst_addr, INT32 *line_no, std::string *file) {
  PIN_LockClient();
  PIN_GetSourceLocation(inst_addr, NULL, line_no, file);
  PIN_UnlockClient();
}

int main(int argc, char *argv[]) {

  // reducer related annotation
  zca.insert_annotation_calls("cilkscreen_begin_reduce_strand",
                              (AFUNPTR)begin_view_aware_strand,
                              IARG_uint32_t, ((enum AccContextType_t)REDUCE),
                              IARG_END);

  zca.insert_annotation_calls("cilkscreen_end_reduce_strand",
                              (AFUNPTR)end_view_aware_strand,
                              IARG_uint32_t, ((enum AccContextType_t)REDUCE),
                              IARG_END);
  
  zca.insert_annotation_calls("cilkscreen_begin_update_strand",
                              (AFUNPTR)begin_view_aware_strand,
                              IARG_uint32_t, ((enum AccContextType_t)UPDATE),
                              IARG_END);

  zca.insert_annotation_calls("cilkscreen_end_update_strand",
                              (AFUNPTR)end_view_aware_strand,
                              IARG_uint32_t, ((enum AccContextType_t)UPDATE),
                              IARG_END);
  
  // Not supported at the moment 
  zca.insert_annotation_calls("cilkscreen_clean",
                              (AFUNPTR)metacall_error_exit,
                              IARG_PTR,
                              (char *)"cilkscreen_clean",
                              IARG_END);

}
