/* SPDX-License-Identifier: GPL-2.0 */
#include "sb-kernel-work.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct sbk_work_pool {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  pthread_t threads[32];
  void **queue;
  unsigned capacity, head, count, workers, active;
  int error;
  bool closing;
  int (*run)(void *);
  void (*dispose)(void *);
  struct sbk_work_stats stats;
};
static void *work_main(void *arg) {
  struct sbk_work_pool *p = arg;
  pthread_mutex_lock(&p->lock);
  for (;;) {
    while (!p->count && !p->closing && !p->error)
      pthread_cond_wait(&p->changed, &p->lock);
    if (p->error || !p->count) break;
    void *item = p->queue[p->head];
    p->head = (p->head + 1) % p->capacity;
    p->count--;
    if (++p->active > p->stats.peak_active) p->stats.peak_active = p->active;
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
    int ret = p->run(item);
    p->dispose(item);
    pthread_mutex_lock(&p->lock);
    p->active--;
    p->stats.completed++;
    if (ret && !p->error) p->error = ret;
    pthread_cond_broadcast(&p->changed);
  }
  pthread_mutex_unlock(&p->lock);
  return NULL;
}
struct sbk_work_pool *sbk_work_create(unsigned workers, unsigned capacity,
                                     int (*run)(void *), void (*dispose)(void *)) {
  struct sbk_work_pool *p;
  int ret;
  if (!workers || workers > 32 || !capacity || !run || !dispose) {
    errno = EINVAL;
    return NULL;
  }
  p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->queue = calloc(capacity, sizeof(*p->queue));
  if (!p->queue) { free(p); return NULL; }
  ret = pthread_mutex_init(&p->lock, NULL);
  if (ret) goto free;
  ret = pthread_cond_init(&p->changed, NULL);
  if (ret) { pthread_mutex_destroy(&p->lock); goto free; }
  p->capacity = capacity;
  p->run = run;
  p->dispose = dispose;
  for (unsigned i = 0; i < workers; i++) {
    ret = pthread_create(&p->threads[i], NULL, work_main, p);
    if (ret) {
      sbk_work_finish(p, true, NULL);
      errno = ret;
      return NULL;
    }
    p->workers++;
  }
  return p;
free:
  free(p->queue);
  free(p);
  errno = ret;
  return NULL;
}
int sbk_work_submit(struct sbk_work_pool *p, void *item) {
  int ret;
  pthread_mutex_lock(&p->lock);
  while (p->count == p->capacity && !p->closing && !p->error)
    pthread_cond_wait(&p->changed, &p->lock);
  ret = p->error ? p->error : p->closing ? -ESHUTDOWN : 0;
  if (!ret) {
    p->queue[(p->head + p->count) % p->capacity] = item;
    if (++p->count > p->stats.peak_queued) p->stats.peak_queued = p->count;
    p->stats.submitted++;
    pthread_cond_broadcast(&p->changed);
  }
  pthread_mutex_unlock(&p->lock);
  return ret;
}
int sbk_work_finish(struct sbk_work_pool *p, bool cancel,
                     struct sbk_work_stats *stats) {
  int ret;
  if (!p) return 0;
  pthread_mutex_lock(&p->lock);
  p->closing = true;
  if (cancel && !p->error) p->error = -ECANCELED;
  pthread_cond_broadcast(&p->changed);
  pthread_mutex_unlock(&p->lock);
  for (unsigned i = 0; i < p->workers; i++) pthread_join(p->threads[i], NULL);
  for (unsigned i = 0; i < p->count; i++)
    p->dispose(p->queue[(p->head + i) % p->capacity]);
  if (stats) *stats = p->stats;
  ret = p->error;
  pthread_cond_destroy(&p->changed);
  pthread_mutex_destroy(&p->lock);
  free(p->queue);
  free(p);
  return ret;
}
