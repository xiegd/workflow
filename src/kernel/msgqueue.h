/*
 *   message queue interface
 *   用于进程或线程将通信
 */

#ifndef _MSGQUEUE_H_
#define _MSGQUEUE_H_

#include <stddef.h>

typedef struct __msgqueue msgqueue_t;

#ifdef __cplusplus
extern "C" {
#endif

/* A simple implementation of message queue. The max pending messages may
 * reach two times 'maxlen' when the queue is in blocking mode, and infinite
 * in nonblocking mode. 'linkoff' is the offset from the head of each message,
 * where spaces of one pointer size should be available for internal usage.
 * 'linkoff' can be positive or negative or zero. */

// 创建消息队列
msgqueue_t *msgqueue_create(size_t maxlen, int linkoff);
// 从队尾获取一个消息
void *msgqueue_get(msgqueue_t *queue);
// 将消息添加到队尾
void msgqueue_put(void *msg, msgqueue_t *queue);
// 将消息添加到队头
void msgqueue_put_head(void *msg, msgqueue_t *queue);
// 设置消息队列为非阻塞模式
void msgqueue_set_nonblock(msgqueue_t *queue);
// 设置消息队列为阻塞模式
void msgqueue_set_block(msgqueue_t *queue);
// 销毁消息队列
void msgqueue_destroy(msgqueue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
