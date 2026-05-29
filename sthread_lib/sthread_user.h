/*
 * sthread_user.h - Interface da implementação user-level de sthreads.
 */

#ifndef STHREAD_USER_H
#define STHREAD_USER_H 1

/* Básicas */
void sthread_user_init(void);
sthread_t sthread_user_create(sthread_start_func_t start_routine, void *arg);
sthread_t sthread_user_create_p(sthread_start_func_t start_routine, void *arg, int priority);
void sthread_user_exit(void *ret);
void  sthread_user_yield(void);

/* Avançadas */
int  sthread_user_sleep(int time);
int  sthread_user_join(sthread_t thread, void **value_ptr);

/* Escalonamento */
int  sthread_user_nice(int nice);
void sthread_user_dump(void);

/* Mutex */
sthread_mutex_t sthread_user_mutex_init(void);
void  sthread_user_mutex_free(sthread_mutex_t lock);
void sthread_user_mutex_lock(sthread_mutex_t lock);
void sthread_user_mutex_unlock(sthread_mutex_t lock);

/* Monitor */
sthread_mon_t sthread_user_monitor_init(void);
void  sthread_user_monitor_free(sthread_mon_t mon);
void   sthread_user_monitor_enter(sthread_mon_t mon);
void  sthread_user_monitor_exit(sthread_mon_t mon);
void  sthread_user_monitor_wait(sthread_mon_t mon);
void sthread_user_monitor_signal(sthread_mon_t mon);

/* Dummies (pthreads) */
sthread_mon_t sthread_dummy_monitor_init(void);
void sthread_dummy_monitor_free(sthread_mon_t mon);
void sthread_dummy_monitor_enter(sthread_mon_t mon);
void sthread_dummy_monitor_exit(sthread_mon_t mon);
void  sthread_dummy_monitor_wait(sthread_mon_t mon);
void    sthread_dummy_monitor_signal(sthread_mon_t mon);

#endif /* STHREAD_USER_H */
