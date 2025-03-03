/*
 * myth_worker_func.h
 */
#pragma once
#ifndef MYTH_WORKER_FUNC_H_
#define MYTH_WORKER_FUNC_H_

#include <signal.h>
#include <sys/time.h>

#include "myth_config.h"
#include "myth_init.h"
#include "myth_misc.h"
#include "myth_bind_worker.h"
#include "myth_worker.h"
#include "myth_sched.h"
#include "myth_log.h"
#include "myth_io.h"

#include "myth_init_func.h"
#include "myth_misc_func.h"
#include "myth_context_func.h"
#include "myth_wsqueue_func.h"
#include "myth_sched_func.h"
#include "myth_log_func.h"
#include "myth_io_func.h"
#include "myth_thread_pool_func.h"
#include "myth_steal_range_func.h"
#include "myth_workers_range_func.h"
#include "myth_adws_sched_func.h"

#if MYTH_ECO_MODE
#include "myth_eco.h"
#endif

static void myth_sched_loop(void);

// pthread key to indicate a worker is a worker,
// so that user code can tell if the running pthread
// is a massivethread worker or not
extern pthread_key_t g_worker_key;
static inline void myth_worker_key_init() {
  real_pthread_key_create(&g_worker_key, NULL);
}
static inline void myth_set_worker_key() {
  /* TODO: shouldn't we set something more useful? */
  real_pthread_setspecific(g_worker_key, (void*)1);
}
static inline void * myth_get_worker_key() {
  /* TODO: shouldn't we set something more useful? */
  void * x = real_pthread_getspecific(g_worker_key);
  return x;
}
static inline int myth_is_myth_worker_body(void) {
  return (myth_get_worker_key() ? 1 : 0);
}

//TLS implementations
#if WENV_IMPL == WENV_IMPL_PTHREAD
//TLS by pthread_key_XXX
extern pthread_key_t g_env_key;
static void myth_env_init(void) {
  real_pthread_key_create(&g_env_key, NULL);
}

static void myth_env_fini(void) {
  real_pthread_key_delete(g_env_key);
}

static void myth_set_current_env(myth_running_env_t e) {
  real_pthread_setspecific(g_env_key, (void*)e);
}

//Return current worker thread descriptor
static inline myth_running_env_t myth_get_current_env(void) {
  myth_running_env_t env
    = (myth_running_env_t)real_pthread_getspecific(g_env_key);
  return env;
}
#elif WENV_IMPL == WENV_IMPL_ELF
//Initialize
static inline void myth_env_init(void) { }
//Cleanup
static inline void myth_env_fini(void) { }
//Set worker thread descriptor
static inline void myth_set_current_env(myth_running_env_t e) {
  g_worker_rank = e->rank;
}
//Return current worker thread descriptor
static inline myth_running_env_t __attribute__((always_inline)) myth_get_current_env(void) {
  /* if you hit this, it is likely that you create a native pthread,
     which calls into a MassiveThreads function that in turn calls this */
  myth_assert(0 <= g_worker_rank);
  myth_assert(g_worker_rank < g_envs_sz);
  myth_running_env_t env = &g_envs[g_worker_rank];
  myth_assert(env->rank == g_worker_rank);
  return env;
}
#elif WENV_IMPL == WENV_IMPL_NONE
//Just a global variable. It works one worker thread only.
//Initialize
static void myth_env_init(void) { }
//Cleanup
static void myth_env_fini(void) { }
//Set worker thread descriptor
static void myth_set_current_env(myth_running_env_t e) { }
//Return current worker thread descriptor
static inline myth_running_env_t myth_get_current_env(void) {
  return &g_envs[0];
}
#else
#error "invalide WENV_IMPL"
#endif

#if WS_TARGET_RANDOM
//Search a worker thread that seems to be busy
static inline myth_running_env_t myth_env_get_first_busy(myth_running_env_t e) {
  //If number of worker threads == 1 , always fails
  if (g_attr.n_workers == 1) return NULL;
  //Choose randomly
  int idx;
  do {
    idx = myth_random(0,g_attr.n_workers);
  } while (idx == e->rank);
  return &g_envs[idx];
}
#else

//Search a worker thread that seems to be busy
static inline myth_running_env_t myth_env_get_first_busy(myth_running_env_t e) {
  return e->next;
}
#endif

static inline myth_running_env_t myth_env_get_randomly(void) {
  //If number of worker threads == 1 , always fails
  if (g_attr.n_workers == 1) return &g_envs[0];
  //Choose randomly
  int idx;
  idx = myth_random(0, g_attr.n_workers);
  return &g_envs[idx];
}

void myth_alrm_sighandler(int signum,siginfo_t *sinfo,void* ctx);

static void myth_setup_worker(int rank) {
  myth_running_env_t env = &g_envs[rank];
  env->rank = rank;
  env->exit_flag = 0;
  memset(&env->prof_data, 0, sizeof(myth_prof_data));
  //Initialize allocators
  myth_flmalloc_init_worker(rank);
  //myth_malloc_wrapper_init_worker(rank);
  //Initialize logger
  myth_log_worker_init(env);
  myth_set_worker_key();
  myth_set_current_env(env);
  //Initialize random seed
  myth_random_init(((unsigned)time(NULL)) + rank);
  //Initialize runqueue
  myth_queue_init(&env->runnable_q);
  myth_queue_clear(&env->runnable_q);
#if MYTH_ENABLE_ADWS
  myth_queue_init(&env->migration_q);
  myth_queue_clear(&env->migration_q);
  env->use_migration_q = 0;
  myth_freelist_init(&env->freelist_steal_range);
  env->ready_to_activate_steal_range = 0;
#endif
  //Initialize freelist for thread descriptor
  myth_thread_pool_init(&env->thread_pool, env);
#if (INITIAL_STACK_ALLOC_UNIT > 0)
  myth_thread_t th = myth_alloc_new_thread_desc(env, INITIAL_STACK_ALLOC_UNIT);
#if MYTH_SPLIT_STACK_DESC
  th->stack = myth_alloc_new_thread_stack(env, INITIAL_STACK_ALLOC_UNIT);
  myth_free_thread_stack(env, th);
#endif
  myth_free_thread_desc(env, th);
#endif
#if MYTH_WRAP_SOCKIO
  //Initialize I/O
  myth_io_worker_init(env,&env->io_struct);
#endif //MYTH_WRAP_SOCKIO
  env->this_thread = NULL;
  //Wait for other worker threads
  myth_internal_barrier_wait(&g_worker_barrier);
  //set signal mask
  if (env->rank != 0){
    sigset_t ss;
    sigemptyset( &ss );
    sigaddset(&ss,SIGALRM);
    real_pthread_sigmask(SIG_BLOCK,&ss,NULL);
  }
  else{
    struct sigaction newact;
    memset(&newact,0,sizeof(newact));
    newact.sa_sigaction = myth_alrm_sighandler;
    newact.sa_flags = SA_SIGINFO|SA_RESTART;
    sigaction(SIGALRM, &newact, NULL);
    sigaction(SIGVTALRM, &newact, NULL);
    //Start interval timer
    struct itimerval tv;
    memset(&tv, 0, sizeof(struct itimerval));
    tv.it_value.tv_usec = 10000;
    tv.it_interval.tv_usec = 10000;
#if MYTH_USE_ITIMER
    setitimer(ITIMER_REAL,&tv,NULL);
#endif
  }
}

//Cleanup a worker thread
static inline void myth_cleanup_worker(int rank) {
#if MYTH_ECO_MODE
  if (g_eco_mode_enabled){
    myth_wakeup_all();
    g_envs[rank].c = EXITED;
  }
#endif
  //synchronize
  myth_internal_barrier_wait(&g_worker_barrier);
  myth_running_env_t env;
  env=myth_get_current_env();
  //cleanup timer
  if (rank==0){
    //Stop interval timer
    struct itimerval tv;
    memset(&tv,0,sizeof(struct itimerval));
    tv.it_value.tv_usec=0;
    tv.it_interval.tv_usec=0;
    setitimer(ITIMER_REAL,&tv,NULL);
  }
  //Release scheduler's stack
  if (env->sched.stack)
    myth_free_with_size(env->sched.stack,0);
#if MYTH_WRAP_SOCKIO
  //Release I/O
  myth_io_worker_fini(env,&env->io_struct);
#endif
  //Release runqueue
  myth_queue_fini(&env->runnable_q);
#if MYTH_ENABLE_ADWS
  myth_queue_fini(&env->migration_q);
#endif
  //Release thread descriptor of current thread
  if (env->this_thread) myth_free_thread_desc(env, env->this_thread);
  //Count freelist entry
#if MYTH_FL_PROF
  int fl_num=0;
  void **ret;
  while (1){
    myth_freelist_pop(env->freelist_stack,ret);
    if (!ret)break;
    fl_num++;
  }
  if (fl_num)
    fprintf(stderr,"Rank : %d freelist stack entry : %d\n",rank,fl_num);
#endif
  //Release allocator
  myth_flmalloc_fini_worker(rank);
  //myth_malloc_wrapper_fini_worker(rank);
  //finalize logger
  myth_log_worker_fini(env);
}

//Execute worker thread scheduling loop
static inline void myth_worker_start_ex_body(int rank) {
  myth_setup_worker(rank);
  myth_running_env_t env = myth_get_current_env();
  env->sched.stack = NULL;
  //Call thread scheduler
  myth_sched_loop();
  myth_cleanup_worker(rank);
}

MYTH_CTX_CALLBACK void myth_startpoint_init_ex_1(void* arg1, void* arg2, void* arg3) {
  myth_running_env_t env         = (myth_running_env_t)arg1;
  myth_thread_t      this_thread = (myth_thread_t)arg2;
  (void)arg3;
  //Add to runqueue
  myth_queue_push(&env->runnable_q, this_thread);
}

//Execute worker thread scheduling loop introducing current context as a new thread
static inline void myth_startpoint_init_ex_body(int rank) {
  myth_setup_worker(rank);
  myth_running_env_t env = myth_get_current_env();
  env->exit_flag = -1;
  //Allocate thread descriptor for current thread
  myth_thread_t this_thread = myth_get_new_thread_desc(env);
#if MYTH_SPLIT_STACK_DESC
  this_thread->stack = NULL;
#endif
#if MYTH_ENABLE_THREAD_ANNOTATION && MYTH_COLLECT_LOG
  sprintf(this_thread->annotation_str, "%p(main)", this_thread);
#endif
  /* todo: make this a part of allocation and unify
     with myth_create_ex_body */
  myth_tls_tree_init(this_thread->tls);
  //Set worker thread descrptor
  this_thread->env = env;
#if MYTH_ENABLE_ADWS
  myth_init_workers_range(&this_thread->workers_range, (double)g_attr.n_workers, 0.0);
  myth_set_new_steal_range(this_thread->workers_range, env);
  for (int i = 1; i < g_attr.n_workers; i++) {
    // initially set the root steal range to all workers to avoid NULL steal range
    g_envs[i].cur_steal_range = env->cur_steal_range;
  }
#endif
  //Initialize context for scheduler
  env->sched.stack = myth_malloc(MYTH_SCHED_STACK_SIZE);
  myth_make_context_voidcall(&env->sched.context, myth_sched_loop,
      (void*)(((char*)env->sched.stack) + MYTH_SCHED_STACK_SIZE - sizeof(void*)),
      MYTH_SCHED_STACK_SIZE - sizeof(void*));
  //Switch to scheduler
  myth_swap_context_withcall(&this_thread->context, &env->sched.context,
                             myth_startpoint_init_ex_1, env, this_thread, NULL);
}

MYTH_CTX_CALLBACK void myth_startpoint_exit_ex_1(void* arg1, void* arg2, void* arg3) {
  myth_thread_t th   = (myth_thread_t)arg1;
  intptr_t      rank = (intptr_t)arg2;
  (void)arg3;
  myth_running_env_t target=&g_envs[rank];
  th->env=target;
  while (!myth_queue_trypass(&target->runnable_q, th)) {
    target=myth_env_get_randomly();
    th->env=target;
  }
}

//Tell all the worker threads to terminate
//Worker threads initialized by "myth_startpoint_init_ex" is NOT terminated by this function
//To terminate them, call "myth_startpoint_exit_ex" from the context that "myth_startpoint_init_ex" called
static void myth_notify_workers_exit(void) {
  int i;
  for (i = 0; i < g_attr.n_workers; i++) {
#if MYTH_ECO_MODE
    if (g_eco_mode_enabled){
      //	  if (g_envs[i].exit_flag==0){
      g_envs[i].exit_flag=1;
      g_envs[i].c = FINISH;
    }
#else
    if (0) { }
#endif
    else{
      if (g_envs[i].exit_flag == 0)
        g_envs[i].exit_flag = 1;
    }
  }
}

//Return the context introduced by myth_startpoint_init_ex to the original pthread
static inline void myth_startpoint_exit_ex_body(int rank) {
  //First, make sure that the current thread is running on the initial worker
  myth_running_env_t env = myth_get_current_env();
#if MYTH_ECO_MODE
  if (g_eco_mode_enabled){
    // Wake up all the workers
    int i;
    for(i = 0; i < g_attr.n_workers; i++) g_envs[i].finish_ready=1;
    while (1){
      myth_wakeup_all_force();
      for(i = 0; i < g_attr.n_workers; i++) {
	if (i == env->rank) continue;
	if (g_envs[i].finish_ready != 2) break;
      }
      if (i == g_attr.n_workers) break;
    }
  }
#endif
  //If running on a different worker, switch context
  while (env->rank != rank) {
    intptr_t rank_ = rank;
    myth_thread_t th;
    th = env->this_thread;
    myth_swap_context_withcall(&th->context, &env->sched.context, 
			       myth_startpoint_exit_ex_1,
			       (void*)th, (void*) rank_, NULL);
    myth_rwbarrier();
    //Obtain worker thread descriptor again, because env may be changed
    env = th->env;
  }
  //Set exit flag
  myth_notify_workers_exit();
  //Cleanup
  myth_cleanup_worker(rank);
}

int myth_scheduler_worker_init(int rank, int nw);

  //Initialize each worker thread
static inline void *myth_worker_thread_fn(void *args) {
  intptr_t rank = (intptr_t)args;
  if (g_attr.bind_workers > 0){
    myth_bind_worker(rank);
  }
#if EXPERIMENTAL_SCHEDULER
  myth_scheduler_worker_init(rank, g_attr.n_workers);
#endif
  if (rank == 0) {
    //setup as a main thread
    myth_startpoint_init_ex_body(rank);
  } else {
    //setup as a worker
    myth_worker_start_ex_body(rank);
  }
  return NULL;
}

extern myth_steal_func_t g_myth_steal_func;

static inline myth_thread_t myth_steal_body(void) {
  //Choose a worker thread that seems to be busy
  myth_running_env_t target = myth_env_get_randomly();
  myth_assert(target);
  //Try to steal thread
  myth_thread_t ret = myth_queue_take(&target->runnable_q);
  if (ret) {
    if (!ret->stack) {
      //Cannot be stolen because it is main thread
      while (!myth_queue_trypass(&target->runnable_q, ret)) {
        //Retry changing the target
        target = myth_env_get_randomly();
      }
      return NULL;
    }
    myth_assert(ret->status == MYTH_STATUS_READY);
    //Change worker thread descriptor
    ret->env = NULL;
  }
  return ret;
}

static inline void myth_import_body(myth_thread_t th)
{
  myth_running_env_t e = myth_get_current_env();
  th->env=e;
  myth_queue_push(&e->runnable_q,th);
}

static inline void myth_ext_import_body(myth_thread_t th)
{
  myth_running_env_t target;
  do{
    target = myth_env_get_randomly();
  }
  while (!myth_queue_trypass(&target->runnable_q,th));
}

#if MYTH_ECO_MODE
//extern static void myth_eco_sched_loop(myth_running_env_t env);
static void myth_eco_sched_loop(myth_running_env_t env) {
  //  printf("%d\n",FINISH);
  while (1) {
    //sched_yield();
    myth_thread_t next_run;
    //Get runnable thread
    next_run = myth_queue_pop(&env->runnable_q);
#if MYTH_WRAP_SOCKIO
    //If there is no runnable thread, check I/O
    if (!next_run){
      next_run = myth_io_polling(env);
    }
#endif
    //If there is no runnable thread after I/O checking, try work-stealing
    if (!next_run){
      if (env->finish_ready == 0)
	next_run = myth_eco_steal(env->rank);
      else{
	next_run = g_myth_steal_func(env->rank);
	env->finish_ready = 2;
      }
    }
    if ( ((intptr_t)next_run) == ((intptr_t)FINISH) ) { //next_run == FINISH
      if (env->rank != 0) {
	env->this_thread = NULL;
	return;
      } else {
	while (1) {
	  MAY_BE_UNUSED int temp = 0;
	  int j;
	  for (j = 1; j < g_attr.n_workers; j++) {
	    if (g_envs[j].c != EXITED) {
	      temp = 1;
	    }
	  }
	  //	  if(temp == 0) return;
	}
      }
    }
    if (next_run) {
      //sanity check
      myth_assert(next_run->status==MYTH_STATUS_READY);
      env->this_thread = next_run;
      next_run->env = env;
      //Switch to runnable thread
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("myth_sched_loop:switching to thread:%p\n",next_run);
#endif
      myth_assert(next_run->status==MYTH_STATUS_READY);
      myth_swap_context(&env->sched.context, &next_run->context);
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("myth_sched_loop:returned from thread:%p\n",(void*)next_run);
#endif
      env->this_thread = NULL;
    }
    //Check exit flag
    if (env->exit_flag == 1) {
      if (env->rank == 0)
	while (1) {
	  int temp = 0;
	  int j;
	  for (j = 1; j < g_attr.n_workers; j++) {
	    if (g_envs[j].c != EXITED) {
	      temp = 1;
	    }
	  }
	  if (temp == 0)
	    return;
	}
      env->this_thread = NULL;
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("env %p received exit signal,exiting\n",env);
#endif
      return;
    }
  }
}
#endif

#if MYTH_ENABLE_ADWS

static inline myth_thread_t myth_adws_try_steal(myth_running_env_t env) {
  myth_thread_t ret = NULL;

  myth_assert(env->use_migration_q == 0);

  myth_scan_steal_range(env);

  ret = myth_adws_get_search_root_thread(env);
  if (ret) return ret;

  myth_steal_range_t* steal_range = env->cur_steal_range;
  myth_assert(steal_range);

  /* Choose a victim */
  int left_w  = myth_workers_range_at_left_boundary(steal_range->workers_range) ? steal_range->workers_range.left_worker - 1 : steal_range->workers_range.left_worker;
  int right_w = steal_range->workers_range.right_worker;
  if (left_w <= right_w) return NULL;

  if (env->rank > left_w || right_w > env->rank) return NULL;
  int target = myth_random(right_w, left_w);
  // ensure target != env->rank
  if (target >= env->rank) target++;

  myth_assert(target < g_attr.n_workers);
  myth_running_env_t target_env = &g_envs[target];

  /* Try steal */
  myth_thread_queue_t queues[2] = {NULL, NULL};
  if (target != right_w) {
    queues[0] = &target_env->migration_q;
  }
  if (target != left_w || (target == left_w && myth_workers_range_at_left_boundary(steal_range->workers_range))) {
    queues[1] = &target_env->runnable_q;
  }

  for (int i = 0; i < 2; i++) {
    myth_thread_queue_t q = queues[i];
    if (!q) continue;
    int lock_acquired = 0;
    do {
      if (q->top - q->base <= 0) break;
      if (!steal_range->active) return NULL;
      ret = myth_adws_get_search_root_thread(env);
      if (ret) return ret;
    } while (!(lock_acquired = myth_spin_trylock_body(&q->lock)));
    if (lock_acquired) {
      ret = myth_queue_take_unsafe(q);
      myth_spin_unlock_body(&q->lock);
      if (ret) return ret;
    }
    myth_assert(!ret);
  }

  return ret;
}

static inline myth_thread_t myth_find_runnable_thread_adws(myth_running_env_t env) {
  myth_thread_t next_thread = myth_adws_pop_runnable_thread(env);
  // try to steal
  if (g_adws_stealable && !next_thread) {
    next_thread = myth_adws_try_steal(env);
  }
  return next_thread;
}

#endif

static inline myth_thread_t myth_find_runnable_thread_ws(myth_running_env_t env) {
  //Get runnable thread
  myth_thread_t next_thread = myth_queue_pop(&env->runnable_q);
#if MYTH_WRAP_SOCKIO
  //If there is no runnable thread, check I/O
  if (!next_thread) {
    next_thread = myth_io_polling(env);
  }
#endif
  //If there is no runnable thread after I/O checking, try work-stealing
  if (!next_thread) {
    next_thread = g_myth_steal_func(env->rank);
  }
  return next_thread;
}

static inline myth_thread_t myth_find_runnable_thread(myth_running_env_t env) {
#if MYTH_ENABLE_ADWS
  if (g_sched_adws) {
    return myth_find_runnable_thread_adws(env);
  } else {
    return myth_find_runnable_thread_ws(env);
  }
#else
  return myth_find_runnable_thread_ws(env);
#endif
}

//Main loop of scheduler
static void myth_sched_loop(void) {
  MAY_BE_UNUSED uint64_t t0,t1;
  t0 = 0;
  t1 = 0;
  myth_running_env_t env = myth_get_current_env();
  myth_log_add_context_switch(env, NULL);
#if MYTH_SCHED_LOOP_DEBUG
  myth_dprintf("myth_sched_loop:entered main loop\n");
#endif
#if MYTH_WS_PROF_DETAIL
  env->prof_data.ws_attempt_count=myth_malloc(sizeof(uint64_t)*g_attr.n_workers);
  for (int i = 0; i < g_attr.n_workers; i++) {
    env->prof_data.ws_attempt_count[i] = 0;
  }
#endif
  //get the first thread
  myth_thread_t first_thread = myth_queue_pop(&env->runnable_q);
  myth_internal_barrier_wait(&g_worker_barrier);
  if (first_thread) {
    //sanity check
    myth_assert(first_thread->status == MYTH_STATUS_READY);
    env->this_thread = first_thread;
    first_thread->env = env;
    //Switch to runnable thread
    myth_assert(first_thread->status == MYTH_STATUS_READY);
    myth_swap_context(&env->sched.context, &first_thread->context);
  }
  env->this_thread = NULL;
#if MYTH_ECO_MODE
  if (g_eco_mode_enabled){
    myth_eco_sched_loop(env);
    return;
  }
#endif
  while (1) {
    //Get runnable thread
    myth_thread_t next_thread = myth_find_runnable_thread(env);
    if (next_thread) {
      //sanity check
      myth_assert(next_thread->status == MYTH_STATUS_READY);
      env->this_thread = next_thread;
      next_thread->env = env;
      //Switch to runnable thread
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("myth_sched_loop:switching to thread:%p\n",next_thread);
#endif
      myth_assert(next_thread->status == MYTH_STATUS_READY);
      myth_swap_context(&env->sched.context, &next_thread->context);
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("myth_sched_loop:returned from thread:%p\n",(void*)next_thread);
#endif
      env->this_thread = NULL;
    }
#if MYTH_PAUSE
    else {
      // TODO: make it portable
      __asm__ __volatile("pause;");
    }
#endif
    //Check exit flag
    if (env->exit_flag == 1) {
      env->this_thread = NULL;
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("env %p received exit signal,exiting\n",env);
#endif
      return;
    }
  }
}

static inline int myth_get_worker_num_body(void) {
  myth_ensure_init();
  myth_running_env_t env = myth_get_current_env();
  return env->rank;
}

static inline int myth_get_num_workers_body(void) {
  myth_ensure_init();
  return g_attr.n_workers;
}

#endif	/* MYTH_WORKER_FUNC_H_ */
