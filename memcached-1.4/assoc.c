/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
 */

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
#include <assert.h>
#include <pthread.h>

static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t maintenance_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hash_items_counter_lock = PTHREAD_MUTEX_INITIALIZER;

typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */

/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;

//hashsize(2)为2的幂，所以hashmask的值的二进制形式就是后面全为1的数。这就很像位操作里面的&
//value & hashmask(n)的结果肯定比hashsize(n)小的一个数字，即结果在hash表里面
#define hashsize(n) ((ub4)1<<(n))
//hashmask(n)也可以称为哈希掩码
#define hashmask(n) (hashsize(n)-1)

/* Main hash table. This is where we look except during expansion. */
//哈希表数组指针  当进行hash扩展的时候，开辟新的hash空间，见assoc_expand  之前的旧hash放入old_hashtable
static item** primary_hashtable = 0;

/*
 * Previous hash table. During expansion, we look here for keys that haven't
 * been moved over to the primary yet.
  */ //当进行hash扩展的时候，开辟新的hash空间，见assoc_expand  之前的旧hash放入old_hashtable
static item** old_hashtable = 0;

/* Number of items in the hash table. */
static unsigned int hash_items = 0;

/* Flag: Are we in the middle of expanding now? */
static bool expanding = false; //hash扩展的时候置1，见assoc_expand
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
static unsigned int expand_bucket = 0;

//默认参数为0.本函数由main函数调用，参数的默认值为0
void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;
    }
	//因为哈希表会慢慢增大，所以要使用动态内存分配。哈希表存储的数据是一个
	//指针，这样更省空间。
	//hashsize(hashpower)就是哈希表的长度了
    primary_hashtable = calloc(hashsize(hashpower), sizeof(void *));
    if (! primary_hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);//哈希表是memcached工作的基础，如果失败只能退出运行
    }
    STATS_LOCK();
    stats.hash_power_level = hashpower;
    stats.hash_bytes = hashsize(hashpower) * sizeof(void *);
    STATS_UNLOCK();
}

//由于哈希值只能确定是在哈希表中的哪个桶(bucket)，但一个桶里面是有一条冲突链的
//此时需要用到具体的键值遍历并一一比较冲突链上的所有节点。虽然key是以'\0'结尾的
//字符串，但调用strlen还是有点耗时(需要遍历键值字符串)。所以需要另外一个参数nkey
//指明这个key的长度       用于快速查找该key对应的item，见assoc_find   item插入hash表函数assoc_insert
item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it = old_hashtable[oldbucket];
    } else {
    	//由哈希值判断这个key是属于哪个桶的
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

	//到这里，已经确定这个key是属于哪个桶的，遍历对应桶的冲突链即可
    item *ret = NULL;
    int depth = 0;
    while (it) {
		//长度相同的情况下才调用memcmp比较，更高效
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }
    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */
//查找item。返回前驱结点的h_next成员地址，如果查找失败那么就返回冲突链中最后
//一个节点的h_next成员地址。因为最后一个节点的h_next的值为NULL。通过对返回值
//使用*运算即可知道有没有查找成功
static item** _hashitem_before (const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;

    if (expanding && //正在扩展哈希表
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        pos = &old_hashtable[oldbucket];
    } else {
    	//找到哈希表中对应的桶的位置
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }

	//遍历桶的冲突链查找item
    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }
	//*pos就可以知道有没有查找成功。如果*pos等于NULL那么查找失败，否则查找成功
    return pos;
}

/* grows the hashtable to the next power of 2. */
//扩大哈希表的表长
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;

	//申请一个新哈希表，并用old_hashtable指向旧哈希表
    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));
    if (primary_hashtable) {
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++;
		//标明已经进入扩展状态
        expanding = true;
		//从0号桶开始数据迁移
        expand_bucket = 0;
        STATS_LOCK();
        stats.hash_power_level = hashpower;
        stats.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats.hash_is_expanding = 1;
        STATS_UNLOCK();
    } else {
        primary_hashtable = old_hashtable;
        /* Bad news, but we can keep running. */
    }
}

//assoc_insert函数会调用本函数，当item数量到了哈希表表长的1.5被才会调用 
static void assoc_start_expand(void) {
    if (started_expanding)
        return;

    started_expanding = true;
    pthread_cond_signal(&maintenance_cond);
}

/* Note: this isn't an assoc_update.  The key must not already exist to call this */
//hv是这个item键值的哈希值，用于快速查找该key对应的item，见assoc_find   item插入hash表函数assoc_insert
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket; // 插入hash表函数为assoc_insert  插入lru队列的函数为item_link_q

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */
	//使用头插法，插入一个item
	//第一次看本函数，直接看else部分
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;
    } else {
    	//使用头插法插入哈希表中
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    pthread_mutex_lock(&hash_items_counter_lock);
    hash_items++;//哈希表的item数量加一
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {
        assoc_start_expand();
    }
    pthread_mutex_unlock(&hash_items_counter_lock);

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
	//得到前驱结点的h_next成员地址
    item **before = _hashitem_before(key, nkey, hv);

    if (*before) {//查找成功
        item *nxt;
        pthread_mutex_lock(&hash_items_counter_lock);
        hash_items--;
        pthread_mutex_unlock(&hash_items_counter_lock);
        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey, hash_items);
		//因为before是一个二级指针，其值为所查找item的前驱item的h_next成员地址
		//所以*before指向的是所查找的item。因为before是一个二级指针，所以*before
		//作为左值时，可以给h_next成员变量赋值。所以下面三行代码是
		//使得删除中间的item后，前后的item还能连接起来。
        nxt = (*before)->h_next;
        (*before)->h_next = 0;   /* probably pointless, but whatever. */
        *before = nxt;
        return;
    }
    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(*before != 0);
}


static volatile int do_run_maintenance_thread = 1;

#define DEFAULT_HASH_BULK_MOVE 1
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;

//数据迁移线程回调函数
static void *assoc_maintenance_thread(void *arg) {

	//do_run_maintenance_thread 是全局变量，初始值为1，在stop_assoc_mainternance_thread
	//函数中会被赋值0，之中迁移线程
    while (do_run_maintenance_thread) {
        int ii = 0;

        /* There is only one expansion thread, so no need to global lock. */
         //上锁
		//进行item迁移
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
            void *item_lock = NULL;

            /* bucket = hv & hashmask(hashpower) =>the bucket of hash table
             * is the lowest N bits of the hv, and the bucket of item_locks is
             *  also the lowest M bits of hv, and N is greater than M.
             *  So we can process expanding with only one item_lock. cool! */
            if ((item_lock = item_trylock(expand_bucket))) {
                    for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {
                        next = it->h_next;
                        bucket = hash(ITEM_key(it), it->nkey) & hashmask(hashpower);
                        it->h_next = primary_hashtable[bucket];
                        primary_hashtable[bucket] = it;
                    }

                    old_hashtable[expand_bucket] = NULL;

                    expand_bucket++;
                    if (expand_bucket == hashsize(hashpower - 1)) {
                        expanding = false;
                        free(old_hashtable);
                        STATS_LOCK();
                        stats.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                        stats.hash_is_expanding = 0;
                        STATS_UNLOCK();
                        if (settings.verbose > 1)
                            fprintf(stderr, "Hash table expansion done\n");
                    }

            } else {
                usleep(10*1000);
            }

            if (item_lock) {
                item_trylock_unlock(item_lock);
                item_lock = NULL;
            }
        }

		//不需要迁移数据了
        if (!expanding) {
            /* We are done expanding.. just wait for next invocation */
			// 重置
            started_expanding = false;
			//挂起迁移线程，直到worker线程插入数据后发现item数量已经到了1.5被哈希表大小，
			//此时调用worker线程调用assoc_start_expand函数，该函数会调用pthread_cond_signal唤醒迁移线程
            pthread_cond_wait(&maintenance_cond, &maintenance_lock);
            /* assoc_expand() swaps out the hash table entirely, so we need
             * all threads to not hold any references related to the hash
             * table while this happens.
             * This is instead of a more complex, possibly slower algorithm to
             * allow dynamic hash table expansion without causing significant
             * wait times.
             */
            pause_threads(PAUSE_ALL_THREADS);
            assoc_expand();//申请更大的哈希表，并将expanding设置为true
            pause_threads(RESUME_ALL_THREADS);
        }
    }
    return NULL;
}

//数据迁移线程
static pthread_t maintenance_tid;

//main函数会调用本函数，启动数据迁移线程
int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");
    if (env != NULL) {
		//hash_bulk_move的作用在后面会说到。这里是通过环境变量给hash_bulk_move赋值
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;
        }
    }
    pthread_mutex_init(&maintenance_lock, NULL);
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

void stop_assoc_maintenance_thread() {
    mutex_lock(&maintenance_lock);
    do_run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    mutex_unlock(&maintenance_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}

