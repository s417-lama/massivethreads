/* 
 * myth_barrier.c --- test barrier
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <myth/myth.h>

myth_barrier_t b[1];

typedef struct {
  long id;
  long n_threads;
  long n;
  long * a;
} arg_t;

void * f(void * arg_) {
  arg_t * arg = arg_;
  long id = arg->id;
  long n_threads = arg->n_threads;
  long n = arg->n;
  long * a = arg->a;
  long t;
  for (t = 0; t < n; t++) {
    a[id] = t + 1;
    myth_barrier_wait(b);
    long j;
    for (j = 0; j < n_threads; j++) {
      assert(a[j] == t + 1);
    }
    myth_barrier_wait(b);
  }
  return 0;
}

int main(int argc, char ** argv) {
  long n_threads = (argc > 1 ? atol(argv[1]) : 1000);
  long n         = (argc > 2 ? atol(argv[2]) : 1000);
  myth_thread_t tids[n_threads];
  long * a = calloc(sizeof(long), n_threads);
  arg_t * args = calloc(sizeof(arg_t), n_threads);
  arg_t arg = { -1, n_threads, n, a };
  long i;
  myth_barrier_init(b, 0, n_threads);
  for (i = 0; i < n_threads; i++) {
    args[i] = arg;
    args[i].id = i;
    tids[i] = myth_create(f, &args[i]);
  }
  for (i = 0; i < n_threads; i++) {
    myth_join(tids[i], 0);
  }
  for (i = 0; i < n_threads; i++) {
    if (a[i] != n) {
      printf("NG: a[%ld] = %ld != %ld\n", i, a[i], n);
      return 1;
    }
  }
  printf("OK\n");
  free(a);
  return 0;
}
