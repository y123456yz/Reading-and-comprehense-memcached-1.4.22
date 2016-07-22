/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

/* Forward Declarations */
static void item_link_q(item *it);
static void item_unlink_q(item *it);

#define HOT_LRU 0
#define WARM_LRU 64
#define COLD_LRU 128
#define NOEXP_LRU 192
static unsigned int lru_type_map[4] = {HOT_LRU, WARM_LRU, COLD_LRU, NOEXP_LRU};

#define CLEAR_LRU(id) (id & ~(3<<6))

#define LARGEST_ID POWER_LARGEST
/*
automove线程要进行自动检测，检测就需要一些实时数据进行分析。然后得出结论：哪个slabclass_t需要更多的内存，
哪个又不需要。automove线程通过全局变量itemstats收集item的各种数据。
itemstats变量是一个数组，它是和slabclass数组一一对应的。itemstats数组的元素负责收集slabclass数组中对应元素的信息。
itemstats_t结构体虽然提供了很多成员，可以收集很多信息，但automove线程只用到第一个成员evicted
*/
typedef struct { //结构属于itemstats[]
    uint64_t evicted; //因为LRU踢了多少个item 
    //即使一个item的exptime设置为0，也是会被踢的  
    uint64_t evicted_nonzero;
    uint64_t reclaimed; //item被重复使用的次数，赋值见do_item_alloc  //在申请item时，发现过期并回收的item数量  
    uint64_t outofmemory; //从slab中获取mem失败的次数，见do_item_alloc  //为item申请内存，失败的次数  
    uint64_t tailrepairs; //需要修复的item数量(除非worker线程有问题否则一般为0)  
    uint64_t expired_unfetched; //被访问过并且超时的item,赋值见do_item_alloc   //直到被超时删除时都还没被访问过的item数量  
    uint64_t evicted_unfetched; //直到被LRU踢出时都还没有被访问过的item数量  
    uint64_t crawler_reclaimed; //该slabclass保存的item数  //被LRU爬虫发现的过期item数量  
    uint64_t crawler_items_checked;
    uint64_t lrutail_reflocked;//申请item而搜索LRU队列时，被其他worker线程引用的item数量  
    uint64_t moves_to_cold;
    uint64_t moves_to_warm;
    uint64_t moves_within_lru;
    uint64_t direct_reclaims;
    rel_time_t evicted_time;
} itemstats_t; //item的状态统计信息，这里就不分析了

typedef struct {
    uint64_t histo[61];
    uint64_t ttl_hourplus;
    uint64_t noexp;
    uint64_t reclaimed;
    uint64_t seen;
    rel_time_t start_time;
    rel_time_t end_time;
    bool run_complete;
} crawlerstats_t;

//指向每一个LRU队列头  LRU队列可以参考http://blog.csdn.net/luotuo44/article/details/42869325
//排在队列前面的表示最近被访问的key，排在head后面的则表示最近没访问该key，因为每次访问key都会把key-value对应的item取出来放到head头部，见do_item_update
//lru队列里面的item是根据time降序排序的
//LRU真正的淘汰机制实现过程在do_item_alloc->it = slabs_alloc(ntotal, id)) == NULL 既没有过期的item，获取新item又失败了，则进行LRU淘汰机制
static item *heads[LARGEST_ID]; //LRU链首指针,  每个classid一个LRU链
//指向每一个LRU队列尾  
static item *tails[LARGEST_ID]; //LRU链尾指针，每个classid一个LRU链 
//crawlers是一个伪item类型数组。如果用户要清理某一个LRU队列，那么就在这个LRU队列中插入一个伪item  
static crawler crawlers[LARGEST_ID]; //lru_crawler_crawl
static itemstats_t itemstats[LARGEST_ID];
//每一个LRU队列有多少个item
static unsigned int sizes[LARGEST_ID];//每个classid的LRU链的长度(item个数）
static crawlerstats_t crawlerstats[MAX_NUMBER_OF_SLAB_CLASSES];

static int crawler_count = 0; //本次任务要处理多少个LRU队列  
static volatile int do_run_lru_crawler_thread = 0; //为1标识已经启用爬虫线程
static volatile int do_run_lru_maintainer_thread = 0;
static int lru_crawler_initialized = 0;//表示是否初始化lru爬虫线程锁和lru爬虫线程条件变量
static int lru_maintainer_initialized = 0;
static int lru_maintainer_check_clsid = 0;
static pthread_mutex_t lru_crawler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  lru_crawler_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t lru_crawler_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lru_maintainer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITIALIZER;

void item_stats_reset(void) {
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        pthread_mutex_lock(&lru_locks[i]);
        memset(&itemstats[i], 0, sizeof(itemstats_t));
        pthread_mutex_unlock(&lru_locks[i]);
    }
}

static int lru_pull_tail(const int orig_id, const int cur_lru,
        const unsigned int total_chunks, const bool do_evict, const uint32_t cur_hv);
static int lru_crawler_start(uint32_t id, uint32_t remaining);

/* Get the next CAS id for a new item. */
/* TODO: refactor some atomics for this. */
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    pthread_mutex_lock(&cas_id_lock);
    uint64_t next_id = ++cas_id;
    pthread_mutex_unlock(&cas_id_lock);
    return next_id;
}

int item_is_flushed(item *it) {
    rel_time_t oldest_live = settings.oldest_live;
    uint64_t cas = ITEM_get_cas(it);
    uint64_t oldest_cas = settings.oldest_cas;
    if (oldest_live == 0 || oldest_live > current_time)
        return 0;
    if ((it->time <= oldest_live)
            || (oldest_cas != 0 && cas != 0 && cas < oldest_cas)) {
        return 1;
    }
    return 0;
}

static unsigned int noexp_lru_size(int slabs_clsid) {
    int id = CLEAR_LRU(slabs_clsid);
    id |= NOEXP_LRU;
    unsigned int ret;
    pthread_mutex_lock(&lru_locks[id]);
    ret = sizes[id];
    pthread_mutex_unlock(&lru_locks[id]);
    return ret;
}

/* Enable this for reference-count debugging. */
#if 0
# define DEBUG_REFCNT(it,op) \
                fprintf(stderr, "item %x refcnt(%c) %d %c%c%c\n", \
                        it, op, it->refcount, \
                        (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                        (it->it_flags & ITEM_SLABBED) ? 'S' : ' ')
#else
# define DEBUG_REFCNT(it,op) while(0)
#endif

/**
 * Generates the variable-sized part of the header for an object.
 *
 * key     - The key
 * nkey    - The length of the key
 * flags   - key flags
 * nbytes  - Number of bytes to hold value and addition CRLF terminator
 * suffix  - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
static size_t item_make_header(const uint8_t nkey, const int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    /* suffix is defined at 40 chars elsewhere.. */
    *nsuffix = (uint8_t) snprintf(suffix, 40, " %d %d\r\n", flags, nbytes - 2);
    //snpintf后，*nsuffic大小应该就是上面""中的字节数
    return sizeof(item) + nkey + *nsuffix + nbytes;
}

/*
当worker线程接收到flush_all命令后，会用全局变量settings的oldest_live成员存储接收到这个命令那一刻的时间(准确地说，
是worker线程解析得知这是一个flush_all命令那一刻再减一)，代码为settings.oldest_live =current_time - 1;然后调用
item_flush_expired函数锁上cache_lock，然后调用do_item_flush_expired函数完成工作。
    惰性删除可以参考do_item_get中的if (settings.oldest_live != 0 && settings.oldest_live <= current_time int i;
do_item_get函数外，do_item_alloc函数也是会处理过期失效item的
*/
//真正存储在slabclass[id]中的tunck中的数据格式见item_make_header
/*@null@*/
//key、flags、exptime三个参数是用户在使用set、add命令存储一条数据时输入的参数
//nkey是key字符串的长度。nbytes则是用户要存储的data长度+2，因为在data结尾处还要加上"\r\n"
//cur_hv则是根据键值key计算得到的哈希值
item *do_item_alloc(char *key, const size_t nkey, const int flags,
                    const rel_time_t exptime, const int nbytes,
                    const uint32_t cur_hv) { //memcached异常相关的打印都包含字符串SERVER_ERROR ,如果是客户端命令等异常一般包含CLIENT_ERROR
    int i;
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];
    unsigned int total_chunks;//要存储这个item需要的总空间
    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

	//根据大小判断从属于哪个slab
    unsigned int id = slabs_clsid(ntotal);
    if (id == 0) //0表示不属于任何一个slab  slabclass是从1开始的
        return 0;

    /* If no memory is available, attempt a direct LRU juggle/eviction */
    /* This is a race in order to simplify lru_pull_tail; in cases where
	//第一次看这个for循环，直接认为search等于NULL，直接看for循环后面的代码
	//这个循环里面会在对应LRU队列中查找过期失效的item，最多尝试tries个item。
	//从LRU的队尾开始尝试。如果item被其他worker线程引用了，那么就尝试下一个
	//如果没有的被其他worker线程所引用，那么就测试该item是否过期失效
	//如果过期失效了，那么就可以使用这个item(最终会返回这个item)。如果没有
	//过期失效，那么不再尝试其他item了(因为是从LRU队列的队尾开始尝试的,对尾的都没有失效，其他前面的肯定不会失效)，
	//直接调用slabs_alloc申请一个新的内存存储item。如果申请新内存都失败，
	//那么在允许LRU淘汰的情况下就会启动踢人机制
     * locked items are on the tail, you want them to fall out and cause
     * occasional OOM's, rather than internally work around them.
     * This also gives one fewer code path for slab alloc/free
     */
    for (i = 0; i < 5; i++) {
        /* Try to reclaim memory first */
        if (!settings.lru_maintainer_thread) {
            lru_pull_tail(id, COLD_LRU, 0, false, cur_hv);
        }
        it = slabs_alloc(ntotal, id, &total_chunks, 0);
        if (settings.expirezero_does_not_evict)
            total_chunks -= noexp_lru_size(id);
        if (it == NULL) {
            if (settings.lru_maintainer_thread) {
                lru_pull_tail(id, HOT_LRU, total_chunks, false, cur_hv);
                lru_pull_tail(id, WARM_LRU, total_chunks, false, cur_hv);
                lru_pull_tail(id, COLD_LRU, total_chunks, true, cur_hv);
            } else {
                lru_pull_tail(id, COLD_LRU, 0, true, cur_hv);
            }
        } else {
            break;
        }
    }

    if (i > 0) {
        pthread_mutex_lock(&lru_locks[id]);
        itemstats[id].direct_reclaims += i;
        pthread_mutex_unlock(&lru_locks[id]);
    }

    if (it == NULL) {
        pthread_mutex_lock(&lru_locks[id]);
        itemstats[id].outofmemory++;
        pthread_mutex_unlock(&lru_locks[id]);
        return NULL;
    }

    assert(it->slabs_clsid == 0);
    //assert(it != heads[id]);

    /* Refcount is seeded to 1 by slabs_alloc() */
    it->next = it->prev = it->h_next = 0;
    /* Items are initially loaded into the HOT_LRU. This is '0' but I want at
     * least a note here. Compiler (hopefully?) optimizes this out.
     */
    if (settings.lru_maintainer_thread) {
        if (exptime == 0 && settings.expirezero_does_not_evict) {
            id |= NOEXP_LRU;
        } else {
            id |= HOT_LRU;
        }
    } else {
        /* There is only COLD in compat-mode */
        id |= COLD_LRU;
    }
    it->slabs_clsid = id;

    DEBUG_REFCNT(it, '*');
    it->it_flags = settings.use_cas ? ITEM_CAS : 0;
    it->nkey = nkey;
    it->nbytes = nbytes;
    memcpy(ITEM_key(it), key, nkey);
    it->exptime = exptime;
    memcpy(ITEM_suffix(it), suffix, (size_t)nsuffix);
    it->nsuffix = nsuffix;
    return it;
}

void item_free(item *it) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
    assert(it != heads[it->slabs_clsid]);
    assert(it != tails[it->slabs_clsid]);
    assert(it->refcount == 0);

    /* so slab size changer can tell later if item is already free or not */
    clsid = ITEM_clsid(it);
    DEBUG_REFCNT(it, 'F');
    slabs_free(it, ntotal, clsid);
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    char prefix[40];
    uint8_t nsuffix;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return slabs_clsid(ntotal) != 0;
}

//将item插入到LRU队列的头部  //将item加入到对应classid的LRU链的head   插入hash表函数为assoc_insert  插入lru队列的函数为item_link_q
static void do_item_link_q(item *it) { /* item is the new head */
    item **head, **tail;
    assert((it->it_flags & ITEM_SLABBED) == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    assert(it != *head);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
	//头插法插入该item
    it->prev = 0;
    it->next = *head;
    if (it->next) it->next->prev = it;
    *head = it;//该item作为对应链表的第一个节点
	//如果该item对应id上的第一个item，那么还会被认为是该id链上最后一个item
	//因为在head那里使用头插法，所以第一个插入的item，到了后面确实成了最后一个item
    if (*tail == 0) *tail = it;
    sizes[it->slabs_clsid]++;
    return;
}

static void item_link_q(item *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_link_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

//将it从对应的LRU队列中删除  //将item从对应classid的LRU链上移除
static void do_item_unlink_q(item *it) {
    item **head, **tail;
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

	//链表上的第一个节点
    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
	//链表上的最后一个节点
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);
	//把item的前驱结点和后区结点连接起来
    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
	//个数减一
    sizes[it->slabs_clsid]--;
    return;
}

static void item_unlink_q(item *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_unlink_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

////将item加入到hashtable并加入到对应classid的LRU链中。
int do_item_link(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    it->it_flags |= ITEM_LINKED;
    it->time = current_time;

    STATS_LOCK();
    stats.curr_bytes += ITEM_ntotal(it);
    stats.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    /* Allocate a new CAS ID on link. */
    ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
    assoc_insert(it, hv); //插入到hash表中，用于快速查找
    item_link_q(it);
    refcount_incr(&it->refcount); //增加refcount引用数

    return 1;
}

//将item从hashtable和LRU链中移除。是do_item_link的逆操作， do_item_unlink解除与hash和lru的关联，真正释放item在do_item_remove
void do_item_unlink(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats.curr_bytes -= ITEM_ntotal(it);
        stats.curr_items -= 1;
        STATS_UNLOCK();
        assoc_delete(ITEM_key(it), it->nkey, hv);
        item_unlink_q(it);
        do_item_remove(it);
    }
}

/* FIXME: Is it necessary to keep this copy/pasted code? */
void do_item_unlink_nolock(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats.curr_bytes -= ITEM_ntotal(it);
        stats.curr_items -= 1;
        STATS_UNLOCK();
        assoc_delete(ITEM_key(it), it->nkey, hv);
        do_item_unlink_q(it);
        do_item_remove(it);
    }
}
//do_item_unlink解除与hash和lru的关联，真正释放item在do_item_remove
void do_item_remove(item *it) {
    MEMCACHED_ITEM_REMOVE(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);
    assert(it->refcount > 0);

    if (refcount_decr(&it->refcount) == 0) {
        item_free(it);
    }
}

/* Copy/paste to avoid adding two extra branches for all common calls, since
 * _nolock is only used in an uncommon case where we want to relink. */
void do_item_update_nolock(item *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
	//下面的代码可以看到update操作时耗时的。如果这个item频繁被访问
	//那么会导致过多的update，过多的一系列费时操作。此时更新间隔就应运而生了
	//如果上一次的访问时间(也可以说是update时间)距离现在(current_time)
	//还在更新间隔内的，就不更新。超出了才更新。
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
			//从LRU队列中删除
            do_item_unlink_q(it);
			//更新访问时间
            it->time = current_time;
			//插入到LRU队列的头部
            do_item_link_q(it);
        }
    }
}

/*
为什么要把item插入到LRU队列头部呢？当然实现简单是其中一个原因。但更重要的是这是一个LRU队列！！还记得操作系统里面的LRU吧。
这是一种淘汰机制。在LRU队列中，排得越靠后就认为是越少使用的item，此时被淘汰的几率就越大。所以新鲜item(访问时间新)，要排
在不那么新鲜item的前面，所以插入LRU队列的头部是不二选择。下面的do_item_update函数佐证了这一点。do_item_update函数是先把
旧的item从LRU队列中删除，然后再插入到LRU队列中(此时它在LRU队列中排得最前)。除了更新item在队列中的位置外，还会更新item的
time成员，该成员指明上一次访问的时间(绝对时间)。如果不是为了LRU，那么do_item_update函数最简单的实现就是直接更新time成员即可。
memcached处理get命令时会调用do_item_update函数更新item的访问时间，更新其在LRU队列的位置。在memcached中get命令是很频繁的命令，
排在LRU队列第一或者前几的item更是频繁被get。对于排在前几名的item来说，调用do_item_update是意义不大的，因为调用do_item_update
后其位置还是前几名，并且LRU淘汰再多item也难于淘汰不到它们(一个LRU队列的item数量是很多的)。另一方面，do_item_update函数耗时还
是会有一定的耗时，因为要抢占cache_lock锁。如果频繁调用do_item_update函数性能将下降很多。于是memcached就是使用了更新间隔。
*/
//更新一个item的最后访问时间，不过需要在一段时间之后  更新访问时间
void do_item_update(item *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        /*
            //下面的代码可以看到update操作是耗时的。如果这个item频繁被访问，  
            //那么会导致过多的update，过多的一系列费时操作。此时更新间隔就应运而生  
            //了。如果上一次的访问时间(也可以说是update时间)距离现在(current_time)  
            //还在更新间隔内的，就不更新。超出了才更新。  
        */
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
            it->time = current_time;
            if (!settings.lru_maintainer_thread) {
                item_unlink_q(it);
                item_link_q(it);
            }
        }
    }
}

//将item用new_item代替
int do_item_replace(item *it, item *new_it, const uint32_t hv) {
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                           ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}

/*@null@*/
/* This is walking the line of violating lock order, but I think it's safe.
 * If the LRU lock is held, an item in the LRU cannot be wiped and freed.
 * The data could possibly be overwritten, but this is only accessing the
 * headers.
 * It may not be the best idea to leave it like this, but for now it's safe.
 * FIXME: only dumps the hot LRU with the new LRU's.
 */
char *item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes) {
    unsigned int memlimit = 2 * 1024 * 1024;   /* 2MB max response size */
    char *buffer;
    unsigned int bufcurr;
    item *it;
    unsigned int len;
    unsigned int shown = 0;
    char key_temp[KEY_MAX_LENGTH + 1];
    char temp[512];
    unsigned int id = slabs_clsid;
    if (!settings.lru_maintainer_thread)
        id |= COLD_LRU;

    pthread_mutex_lock(&lru_locks[id]);
    it = heads[id];

    buffer = malloc((size_t)memlimit);
    if (buffer == 0) {
        return NULL;
    }
    bufcurr = 0;

    while (it != NULL && (limit == 0 || shown < limit)) {
        assert(it->nkey <= KEY_MAX_LENGTH);
        if (it->nbytes == 0 && it->nkey == 0) {
            it = it->next;
            continue;
        }
        /* Copy the key since it may not be null-terminated in the struct */
        strncpy(key_temp, ITEM_key(it), it->nkey);
        key_temp[it->nkey] = 0x00; /* terminate */
        len = snprintf(temp, sizeof(temp), "ITEM %s [%d b; %lu s]\r\n",
                       key_temp, it->nbytes - 2,
                       (unsigned long)it->exptime + process_started);
        if (bufcurr + len + 6 > memlimit)  /* 6 is END\r\n\0 */
            break;
        memcpy(buffer + bufcurr, temp, len);
        bufcurr += len;
        shown++;
        it = it->next;
    }

    memcpy(buffer + bufcurr, "END\r\n", 6);
    bufcurr += 5;

    *bytes = bufcurr;
    pthread_mutex_unlock(&lru_locks[id]);
    return buffer;
}

void item_stats_totals(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    memset(&totals, 0, sizeof(itemstats_t));
    int n;
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        int x;
        int i;
        for (x = 0; x < 4; x++) {
            i = n | lru_type_map[x];
            pthread_mutex_lock(&lru_locks[i]);
            totals.expired_unfetched += itemstats[i].expired_unfetched;
            totals.evicted_unfetched += itemstats[i].evicted_unfetched;
            totals.evicted += itemstats[i].evicted;
            totals.reclaimed += itemstats[i].reclaimed;
            totals.crawler_reclaimed += itemstats[i].crawler_reclaimed;
            totals.crawler_items_checked += itemstats[i].crawler_items_checked;
            totals.lrutail_reflocked += itemstats[i].lrutail_reflocked;
            totals.moves_to_cold += itemstats[i].moves_to_cold;
            totals.moves_to_warm += itemstats[i].moves_to_warm;
            totals.moves_within_lru += itemstats[i].moves_within_lru;
            totals.direct_reclaims += itemstats[i].direct_reclaims;
            pthread_mutex_unlock(&lru_locks[i]);
        }
    }
    APPEND_STAT("expired_unfetched", "%llu",
                (unsigned long long)totals.expired_unfetched);
    APPEND_STAT("evicted_unfetched", "%llu",
                (unsigned long long)totals.evicted_unfetched);
    APPEND_STAT("evictions", "%llu",
                (unsigned long long)totals.evicted);
    APPEND_STAT("reclaimed", "%llu",
                (unsigned long long)totals.reclaimed);
    APPEND_STAT("crawler_reclaimed", "%llu",
                (unsigned long long)totals.crawler_reclaimed);
    APPEND_STAT("crawler_items_checked", "%llu",
                (unsigned long long)totals.crawler_items_checked);
    APPEND_STAT("lrutail_reflocked", "%llu",
                (unsigned long long)totals.lrutail_reflocked);
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("moves_to_cold", "%llu",
                    (unsigned long long)totals.moves_to_cold);
        APPEND_STAT("moves_to_warm", "%llu",
                    (unsigned long long)totals.moves_to_warm);
        APPEND_STAT("moves_within_lru", "%llu",
                    (unsigned long long)totals.moves_within_lru);
        APPEND_STAT("direct_reclaims", "%llu",
                    (unsigned long long)totals.direct_reclaims);
    }
}


/*
stats items
STAT items:1:number 4
STAT items:1:age 571
STAT items:1:evicted 0
STAT items:1:evicted_nonzero 0
STAT items:1:evicted_time 0
STAT items:1:outofmemory 0
STAT items:1:tailrepairs 0
STAT items:1:reclaimed 0
STAT items:1:expired_unfetched 0
STAT items:1:evicted_unfetched 0
STAT items:1:crawler_reclaimed 0
STAT items:1:lrutail_reflocked 0
STAT items:4:number 11
STAT items:4:age 2632
STAT items:4:evicted 0
STAT items:4:evicted_nonzero 0
STAT items:4:evicted_time 0
STAT items:4:outofmemory 0
STAT items:4:tailrepairs 0
STAT items:4:reclaimed 0
STAT items:4:expired_unfetched 0
STAT items:4:evicted_unfetched 0
STAT items:4:crawler_reclaimed 0
STAT items:4:lrutail_reflocked 0
END
*/
void item_stats(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    int n;
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        memset(&totals, 0, sizeof(itemstats_t));
        int x;
        int i;
        unsigned int size = 0;
        unsigned int age  = 0;
        unsigned int lru_size_map[4];
        const char *fmt = "items:%d:%s";
        char key_str[STAT_KEY_LEN];
        char val_str[STAT_VAL_LEN];
        int klen = 0, vlen = 0;
        for (x = 0; x < 4; x++) {
            i = n | lru_type_map[x];
            pthread_mutex_lock(&lru_locks[i]);
            totals.evicted += itemstats[i].evicted;
            totals.evicted_nonzero += itemstats[i].evicted_nonzero;
            totals.outofmemory += itemstats[i].outofmemory;
            totals.tailrepairs += itemstats[i].tailrepairs;
            totals.reclaimed += itemstats[i].reclaimed;
            totals.expired_unfetched += itemstats[i].expired_unfetched;
            totals.evicted_unfetched += itemstats[i].evicted_unfetched;
            totals.crawler_reclaimed += itemstats[i].crawler_reclaimed;
            totals.crawler_items_checked += itemstats[i].crawler_items_checked;
            totals.lrutail_reflocked += itemstats[i].lrutail_reflocked;
            totals.moves_to_cold += itemstats[i].moves_to_cold;
            totals.moves_to_warm += itemstats[i].moves_to_warm;
            totals.moves_within_lru += itemstats[i].moves_within_lru;
            totals.direct_reclaims += itemstats[i].direct_reclaims;
            size += sizes[i];
            lru_size_map[x] = sizes[i];
            if (lru_type_map[x] == COLD_LRU && tails[i] != NULL)
                age = current_time - tails[i]->time;
            pthread_mutex_unlock(&lru_locks[i]);
        }
        if (size == 0)
            continue;
        APPEND_NUM_FMT_STAT(fmt, n, "number", "%u", size);
        if (settings.lru_maintainer_thread) {
            APPEND_NUM_FMT_STAT(fmt, n, "number_hot", "%u", lru_size_map[0]);
            APPEND_NUM_FMT_STAT(fmt, n, "number_warm", "%u", lru_size_map[1]);
            APPEND_NUM_FMT_STAT(fmt, n, "number_cold", "%u", lru_size_map[2]);
            if (settings.expirezero_does_not_evict)
                APPEND_NUM_FMT_STAT(fmt, n, "number_noexp", "%u", lru_size_map[3]);
        }
        APPEND_NUM_FMT_STAT(fmt, n, "age", "%u", age);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted",
                            "%llu", (unsigned long long)totals.evicted);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted_nonzero",
                            "%llu", (unsigned long long)totals.evicted_nonzero);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted_time",
                            "%u", totals.evicted_time);
        APPEND_NUM_FMT_STAT(fmt, n, "outofmemory",
                            "%llu", (unsigned long long)totals.outofmemory);
        APPEND_NUM_FMT_STAT(fmt, n, "tailrepairs",
                            "%llu", (unsigned long long)totals.tailrepairs);
        APPEND_NUM_FMT_STAT(fmt, n, "reclaimed",
                            "%llu", (unsigned long long)totals.reclaimed);
        APPEND_NUM_FMT_STAT(fmt, n, "expired_unfetched",
                            "%llu", (unsigned long long)totals.expired_unfetched);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted_unfetched",
                            "%llu", (unsigned long long)totals.evicted_unfetched);
        APPEND_NUM_FMT_STAT(fmt, n, "crawler_reclaimed",
                            "%llu", (unsigned long long)totals.crawler_reclaimed);
        APPEND_NUM_FMT_STAT(fmt, n, "crawler_items_checked",
                            "%llu", (unsigned long long)totals.crawler_items_checked);
        APPEND_NUM_FMT_STAT(fmt, n, "lrutail_reflocked",
                            "%llu", (unsigned long long)totals.lrutail_reflocked);
        if (settings.lru_maintainer_thread) {
            APPEND_NUM_FMT_STAT(fmt, n, "moves_to_cold",
                                "%llu", (unsigned long long)totals.moves_to_cold);
            APPEND_NUM_FMT_STAT(fmt, n, "moves_to_warm",
                                "%llu", (unsigned long long)totals.moves_to_warm);
            APPEND_NUM_FMT_STAT(fmt, n, "moves_within_lru",
                                "%llu", (unsigned long long)totals.moves_within_lru);
            APPEND_NUM_FMT_STAT(fmt, n, "direct_reclaims",
                                "%llu", (unsigned long long)totals.direct_reclaims);
        }
    }

    /* getting here means both ascii and binary terminators fit */
    add_stats(NULL, 0, NULL, 0, c);
}

/*
stats sizes
STAT 96 4
STAT 192 11
END
*/
//查看某种chunk块的使用情况的
/** dumps out a list of objects of each size, with granularity of 32 bytes */
/*@null@*/
/* Locks are correct based on a technicality. Holds LRU lock while doing the
 * work, so items can't go invalid, and it's only looking at header sizes
 * which don't change.
 */
void item_stats_sizes(ADD_STAT add_stats, void *c) {

    /* max 1MB object, divided into 32 bytes size buckets */
    const int num_buckets = 32768;
    unsigned int *histogram = calloc(num_buckets, sizeof(int));

    if (histogram != NULL) {
        int i;

        /* build the histogram */
        for (i = 0; i < LARGEST_ID; i++) {
            pthread_mutex_lock(&lru_locks[i]);
            item *iter = heads[i];
            while (iter) {
                int ntotal = ITEM_ntotal(iter);
                int bucket = ntotal / 32;
                if ((ntotal % 32) != 0) bucket++;
                if (bucket < num_buckets) histogram[bucket]++;
                iter = iter->next;
            }
            pthread_mutex_unlock(&lru_locks[i]);
        }

        /* write the buffer */
        for (i = 0; i < num_buckets; i++) {
            if (histogram[i] != 0) {
                char key[8];
                snprintf(key, sizeof(key), "%d", i * 32);
                APPEND_STAT(key, "%u", histogram[i]);
            }
        }
        free(histogram);
    }
    add_stats(NULL, 0, NULL, 0, c);
}

/*
当worker线程接收到flush_all命令后，会用全局变量settings的oldest_live成员存储接收到这个命令那一刻的时间(准确地说，
是worker线程解析得知这是一个flush_all命令那一刻再减一)，代码为settings.oldest_live =current_time - 1;然后调用
item_flush_expired函数锁上cache_lock，然后调用do_item_flush_expired函数完成工作。
    惰性删除可以参考do_item_get中的if (settings.oldest_live != 0 && settings.oldest_live <= current_time int i;
do_item_get函数外，do_item_alloc函数也是会处理过期失效item的
*/
/** wrapper around assoc_find which does the lazy expiration logic */
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv) {
    item *it = assoc_find(key, nkey, hv); //assoc_find函数内部没有加锁
    if (it != NULL) {
        refcount_incr(&it->refcount);
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock order
         * of item_lock, slabs_lock. */
        /* This was made unsafe by removal of the cache_lock:
         * slab_rebalance_signal and slab_rebal.* are modified in a separate
         * thread under slabs_lock. If slab_rebalance_signal = 1, slab_start =
         * NULL (0), but slab_end is still equal to some value, this would end
         * up unlinking every item fetched.
         * This is either an acceptable loss, or if slab_rebalance_signal is
         * true, slab_start/slab_end should be put behind the slabs_lock.
         * Which would cause a huge potential slowdown.
         * Could also use a specific lock for slab_rebal.* and
         * slab_rebalance_signal (shorter lock?)
         */
        /*
		//这个item刚好在要移动的内存页里面。此时不能返回这个item  
            //worker线程要负责把这个item从哈希表和LRU队列中删除这个item，避免  
            //后面有其他worker线程又访问这个不能使用的item  

		if (slab_rebalance_signal &&
            ((void *)it >= slab_rebal.slab_start && (void *)it < slab_rebal.slab_end)) {
            do_item_unlink(it, hv);//引用计数会减一
            do_item_remove(it);//引用计数减一，如果引用计数等于0，就删除
            it = NULL;
        }*/
    }
    int was_found = 0;

    if (settings.verbose > 2) {
        int ii;
        if (it == NULL) {
            fprintf(stderr, "> NOT FOUND ");
        } else {
            fprintf(stderr, "> FOUND KEY ");
            was_found++;
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
    }

    if (it != NULL) {
        if (item_is_flushed(it)) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            if (was_found) {
                fprintf(stderr, " -nuked by flush");
            }
        } else if (it->exptime != 0 && it->exptime <= current_time) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            if (was_found) {
                fprintf(stderr, " -nuked by expire");
            }
        } else {
            it->it_flags |= ITEM_FETCHED|ITEM_ACTIVE;
            DEBUG_REFCNT(it, '+');
        }
    }

    if (settings.verbose > 2)
        fprintf(stderr, "\n");

    return it;
}

item *do_item_touch(const char *key, size_t nkey, uint32_t exptime,
                    const uint32_t hv) {
    item *it = do_item_get(key, nkey, hv);
    if (it != NULL) {
        it->exptime = exptime;
    }
    return it;
}

/*** LRU MAINTENANCE THREAD ***/

/* Returns number of items remove, expired, or evicted.
 * Callable from worker threads or the LRU maintainer thread */
static int lru_pull_tail(const int orig_id, const int cur_lru,
        const unsigned int total_chunks, const bool do_evict, const uint32_t cur_hv) {
    item *it = NULL;
    int id = orig_id;
    int removed = 0;
    if (id == 0)
        return 0;

    int tries = 5;
    item *search;
    item *next_it;
    void *hold_lock = NULL;
    unsigned int move_to_lru = 0;
    uint64_t limit;

    id |= cur_lru;
    pthread_mutex_lock(&lru_locks[id]);
    search = tails[id];
    /* We walk up *only* for locked items, and if bottom is expired. */
    for (; tries > 0 && search != NULL; tries--, search=next_it) {
        /* we might relink search mid-loop, so search->prev isn't reliable */
        next_it = search->prev;
        if (search->nbytes == 0 && search->nkey == 0 && search->it_flags == 1) {
            /* We are a crawler, ignore it. */
            tries++;
            continue;
        }
        uint32_t hv = hash(ITEM_key(search), search->nkey);
        /* Attempt to hash item lock the "search" item. If locked, no
         * other callers can incr the refcount. Also skip ourselves. */
        if (hv == cur_hv || (hold_lock = item_trylock(hv)) == NULL)
            continue;
        /* Now see if the item is refcount locked */
        if (refcount_incr(&search->refcount) != 2) {
            /* Note pathological case with ref'ed items in tail.
             * Can still unlink the item, but it won't be reusable yet */
            itemstats[id].lrutail_reflocked++;
            /* In case of refcount leaks, enable for quick workaround. */
            /* WARNING: This can cause terrible corruption */
            if (settings.tail_repair_time &&
                    search->time + settings.tail_repair_time < current_time) {
                itemstats[id].tailrepairs++;
                search->refcount = 1;
                /* This will call item_remove -> item_free since refcnt is 1 */
                do_item_unlink_nolock(search, hv);
                item_trylock_unlock(hold_lock);
                continue;
            }
        }

        /* Expired or flushed */
        if ((search->exptime != 0 && search->exptime < current_time)
            || item_is_flushed(search)) {
            itemstats[id].reclaimed++;
            if ((search->it_flags & ITEM_FETCHED) == 0) {
                itemstats[id].expired_unfetched++;
            }
            /* refcnt 2 -> 1 */
            do_item_unlink_nolock(search, hv);
            /* refcnt 1 -> 0 -> item_free */
            do_item_remove(search);
            item_trylock_unlock(hold_lock);
            removed++;

            /* If all we're finding are expired, can keep going */
            continue;
        }

        /* If we're HOT_LRU or WARM_LRU and over size limit, send to COLD_LRU.
         * If we're COLD_LRU, send to WARM_LRU unless we need to evict
         */
        switch (cur_lru) {
            case HOT_LRU:
                limit = total_chunks * settings.hot_lru_pct / 100;
            case WARM_LRU:
                limit = total_chunks * settings.warm_lru_pct / 100;
                if (sizes[id] > limit) {
                    itemstats[id].moves_to_cold++;
                    move_to_lru = COLD_LRU;
                    do_item_unlink_q(search);
                    it = search;
                    removed++;
                    break;
                } else if ((search->it_flags & ITEM_ACTIVE) != 0) {
                    /* Only allow ACTIVE relinking if we're not too large. */
                    itemstats[id].moves_within_lru++;
                    search->it_flags &= ~ITEM_ACTIVE;
                    do_item_update_nolock(search);
                    do_item_remove(search);
                    item_trylock_unlock(hold_lock);
                } else {
                    /* Don't want to move to COLD, not active, bail out */
                    it = search;
                }
                break;
            case COLD_LRU:
                it = search; /* No matter what, we're stopping */
                if (do_evict) {
                    if (settings.evict_to_free == 0) {
                        /* Don't think we need a counter for this. It'll OOM.  */
                        break;
                    }
                    itemstats[id].evicted++;
                    itemstats[id].evicted_time = current_time - search->time;
                    if (search->exptime != 0)
                        itemstats[id].evicted_nonzero++;
                    if ((search->it_flags & ITEM_FETCHED) == 0) {
                        itemstats[id].evicted_unfetched++;
                    }
                    do_item_unlink_nolock(search, hv);
                    removed++;
                    if (settings.slab_automove == 2) {
                        slabs_reassign(-1, orig_id);
                    }
                } else if ((search->it_flags & ITEM_ACTIVE) != 0
                        && settings.lru_maintainer_thread) {
                    itemstats[id].moves_to_warm++;
                    search->it_flags &= ~ITEM_ACTIVE;
                    move_to_lru = WARM_LRU;
                    do_item_unlink_q(search);
                    removed++;
                }
                break;
        }
        if (it != NULL)
            break;
    }

    pthread_mutex_unlock(&lru_locks[id]);

    if (it != NULL) {
        if (move_to_lru) {
            it->slabs_clsid = ITEM_clsid(it);
            it->slabs_clsid |= move_to_lru;
            item_link_q(it);
        }
        do_item_remove(it);
        item_trylock_unlock(hold_lock);
    }

    return removed;
}

/* Loop up to N times:
 * If too many items are in HOT_LRU, push to COLD_LRU
 * If too many items are in WARM_LRU, push to COLD_LRU
 * If too many items are in COLD_LRU, poke COLD_LRU tail
 * 1000 loops with 1ms min sleep gives us under 1m items shifted/sec. The
 * locks can't handle much more than that. Leaving a TODO for how to
 * autoadjust in the future.
 */
static int lru_maintainer_juggle(const int slabs_clsid) {
    int i;
    int did_moves = 0;
    bool mem_limit_reached = false;
    unsigned int total_chunks = 0;
    unsigned int chunks_perslab = 0;
    unsigned int chunks_free = 0;
    /* TODO: if free_chunks below high watermark, increase aggressiveness */
    chunks_free = slabs_available_chunks(slabs_clsid, &mem_limit_reached,
            &total_chunks, &chunks_perslab);
    if (settings.expirezero_does_not_evict)
        total_chunks -= noexp_lru_size(slabs_clsid);

    /* If slab automove is enabled on any level, and we have more than 2 pages
     * worth of chunks free in this class, ask (gently) to reassign a page
     * from this class back into the global pool (0)
     */
    if (settings.slab_automove > 0 && chunks_free > (chunks_perslab * 2.5)) {
        slabs_reassign(slabs_clsid, SLAB_GLOBAL_PAGE_POOL);
    }

    /* Juggle HOT/WARM up to N times */
    for (i = 0; i < 1000; i++) {
        int do_more = 0;
        if (lru_pull_tail(slabs_clsid, HOT_LRU, total_chunks, false, 0) ||
            lru_pull_tail(slabs_clsid, WARM_LRU, total_chunks, false, 0)) {
            do_more++;
        }
        do_more += lru_pull_tail(slabs_clsid, COLD_LRU, total_chunks, false, 0);
        if (do_more == 0)
            break;
        did_moves++;
    }
    return did_moves;
}

/* Will crawl all slab classes a minimum of once per hour */
#define MAX_MAINTCRAWL_WAIT 60 * 60

/* Hoping user input will improve this function. This is all a wild guess.
 * Operation: Kicks crawler for each slab id. Crawlers take some statistics as
 * to items with nonzero expirations. It then buckets how many items will
 * expire per minute for the next hour.
 * This function checks the results of a run, and if it things more than 1% of
 * expirable objects are ready to go, kick the crawler again to reap.
 * It will also kick the crawler once per minute regardless, waiting a minute
 * longer for each time it has no work to do, up to an hour wait time.
 * The latter is to avoid newly started daemons from waiting too long before
 * retrying a crawl.
 */
static void lru_maintainer_crawler_check(void) {
    int i;
    static rel_time_t last_crawls[MAX_NUMBER_OF_SLAB_CLASSES];
    static rel_time_t next_crawl_wait[MAX_NUMBER_OF_SLAB_CLASSES];
    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        crawlerstats_t *s = &crawlerstats[i];
        /* We've not successfully kicked off a crawl yet. */
        if (last_crawls[i] == 0) {
            if (lru_crawler_start(i, 0) > 0) {
                last_crawls[i] = current_time;
            }
        }
        pthread_mutex_lock(&lru_crawler_stats_lock);
        if (s->run_complete) {
            int x;
            /* Should we crawl again? */
            uint64_t possible_reclaims = s->seen - s->noexp;
            uint64_t available_reclaims = 0;
            /* Need to think we can free at least 1% of the items before
             * crawling. */
            /* FIXME: Configurable? */
            uint64_t low_watermark = (s->seen / 100) + 1;
            rel_time_t since_run = current_time - s->end_time;
            /* Don't bother if the payoff is too low. */
            if (settings.verbose > 1)
                fprintf(stderr, "maint crawler: low_watermark: %llu, possible_reclaims: %llu, since_run: %u\n",
                        (unsigned long long)low_watermark, (unsigned long long)possible_reclaims,
                        (unsigned int)since_run);
            for (x = 0; x < 60; x++) {
                if (since_run < (x * 60) + 60)
                    break;
                available_reclaims += s->histo[x];
            }
            if (available_reclaims > low_watermark) {
                last_crawls[i] = 0;
                if (next_crawl_wait[i] > 60)
                    next_crawl_wait[i] -= 60;
            } else if (since_run > 5 && since_run > next_crawl_wait[i]) {
                last_crawls[i] = 0;
                if (next_crawl_wait[i] < MAX_MAINTCRAWL_WAIT)
                    next_crawl_wait[i] += 60;
            }
            if (settings.verbose > 1)
                fprintf(stderr, "maint crawler: available reclaims: %llu, next_crawl: %u\n", (unsigned long long)available_reclaims, next_crawl_wait[i]);
        }
        pthread_mutex_unlock(&lru_crawler_stats_lock);
    }
}

static pthread_t lru_maintainer_tid;

#define MAX_LRU_MAINTAINER_SLEEP 1000000
#define MIN_LRU_MAINTAINER_SLEEP 1000

static void *lru_maintainer_thread(void *arg) {
    int i;
    useconds_t to_sleep = MIN_LRU_MAINTAINER_SLEEP;
    rel_time_t last_crawler_check = 0;

    pthread_mutex_lock(&lru_maintainer_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "Starting LRU maintainer background thread\n");
    while (do_run_lru_maintainer_thread) {
        int did_moves = 0;
        pthread_mutex_unlock(&lru_maintainer_lock);
        usleep(to_sleep);
        pthread_mutex_lock(&lru_maintainer_lock);

        STATS_LOCK();
        stats.lru_maintainer_juggles++;
        STATS_UNLOCK();
        /* We were asked to immediately wake up and poke a particular slab
         * class due to a low watermark being hit */
        if (lru_maintainer_check_clsid != 0) {
            did_moves = lru_maintainer_juggle(lru_maintainer_check_clsid);
            lru_maintainer_check_clsid = 0;
        } else {
            for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
                did_moves += lru_maintainer_juggle(i);
            }
        }
        if (did_moves == 0) {
            if (to_sleep < MAX_LRU_MAINTAINER_SLEEP)
                to_sleep += 1000;
        } else {
            to_sleep /= 2;
            if (to_sleep < MIN_LRU_MAINTAINER_SLEEP)
                to_sleep = MIN_LRU_MAINTAINER_SLEEP;
        }
        /* Once per second at most */
        if (settings.lru_crawler && last_crawler_check != current_time) {
            lru_maintainer_crawler_check();
            last_crawler_check = current_time;
        }
    }
    pthread_mutex_unlock(&lru_maintainer_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "LRU maintainer thread stopping\n");

    return NULL;
}
int stop_lru_maintainer_thread(void) {
    int ret;
    pthread_mutex_lock(&lru_maintainer_lock);
    /* LRU thread is a sleep loop, will die on its own */
    do_run_lru_maintainer_thread = 0;
    pthread_mutex_unlock(&lru_maintainer_lock);
    if ((ret = pthread_join(lru_maintainer_tid, NULL)) != 0) {
        fprintf(stderr, "Failed to stop LRU maintainer thread: %s\n", strerror(ret));
        return -1;
    }
    settings.lru_maintainer_thread = false;
    return 0;
}

int start_lru_maintainer_thread(void) {
    int ret;

    pthread_mutex_lock(&lru_maintainer_lock);
    do_run_lru_maintainer_thread = 1;
    settings.lru_maintainer_thread = true;
    if ((ret = pthread_create(&lru_maintainer_tid, NULL,
        lru_maintainer_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create LRU maintainer thread: %s\n",
            strerror(ret));
        pthread_mutex_unlock(&lru_maintainer_lock);
        return -1;
    }
    pthread_mutex_unlock(&lru_maintainer_lock);

    return 0;
}

/* If we hold this lock, crawler can't wake up or move */
void lru_maintainer_pause(void) {
    pthread_mutex_lock(&lru_maintainer_lock);
}

void lru_maintainer_resume(void) {
    pthread_mutex_unlock(&lru_maintainer_lock);
}

int init_lru_maintainer(void) {
    if (lru_maintainer_initialized == 0) {
        pthread_mutex_init(&lru_maintainer_lock, NULL);
        lru_maintainer_initialized = 1;
    }
    return 0;
}

/*** ITEM CRAWLER THREAD ***/

static void crawler_link_q(item *it) { /* item is the new tail */
    item **head, **tail;
    assert(it->it_flags == 1);
    assert(it->nbytes == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    assert(*tail != 0);
    assert(it != *tail);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
    it->prev = *tail;
    it->next = 0;
    if (it->prev) {
        assert(it->prev->next == 0);
        it->prev->next = it;
    }
    *tail = it;
    if (*head == 0) *head = it;
    return;
}

static void crawler_unlink_q(item *it) {
    item **head, **tail;
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);

    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
    return;
}

/*
伪item通过与前驱节点交换位置实现前进。如果伪item是LRU队列的头节点，那么就将这个伪item移出LRU队列。
函数crawler_crawl_q完成这个交换操作，并且返回交换前伪item的前驱节点(当然在交换后就变成伪item的后驱节点了)。
如果伪item处于LRU队列的头部，那么就返回NULL(此时没有前驱节点了)
*/
/* This is too convoluted, but it's a difficult shuffle. Try to rewrite it
 * more clearly. */
static item *crawler_crawl_q(item *it) {
    item **head, **tail;
    assert(it->it_flags == 1);
    assert(it->nbytes == 0);
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    /* We've hit the head, pop off */
    if (it->prev == 0) {
        assert(*head == it);
        if (it->next) {
            *head = it->next;
            assert(it->next->prev == it);
            it->next->prev = 0;
        }
        return NULL; /* Done */
    }

    /* Swing ourselves in front of the next item */
    /* NB: If there is a prev, we can't be the head */
    assert(it->prev != it);
    if (it->prev) {
        if (*head == it->prev) {
            /* Prev was the head, now we're the head */
            *head = it;
        }
        if (*tail == it) {
            /* We are the tail, now they are the tail */
            *tail = it->prev;
        }
        assert(it->next != it);
        if (it->next) {
            assert(it->prev->next == it);
            it->prev->next = it->next;
            it->next->prev = it->prev;
        } else {
            /* Tail. Move this above? */
            it->prev->next = 0;
        }
        /* prev->prev's next is it->prev */
        it->next = it->prev;
        it->prev = it->next->prev;
        it->next->prev = it;
        /* New it->prev now, if we're not at the head. */
        if (it->prev) {
            it->prev->next = it;
        }
    }
    assert(it->next != it);
    assert(it->prev != it);

    return it->next; /* success */
}

/* I pulled this out to make the main thread clearer, but it reaches into the
 * main thread's values too much. Should rethink again.
 */ //如果这个item过期失效了，那么就删除
static void item_crawler_evaluate(item *search, uint32_t hv, int i) {
    int slab_id = CLEAR_LRU(i);
    crawlerstats_t *s = &crawlerstats[slab_id];
    itemstats[i].crawler_items_checked++;
    if ((search->exptime != 0 && search->exptime < current_time)//这个item的exptime时间戳到了，已经过期失效了  
        || item_is_flushed(search)) {//因为客户端发送flush_all命令，导致这个item失效了
        itemstats[i].crawler_reclaimed++;
        s->reclaimed++;

        if (settings.verbose > 1) {
            int ii;
            char *key = ITEM_key(search);
            fprintf(stderr, "LRU crawler found an expired item (flags: %d, slab: %d): ",
                search->it_flags, search->slabs_clsid);
            for (ii = 0; ii < search->nkey; ++ii) {
                fprintf(stderr, "%c", key[ii]);
            }
            fprintf(stderr, "\n");
        }
        if ((search->it_flags & ITEM_FETCHED) == 0) {
            itemstats[i].expired_unfetched++;
        }
         //将item从LRU队列中删除  
        do_item_unlink_nolock(search, hv);
        do_item_remove(search);
        assert(search->slabs_clsid == 0);
    } else {
        s->seen++;
        refcount_decr(&search->refcount);
        if (search->exptime == 0) {
            s->noexp++;
        } else if (search->exptime - current_time > 3599) {
            s->ttl_hourplus++;
        } else {
            rel_time_t ttl_remain = search->exptime - current_time;
            int bucket = ttl_remain / 60;
            s->histo[bucket]++;
        }
    }
}

/*
可以用命令lru_crawler tocrawl num指定每个LRU队列最多只检查num-1个item。看清楚点，是检查数，不是删除数，
而且是num-1个。首先要调用item_crawler_evaluate函数检查一个item是否过期，是的话就删除。如果检查完num-1个，
伪item都还没有到达LRU队列的头部，那么就直接将这个伪item从LRU队列中删除。
*/
static void *item_crawler_thread(void *arg) {
    int i;
    int crawls_persleep = settings.crawls_persleep;

    pthread_mutex_lock(&lru_crawler_lock);
    pthread_cond_signal(&lru_crawler_cond);
    settings.lru_crawler = true;
    if (settings.verbose > 2)
        fprintf(stderr, "Starting LRU crawler background thread\n");
    while (do_run_lru_crawler_thread) {
        //等待worker线程指定要处理的LRU队列,也就是等待客户端执行lru_crawler tocrawl <classid,classid,classid|all> 
        //命令”lru_crawler crawl<classid,classid,classid|all>”才是指定任务的。该命令指明了要对哪个LRU队列进行清理
    pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);

        while (crawler_count) {//crawler_count表明要处理多少个LRU队列  
        item *search = NULL;
        void *hold_lock = NULL;

        for (i = POWER_SMALLEST; i < LARGEST_ID; i++) {
            if (crawlers[i].it_flags != 1) {
                continue;
            }
            pthread_mutex_lock(&lru_locks[i]);
                //返回crawlers[i]的前驱节点,并交换crawlers[i]和前驱节点的位置  
            search = crawler_crawl_q((item *)&crawlers[i]);
            if (search == NULL ||
                (crawlers[i].remaining && --crawlers[i].remaining < 1)) {
                    //crawlers[i]是头节点，没有前驱节点  
                //remaining的值为settings.lru_crawler_tocrawl。每次启动lru  
                //爬虫线程，检查每一个lru队列的多少个item。
                if (settings.verbose > 2)
                    fprintf(stderr, "Nothing left to crawl for %d\n", i);
                    crawlers[i].it_flags = 0;//检查了足够多次，退出检查这个lru队列  
                    crawler_count--;//清理完一个LRU队列,任务数减一  
                    crawler_unlink_q((item *)&crawlers[i]);//将这个伪item从LRU队列中删除  
                pthread_mutex_unlock(&lru_locks[i]);
                pthread_mutex_lock(&lru_crawler_stats_lock);
                crawlerstats[CLEAR_LRU(i)].end_time = current_time;
                crawlerstats[CLEAR_LRU(i)].run_complete = true;
                pthread_mutex_unlock(&lru_crawler_stats_lock);
                continue;
            }
            uint32_t hv = hash(ITEM_key(search), search->nkey);
            /* Attempt to hash item lock the "search" item. If locked, no
             * other callers can incr the refcount
             */
                if ((hold_lock = item_trylock(hv)) == NULL) {//尝试锁住控制这个item的哈希表段级别锁  
                pthread_mutex_unlock(&lru_locks[i]);
                continue;
            }
            /* Now see if the item is refcount locked */
                if (refcount_incr(&search->refcount) != 2) { //此时有其他worker线程在引用这个item  
                    refcount_decr(&search->refcount); //lru爬虫线程放弃引用该item  
                if (hold_lock)
                    item_trylock_unlock(hold_lock);
                pthread_mutex_unlock(&lru_locks[i]);
                continue;
            }

            /* Frees the item or decrements the refcount. */
            /* Interface for this could improve: do the free/decr here
                 * instead? *///如果这个item过期失效了，那么就删除这个item  
            pthread_mutex_lock(&lru_crawler_stats_lock);
            item_crawler_evaluate(search, hv, i);
            pthread_mutex_unlock(&lru_crawler_stats_lock);

            if (hold_lock)
                item_trylock_unlock(hold_lock);
            pthread_mutex_unlock(&lru_locks[i]);

            if (crawls_persleep <= 0 && settings.lru_crawler_sleep) {
                usleep(settings.lru_crawler_sleep);
                crawls_persleep = settings.crawls_persleep;
            }
        }
    }
    if (settings.verbose > 2)
        fprintf(stderr, "LRU crawler thread sleeping\n");
    STATS_LOCK();
    stats.lru_crawler_running = false;
    STATS_UNLOCK();
    }
    pthread_mutex_unlock(&lru_crawler_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "LRU crawler thread stopping\n");

    return NULL;
}

static pthread_t item_crawler_tid; //爬虫线程ID

//worker线程在接收到"lru_crawler disable"命令会执行这个函数  
int stop_item_crawler_thread(void) {
    int ret;
    pthread_mutex_lock(&lru_crawler_lock);
    do_run_lru_crawler_thread = 0;
    //LRU爬虫线程可能休眠于等待条件变量，需要唤醒才能停止LRU爬虫线程  
    pthread_cond_signal(&lru_crawler_cond);
    pthread_mutex_unlock(&lru_crawler_lock);
    if ((ret = pthread_join(item_crawler_tid, NULL)) != 0) {
        fprintf(stderr, "Failed to stop LRU crawler thread: %s\n", strerror(ret));
        return -1;
    }
    settings.lru_crawler = false;
    return 0;
}

/* Lock dance to "block" until thread is waiting on its condition:
 * caller locks mtx. caller spawns thread.
 * thread blocks on mutex.
 * caller waits on condition, releases lock.
 * thread gets lock, sends signal.
 * caller can't wait, as thread has lock.
 * thread waits on condition, releases lock
 * caller wakes on condition, gets lock.
 * caller immediately releases lock.
 * thread is now safely waiting on condition before the caller returns.
 */
/*
LRU爬虫：
        前面说到，memcached是懒惰删除过期失效item的。所以即使用户在客户端使用了flush_all命令使得全部item都过期失效了，
        但这些item还是占据者哈希表和LRU队列并没有归还给slab分配器。

LRU爬虫线程：
        有没有办法强制清除这些过期失效的item，不再占据哈希表和LRU队列的空间并归还给slabs呢？当然是有的。memcached
        提供了LRU爬虫可以实现这个功能。
        要使用LRU爬虫就必须在客户端使用lru_crawler命令。memcached服务器根据具体的命令参数进行处理。
        memcached是用一个专门的线程负责清除这些过期失效item的，本文将称这个线程为LRU爬虫线程。默认情况下memcached
        是不启动这个线程的，但可以在启动memcached的时候添加参数-o lru_crawler启动这个线程。也可以通过客户端命令启
        动。即使启动了这个LRU爬虫线程，该线程还是不会工作。需要另外发送命令，指明要对哪个LRU队列进行清除处理。现
        在看一下lru_crawler有哪些参数。

LRU爬虫命令：
lru_crawler  <enable|disable>  启动或者停止一个LRU爬虫线程。任何时刻，最多只有一个LRU爬虫线程。该命令对
settings.lru_crawler进行赋值为true或者false
lru_crawler crawl <classid,classid,classid|all>  可以使用2,3,6这样的列表指明要对哪个LRU队列进行清除处理。
也可以使用all对所有的LRU队列进行处理
lru_crawler sleep <microseconds>  LRU爬虫线程在清除item的时候会占用锁，会妨碍worker线程的正常业务。所以LRU
爬虫在处理的时候需要时不时休眠一下。默认休眠时间为100微秒。该命令对settings.lru_crawler_sleep进行赋值
lru_crawler tocrawl <32u>  一个LRU队列可能会有很多过期失效的item。如果一直检查和清除下去，势必会妨碍worker
线程的正常业务。这个参数用来指明最多只检查每一条LRU队列的多少个item。默认值为0，所以如果不指定那么就不会工
作。该命令对settings.lru_crawler_tocrawl进行赋值
如果要启动LRU爬虫主动删除过期的item，需要这样做：首先使用lru_crawler enable命令启动一个LRU爬虫线程。
然后使用lru_crawler tocrawl num命令确定每一个LRU队列最多检查num-1个item。最后使用命令
lru_crawler crawl <classid,classid,classid|all> 指定要处理的LRU队列。lru_crawler sleep可以不设置，
如果要设置那么可以在lru_crawler crawl命令之前设置即可。
*/ //参考http://blog.csdn.net/luotuo44/article/details/42963793
int start_item_crawler_thread(void) {//可以在启动命令行中加上-o lru_crawler或者客户端执行lru_crawler enable命令来启动爬虫线程
    int ret;
    //worker线程接收到"lru_crawler enable"命令后会调用本函数  
    //启动memcached时如果有-o lru_crawler参数也是会调用本函数  

    //在stop_item_crawler_thread函数可以看到pthread_join函数  
    //在pthread_join返回后，才会把settings.lru_crawler设置为false。  
    //所以不会出现同时出现两个crawler线程  
    if (settings.lru_crawler) //已经启用了怕从线程，不用再创建线程了
        return -1;
    pthread_mutex_lock(&lru_crawler_lock);
    do_run_lru_crawler_thread = 1;
    //创建一个LRU爬虫线程，线程函数为item_crawler_thread。LRU爬虫线程在进入  
    //item_crawler_thread函数后，会调用pthread_cond_wait，等待worker线程指定  
    //要处理的LRU队列  
    if ((ret = pthread_create(&item_crawler_tid, NULL,
        item_crawler_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create LRU crawler thread: %s\n",
            strerror(ret));
        pthread_mutex_unlock(&lru_crawler_lock);
        return -1;
    }
    /* Avoid returning until the crawler has actually started */
    pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);
    pthread_mutex_unlock(&lru_crawler_lock);

    return 0;
}

/* 'remaining' is passed in so the LRU maintainer thread can scrub the whole
 * LRU every time.
 */
static int do_lru_crawler_start(uint32_t id, uint32_t remaining) {
    int i;
    uint32_t sid;
    uint32_t tocrawl[3];
    int starts = 0;
    tocrawl[0] = id | HOT_LRU;
    tocrawl[1] = id | WARM_LRU;
    tocrawl[2] = id | COLD_LRU;

    for (i = 0; i < 3; i++) {
        sid = tocrawl[i];
        pthread_mutex_lock(&lru_locks[sid]);
        if (tails[sid] != NULL) {
            if (settings.verbose > 2)
                fprintf(stderr, "Kicking LRU crawler off for LRU %d\n", sid);
            crawlers[sid].nbytes = 0;
            crawlers[sid].nkey = 0;
            crawlers[sid].it_flags = 1; /* For a crawler, this means enabled. */
            crawlers[sid].next = 0;
            crawlers[sid].prev = 0;
            crawlers[sid].time = 0;
            crawlers[sid].remaining = remaining;
            crawlers[sid].slabs_clsid = sid;
            crawler_link_q((item *)&crawlers[sid]);
            crawler_count++;
            starts++;
        }
        pthread_mutex_unlock(&lru_locks[sid]);
    }
    if (starts) {
        STATS_LOCK();
        stats.lru_crawler_running = true;
        stats.lru_crawler_starts++;
        STATS_UNLOCK();
        pthread_mutex_lock(&lru_crawler_stats_lock);
        memset(&crawlerstats[id], 0, sizeof(crawlerstats_t));
        crawlerstats[id].start_time = current_time;
        pthread_mutex_unlock(&lru_crawler_stats_lock);
    }
    return starts;
}

static int lru_crawler_start(uint32_t id, uint32_t remaining) {
    int starts;
    if (pthread_mutex_trylock(&lru_crawler_lock) != 0) {
        return 0;
    }
    starts = do_lru_crawler_start(id, remaining);
    if (starts) {
        pthread_cond_signal(&lru_crawler_cond);
    }
    pthread_mutex_unlock(&lru_crawler_lock);
    return starts;
}

/* FIXME: Split this into two functions: one to kick a crawler for a sid, and one to
 * parse the string. LRU maintainer code is generating a string to set up a
 * sid.
 * Also only clear the crawlerstats once per sid.
 */
 
 /*
lru_crawler_crawl函数，memcached会在这个函数会把伪item插入到LRU队列尾部的。当worker线程接收到lru_crawler 
crawl<classid,classid,classid|all>命令时就会调用这个函数。因为用户可能要求LRU爬虫线程清理多个LRU队列的过
期失效item，所以需要一个伪item数组。伪item数组的大小等于LRU队列的个数，它们是一一对应的*/
 
 //当客户端使用命令lru_crawler crawl <classid,classid,classid|all>时，  
//worker线程就会调用本函数,并将命令的第二个参数作为本函数的参数  
enum crawler_result_type lru_crawler_crawl(char *slabs) {
    char *b = NULL;
    uint32_t sid = 0;
    int starts = 0;
    uint8_t tocrawl[MAX_NUMBER_OF_SLAB_CLASSES];
//LRU爬虫线程进行清理的时候，会锁上lru_crawler_lock。直到完成所有  
    //的清理任务才会解锁。所以客户端的前一个清理任务还没结束前，不能  
    //再提交另外一个清理任务  
    if (pthread_mutex_trylock(&lru_crawler_lock) != 0) {
        return CRAWLER_RUNNING;
    }

    /* FIXME: I added this while debugging. Don't think it's needed? */
	//解析命令，如果命令要求对某一个LRU队列进行清理，那么就在tocrawl数组  
    //对应元素赋值1作为标志  
    memset(tocrawl, 0, sizeof(uint8_t) * MAX_NUMBER_OF_SLAB_CLASSES);
    if (strcmp(slabs, "all") == 0) { //处理全部lru队列  
        for (sid = 0; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
            tocrawl[sid] = 1;
        }
    } else {
        for (char *p = strtok_r(slabs, ",", &b);//解析出一个个的sid  
             p != NULL;
             p = strtok_r(NULL, ",", &b)) {

            if (!safe_strtoul(p, &sid) || sid < POWER_SMALLEST
                    || sid >= MAX_NUMBER_OF_SLAB_CLASSES-1) {
                pthread_mutex_unlock(&lru_crawler_lock);
                return CRAWLER_BADCLASS;
            }
            tocrawl[sid] = 1;
        }
    }

    //crawlers是一个伪item类型数组。如果用户要清理某一个LRU队列，那么  
    //就在这个LRU队列中插入一个伪item  
    for (sid = POWER_SMALLEST; sid < MAX_NUMBER_OF_SLAB_CLASSES; sid++) {
        if (tocrawl[sid])
            starts += do_lru_crawler_start(sid, settings.lru_crawler_tocrawl);
    }
    if (starts) {
    //命令”lru_crawler crawl<classid,classid,classid|all>”才是指定任务的。该命令指明了要对哪个LRU队列进行清理
    pthread_cond_signal(&lru_crawler_cond); //有任务了，唤醒LRU爬虫线程，让其执行清理任务  
        pthread_mutex_unlock(&lru_crawler_lock);
        return CRAWLER_OK;
    } else {
        pthread_mutex_unlock(&lru_crawler_lock);
        return CRAWLER_NOTSTARTED;
    }
}

/* If we hold this lock, crawler can't wake up or move */
void lru_crawler_pause(void) {
    pthread_mutex_lock(&lru_crawler_lock);
}

void lru_crawler_resume(void) {
    pthread_mutex_unlock(&lru_crawler_lock);
}

int init_lru_crawler(void) {
    if (lru_crawler_initialized == 0) {
        memset(&crawlerstats, 0, sizeof(crawlerstats_t) * MAX_NUMBER_OF_SLAB_CLASSES);
        if (pthread_cond_init(&lru_crawler_cond, NULL) != 0) {
            fprintf(stderr, "Can't initialize lru crawler condition\n");
            return -1;
        }
        pthread_mutex_init(&lru_crawler_lock, NULL);
        lru_crawler_initialized = 1;
    }
    return 0;
}
