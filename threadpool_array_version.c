#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>

#define DEFAULT_CHECK_TIME      10
#define MIN_WAIT_JOB_NUM        10
#define DEFAULT_THREAD_CHANGE   10
#define false                   0
#define true                    1

typedef struct {
    void *(*job_function)(void *);      /*任务回调函数*/
    void *arg;                          /*任务回调函数参数*/
} threadpool_job;

typedef struct threadpool_t {
    pthread_mutex_t lock;               /*保证本结构体的互斥性*/
    pthread_mutex_t thread_counter;     /*记录忙线程个数的锁*/
    pthread_cond_t  queue_not_full;     /*条件变量，任务队列满时，添加任务线程阻塞，需要等待该条件变量*/
    pthread_cond_t  queue_not_empty;    /*条件变量，任务队列不为空时，用于通知等待任务的线程*/

    pthread_t *threads;                 /*存放线程池中国的各个线程的tid，数组实现*/
    pthread_t adjust_tid;               /*存储线程管理器所在的线程id*/
    threadpool_job *task_queue;         /*任务队列*/


    int min_thread_num;                 /*最小线程数*/
    int max_thread_num;                 /*最大线程数*/
    int live_thread_num;                /*当前存活的线程数*/
    int busy_thread_num;                /*当前忙的线程数*/
    int wait_exit_thread_num;           /*要销毁的线程数*/

    int queue_front;                    /*任务队列队头下标*/
    int queue_rear;                     /*任务队列队尾下标*/
    int queue_size;                     /*任务队列中实际任务数*/
    int queue_max_size;                 /*任务队列中可容纳任务数上限*/

    int if_shutdown;                    /*标志位*/
} threadpool_t;

threadpool_t *threadpool_create(int min_thread_num, int max_thread_num, int queue_max_size);

int threadpool_add(threadpool_t *pool, void*(*function)(void *arg), void *arg);

void *pthreadpool_running(void *threadpool);
void *adjust_running(void *threadpool);
int threadpool_free(threadpool_t* threadpool);
int threadpool_destroy(threadpool_t *pool);

int is_thread_alive(pthread_t tid);

/*暂未使用到的函数*/
int threadpool_all_threadnum(threadpool_t *pool);
int threadpool_busy_threadnum(threadpool_t *pool);

threadpool_t *threadpool_create(int min_thread_num, int max_thread_num, int queue_max_size) {
    threadpool_t *pool = NULL;
    do {
        if ((pool = (threadpool_t*)malloc(sizeof(threadpool_t))) == NULL) {
            perror("malloc threadpool:");
            break;
        }

        // 值的初始化
        pool->min_thread_num = min_thread_num;
        pool->max_thread_num = max_thread_num;
        pool->busy_thread_num = 0;
        pool->live_thread_num = min_thread_num;     /*初值为最小线程数*/
        pool->queue_size = 0;
        pool->queue_max_size = queue_max_size;
        pool->queue_front = 0;
        pool->queue_rear = 0;
        pool->if_shutdown = false;

        pool->threads = (pthread_t *)malloc(sizeof(pthread_t)*max_thread_num);
        if (pool->threads == NULL) {
            perror("malloc threadpool threads:");
            break;
        }
        memset(pool->threads, 0, sizeof(pthread_t)*max_thread_num);

        pool->task_queue = (threadpool_job*)malloc(sizeof(threadpool_job)*queue_max_size);
        if (pool->task_queue == NULL) {
            perror("malloc threadpool tasks queue:");
            break;
        }

        if (pthread_mutex_init(&(pool->lock), NULL) != 0
            || pthread_mutex_init(&(pool->thread_counter), NULL) != 0
            || pthread_cond_init(&(pool->queue_not_empty), NULL) != 0
            || pthread_cond_init(&(pool->queue_not_full), NULL) != 0) {
                perror("mutex initialization:");
                break;
            }

        int i = 0;
        for (i = 0; i < min_thread_num; ++i) {
            pthread_create(&(pool->threads[i]), NULL, pthreadpool_running, (void*)pool);
            printf("==> starting thread %lu...\n", pool->threads[i]);
        }
        pthread_create(&(pool->adjust_tid), NULL, adjust_running, (void*)pool);

        return pool;

    } while (0);

    // 失败需要处理
    if (pool != NULL)
        threadpool_free(pool);

    return NULL;
}


int threadpool_free(threadpool_t* threadpool) {
    if (threadpool == NULL) return -1;

    if (threadpool->task_queue) {
        free(threadpool->task_queue);
    }
    if (threadpool->threads) {
        free(threadpool->threads);
        pthread_mutex_lock(&(threadpool->lock));
        pthread_mutex_destroy(&(threadpool->lock));
        pthread_mutex_lock(&(threadpool->thread_counter));
        pthread_mutex_destroy(&(threadpool->thread_counter));
        
        pthread_cond_destroy(&(threadpool->queue_not_empty));
        pthread_cond_destroy(&(threadpool->queue_not_full));
    }

    free(threadpool);
    threadpool = NULL;

    return 0;
}


int threadpool_destroy(threadpool_t *pool) {
    if (pool == NULL) {
        return -1;
    }
    // printf("what????\n");

    pool->if_shutdown = true;

    // 销毁管理线程
    pthread_join(pool->adjust_tid, NULL);

    int i;

    for (i = 0; i < pool->live_thread_num; ++i) {
        pthread_cond_broadcast(&(pool->queue_not_empty));
    }
    // printf("what??\n");

    for (i = 0; i < pool->live_thread_num; ++i) {
        pthread_join(pool->threads[i], NULL);
    }
    // printf("why??\n");
    threadpool_free(pool);
    return 0;
}

/*工作线程*/
void *pthreadpool_running(void *threadpool) {
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_job job;

    while (1) {
        /*锁住，等待任务队列中有任务，否则通过锁阻塞,结合下面的条件变量使用*/
        pthread_mutex_lock(&(pool->lock));

        /*线程池未关闭且任务队列为0时*/
        while ((pool->queue_size == 0) && (!pool->if_shutdown)) {
            printf("thread %lu waiting\n", pthread_self());
            // 条件变量是利用线程间共享的全局变量进行同步的一种机制，
            // 主要包括两个动作：一个线程等待"条件变量的条件成立"而挂起；
            // 另一个线程使"条件成立"（给出条件成立信号）。
            // 为了防止竞争，条件变量的使用总是和一个互斥锁结合在一起。
            // 进入的时候unlock——>pool->lock
            // 退出的时候lock——>pool->lock
            pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));


            if (pool->wait_exit_thread_num > 0) {
                pool->wait_exit_thread_num--;

                if (pool->live_thread_num > pool->min_thread_num) {
                    printf("thread %lu is exiting...\n", pthread_self());
                    pool->live_thread_num--;
                    pthread_mutex_unlock(&(pool->lock));
                    pthread_exit(NULL);
                }
            }
        }


        if (pool->if_shutdown) {
            pthread_mutex_unlock(&(pool->lock));
            printf("thread %lu is exiting...\n", pthread_self());
            pthread_exit(NULL);
        }

        /*从任务队列中获取任务并出队*/
        job.job_function = pool->task_queue[pool->queue_front].job_function;
        job.arg = pool->task_queue[pool->queue_front].arg;

        pool->queue_front = (pool->queue_front + 1) % pool->queue_max_size;
        pool->queue_size--;

        /*通知新任务到来可以执行了*/
        pthread_cond_broadcast(&(pool->queue_not_full));

        /*任务取出后需要将线程池锁释放*/
        pthread_mutex_unlock(&(pool->lock));

        /*running*/
        printf("thread %lu running...\n", pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thread_num++;
        pthread_mutex_unlock(&(pool->thread_counter));
        (*(job.job_function))(job.arg);

        /*finished*/
        printf("thread %lu finishing...\n", pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thread_num--;
        pthread_mutex_unlock(&(pool->thread_counter));

    }

    pthread_exit(NULL);
}

/*管理线程*/
void *adjust_running(void *threadpool) {
    threadpool_t *pool = (threadpool_t *)threadpool;
    while (!pool->if_shutdown) {
        sleep(DEFAULT_CHECK_TIME);

        pthread_mutex_lock(&(pool->lock));
        int queue_size = pool->queue_size;
        int live_thread_num = pool->live_thread_num;
        pthread_mutex_unlock(&(pool->lock));

        pthread_mutex_lock(&(pool->thread_counter));
        int busy_thread_num = pool->busy_thread_num;
        pthread_mutex_unlock(&(pool->thread_counter));

        /*当 任务数 > 最小线程池个数 且 存活的线程数少于最大线程数时， 创建新线程*/
        if (queue_size >= MIN_WAIT_JOB_NUM && live_thread_num < pool->max_thread_num) {
            pthread_mutex_lock(&(pool->lock));
            int add = 0;

            int i = 0;
            for (i = 0; i < pool->max_thread_num && add < DEFAULT_THREAD_CHANGE
                    && pool->live_thread_num < pool->max_thread_num; ++i) {
                if (pool->threads[i] == 0 || !is_thread_alive(pool->threads[i])) {
                    pthread_create(&(pool->threads[i]), NULL, pthreadpool_running, (void*)pool);
                    ++add;
                    pool->live_thread_num++;
                }
            }

            pthread_mutex_unlock(&(pool->lock));
        }

        /*销毁多余线程，若忙线程数*2 < 存活的线程数 且 存活的线程数 > 最小线程数 时按量销毁*/
        if ((busy_thread_num * 2) < live_thread_num && live_thread_num > pool->min_thread_num) {
            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thread_num = DEFAULT_THREAD_CHANGE;
            pthread_mutex_unlock(&(pool->lock));

            int i = 0;
            for (i = 0; i < DEFAULT_THREAD_CHANGE; ++i) {
                // 通过这一个函数去通知处于空闲装态的线程，它们在上面的处理逻辑中终止
                pthread_cond_signal(&(pool->queue_not_empty));
            }
        }
    }

    return NULL;
}

/*添加任务*/
int threadpool_add(threadpool_t *pool, void*(*function)(void *arg), void *arg) {
    pthread_mutex_lock(&(pool->lock));

    while ((pool->queue_size == pool->queue_max_size) && !(pool->if_shutdown)) {
        pthread_cond_wait(&(pool->queue_not_full), &(pool->lock));
    }

    if (pool->if_shutdown) {
        pthread_mutex_unlock(&(pool->lock));
    }

    // 清空工作线程的回调函数原来的参数arg
    if (pool->task_queue[pool->queue_rear].arg != NULL) {
        free(pool->task_queue[pool->queue_rear].arg);
        pool->task_queue[pool->queue_rear].arg = NULL;
    }

    pool->task_queue[pool->queue_rear].job_function = function;
    pool->task_queue[pool->queue_rear].arg = arg;
    pool->queue_rear = (pool->queue_rear+1) % pool->queue_max_size;
    pool->queue_size++;

    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));

    return 0;
}


int is_thread_alive(pthread_t tid) {
    // 0信号测试存活与否
    int status = pthread_kill(tid, 0);
    if (status == ESRCH) return false;
    return true;
}

int threadpool_all_threadnum(threadpool_t *pool) {
    int ret = -1;
    pthread_mutex_lock(&(pool->thread_counter));
    ret = pool->live_thread_num;
    pthread_mutex_unlock(&(pool->thread_counter));
    return ret;
}


int threadpool_busy_threadnum(threadpool_t *pool) {
    int busy_thread_num = -1;
    pthread_mutex_lock(&(pool->thread_counter));
    busy_thread_num = pool->busy_thread_num;
    pthread_mutex_unlock(&(pool->thread_counter));
    return busy_thread_num;
}


/*-----------------------test--------------------------*/
#if 1
void *WZP_RUNNER(void *arg) {
    int index = *(int*)arg;
    printf("WZP %d RUNNER in thread id :%lu \n", index, pthread_self());
    sleep(10);
    return NULL;
}

int main()
{
    threadpool_t *pool = threadpool_create(3, 100, 100);
    printf("pthread pool init...\n");
    int i;
    int args[20];
    for (i = 0; i < 20; ++i) {
        args[i] = i;
        threadpool_add(pool, WZP_RUNNER, (void*)&args[i]);
    }

    sleep(10);
    getchar();
    
    printf("\n\n\n\n");
    threadpool_destroy(pool);

    return 0;
}

#endif