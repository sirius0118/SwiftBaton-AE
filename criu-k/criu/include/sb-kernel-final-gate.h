/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_KERNEL_FINAL_GATE_H
#define SB_KERNEL_FINAL_GATE_H
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

/* Protect catalog reservations and the lifetime of the session/PS catalog.
 * Each admitted worker owns one reference, including while its RPC blocks.
 * The coordinator seals admission before sending any final descriptor. */
struct sbk_final_gate {
  pthread_mutex_t lock;
  pthread_cond_t idle;
  unsigned active;
  int error;
  bool sealed, closing;
};
#define SBK_FINAL_GATE_INIT { .lock = PTHREAD_MUTEX_INITIALIZER, \
                            .idle = PTHREAD_COND_INITIALIZER }

/* Success leaves the lock held for catalog reservation; failure unlocks it. */
static inline int sbk_final_enter(struct sbk_final_gate *g) {
  int ret;
  pthread_mutex_lock(&g->lock);
  ret = g->closing ? -ESHUTDOWN : g->error ? g->error : g->sealed ? -EBUSY : 0;
  if (ret) pthread_mutex_unlock(&g->lock);
  else g->active++;
  return ret;
}
static inline void sbk_final_release(struct sbk_final_gate *g, int ret) {
  pthread_mutex_lock(&g->lock);
  if (ret && !g->error) g->error = ret;
  if (!--g->active) pthread_cond_broadcast(&g->idle);
  pthread_mutex_unlock(&g->lock);
}
/* Success holds the lock until the complete catalog has been sent. On failure
 * no caller may publish even the header of a partially prepared catalog. */
static inline int sbk_final_seal(struct sbk_final_gate *g) {
  int ret;
  pthread_mutex_lock(&g->lock);
  g->sealed = true;
  while (g->active) pthread_cond_wait(&g->idle, &g->lock);
  ret = g->closing ? -ESHUTDOWN : g->error;
  if (ret) pthread_mutex_unlock(&g->lock);
  return ret;
}
/* Returns with the lock held. Keep it through resource destruction. Admission
 * stays closed until the next (coordinator-only) connection resets the gate. */
static inline void sbk_final_close(struct sbk_final_gate *g) {
  pthread_mutex_lock(&g->lock);
  g->closing = true;
  while (g->active) pthread_cond_wait(&g->idle, &g->lock);
}
#endif
