#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>

#include <pthread.h>
#include <unistd.h>

// 封装一个抽象类
typedef struct {
    size_t size;
    void* (*ctor)(void *_self, va_list *params);
    void* (*dtor)(void *_self);
} AbstractClass;


/*

- 重要组件
  线程s
  任务s
  管理组件，保证互斥
*/

// 头插法
#define LL_ADD(item, list) do {             \
    item->prev = NULL;                      \
    item->next = list;                      \
    if (list != NULL) list->prev = item;    \
    list = item;                            \
} while (0)

// 删除任意节点
#define LL_REMOVE(item, list) do {                              \
    if (item->prev != NULL) item->prev->next = item->next;      \
    if (item->next != NULL) item->next->prev = item->prev;      \
    if (list == item) list = item->next;                        \
    item->prev = item->next = NULL;                             \
} while(0)

// 线程worker
typedef struct NWORKER {
    // 线程id
    pthread_t thread;
    // 判断是否退出
    int terminate;

    // 该工作线程在哪个线程中
    struct NWORKQUEUE *workqueue;

    // 双向链表
    struct NWORKER *next;
    struct NWORKER *prev;
} nWorker;

// 任务
typedef struct NJOB {
    // 回调函数
    void (*job_function)(struct NJOB *job);
    void *user_data;

    // 任务队列
    struct NJOB *prev;
    struct NJOB *next;
} nJob;

// 管理组件
typedef struct NWORKQUEUE {
    struct NWORKER *workers;
    struct NJOB *waiting_jobs;

    // 避免出现两个worker同时处理一个任务
    pthread_mutex_t jobs_mtx;
    // 条件变量，线程挂起，等待任务到来
    pthread_cond_t jobs_cond;
} nWorkQueue;

typedef nWorkQueue nThreadPool;

// 线程回调函数
static void *ntyWorkerThread(void* ptr) {
    nWorker *worker = (nWorker*) ptr;
    // 1.判断任务队列是否为空
    // 2.从任务队列中取出任务job
    // 3.执行job
    while (1) {
        // 先上锁
        pthread_mutex_lock(&worker->workqueue->jobs_mtx);

        // 1.判断任务队列是否为空
        while (worker->workqueue->waiting_jobs == NULL) {
            // 在线程池销毁的时候才会到达此处
            if (worker->terminate) break;
            // 条件变量是利用线程间共享的全局变量进行同步的一种机制，
            // 主要包括两个动作：一个线程等待"条件变量的条件成立"而挂起；
            // 另一个线程使"条件成立"（给出条件成立信号）。
            // 为了防止竞争，条件变量的使用总是和一个互斥锁结合在一起。
            // 进入的时候unlock——>worker->workqueue->jobs_mtx
            // 退出的时候lock——>worker->workqueue->jobs_mtx
            pthread_cond_wait(&worker->workqueue->jobs_cond, &worker->workqueue->jobs_mtx);
        }

        if (worker->terminate) {
            // 在线程池销毁的时候才会到达此处
            // 需要解锁
            pthread_mutex_unlock(&worker->workqueue->jobs_mtx);
            break;
        }

        // 2.从该工作线程所在的线程管理器中的
        //   任务队列中取出任务job，即从任务队列中移除一个
        nJob *job = worker->workqueue->waiting_jobs;
        if (job != NULL) {
            LL_REMOVE(job, worker->workqueue->waiting_jobs);
        }

        // 需要解锁
        pthread_mutex_unlock(&worker->workqueue->jobs_mtx);

        if (job == NULL) continue;

        // 调用工作回调函数
        job->job_function(job);
    }

    // 在线程池销毁的时候才会到达此处
    free(worker);
    pthread_exit(NULL);
}

// 对nThreadPool也即NWORKQUEUE中的每一项进行初始化即可
int ntyThreadPoolCreate(nThreadPool *workqueue, int numWorkers) {
    if (workqueue == NULL) return -1;
    if (numWorkers < 1) numWorkers = 1;
    memset(workqueue, 0, sizeof(nThreadPool));

    pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
    memcpy(&workqueue->jobs_mtx, &blank_mutex, sizeof(workqueue->jobs_mtx));

    pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
    memcpy(&workqueue->jobs_cond, &blank_cond, sizeof(workqueue->jobs_cond));

    int i = 0;
    for (i = 0; i < numWorkers; ++i) {
        nWorker *worker = (nWorker*)malloc(sizeof(nWorker));
        if (worker == NULL) {
            perror("malloc");
            return 1;
        }

        memset(worker, 0, sizeof(nWorker));
        // 将当前线程池赋予到各个工作线程的结构体中的workqueue变量中
        worker->workqueue = workqueue;

        int ret = pthread_create(&worker->thread, NULL, ntyWorkerThread, (void*)worker);
        if (ret) {
            perror("pthread_create");
            free(worker);
            return 1;
        }

        // 添加到worker的链表中(头插)
        LL_ADD(worker, worker->workqueue->workers);
    }

    return 0;
}


// 向线程池中添加任务
void ntyThreadPoolQueue(nThreadPool *workqueue, nJob *job) {
    pthread_mutex_lock(&workqueue->jobs_mtx);

    LL_ADD(job, workqueue->waiting_jobs);

    // 通过信号唤醒某些线程
    pthread_cond_signal(&workqueue->jobs_cond);
    pthread_mutex_unlock(&workqueue->jobs_mtx);
}

void ntyThreadPoolShutdown(nThreadPool *workqueue) {
    nWorker *worker = NULL;

    for (worker = workqueue->workers; worker != NULL; worker = worker->next) {
        worker->terminate = 1;
    }

    // 锁住
    pthread_mutex_lock(&workqueue->jobs_mtx);

    workqueue->workers = NULL;
    workqueue->waiting_jobs = NULL;

    // 广播唤醒
    pthread_cond_broadcast(&workqueue->jobs_cond);
    // 解锁
    pthread_mutex_unlock(&workqueue->jobs_mtx);
}

#if 0
// -----------------------------纯底层测试-------------------------
#define WZP_MAX_ThREAD      80
#define WZP_COUNTER_SIZE    1000

void wzp_counter(nJob *job) {
    int index = *(int*)job->user_data;

    printf("index: %d, selfid: %lu\n", index, pthread_self());

    free(job->user_data);
    free(job);
    sleep(1);
}

int main()
{
    nThreadPool pool;
    ntyThreadPoolCreate(&pool, WZP_MAX_ThREAD);

    int i = 0;
    for (i = 0; i < WZP_COUNTER_SIZE; ++i) {
        nJob *job = (nJob*)malloc(sizeof(nJob));
        if (job == NULL) {
            perror("malloc");
            exit(1);
        }

        job->job_function = wzp_counter;
        job->user_data = malloc(sizeof(int));
        *(int*)job->user_data = i;

        ntyThreadPoolQueue(&pool, job);
    }

    getchar();
    printf("\n\n\n\n");
}

#endif


// 封装接口

// 高层线程池结构体
typedef struct _ThreadPool {
    // 这个不知道是用来干嘛的?
    // 推测是为了跟_ThreadPoolOpera对齐，然后可以强制转换？
    // 因为_ThreadPoolOpera的一个元素是8字节的size_t，正好与下方8字节的指针抵消
    // 然后wq是nThreadPool类型的，然后_ThreadPoolOpera中的第二个元素是回调函数
    // 返回值似乎也是一个???

    // 最终在我的修改下可以弄掉了
    //const void *_;
    nThreadPool *wq;
} ThreadPool;

// 封装一个内含线程池各种信息的结构体——>线程池管理结构体大小、构造、析构和
// 添加任务的回调函数指针
typedef struct _ThreadPoolOpera {
    // 线程池大小
    size_t size;
    // VA_LIST 是在C语言中解决变参问题的一组宏，
    // 所在头文件：#include ，用于获取不确定个数的参数。
    // 构造
    void* (*ctor)(void *_self, va_list *params);
    // 析构
    void* (*dtor)(void *_self);
    // 添加任务
    void* (*addJob)(void *_self, void *task);
} ThreadPoolOpera;


// 封装的构造函数，即构建好对应的线程池管理器，即调用它的方法(里面会创建目标数的线程)
void *ntyThreadPoolCtor(void *_self, va_list *params) {
    ThreadPool *pool = (ThreadPool*)_self;

    // 分配内存给封装下的实际线程池管理变量(对象)
    pool->wq = (nThreadPool*)malloc(sizeof(nThreadPool));

    /*
    // 如果执行下面这些内容实际上会发生内存泄漏，因为在底层的线程池会直接将
    // 传入的pool->wq首先memset为0

    // 分配内存给该线程池管理下的工作队列
    pool->wq->workers = malloc(sizeof(nWorker));
    if (pool->wq->workers == NULL) {
        perror("malloc pool->wq->workers:");
        exit(1);
    }
    memset(pool->wq->workers, 0, sizeof(nWorker));
    // 分配内存给该线程池管理下的任务队列
    pool->wq->waiting_jobs = malloc(sizeof(nJob));
    if (pool->wq->workers == NULL) {
        perror("malloc pool->wq->waiting_jobs:");
        exit(1);
    }
    memset(pool->wq->waiting_jobs, 0, sizeof(nJob));*/

    int arg = va_arg(params, int);
    printf("ntyThreadPoolCtor --> %d\n", arg);
    ntyThreadPoolCreate(pool->wq, arg);

    return pool;
}

// 封装好的析构函数
void* ntyThreadPoolDtor(void *_self) {
    ThreadPool *pool = (ThreadPool*)_self;
    ntyThreadPoolShutdown(pool->wq);
    free(pool->wq);

    return pool;
}

// 封装好的添加任务函数
void* ntyThreadPoolAddJob(void *_self, void* task) {
    ThreadPool *pool = (ThreadPool*)_self;
    nJob *job = (nJob*)task;

    // 调用底层线程池的接口——>将job任务加入到线程池的工作队列中
    // (其中第一个参数就是线程池的管理件)
    ntyThreadPoolQueue(pool->wq, job);
}

// 将相关操作封装到变量ntyThreadPoolOpera中
const ThreadPoolOpera ntyThreadPoolOpera = {
    sizeof(ThreadPool),
    ntyThreadPoolCtor,
    ntyThreadPoolDtor,
    ntyThreadPoolAddJob,
};



// ---------------------上面是对线程池的接口封装----------------

// ---------------------下面是对上面封装的接口进行高层次逻辑的实现-----------------

// 进一步封装(似乎被用到的地方只有一个，以后再考虑重用问题吧)
const void *pNtyThreadPoolOpera = &ntyThreadPoolOpera;
// 线程池单例
static void *pThreadPool = NULL;


// 给单例创建程序使用——>创建一个线程池
void *New(const void *_class, ...) {
    // 使用一个抽象类来接收ThreadPoolOpera类型的变量，即线程池管理件相关参数
    const AbstractClass *class = _class;

    // 分配线程池管理件需要的内存空间
    void *p = calloc(1, class->size);
    memset(p, 0, class->size);

    assert(p);

    if (class->ctor) {
        va_list params;
        va_start(params, _class);
        // 构造内存池管理件
        p = class->ctor(p, &params);
        va_end(params);
    }

    // printf("p size %ld\n", sizeof(p));
    return p;
}

// 迷之操作——>需要使用一个结构体或联合体(union)来操作，因为void*的局限性
void Delete(void *_class) {
    const ThreadPoolOpera *pThreadPoolOpera = &ntyThreadPoolOpera;
    if (_class && pThreadPoolOpera) {
        pThreadPoolOpera->dtor(_class);
    }
    free(_class);
}


// 添加一个task到线程池中
int ntyThreadPoolPush(void *self, void *task) {
    const ThreadPoolOpera *pThreadPoolOpera = &ntyThreadPoolOpera;

    if (self && pThreadPoolOpera) {
        pThreadPoolOpera->addJob(self, task);
        return 0;
    }

    return 1;
}


// --------------下面是单例模式的奇怪&简陋实现------------------

// 简陋的单例初始化
void *ntyThreadPoolInstance(int nWorker) {
    if (pThreadPool == NULL) {
        pThreadPool = New(pNtyThreadPoolOpera, nWorker);
    }

    return pThreadPool;
}

// 简陋的单例获取
void* ntyGetInstance(void) {
    if (pThreadPool != NULL) return pThreadPool;
}

// 简陋的单例释放
void ntyThreadPoolRelease(void) {
    Delete(pThreadPool);
    pThreadPool = NULL;
}


// --------------------- 测试程序 ---------------------

#if 1

#define WZP_MAX_THREAD			20
#define WZP_COUNTER_SIZE		1000

// 工作函数
void wzp_counter(nJob *job) {

	int index = *(int*)job->user_data;

	printf("index : %d, selfid : %lu\n", index, pthread_self());
	
	free(job->user_data);
	free(job);
    
    /*// for test
    sleep(1);*/
    
}


int main(int argc, char *argv[]) {

    int numWorkers = WZP_MAX_THREAD;
    printf("1\n");
    void *pool = ntyThreadPoolInstance(numWorkers);

    int i = 0;
    for (i = 0;i < WZP_COUNTER_SIZE;i ++) {
		nJob *job = (nJob*)malloc(sizeof(nJob));
		if (job == NULL) {
			perror("malloc");
			exit(1);
		}
		
		job->job_function = wzp_counter;
		job->user_data = malloc(sizeof(int));
		*(int*)job->user_data = i;
		ntyThreadPoolPush(pool, job);
        
        /*// for test
        if (i % 100 == 0) {
            sleep(2);
        }*/
		
	}

	getchar();
	printf("\n");
    // 释放内存
    ntyThreadPoolRelease();
}

#endif
