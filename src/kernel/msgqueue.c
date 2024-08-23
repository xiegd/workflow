/*
 * This message queue originates from the project of Sogou C++ Workflow:
 * https://github.com/sogou/workflow
 *
 * 使用双链表加锁的机制实现线程安全的消息传递，两个链表get_list, put_list分别处理消息
 * 的获取和放入操作。
 *  当get_list不为空是，消费者取走一个消息，否则消费者会等待直到put_list不为空，
 *  并交换两个列表。这种方法在队列非常繁忙，消费者数量较大时表现良好
 */

#include "msgqueue.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

// 定义内部结构体
struct __msgqueue {
  size_t msg_max; // 队列最大长度
  size_t msg_cnt; // 队列当前长度
  int linkoff;  // 消息结构体中消息链表指针(指向下一条消息的)相对消息结构体的起始地址的偏移
  int nonblock; // 是否处于非阻塞模式
  void *head1;  // 第一个list头部, get_list
  void *head2;  // 第二个list头部, put_list
  void **get_head;  // 指向get_list的头部指针， 头部也是一个指针，指向msg结构体
  void **put_head;  // 指向put_list的头部指针
  void **put_tail;  // 指向put_list的尾部指针, 指向最后一条消息的指针域?
  pthread_mutex_t get_mutex;  // 获取操作的互斥锁
  pthread_mutex_t put_mutex;  // 放入操作的互斥锁
  pthread_cond_t get_cond;  // 获取操作的条件变量
  pthread_cond_t put_cond;  // 放入操作的条件变量
};

// 将消息队列设置为非阻塞模式
void msgqueue_set_nonblock(msgqueue_t *queue) {
  queue->nonblock = 1;  // 设置队列非阻塞标志
  pthread_mutex_lock(&queue->put_mutex);  // 锁定队列的读写锁
  pthread_cond_signal(&queue->get_cond);  // 通知获取操作的线程
  pthread_cond_broadcast(&queue->put_cond); // 广播放入操作的线程
  pthread_mutex_unlock(&queue->put_mutex);  // 解锁队列的读写锁
}

void msgqueue_set_block(msgqueue_t *queue) { queue->nonblock = 0; }

// 将消息添加到队尾
void msgqueue_put(void *msg, msgqueue_t *queue) {
  void **link = (void **)((char *)msg + queue->linkoff);  // 获取msg所链接的下一条消息的指针的地址, msg包含数据域和指针域
  // 转化为char保证以字节为单位进行指针运算
  *link = NULL; // 将指针域置为NULL
  pthread_mutex_lock(&queue->put_mutex);
  // 如果队列已满，且不处于非阻塞模式，则等待
  while (queue->msg_cnt > queue->msg_max - 1 && !queue->nonblock)
    pthread_cond_wait(&queue->put_cond, &queue->put_mutex);
  // 为什么添加的是link?
  *queue->put_tail = link;  // 对put_tail进行解引用，获得的是尾部的msg结构体的指针，这里更新了尾部msg结构体的指针域，即添加了一条消息
  queue->put_tail = link; // 更新put_tail, 使其指向更新后的尾部
  queue->msg_cnt++;
  pthread_mutex_unlock(&queue->put_mutex);
  pthread_cond_signal(&queue->get_cond);
}

// 将消息添加到队头
void msgqueue_put_head(void *msg, msgqueue_t *queue) {
  void **link = (void **)((char *)msg + queue->linkoff);

  pthread_mutex_lock(&queue->put_mutex);
  // 检查是否有头元素
  while (*queue->get_head) {
    // 尝试锁定互斥锁get_mutex, 成功返回0，
    if (pthread_mutex_trylock(&queue->get_mutex) == 0) {
      pthread_mutex_unlock(&queue->put_mutex);  // 这个释放锁的位置是不是有问题？
      *link = *queue->get_head; // 更新msg的指针域为原来的队头
      *queue->get_head = link;  // 更新队头为添加的指针域，完成头插
      pthread_mutex_unlock(&queue->get_mutex);
      return; // 这里怎么没有msg_cnt++?
    }
  }

  while (queue->msg_cnt > queue->msg_max - 1 && !queue->nonblock)
    pthread_cond_wait(&queue->put_cond, &queue->put_mutex);

  *link = *queue->put_head;
  if (*link == NULL)
    queue->put_tail = link;

  *queue->put_head = link;
  queue->msg_cnt++;
  pthread_mutex_unlock(&queue->put_mutex);
  pthread_cond_signal(&queue->get_cond);
}

// 交换消息队列的头指针和尾指针，返回队列中消息的数量
static size_t __msgqueue_swap(msgqueue_t *queue) {
  void **get_head = queue->get_head;
  size_t cnt;

  pthread_mutex_lock(&queue->put_mutex);
  while (queue->msg_cnt == 0 && !queue->nonblock)
    pthread_cond_wait(&queue->get_cond, &queue->put_mutex);

  cnt = queue->msg_cnt;
  if (cnt > queue->msg_max - 1)
    pthread_cond_broadcast(&queue->put_cond);
  // 交换读写指针
  queue->get_head = queue->put_head;
  queue->put_head = get_head;
  queue->put_tail = get_head; // 重置写指针到新的头部
  queue->msg_cnt = 0; // 重置消息数量
  pthread_mutex_unlock(&queue->put_mutex);
  return cnt;
}

// 从消息队列队尾获取一个消息
void *msgqueue_get(msgqueue_t *queue) {
  void *msg;

  pthread_mutex_lock(&queue->get_mutex);
  if (*queue->get_head || __msgqueue_swap(queue) > 0) {
    msg = (char *)*queue->get_head - queue->linkoff;
    *queue->get_head = *(void **)*queue->get_head;
  } else
    msg = NULL;

  pthread_mutex_unlock(&queue->get_mutex);
  return msg;
}

// 创建消息队列
msgqueue_t *msgqueue_create(size_t maxlen, int linkoff) {
  msgqueue_t *queue = (msgqueue_t *)malloc(sizeof(msgqueue_t));
  int ret;

  if (!queue)
    return NULL;

  ret = pthread_mutex_init(&queue->get_mutex, NULL);  // 初始化获取操作的互斥锁 
  if (ret == 0) {
    ret = pthread_mutex_init(&queue->put_mutex, NULL);  // 初始化放入操作的互斥锁
    if (ret == 0) {
      ret = pthread_cond_init(&queue->get_cond, NULL);  // 初始化获取操作的条件变量
      if (ret == 0) {
        ret = pthread_cond_init(&queue->put_cond, NULL);  // 初始化放入操作的条件变量
        if (ret == 0) {
          queue->msg_max = maxlen;
          queue->linkoff = linkoff; // 消息结构体中消息链表指针的偏移， 和消息类型有关
          queue->head1 = NULL;
          queue->head2 = NULL;
          queue->get_head = &queue->head1;
          queue->put_head = &queue->head2;
          queue->put_tail = &queue->head2;
          queue->msg_cnt = 0;
          queue->nonblock = 0;
          return queue;
        }

        pthread_cond_destroy(&queue->get_cond);
      }

      pthread_mutex_destroy(&queue->put_mutex);
    }

    pthread_mutex_destroy(&queue->get_mutex);
  }

  errno = ret;
  free(queue);
  return NULL;
}

// 销毁消息队列
void msgqueue_destroy(msgqueue_t *queue) {
  // 销毁条件变量和互斥锁
  pthread_cond_destroy(&queue->put_cond);
  pthread_cond_destroy(&queue->get_cond);
  pthread_mutex_destroy(&queue->put_mutex);
  pthread_mutex_destroy(&queue->get_mutex);
  free(queue);  // 释放消息队列结构体
}
