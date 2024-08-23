/*
 * Thread pool originates from project Sogou C++ Workflow
 * https://github.com/sogou/workflow
 *
 * A thread task can be scheduled by another task, which is very important,
 * even if the pool is being destroyed. Because thread task is hard to know
 * what's happening to the pool.
 * The thread pool can also be destroyed by a thread task. This may sound
 * strange, but it's very logical. Destroying thread pool in thread task
 * does not end the task thread. It'll run till the end of task.
 * 一个线程任务可以被另一个线程任务调度，即使线程池被销毁。因为线程任务很难确定线程池的状态。
 * 线程池也可以被线程任务销毁。在线程任务中销毁线程池并不会终止任务线程。任务线程会运行到任务结束
 */

#ifndef _THRDPOOL_H_
#define _THRDPOOL_H_

#include <stddef.h>

typedef struct __thrdpool thrdpool_t;

// 线程池中的任务的结构
struct thrdpool_task {
  void (*routine)(void *);  // 函数指针，指向任务的具体执行函数，
  void *context;  // 指向任务上下文的指针，用于传递任务相关数据/参数
};

#ifdef __cplusplus
extern "C" {
#endif

thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize);
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool);
int thrdpool_in_pool(thrdpool_t *pool);
int thrdpool_increase(thrdpool_t *pool);
int thrdpool_decrease(thrdpool_t *pool);
void thrdpool_exit(thrdpool_t *pool);
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
                      thrdpool_t *pool);

#ifdef __cplusplus
}
#endif

#endif
