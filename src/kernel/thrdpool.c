#include "thrdpool.h"
#include "msgqueue.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct __thrdpool {
  msgqueue_t *msgqueue; // 消息队列指针
  size_t nthreads;  // 线程池中线程的数量
  size_t stacksize; // 线程池中线程的栈大小
  pthread_t tid;  // 线程池中第一个线程的 ID
  pthread_mutex_t mutex;  // 互斥锁
  pthread_key_t key;  // 线程池中线程的键，用于存储特定于线程的数据
  pthread_cond_t *terminate;  // 终止条件变量指针
};

struct __thrdpool_task_entry {
  void *link; // 指向下一个任务的指针
  struct thrdpool_task task;  // 任务
};

static pthread_t __zero_tid;

// 线程池的退出函数
static void __thrdpool_exit_routine(void *context) {
  thrdpool_t *pool = (thrdpool_t *)context;
  pthread_t tid;

  /* One thread joins another. Don't need to keep all thread IDs. */
  pthread_mutex_lock(&pool->mutex);
  tid = pool->tid;
  pool->tid = pthread_self(); // 将当前tid设置为线程池的tid
  // 如果线程池中没有别的线程(只有当前这个执行结束，但未退出的线程)，并被标记为终止，则发送信号以唤醒等待终止条件的线程
  if (--pool->nthreads == 0 && pool->terminate)
    pthread_cond_signal(pool->terminate);

  pthread_mutex_unlock(&pool->mutex);
  // 如果当前线程不是初始线程，
  if (!pthread_equal(tid, __zero_tid))
    pthread_join(tid, NULL);  // 等待tid对应的线程结束

  pthread_exit(NULL);
}

/* 线程池的执行函数，负责从消息队列中获取任务并执行
* @param arg 线程池指针, 标识执行函数所属的线程池
*/
static void *__thrdpool_routine(void *arg) {
  thrdpool_t *pool = (thrdpool_t *)arg;
  struct __thrdpool_task_entry *entry;
  void (*task_routine)(void *); // 任务函数指针
  void *task_context; // 任务上下文

  pthread_setspecific(pool->key, pool); // 设置线程特定数据, 将pool->key和pool关联，以便在线程退出时能够正确清理
  // 如果线程池没有被终止，则从消息队列中获取任务并执行
  while (!pool->terminate) {
    entry = (struct __thrdpool_task_entry *)msgqueue_get(pool->msgqueue);
    if (!entry)
      break;  // 如果消息队列为空，则退出循环

    task_routine = entry->task.routine;
    task_context = entry->task.context;
    free(entry);  // 释放从消息队列中取出的消息的内存
    task_routine(task_context); // 执行任务函数

    if (pool->nthreads == 0) {
      /* Thread pool was destroyed by the task. */
      free(pool);
      return NULL;
    }
  }

  __thrdpool_exit_routine(pool);
  return NULL;
}

// 终止线程池
static void __thrdpool_terminate(int in_pool, thrdpool_t *pool) {
  pthread_cond_t term = PTHREAD_COND_INITIALIZER;

  pthread_mutex_lock(&pool->mutex);
  msgqueue_set_nonblock(pool->msgqueue);  // 设置消息队列为非阻塞模式
  pool->terminate = &term;

  if (in_pool) {
    /* Thread pool destroyed in a pool thread is legal. */
    pthread_detach(pthread_self()); // 将当前线程从线程池中分离，避免其资源被回收
    pool->nthreads--;
  }
  // 等待所有线程完成
  while (pool->nthreads > 0)
    pthread_cond_wait(&term, &pool->mutex);

  pthread_mutex_unlock(&pool->mutex);
  if (!pthread_equal(pool->tid, __zero_tid))
    pthread_join(pool->tid, NULL);
}

// 创建线程池中的线程
static int __thrdpool_create_threads(size_t nthreads, thrdpool_t *pool) {
  pthread_attr_t attr;  // 线程属性
  pthread_t tid;  // 线程ID
  int ret;

  ret = pthread_attr_init(&attr); // 初始化线程属性, 所有线程具有相同的属性
  if (ret == 0) {
    if (pool->stacksize)
      pthread_attr_setstacksize(&attr, pool->stacksize);  // 设置线程栈大小
    // 如果线程池中的线程数量小于指定的线程数量，继续创建线程
    while (pool->nthreads < nthreads) {
      ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);  // pool传递给__thrdpool_routine函数的参数
      if (ret == 0)
        pool->nthreads++;
      else
        break;
    }

    pthread_attr_destroy(&attr);
    if (pool->nthreads == nthreads)
      return 0; // 创建成功返回0

    __thrdpool_terminate(0, pool);  // 创建失败，终止线程池
  }

  errno = ret;
  return -1;
}

// 创建一个包含指定数量线程和栈大小的线程池
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize) {
  thrdpool_t *pool;
  int ret;

  pool = (thrdpool_t *)malloc(sizeof(thrdpool_t));  // 分配内存空间， 分配失败返回NULL
  if (!pool)
    return NULL;  // 创建失败返回NULL

  pool->msgqueue = msgqueue_create(0, 0); // 创建消息队列，用于接收和存储任务, 创建失败返回NULL
  if (pool->msgqueue) {
    ret = pthread_mutex_init(&pool->mutex, NULL); // 初始化互斥锁， 成功返回0，失败返回errno
    if (ret == 0) {
      ret = pthread_key_create(&pool->key, NULL); // 创建TSD key, 成功返回0， 失败返回errno
      if (ret == 0) {
        // 如果线程池内存分配成功，msgqueue创建成功，mutex初始化成功, TSD key创建成功
        pool->stacksize = stacksize;
        pool->nthreads = 0;
        pool->tid = __zero_tid;
        pool->terminate = NULL;
        if (__thrdpool_create_threads(nthreads, pool) >= 0)
          return pool;

        pthread_key_delete(pool->key);
      }

      pthread_mutex_destroy(&pool->mutex);
    }

    errno = ret;
    msgqueue_destroy(pool->msgqueue);
  }

  free(pool);
  return NULL;
}

inline void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
                                thrdpool_t *pool);

void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
                         thrdpool_t *pool) {
  ((struct __thrdpool_task_entry *)buf)->task = *task;
  msgqueue_put(buf, pool->msgqueue);  // 将任务添加到线程池的消息队列
}

// 将任务添加到线程池
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool) {
  void *buf = malloc(sizeof(struct __thrdpool_task_entry));

  if (buf) {
    __thrdpool_schedule(task, buf, pool);
    return 0;
  }

  return -1;
}

inline int thrdpool_in_pool(thrdpool_t *pool);

// 判断当前线程是否在指定的线程池中
int thrdpool_in_pool(thrdpool_t *pool) {
  return pthread_getspecific(pool->key) == pool;  // 
}

// 增加线程池中线程的数量
int thrdpool_increase(thrdpool_t *pool) {
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  ret = pthread_attr_init(&attr);
  if (ret == 0) {
    if (pool->stacksize)
      pthread_attr_setstacksize(&attr, pool->stacksize);

    pthread_mutex_lock(&pool->mutex);
    ret = pthread_create(&tid, &attr, __thrdpool_routine, pool);
    if (ret == 0)
      pool->nthreads++;

    pthread_mutex_unlock(&pool->mutex);
    pthread_attr_destroy(&attr);
    if (ret == 0)
      return 0;
  }

  errno = ret;
  return -1;
}

// 减少线程池中线程的数量
int thrdpool_decrease(thrdpool_t *pool) {
  void *buf = malloc(sizeof(struct __thrdpool_task_entry));
  struct __thrdpool_task_entry *entry;

  if (buf) {
    entry = (struct __thrdpool_task_entry *)buf;
    entry->task.routine = __thrdpool_exit_routine;  // 设置任务例程为退出例程
    entry->task.context = pool; // 设置上下文为线程池指针
    msgqueue_put_head(entry, pool->msgqueue); // 将任务放入线程池的消息队列队头，触发线程退出
    return 0;
  }

  return -1;
}

// 退出线程池
void thrdpool_exit(thrdpool_t *pool) {
  if (thrdpool_in_pool(pool))
    __thrdpool_exit_routine(pool);  // 如果当前线程在线程池中则退出
}

// 销毁线程池并处理未完成的任务
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
                      thrdpool_t *pool) {
  int in_pool = thrdpool_in_pool(pool);
  struct __thrdpool_task_entry *entry;

  __thrdpool_terminate(in_pool, pool);
  // 处理消息队列中剩余的任务
  while (1) {
    entry = (struct __thrdpool_task_entry *)msgqueue_get(pool->msgqueue);
    if (!entry)
      break;

    if (pending && entry->task.routine != __thrdpool_exit_routine)
      pending(&entry->task);

    free(entry);
  }

  pthread_key_delete(pool->key);
  pthread_mutex_destroy(&pool->mutex);
  msgqueue_destroy(pool->msgqueue);
  if (!in_pool)
    free(pool);
}
