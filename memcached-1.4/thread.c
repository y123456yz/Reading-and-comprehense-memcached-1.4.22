/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "memcached.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#ifdef __sun
#include <atomic.h>
#endif

#define ITEMS_PER_ALLOC 64

/* An item in the connection queue. */
//CQ_ITEM是主线程accept后返回的已建立连接的fd的封装。 最终全部存入LIBEVENT_THREAD->new_conn_queue
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item { //创建空间和赋值见dispatch_conn_new
    int               sfd; //客户端连接的fd
    enum conn_states  init_state;
    int               event_flags; //EV_READ | EV_PERSIST等
    int               read_buffer_size; //默认DATA_BUFFER_SIZE
    enum network_transport     transport; //tcp连接还是udp连接
    CQ_ITEM          *next;
};

/* A connection queue. */
typedef struct conn_queue CQ;
struct conn_queue {
    CQ_ITEM *head; //指向队列的第一个节点
    CQ_ITEM *tail; //指向队列的最后一个节点
    pthread_mutex_t lock; //一个队列就对应一个锁
};

/* Lock for cache operations (item_*, assoc_*) */
pthread_mutex_t cache_lock;

/* Connection lock around accepting new connections */
pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

#if !defined(HAVE_GCC_ATOMICS) && !defined(__sun)
pthread_mutex_t atomics_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Lock for global stats */
static pthread_mutex_t stats_lock;

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;

//可以参考http://blog.csdn.net/luotuo44/article/details/42913549
static pthread_mutex_t *item_locks; //初始化和赋值见thread_init
/* size of the item lock hash table */
static uint32_t item_lock_count;


static unsigned int item_lock_hashpower;
#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)
/* this lock is temporarily engaged during a hash table expansion */
static pthread_mutex_t item_global_lock;
/* thread-specific variable for deeply finding the item lock type */
static pthread_key_t item_lock_type_key; ////线程私有数据的键值    不同线程的修改互不干扰

static LIBEVENT_DISPATCHER_THREAD dispatcher_thread;

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */ //创建空间和赋值在thread_init
static LIBEVENT_THREAD *threads;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;


static void thread_libevent_process(int fd, short which, void *arg);

unsigned short refcount_incr(unsigned short *refcount) {
#ifdef HAVE_GCC_ATOMICS
    return __sync_add_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_inc_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)++;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

unsigned short refcount_decr(unsigned short *refcount) {
#ifdef HAVE_GCC_ATOMICS
    return __sync_sub_and_fetch(refcount, 1);
#elif defined(__sun)
    return atomic_dec_ushort_nv(refcount);
#else
    unsigned short res;
    mutex_lock(&atomics_mutex);
    (*refcount)--;
    res = *refcount;
    mutex_unlock(&atomics_mutex);
    return res;
#endif
}

/* Convenience functions for calling *only* when in ITEM_LOCK_GLOBAL mode */
void item_lock_global(void) {
    mutex_lock(&item_global_lock);
}

void item_unlock_global(void) {
    mutex_unlock(&item_global_lock);
}

void item_lock(uint32_t hv) {
    uint8_t *lock_type = pthread_getspecific(item_lock_type_key); //获取线程私有变量  
    //likely这个宏定义用于代码指令优化  
    //likely(*lock_type == ITEM_LOCK_GRANULAR)用来告诉编译器  
    //*lock_type等于ITEM_LOCK_GRANULAR的可能性很大  
    if (likely(*lock_type == ITEM_LOCK_GRANULAR)) {
        mutex_lock(&item_locks[hv & hashmask(item_lock_hashpower)]); //对某些桶的item加锁  
    } else {
        mutex_lock(&item_global_lock);//对所有item加锁  
    }
}

/* Special case. When ITEM_LOCK_GLOBAL mode is enabled, this should become a
 * no-op, as it's only called from within the item lock if necessary.
 * However, we can't mix a no-op and threads which are still synchronizing to
 * GLOBAL. So instead we just always try to lock. When in GLOBAL mode this
 * turns into an effective no-op. Threads re-synchronize after the power level
 * switch so it should stay safe.
 */
void *item_trylock(uint32_t hv) {
    pthread_mutex_t *lock = &item_locks[hv & hashmask(item_lock_hashpower)];
    if (pthread_mutex_trylock(lock) == 0) {
        return lock;
    }
    return NULL;
}

void item_trylock_unlock(void *lock) {
    mutex_unlock((pthread_mutex_t *) lock);
}

void item_unlock(uint32_t hv) {
    uint8_t *lock_type = pthread_getspecific(item_lock_type_key);
    if (likely(*lock_type == ITEM_LOCK_GRANULAR)) {
        mutex_unlock(&item_locks[hv & hashmask(item_lock_hashpower)]);
    } else {
        mutex_unlock(&item_global_lock);
    }
}

/*
即主线程阻塞如此，等待worker_libevent发出的init_cond信号，唤醒后检查init_count < nthreads是否为假
（即创建的线程数目是否达到要求），否则继续等待。
*/
static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

//结合wait_for_thread_registration使用，保证子线程先运行起来
static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}

/*
    迁移线程为什么要这么迂回曲折地切换workers线程的锁类型呢？直接修改所有线程的LIBEVENT_THREAD结构的item_lock_type
 成员变量不就行了吗？
    这主要是因为迁移线程不知道worker线程此刻在干些什么。如果worker线程正在访问item，并抢占了段级别锁。此时你把worker
 线程的锁切换到全局锁，等worker线程解锁的时候就会解全局锁(参考前面的item_lock和item_unlock代码)，这样程序就崩溃了。
 所以不能迁移线程去切换，只能迁移线程通知worker线程，然后worker线程自己去切换。当然是要worker线程忙完了手头上的事情
 后，才会去修改切换的。所以迁移线程在通知完所有的worker线程后，会调用wait_for_thread_registration函数休眠等待所有的
 worker线程都切换到指定的锁类型后才醒来。
*/

//工作子线程读管道有数据到来，thread_libevent_process从读管道读取到信息  
//对应的主线程写管道在dispatch_conn_new或者switch_item_lock_type

//哈希表迁移线程会在assoc.c文件中的assoc_maintenance_thread函数调用switch_item_lock_type函数，让所有的
//workers线程都切换到段级别锁或者全局级别锁
void switch_item_lock_type(enum item_lock_types type) { //函数thread_libevent_process接收l 或者g信息
    char buf[1];
    int i;

    switch (type) {
        case ITEM_LOCK_GRANULAR:
            buf[0] = 'l';//用l表示ITEM_LOCK_GRANULAR 段级别锁  
            break;
        case ITEM_LOCK_GLOBAL:
            buf[0] = 'g';//用g表示ITEM_LOCK_GLOBAL 全局级别锁  
            break;
        default: //通过向worker监听的管道写入一个字符通知worker线程  
            fprintf(stderr, "Unknown lock type: %d\n", type);
            assert(1 == 0);
            break;
    }

    pthread_mutex_lock(&init_lock);
    init_count = 0;
    for (i = 0; i < settings.num_threads; i++) {
        if (write(threads[i].notify_send_fd, buf, 1) != 1) {
            perror("Failed writing to notify pipe");
            /* TODO: This is a fatal problem. Can it ever happen temporarily? */
        }
    }
    //等待所有的workers线程都把锁切换到type指明的锁类型  
    wait_for_thread_registration(settings.num_threads);
    pthread_mutex_unlock(&init_lock);
}

/*
 * Initializes a connection queue.
 */
static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

/*
 * Looks for an item on a connection queue, but doesn't block if there isn't
 * one.
 * Returns the item, or NULL if no item is available
 */
static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/*
 * Adds an item to a connection queue.
 */
static void cq_push(CQ *cq, CQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}

/*
 * Returns a fresh connection queue item.
 */
 //本函数采用了一些优化手段，并非每调用一次本函数就申请一块内存。这会导致
 //内存碎片。这里采取的优化方法是，一次性分配64个CQ_ITEM大小的内存(即预分配)
 //下次调用本函数的时候，直接从之前分配64个中要一个即可。
 //由于是为了防止内存碎片，所以不是以链表的形式存放着64个CQ_ITEM。而是数组的形式
 //于是， cqi_free函数就有点特别，它并不真正释放，而是像内存池那样归还
static CQ_ITEM *cqi_new(void) {//CQ_ITEM是主线程accept后返回的已建立连接的fd的封装。
    CQ_ITEM *item = NULL;
	//所有线程都会访问cqi_freelist，所以需要加锁
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist) {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

	//没有多余的CQ_ITEM了
    if (NULL == item) {
        int i;

        /* Allocate a bunch of items at once to reduce fragmentation */
        item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return NULL;
        }

        /*
         * Link together all the new items except the first one
         * (which we'll return to the caller) for placement on
         * the freelist.
         */
         //item[0]直接返回为调用者，不用next指针连在一起。调用者负责将
         //item[0].next赋值为NULL
       	//将这块内存分成一个个的item并且用next指针像链表一样连起来
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
		//因为主线程负责申请CQ_ITEM，子线程负责释放CQ_ITEM。所以cqi_freelist此刻
		//可能并不等于NULL。由于使用头插法，所以无论cqi_freelist是否为NULL，都能把链表连起来的
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}


/*
 * Frees a connection queue item (adds it to the freelist.)
 */
 //并非释放，而是像内存池那样归还
static void cqi_free(CQ_ITEM *item) {
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item; //头插法归还
    pthread_mutex_unlock(&cqi_freelist_lock);
}


/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
}

/*
 * Sets whether or not we accept new connections.
 */
void accept_new_conns(const bool do_accept) {
    pthread_mutex_lock(&conn_lock);
    do_accept_new_conns(do_accept);
    pthread_mutex_unlock(&conn_lock);
}
/****************************** LIBEVENT THREADS *****************************/

/*
 用户线程使用libevent则通常按以下步骤：
 1）用户线程通过event_init()函数创建一个event_base对象。event_base对象管理所有注册到自己内部的IO事件。
 多线程环境下，event_base对象不能被多个线程共享，即一个event_base对象只能对应一个线程。
 2）然后该线程通过event_add函数，将与自己感兴趣的文件描述符相关的IO事件，注册到event_base对象，同时指
 定事件发生时所要调用的事件处理函数（event handler）。服务器程序通常监听套接字（socket）的可读事件。
 比如，服务器线程注册套接字sock1的EV_READ事件，并指定event_handler1()为该事件的回调函数。libevent将
 IO事件封装成struct event类型对象，事件类型用EV_READ/EV_WRITE等常量标志。
 3） 注册完事件之后，线程调用event_base_loop进入循环监听（monitor）状态。该循环内部会调用epoll等IO复
 用函数进入阻塞状态，直到描述符上发生自己感兴趣的事件。此时，线程会调用事先指定的回调函数处理该事件。
 例如，当套接字sock1发生可读事件，即sock1的内核buff中已有可读数据时，被阻塞的线程立即返回（wake up）
 并调用event_handler1()函数来处理该次事件。
 4）处理完这次监听获得的事件后，线程再次进入阻塞状态并监听，直到下次事件发生。


 * Set up a thread's information.
 */
static void setup_thread(LIBEVENT_THREAD *me) {
	//新建一个event_base
    me->base = event_init();
    if (! me->base) {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
	//监听管道的读端
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);
	//将event_base和event想关联
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }
	// 创建一个CQ队列
    me->new_conn_queue = malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL) {
        perror("Failed to allocate memory for connection queue");
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }

    me->suffix_cache = cache_create("suffix", SUFFIX_SIZE, sizeof(char*),
                                    NULL, NULL);
    if (me->suffix_cache == NULL) {
        fprintf(stderr, "Failed to create suffix cache\n");
        exit(EXIT_FAILURE);
    }
}

/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = arg;

    /* Any per-thread setup can happen here; thread_init() will block until
     * all threads have finished initializing.
     */

    /* set an indexable thread-specific memory item for the lock type.
     * this could be unnecessary if we pass the conn *c struct through
     * all item_lock calls...
     */
    me->item_lock_type = ITEM_LOCK_GRANULAR;//初试状态使用段级别锁  
    //为workers线程设置线程私有数据  
    //因为所有的workers线程都会调用这个函数，所以所有的workers线程都设置了相同键值的  
    //线程私有数据  
    pthread_setspecific(item_lock_type_key, &me->item_lock_type);

    register_thread_initialized();

    event_base_loop(me->base, 0); //等待事件到来触发setup_thread中的thread_libevent_process执行
    return NULL;
}


/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */ 
//thread_libevent_process这个管道事件回调使用于主线程接受到客户端连接后通知工作子线程重新创建一个新的
//conn，在conn_new重新设置网络事件回调函数conn_new->event_handler
 
 //工作子线程读管道有数据到来，thread_libevent_process从读管道读取到信息  
 //对应的主线程写管道在dispatch_conn_new或者switch_item_lock_type
static void thread_libevent_process(int fd, short which, void *arg) {
    LIBEVENT_THREAD *me = arg;
    CQ_ITEM *item;
    char buf[1];

    if (read(fd, buf, 1) != 1)
        if (settings.verbose > 0)
            fprintf(stderr, "Can't read from libevent pipe\n");

    switch (buf[0]) {
    case 'c': //dispatch_conn_new
	//从CQ队列中读取一个item，因为是pop所以读取后，CQ队列会把这个item从队列中删除
    item = cq_pop(me->new_conn_queue);

    if (NULL != item) {
		//为sfd分配一个conn结构体，并且为这个sfd建立一个event，然后让base监听这个event
		//这个sfd的事件回调函数是event_handler
        conn *c = conn_new(item->sfd, item->init_state, item->event_flags,
                           item->read_buffer_size, item->transport, me->base);
        if (c == NULL) {
            if (IS_UDP(item->transport)) {
                fprintf(stderr, "Can't listen for events on UDP socket\n");
                exit(1);
            } else {
                if (settings.verbose > 0) {
                    fprintf(stderr, "Can't listen for events on fd %d\n",
                        item->sfd);
                }
                close(item->sfd);
            }
        } else {
            c->thread = me;
        }
        cqi_free(item);
    }
        break;
    //switch_item_lock_type触发走到这里
    /* we were told to flip the lock type and report in */
    case 'l': //参考switch_item_lock_type //切换item到段级别  
    //唤醒睡眠在init_cond条件变量上的迁移线程  
    me->item_lock_type = ITEM_LOCK_GRANULAR;
    register_thread_initialized();
        break;
    case 'g'://切换item锁到全局级别  
    me->item_lock_type = ITEM_LOCK_GLOBAL;
    register_thread_initialized();
        break;
    }
}

/* Which thread we assigned a connection to most recently. */
static int last_thread = -1; 

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */

/*
每个线程都是一个单独的libevent实例,主线程eventloop负责处理监听fd，监听客户端的建立连接请求，以及
accept连接，将已建立的连接round robin到各个worker。workers线程负责处理已经建立好的连接的读写等事件
*/

 
//工作子线程读管道有数据到来，thread_libevent_process从读管道读取到信息  
//对应的主线程写管道在dispatch_conn_new或者switch_item_lock_type

//主线程检查到有新的链接到来，则通过管道通知子线程
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                       int read_buffer_size, enum network_transport transport) {
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL) {
        close(sfd);
        /* given that malloc failed this may also fail, but let's try */
        fprintf(stderr, "Failed to allocate memory for connection object\n");
        return ;
    }
	//轮询的方式选定一个worker线程
    int tid = (last_thread + 1) % settings.num_threads;

    LIBEVENT_THREAD *thread = threads + tid; //轮询选择由那个工作现场来处理

    last_thread = tid;

    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;
    item->transport = transport;
	//把这个item放到选定的worker线程的CQ队列中
    cq_push(thread->new_conn_queue, item);

    MEMCACHED_CONN_DISPATCH(sfd, thread->thread_id);
    buf[0] = 'c';
	// 通知worker线程，有新客户单连接到来
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
}

/*
 * Returns true if this is the thread that listens for new TCP connections.
 */
int is_listen_thread() {
    return pthread_self() == dispatcher_thread.thread_id;
}

/********************************* ITEM ACCESS *******************************/

/*
 * Allocates a new item.
 */
item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes) {
    item *it;
    /* do_item_alloc handles its own locks */
    it = do_item_alloc(key, nkey, flags, exptime, nbytes, 0);
    return it;
}

/*
 * Returns an item if it hasn't been marked as expired,
 * lazy-expiring as needed.
 */
item *item_get(const char *key, const size_t nkey) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_get(key, nkey, hv);
    item_unlock(hv);
    return it;
}

item *item_touch(const char *key, size_t nkey, uint32_t exptime) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_touch(key, nkey, exptime, hv);
    item_unlock(hv);
    return it;
}

/*
 * Links an item into the LRU and hashtable.
 */
int item_link(item *item) {
    int ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_item_link(item, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Decrements the reference count on an item and adds it to the freelist if
 * needed.
 */
void item_remove(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_remove(item);
    item_unlock(hv);
}

/*
 * Replaces one item with another in the hashtable.
 * Unprotected by a mutex lock since the core server does not require
 * it to be thread-safe.
 *///把旧的删除，插入新的。replace命令会调用本函数.  
//无论旧item是否有其他worker线程在引用，都是直接将之从哈希表和LRU队列中删除  
int item_replace(item *old_it, item *new_it, const uint32_t hv) { //这里面会unlink old_it,然后link new_it
    return do_item_replace(old_it, new_it, hv); //注意这里面old_it->refcount会减1，new_it会增1
}

/*
 * Unlinks an item from the LRU and hashtable.
 */ //取消item和LRU与hashtable的关联
void item_unlink(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    do_item_unlink(item, hv);
    item_unlock(hv);
}

/*
 * Moves an item to the back of the LRU queue.
 */
void item_update(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_update(item);
    item_unlock(hv);
}

/*
 * Does arithmetic on a numeric item value.
 */ //inc和dec命令处理
enum delta_result_type add_delta(conn *c, const char *key,
                                 const size_t nkey, int incr,
                                 const int64_t delta, char *buf,
                                 uint64_t *cas) {
    enum delta_result_type ret;
    uint32_t hv;

    hv = hash(key, nkey);
    item_lock(hv);
    ret = do_add_delta(c, key, nkey, incr, delta, buf, cas, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Stores an item in the cache (high level, obeys set/add/replace semantics)
 */
enum store_item_type store_item(item *item, int comm, conn* c) { //注意该函数外层在该函数执行完后一般会调用一次item_remove
    enum store_item_type ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_store_item(item, comm, c, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Flushes expired items after a flush_all call
 */
void item_flush_expired() {
    mutex_lock(&cache_lock);
    do_item_flush_expired();
    mutex_unlock(&cache_lock);
}

/*
 stats cachedump 4 100
ITEM foo_rand112722700000 [100 b; 1462575443 s]
ITEM foo_rand841447400000 [100 b; 1462575443 s]
ITEM foo_rand708137200000 [100 b; 1462575443 s]
ITEM foo_rand435945600000 [100 b; 1462575443 s]
ITEM foo_rand708670400000 [100 b; 1462575443 s]
ITEM foo_rand606717400000 [100 b; 1462575443 s]
ITEM foo_rand000000000000 [100 b; 1462575443 s]
ITEM foo_rand718332000000 [100 b; 1462575443 s]
ITEM foo_rand264939300000 [100 b; 1462575443 s]
ITEM foo_rand966316900000 [100 b; 1462575443 s]
END
stats cachedump 5 100
END
stats cachedump 4 1
ITEM foo_rand112722700000 [100 b; 1462575443 s]
END
stats cachedump 4 20
ITEM foo_rand112722700000 [100 b; 1462575443 s]
ITEM foo_rand841447400000 [100 b; 1462575443 s]
ITEM foo_rand708137200000 [100 b; 1462575443 s]
ITEM foo_rand435945600000 [100 b; 1462575443 s]
ITEM foo_rand708670400000 [100 b; 1462575443 s]
ITEM foo_rand606717400000 [100 b; 1462575443 s]
ITEM foo_rand000000000000 [100 b; 1462575443 s]
ITEM foo_rand718332000000 [100 b; 1462575443 s]
ITEM foo_rand264939300000 [100 b; 1462575443 s]
ITEM foo_rand966316900000 [100 b; 1462575443 s]
END

ERROR
set yang 0 0 100 xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
STORED
stats cachedump 4 100
ITEM yang [100 b; 1462575443 s]
ITEM foo_rand112722700000 [100 b; 1462575443 s]
ITEM foo_rand841447400000 [100 b; 1462575443 s]
ITEM foo_rand708137200000 [100 b; 1462575443 s]
ITEM foo_rand435945600000 [100 b; 1462575443 s]
ITEM foo_rand708670400000 [100 b; 1462575443 s]
ITEM foo_rand606717400000 [100 b; 1462575443 s]
ITEM foo_rand000000000000 [100 b; 1462575443 s]
ITEM foo_rand718332000000 [100 b; 1462575443 s]
ITEM foo_rand264939300000 [100 b; 1462575443 s]
ITEM foo_rand966316900000 [100 b; 1462575443 s]
*/
/*
 * Dumps part of the cache
 */
char *item_cachedump(unsigned int slabs_clsid, unsigned int limit, unsigned int *bytes) {
    char *ret;

    mutex_lock(&cache_lock);
    ret = do_item_cachedump(slabs_clsid, limit, bytes);
    mutex_unlock(&cache_lock);
    return ret;
}

/*
 * Dumps statistics about slab classes
 */
void  item_stats(ADD_STAT add_stats, void *c) {
    mutex_lock(&cache_lock);
    do_item_stats(add_stats, c);
    mutex_unlock(&cache_lock);
}

void  item_stats_totals(ADD_STAT add_stats, void *c) {
    mutex_lock(&cache_lock);
    do_item_stats_totals(add_stats, c);
    mutex_unlock(&cache_lock);
}

/*
 * Dumps a list of objects of each size in 32-byte increments
 */
void  item_stats_sizes(ADD_STAT add_stats, void *c) {
    mutex_lock(&cache_lock);
    do_item_stats_sizes(add_stats, c);
    mutex_unlock(&cache_lock);
}

/******************************* GLOBAL STATS ******************************/

void STATS_LOCK() {
    pthread_mutex_lock(&stats_lock);
}

void STATS_UNLOCK() {
    pthread_mutex_unlock(&stats_lock);
}

void threadlocal_stats_reset(void) {
    int ii, sid;
    for (ii = 0; ii < settings.num_threads; ++ii) {
        pthread_mutex_lock(&threads[ii].stats.mutex);

        threads[ii].stats.get_cmds = 0;
        threads[ii].stats.get_misses = 0;
        threads[ii].stats.touch_cmds = 0;
        threads[ii].stats.touch_misses = 0;
        threads[ii].stats.delete_misses = 0;
        threads[ii].stats.incr_misses = 0;
        threads[ii].stats.decr_misses = 0;
        threads[ii].stats.cas_misses = 0;
        threads[ii].stats.bytes_read = 0;
        threads[ii].stats.bytes_written = 0;
        threads[ii].stats.flush_cmds = 0;
        threads[ii].stats.conn_yields = 0;
        threads[ii].stats.auth_cmds = 0;
        threads[ii].stats.auth_errors = 0;

        for(sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
            threads[ii].stats.slab_stats[sid].set_cmds = 0;
            threads[ii].stats.slab_stats[sid].get_hits = 0;
            threads[ii].stats.slab_stats[sid].touch_hits = 0;
            threads[ii].stats.slab_stats[sid].delete_hits = 0;
            threads[ii].stats.slab_stats[sid].incr_hits = 0;
            threads[ii].stats.slab_stats[sid].decr_hits = 0;
            threads[ii].stats.slab_stats[sid].cas_hits = 0;
            threads[ii].stats.slab_stats[sid].cas_badval = 0;
        }

        pthread_mutex_unlock(&threads[ii].stats.mutex);
    }
}

void threadlocal_stats_aggregate(struct thread_stats *stats) {
    int ii, sid;

    /* The struct has a mutex, but we can safely set the whole thing
     * to zero since it is unused when aggregating. */
    memset(stats, 0, sizeof(*stats));

    for (ii = 0; ii < settings.num_threads; ++ii) {
        pthread_mutex_lock(&threads[ii].stats.mutex);

        stats->get_cmds += threads[ii].stats.get_cmds;
        stats->get_misses += threads[ii].stats.get_misses;
        stats->touch_cmds += threads[ii].stats.touch_cmds;
        stats->touch_misses += threads[ii].stats.touch_misses;
        stats->delete_misses += threads[ii].stats.delete_misses;
        stats->decr_misses += threads[ii].stats.decr_misses;
        stats->incr_misses += threads[ii].stats.incr_misses;
        stats->cas_misses += threads[ii].stats.cas_misses;
        stats->bytes_read += threads[ii].stats.bytes_read;
        stats->bytes_written += threads[ii].stats.bytes_written;
        stats->flush_cmds += threads[ii].stats.flush_cmds;
        stats->conn_yields += threads[ii].stats.conn_yields;
        stats->auth_cmds += threads[ii].stats.auth_cmds;
        stats->auth_errors += threads[ii].stats.auth_errors;

        for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
            stats->slab_stats[sid].set_cmds +=
                threads[ii].stats.slab_stats[sid].set_cmds;
            stats->slab_stats[sid].get_hits +=
                threads[ii].stats.slab_stats[sid].get_hits;
            stats->slab_stats[sid].touch_hits +=
                threads[ii].stats.slab_stats[sid].touch_hits;
            stats->slab_stats[sid].delete_hits +=
                threads[ii].stats.slab_stats[sid].delete_hits;
            stats->slab_stats[sid].decr_hits +=
                threads[ii].stats.slab_stats[sid].decr_hits;
            stats->slab_stats[sid].incr_hits +=
                threads[ii].stats.slab_stats[sid].incr_hits;
            stats->slab_stats[sid].cas_hits +=
                threads[ii].stats.slab_stats[sid].cas_hits;
            stats->slab_stats[sid].cas_badval +=
                threads[ii].stats.slab_stats[sid].cas_badval;
        }

        pthread_mutex_unlock(&threads[ii].stats.mutex);
    }
}

void slab_stats_aggregate(struct thread_stats *stats, struct slab_stats *out) {
    int sid;

    out->set_cmds = 0;
    out->get_hits = 0;
    out->touch_hits = 0;
    out->delete_hits = 0;
    out->incr_hits = 0;
    out->decr_hits = 0;
    out->cas_hits = 0;
    out->cas_badval = 0;

    for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
        out->set_cmds += stats->slab_stats[sid].set_cmds;
        out->get_hits += stats->slab_stats[sid].get_hits;
        out->touch_hits += stats->slab_stats[sid].touch_hits;
        out->delete_hits += stats->slab_stats[sid].delete_hits;
        out->decr_hits += stats->slab_stats[sid].decr_hits;
        out->incr_hits += stats->slab_stats[sid].incr_hits;
        out->cas_hits += stats->slab_stats[sid].cas_hits;
        out->cas_badval += stats->slab_stats[sid].cas_badval;
    }
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
 //参数nthread是woker线程的数量。main_base则是主线程的event_base
 //主线程在main函数调用本函数，创建nthreads个worker线程
void thread_init(int nthreads, struct event_base *main_base) {
    int         i;
    int         power;

	//申请一个CQ_ITEM时需要加锁
    pthread_mutex_init(&cache_lock, NULL);
    pthread_mutex_init(&stats_lock, NULL);

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    /* Want a wide lock table, but don't waste memory */
    if (nthreads < 3) {
        power = 10;
    } else if (nthreads < 4) {
        power = 11;
    } else if (nthreads < 5) {
        power = 12;
    } else {
        /* 8192 buckets, and central locks don't scale much past 5 threads */
        power = 13;
    }

    item_lock_count = hashsize(power);
    item_lock_hashpower = power;

    //哈希表中段级别的锁。并不是一个桶就对应有一个锁。而是多个桶共用一个锁  
    item_locks = calloc(item_lock_count, sizeof(pthread_mutex_t));
    if (! item_locks) {
        perror("Can't allocate item locks");
        exit(1);
    }
    for (i = 0; i < item_lock_count; i++) {
        pthread_mutex_init(&item_locks[i], NULL);
    }
    pthread_key_create(&item_lock_type_key, NULL);
    pthread_mutex_init(&item_global_lock, NULL);

	//申请具有nthreads个元素的LIBEVENT_THREAD数据
    threads = calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (! threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    //主线程对应的
    dispatcher_thread.base = main_base;
    dispatcher_thread.thread_id = pthread_self();

    //子线程的
    for (i = 0; i < nthreads; i++) {
        int fds[2];
		//为每个worker线程分配一个管道，用于通知worker线程
        if (pipe(fds)) {
            perror("Can't create notify pipe");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];
		//每一个线程配一个event_base，并设置event监听notify_receive_fd的读事件
		//同时还为这个线程分配一个conn_queue队列
        setup_thread(&threads[i]);
        /* Reserve three fds for the libevent base, and two for the pipe */
        stats.reserved_fds += 5;
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++) {
		//创建线程，线程函数为worker_libevent，线程参数为&thread[i]
        create_worker(worker_libevent, &threads[i]);
    }
    
    /* Wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads); //主线程阻塞等待事件到来
    pthread_mutex_unlock(&init_lock);
}

