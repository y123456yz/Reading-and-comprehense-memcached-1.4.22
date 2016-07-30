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
    uint64_t evicted_nonzero;//被踢的item中，超时时间(exptime)不为0的item数  
    rel_time_t evicted_time;//最后一次踢item时，被踢的item已经过期多久了  
    uint64_t reclaimed; //item被重复使用的次数，赋值见do_item_alloc  //在申请item时，发现过期并回收的item数量  
    uint64_t outofmemory; //从slab中获取mem失败的次数，见do_item_alloc  //为item申请内存，失败的次数  
    uint64_t tailrepairs; //需要修复的item数量(除非worker线程有问题否则一般为0)  
    uint64_t expired_unfetched; //被访问过并且超时的item,赋值见do_item_alloc   //直到被超时删除时都还没被访问过的item数量  
    uint64_t evicted_unfetched; //直到被LRU踢出时都还没有被访问过的item数量  
    uint64_t crawler_reclaimed; //该slabclass保存的item数  //被LRU爬虫发现的过期item数量  
    uint64_t lrutail_reflocked;//申请item而搜索LRU队列时，被其他worker线程引用的item数量  
} itemstats_t; //item的状态统计信息，这里就不分析了

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

static int crawler_count = 0; //本次任务要处理多少个LRU队列  
static volatile int do_run_lru_crawler_thread = 0; //为1标识已经启用爬虫线程
static int lru_crawler_initialized = 0;//表示是否初始化lru爬虫线程锁和lru爬虫线程条件变量
static pthread_mutex_t lru_crawler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  lru_crawler_cond = PTHREAD_COND_INITIALIZER;

void item_stats_reset(void) {
    mutex_lock(&cache_lock);
    memset(itemstats, 0, sizeof(itemstats));
    mutex_unlock(&cache_lock);
}


/* Get the next CAS id for a new item. */
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    return ++cas_id;
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
    return sizeof(item) + nkey + *nsuffix + nbytes; //expire保存到it->expire  cas保存在it->data->case中的
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
//nkey是key字符串的长度。nbytes则是用户要存储的data长度+2，因为在data结尾处还要加上"\r\n",data的\r\n在该函数前+2的
//cur_hv则是根据键值key计算得到的哈希值
item *do_item_alloc(char *key, const size_t nkey, const int flags,
                    const rel_time_t exptime, const int nbytes,
                    const uint32_t cur_hv) { //do_item_alloc (分配item) 和 item_free （释放item）
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];

	//要存储这个item需要的总空间  nkey+1是因为set key 0 0 3\n  abc\r\n  对应的key行后面有\n
    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

	//根据大小判断从属于哪个slab
    unsigned int id = slabs_clsid(ntotal);
    if (id == 0) //0表示不属于任何一个slab  slabclass是从1开始的
        return 0;

    mutex_lock(&cache_lock);
    /* do a quick check if we have any expired items in the tail.. */
    int tries = 5;
    /* Avoid hangs if a slab has nothing but refcounted stuff in it. */
    int tries_lrutail_reflocked = 1000;
    int tried_alloc = 0;
    item *search;
    item *next_it;
    void *hold_lock = NULL;
    rel_time_t oldest_live = settings.oldest_live;

    search = tails[id];
    /* We walk up *only* for locked items. Never searching for expired.
     * Waste of CPU for almost all deployments */

	//第一次看这个for循环，直接认为search等于NULL，直接看for循环后面的代码
	//这个循环里面会在对应LRU队列中查找过期失效的item，最多尝试tries个item。
	//从LRU的队尾开始尝试。如果item被其他worker线程引用了，那么就尝试下一个
	//如果没有的被其他worker线程所引用，那么就测试该item是否过期失效
	//如果过期失效了，那么就可以使用这个item(最终会返回这个item)。如果没有
	//过期失效，那么不再尝试其他item了(因为是从LRU队列的队尾开始尝试的,对尾的都没有失效，其他前面的肯定不会失效)，
	//直接调用slabs_alloc申请一个新的内存存储item。如果申请新内存都失败，
	//那么在允许LRU淘汰的情况下就会启动踢人机制
	
    for (; tries > 0 && search != NULL; tries--, search=next_it) {
        /* we might relink search mid-loop, so search->prev isn't reliable */
        next_it = search->prev;
        if (search->nbytes == 0 && search->nkey == 0 && search->it_flags == 1) {
            /* We are a crawler, ignore it. */
			//这是一个爬虫item，直接跳过
            tries++; //爬虫item不计入尝试的item数中
            continue;
        }
        uint32_t hv = hash(ITEM_key(search), search->nkey);
        /* Attempt to hash item lock the "search" item. If locked, no
         * other callers can incr the refcount
         */
        /* Don't accidentally grab ourselves, or bail if we can't quicklock */
		//尝试抢占锁，抢不了就走人，不等待锁。
        if (hv == cur_hv || (hold_lock = item_trylock(hv)) == NULL)
            continue;
        /* Now see if the item is refcount locked */
        if (refcount_incr(&search->refcount) != 2) { //引用计数>=3  //引用数，还有其他线程在引用，不能霸占这个item  
            /* Avoid pathological case with ref'ed items in tail */
			//刷新这个item的访问时间以及在LRU队列中的位置
            do_item_update_nolock(search);
            tries_lrutail_reflocked--;
            tries++;
            refcount_decr(&search->refcount);

			//此时引用数>=2
            itemstats[id].lrutail_reflocked++;
            /* Old rare bug could cause a refcount leak. We haven't seen
             * it in years, but we leave this code in to prevent failures
             * just in case */
            if (settings.tail_repair_time &&
                    search->time + settings.tail_repair_time < current_time) {
                itemstats[id].tailrepairs++;
                search->refcount = 1;
                do_item_unlink_nolock(search, hv);
            }
            if (hold_lock)
                item_trylock_unlock(hold_lock);

            if (tries_lrutail_reflocked < 1)
                break;

            continue;
        }


//search指向的item的refcount等于2，这说明此时这个item除了本worker线程外，没有其他任何worker线程索引其。可以放心释放并重用这个item  
          
//因为这个循环是从lru链表的后面开始遍历的。所以一开始search就指向了最不常用的item，如果这个item都没有过期。那么其他的比其更常用  
//的item就不要删除了(即使它们过期了)。此时只能向slabs申请内存   
        /* Expired or flushed */
        if ((search->exptime != 0 && search->exptime < current_time)
            || (search->time <= oldest_live && oldest_live <= current_time)) { 
            ////search指向的item是一个过期失效的item，可以使用之  
            itemstats[id].reclaimed++;
            if ((search->it_flags & ITEM_FETCHED) == 0) {
                itemstats[id].expired_unfetched++;
            }
            it = search;
            //重新计算一下这个slabclass_t分配出去的内存大小直接霸占旧的item就需要重新计算  
            slabs_adjust_mem_requested(it->slabs_clsid, ITEM_ntotal(it), ntotal); 
            do_item_unlink_nolock(it, hv);  
            /* Initialize the item block: */
            it->slabs_clsid = 0;
        } else if ((it = slabs_alloc(ntotal, id)) == NULL) { //该id对于的slab中没有过期的item，则从slab重新获取一个item
            tried_alloc = 1;
            if (settings.evict_to_free == 0) { //设置了不进行LRU淘汰item  
                itemstats[id].outofmemory++; //此时只能向客户端回复错误了  
            } else {
                //此刻，过期失效的item没有找到，申请内存又失败了。看来只能使用  
                //LRU淘汰一个item(即使这个item并没有过期失效)  
                itemstats[id].evicted++;//增加被踢的item数  
                itemstats[id].evicted_time = current_time - search->time;
                if (search->exptime != 0) //即使一个item的exptime成员设置为永不超时(0)，还是会被踢的  
                    itemstats[id].evicted_nonzero++;
                if ((search->it_flags & ITEM_FETCHED) == 0) { 
                    itemstats[id].evicted_unfetched++;
                }
                it = search;
                slabs_adjust_mem_requested(it->slabs_clsid, ITEM_ntotal(it), ntotal);
                do_item_unlink_nolock(it, hv);
                /* Initialize the item block: */
                it->slabs_clsid = 0;

                /* If we've just evicted an item, and the automover is set to
                 * angry bird mode, attempt to rip memory into this slab class.
                 * TODO: Move valid object detection into a function, and on a
                 * "successful" memory pull, look behind and see if the next alloc
                 * would be an eviction. Then kick off the slab mover before the
                 * eviction happens.
                 */
                //一旦发现有item被踢，那么就启动内存页重分配操作  
                //这个太频繁了，不推荐   
                if (settings.slab_automove == 2)
                    slabs_reassign(-1, id);
            }
        }

        //引用计数减一。此时该item已经没有任何worker线程索引其，并且哈希表也  
        //不再索引其  
        refcount_decr(&search->refcount);
        /* If hash values were equal, we don't grab a second lock */
        if (hold_lock)
            item_trylock_unlock(hold_lock);
        break;
    }

    if (!tried_alloc && (tries == 0 || search == NULL))
        it = slabs_alloc(ntotal, id);

    if (it == NULL) {
        itemstats[id].outofmemory++;
        mutex_unlock(&cache_lock);
        return NULL;
    }

    assert(it->slabs_clsid == 0);
    assert(it != heads[id]);

    /* Item initialization can happen outside of the lock; the item's already
     * been removed from the slab LRU.
     */
    it->refcount = 1;     /* the caller will have a reference */  //新开盘的默认初值为1
    mutex_unlock(&cache_lock);
    it->next = it->prev = it->h_next = 0;
    it->slabs_clsid = id;

    DEBUG_REFCNT(it, '*');
    it->it_flags = settings.use_cas ? ITEM_CAS : 0;
    it->nkey = nkey;
    it->nbytes = nbytes;  //set cas等命令行中的expire保存到it->expire  cas保存在it->data->case中的
    memcpy(ITEM_key(it), key, nkey);
    it->exptime = exptime;
    memcpy(ITEM_suffix(it), suffix, (size_t)nsuffix);
    it->nsuffix = nsuffix; //实际分配的item空间大小为it->nkey +1 + it->nsuffix +it->nbytes
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
    clsid = it->slabs_clsid;
    it->slabs_clsid = 0;
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
static void item_link_q(item *it) { /* item is the new head */
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
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

//将it从对应的LRU队列中删除  //将item从对应classid的LRU链上移除
static void item_unlink_q(item *it) {
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
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

////将item加入到hashtable并加入到对应classid的LRU链中。
int do_item_link(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    mutex_lock(&cache_lock);
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
    item_link_q(it); //加入到LRU队列
    refcount_incr(&it->refcount); //增加refcount引用数
    mutex_unlock(&cache_lock);

    return 1;
}

/*
当一个item插入到哈希表和LRU队列后，那么这个item就被哈希表和LRU队列所引用了。此时，如果没有其他线程在引用这个item的话，
那么这个item的引用数为1(哈希表和LRU队列看作一个引用)。所以一个worker线程要删除一个item(当然在删除前这个worker线程要
占有这个item)，那么需要减少两次item的引用数，一次是减少哈希表和LRU队列的引用，另外一次是减少自己的引用。所以经常能
在代码中看到删除一个item需要调用函数do_item_unlink (it, hv)和do_item_remove(it)这两个函数。
*/
//将item从hashtable和LRU链中移除。是do_item_link的逆操作， do_item_unlink解除与hash和lru的关联，真正释放item在do_item_remove
void do_item_unlink(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    mutex_lock(&cache_lock);
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
    mutex_unlock(&cache_lock);  
}

/* FIXME: Is it necessary to keep this copy/pasted code? */
//从hash和item中删除
void do_item_unlink_nolock(item *it, const uint32_t hv) {
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
 * _nolock is only used in an uncommon case. */
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
            item_unlink_q(it);
			//更新访问时间
            it->time = current_time;
			//插入到LRU队列的头部
            item_link_q(it);
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

        mutex_lock(&cache_lock);
        if ((it->it_flags & ITEM_LINKED) != 0) {
            item_unlink_q(it);
            it->time = current_time;
            item_link_q(it);
        }
        mutex_unlock(&cache_lock);
    }
}

//将item用new_item代替
int do_item_replace(item *it, item *new_it, const uint32_t hv) {  //注意这里面old_it->refcount会减1，new_it会增1
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                           ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}

/*@null@*/
char *do_item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes) {
    unsigned int memlimit = 2 * 1024 * 1024;   /* 2MB max response size */
    char *buffer;
    unsigned int bufcurr;
    item *it;
    unsigned int len;
    unsigned int shown = 0;
    char key_temp[KEY_MAX_LENGTH + 1];
    char temp[512];

    it = heads[slabs_clsid];

    buffer = malloc((size_t)memlimit);
    if (buffer == 0) return NULL;
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
    return buffer;
}

void item_stats_evictions(uint64_t *evicted) {
    int i;
    mutex_lock(&cache_lock);
    for (i = 0; i < LARGEST_ID; i++) {
        evicted[i] = itemstats[i].evicted;
    }
    mutex_unlock(&cache_lock);
}

void do_item_stats_totals(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    memset(&totals, 0, sizeof(itemstats_t));
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        totals.expired_unfetched += itemstats[i].expired_unfetched;
        totals.evicted_unfetched += itemstats[i].evicted_unfetched;
        totals.evicted += itemstats[i].evicted;
        totals.reclaimed += itemstats[i].reclaimed;
        totals.crawler_reclaimed += itemstats[i].crawler_reclaimed;
        totals.lrutail_reflocked += itemstats[i].lrutail_reflocked;
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
    APPEND_STAT("lrutail_reflocked", "%llu",
                (unsigned long long)totals.lrutail_reflocked);
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
void do_item_stats(ADD_STAT add_stats, void *c) {
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        if (tails[i] != NULL) { //只有对应的slab上面包含有有效item才会进行统计
            const char *fmt = "items:%d:%s";
            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;
            if (tails[i] == NULL) {
                /* We removed all of the items in this slab class */
                continue;
            }
            APPEND_NUM_FMT_STAT(fmt, i, "number", "%u", sizes[i]);
            APPEND_NUM_FMT_STAT(fmt, i, "age", "%u", current_time - tails[i]->time);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted",
                                "%llu", (unsigned long long)itemstats[i].evicted);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted_nonzero",
                                "%llu", (unsigned long long)itemstats[i].evicted_nonzero);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted_time",
                                "%u", itemstats[i].evicted_time);
            APPEND_NUM_FMT_STAT(fmt, i, "outofmemory",
                                "%llu", (unsigned long long)itemstats[i].outofmemory);
            APPEND_NUM_FMT_STAT(fmt, i, "tailrepairs",
                                "%llu", (unsigned long long)itemstats[i].tailrepairs);
            APPEND_NUM_FMT_STAT(fmt, i, "reclaimed",
                                "%llu", (unsigned long long)itemstats[i].reclaimed);
            APPEND_NUM_FMT_STAT(fmt, i, "expired_unfetched",
                                "%llu", (unsigned long long)itemstats[i].expired_unfetched);
            APPEND_NUM_FMT_STAT(fmt, i, "evicted_unfetched",
                                "%llu", (unsigned long long)itemstats[i].evicted_unfetched);
            APPEND_NUM_FMT_STAT(fmt, i, "crawler_reclaimed",
                                "%llu", (unsigned long long)itemstats[i].crawler_reclaimed);
            APPEND_NUM_FMT_STAT(fmt, i, "lrutail_reflocked",
                                "%llu", (unsigned long long)itemstats[i].lrutail_reflocked);
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
void do_item_stats_sizes(ADD_STAT add_stats, void *c) {

    /* max 1MB object, divided into 32 bytes size buckets */
    const int num_buckets = 32768;
    unsigned int *histogram = calloc(num_buckets, sizeof(int));

    if (histogram != NULL) {
        int i;

        /* build the histogram */
        for (i = 0; i < LARGEST_ID; i++) {
            item *iter = heads[i];
            while (iter) {
                int ntotal = ITEM_ntotal(iter);
                int bucket = ntotal / 32;
                if ((ntotal % 32) != 0) bucket++;
                if (bucket < num_buckets) histogram[bucket]++;
                iter = iter->next;
            }
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
////调用do_item_get的函数都已经加上了item_lock(hv)段级别锁或者全局锁  
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv) {
    //mutex_lock(&cache_lock);
    item *it = assoc_find(key, nkey, hv); //assoc_find函数内部没有加锁
    if (it != NULL) {
        refcount_incr(&it->refcount);
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock order
         * of item_lock, cache_lock, slabs_lock. */
         //这个item刚好在要移动的内存页里面。此时不能返回这个item  
            //worker线程要负责把这个item从哈希表和LRU队列中删除这个item，避免  
            //后面有其他worker线程又访问这个不能使用的item  
        if (slab_rebalance_signal &&
            ((void *)it >= slab_rebal.slab_start && (void *)it < slab_rebal.slab_end)) {
            do_item_unlink_nolock(it, hv);//引用计数会减一
            do_item_remove(it);//引用计数减一，如果引用计数等于0，就删除
            it = NULL;
        }
    }
    //mutex_unlock(&cache_lock);
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
        if (settings.oldest_live != 0 && settings.oldest_live <= current_time &&
            it->time <= settings.oldest_live) {//flush_all命令清除
            do_item_unlink(it, hv);//引用计数会减一  
            do_item_remove(it);//引用计数减一,如果引用计数等于0，就删除
            it = NULL;
            if (was_found) {
                fprintf(stderr, " -nuked by flush");
            }
        } else if (it->exptime != 0 && it->exptime <= current_time) {//该item已经过期失效了  
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            if (was_found) {
                fprintf(stderr, " -nuked by expire");
            }
        } else {
            it->it_flags |= ITEM_FETCHED;//把这个item标志为被访问过的  
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

/*
当worker线程接收到flush_all命令后，会用全局变量settings的oldest_live成员存储接收到这个命令那一刻的时间(准确地说，
是worker线程解析得知这是一个flush_all命令那一刻再减一)，代码为settings.oldest_live =current_time - 1;然后调用
item_flush_expired函数锁上cache_lock，然后调用do_item_flush_expired函数完成工作。
    惰性删除可以参考do_item_get中的if (settings.oldest_live != 0 && settings.oldest_live <= current_time int i;
do_item_get函数外，do_item_alloc函数也是会处理过期失效item的

flush_all命令是可以有时间参数的。这个时间和其他时间一样取值范围是 1到REALTIME_MAXDELTA(30天)。如果命令为flush_all 100，
那么99秒后所有的item失效。此时settings.oldest_live的值为current_time+100-1，do_item_flush_expired函数也没有什么用了
(总不会被抢占CPU99秒吧)。也正是这个原因，需要在do_item_get里面，加入settings.oldest_live<= current_time这个判断，防止过早删除了item。
*/

/* expires items that are more recent than the oldest_live setting. */
void do_item_flush_expired(void) { 
//flush_all清除当前系统中所有的item，实际是item->time比当前时间小的还是存在于item中，知识把访问时间比settings.oldest_live大的清除了
//这是一种巧妙算法，因为访问时间item->time比当前时间settings.oldest_live小的item全部通过惰性删除实现，例如do_item_get中的if (settings.oldest_live != 0 && settings.oldest_live <= current_time 
    int i;
    item *iter, *next;
    if (settings.oldest_live == 0)
        return;
    for (i = 0; i < LARGEST_ID; i++) {
        /* The LRU is sorted in decreasing time order, and an item's timestamp
         * is never newer than its last access time, so we only need to walk
         * back until we hit an item older than the oldest_live time.
         * The oldest_live checking will auto-expire the remaining items.
         */
        for (iter = heads[i]; iter != NULL; iter = next) {

            /*
                //iter->time == 0的是lru爬虫item，直接忽略  
                //一般情况下iter->time是小于settings.oldest_live的。但在这种情况下  
                //就有可能出现iter->time >= settings.oldest_live :  worker1接收到  
                //flush_all命令，并给settings.oldest_live赋值为current_time-1。  
                //worker1线程还没来得及调用item_flush_expired函数，就被worker2  
                //抢占了cpu，然后worker2往lru队列插入了一个item。这个item的time  
                //成员就会满足iter->time >= settings.oldest_live  
            */
            
            /* iter->time of 0 are magic objects. */
            if (iter->time != 0 && iter->time >= settings.oldest_live) {
                next = iter->next;
                if ((iter->it_flags & ITEM_SLABBED) == 0) { //说明该节点没有归还给slab，则需要归还给slab
                    //虽然调用的是nolock,但本函数的调用者item_flush_expired  
                    //已经锁上了cache_lock，才调用本函数的  
                    do_item_unlink_nolock(iter, hash(ITEM_key(iter), iter->nkey));
                }
            } else {
                /* We've hit the first old item. Continue to the next queue. */
                break;
            }
        }
    }
}

static void crawler_link_q(item *it) { /* item is the new tail */
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
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
    assert(it->slabs_clsid < LARGEST_ID);
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
    assert(it->slabs_clsid < LARGEST_ID);
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
    rel_time_t oldest_live = settings.oldest_live;
    if ((search->exptime != 0 && search->exptime < current_time)//这个item的exptime时间戳到了，已经过期失效了  
        || (search->time <= oldest_live && oldest_live <= current_time)) { //因为客户端发送flush_all命令，导致这个item失效了    
        itemstats[i].crawler_reclaimed++;

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
        refcount_decr(&search->refcount);
    }
}

/*
可以用命令lru_crawler tocrawl num指定每个LRU队列最多只检查num-1个item。看清楚点，是检查数，不是删除数，
而且是num-1个。首先要调用item_crawler_evaluate函数检查一个item是否过期，是的话就删除。如果检查完num-1个，
伪item都还没有到达LRU队列的头部，那么就直接将这个伪item从LRU队列中删除。
*/
static void *item_crawler_thread(void *arg) {
    int i;

    pthread_mutex_lock(&lru_crawler_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "Starting LRU crawler background thread\n");
    while (do_run_lru_crawler_thread) {
        //等待worker线程指定要处理的LRU队列,也就是等待客户端执行lru_crawler tocrawl <classid,classid,classid|all> 
        //命令”lru_crawler crawl<classid,classid,classid|all>”才是指定任务的。该命令指明了要对哪个LRU队列进行清理
        pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);
        
        while (crawler_count) {//crawler_count表明要处理多少个LRU队列  
            item *search = NULL;
            void *hold_lock = NULL;

            for (i = 0; i < LARGEST_ID; i++) {
                if (crawlers[i].it_flags != 1) {
                    continue;
                }
                pthread_mutex_lock(&cache_lock);
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
                    pthread_mutex_unlock(&cache_lock);
                    continue;
                }
                uint32_t hv = hash(ITEM_key(search), search->nkey);
                /* Attempt to hash item lock the "search" item. If locked, no
                 * other callers can incr the refcount
                 */
                if ((hold_lock = item_trylock(hv)) == NULL) {//尝试锁住控制这个item的哈希表段级别锁  
                    pthread_mutex_unlock(&cache_lock);
                    continue;
                }
                /* Now see if the item is refcount locked */
                if (refcount_incr(&search->refcount) != 2) { //此时有其他worker线程在引用这个item  
                    refcount_decr(&search->refcount); //lru爬虫线程放弃引用该item  
                    if (hold_lock)
                        item_trylock_unlock(hold_lock);
                    pthread_mutex_unlock(&cache_lock);
                    continue;
                }

                /* Frees the item or decrements the refcount. */
                /* Interface for this could improve: do the free/decr here
                 * instead? *///如果这个item过期失效了，那么就删除这个item  
                item_crawler_evaluate(search, hv, i);
                
                if (hold_lock)
                    item_trylock_unlock(hold_lock);
                pthread_mutex_unlock(&cache_lock);
                
                if (settings.lru_crawler_sleep)
                    usleep(settings.lru_crawler_sleep);
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
    settings.lru_crawler = true;

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
    pthread_mutex_unlock(&lru_crawler_lock);

    return 0;
}

/*
lru_crawler_crawl函数，memcached会在这个函数会把伪item插入到LRU队列尾部的。当worker线程接收到lru_crawler 
crawl<classid,classid,classid|all>命令时就会调用这个函数。因为用户可能要求LRU爬虫线程清理多个LRU队列的过
期失效item，所以需要一个伪item数组。伪item数组的大小等于LRU队列的个数，它们是一一对应的
*/

//当客户端使用命令lru_crawler crawl <classid,classid,classid|all>时，  
//worker线程就会调用本函数,并将命令的第二个参数作为本函数的参数  
enum crawler_result_type lru_crawler_crawl(char *slabs) {
    char *b = NULL;
    uint32_t sid = 0;
    uint8_t tocrawl[POWER_LARGEST];

    //LRU爬虫线程进行清理的时候，会锁上lru_crawler_lock。直到完成所有  
    //的清理任务才会解锁。所以客户端的前一个清理任务还没结束前，不能  
    //再提交另外一个清理任务     
    if (pthread_mutex_trylock(&lru_crawler_lock) != 0) {
        return CRAWLER_RUNNING;
    }
    pthread_mutex_lock(&cache_lock);

    //解析命令，如果命令要求对某一个LRU队列进行清理，那么就在tocrawl数组  
    //对应元素赋值1作为标志  
    if (strcmp(slabs, "all") == 0) { //处理全部lru队列  
        for (sid = 0; sid < LARGEST_ID; sid++) {
            tocrawl[sid] = 1;
        }
    } else {
        for (char *p = strtok_r(slabs, ",", &b);//解析出一个个的sid  
             p != NULL;
             p = strtok_r(NULL, ",", &b)) {

            if (!safe_strtoul(p, &sid) || sid < POWER_SMALLEST
                    || sid > POWER_LARGEST) {
                pthread_mutex_unlock(&cache_lock);
                pthread_mutex_unlock(&lru_crawler_lock);
                return CRAWLER_BADCLASS;
            }
            tocrawl[sid] = 1;
        }
    }

    //crawlers是一个伪item类型数组。如果用户要清理某一个LRU队列，那么  
    //就在这个LRU队列中插入一个伪item  
    for (sid = 0; sid < LARGEST_ID; sid++) {
        if (tocrawl[sid] != 0 && tails[sid] != NULL) {
            if (settings.verbose > 2)
                fprintf(stderr, "Kicking LRU crawler off for slab %d\n", sid);
            //对于伪item和真正的item，可以用nkey、time这两个成员的值区别  
            crawlers[sid].nbytes = 0;
            crawlers[sid].nkey = 0;
            crawlers[sid].it_flags = 1; /* For a crawler, this means enabled. */
            crawlers[sid].next = 0;
            crawlers[sid].prev = 0;
            crawlers[sid].time = 0;
            crawlers[sid].remaining = settings.lru_crawler_tocrawl;
            crawlers[sid].slabs_clsid = sid;
            crawler_link_q((item *)&crawlers[sid]); //将这个伪item插入到对应的lru队列的尾部  
            crawler_count++; //要处理的LRU队列数加一  
        }
    }
    pthread_mutex_unlock(&cache_lock);
    //命令”lru_crawler crawl<classid,classid,classid|all>”才是指定任务的。该命令指明了要对哪个LRU队列进行清理
    pthread_cond_signal(&lru_crawler_cond); //有任务了，唤醒LRU爬虫线程，让其执行清理任务  
    STATS_LOCK();
    stats.lru_crawler_running = true;
    STATS_UNLOCK();
    pthread_mutex_unlock(&lru_crawler_lock);
    return CRAWLER_OK;
}

int init_lru_crawler(void) {
    if (lru_crawler_initialized == 0) {
        if (pthread_cond_init(&lru_crawler_cond, NULL) != 0) {
            fprintf(stderr, "Can't initialize lru crawler condition\n");
            return -1;
        }
        pthread_mutex_init(&lru_crawler_lock, NULL);
        lru_crawler_initialized = 1;
    }
    return 0;
}
