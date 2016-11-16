/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/** \file
 * The main memcached header holding commonly used data
 * structures and function prototypes.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <event.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>

#include "protocol_binary.h"
#include "cache.h"

#include "sasl_defs.h"

/** Maximum length of a key. */
#define KEY_MAX_LENGTH 250

/** Size of an incr buf. */
#define INCR_MAX_STORAGE_LEN 24

#define DATA_BUFFER_SIZE 2048
#define UDP_READ_BUFFER_SIZE 65536
#define UDP_MAX_PAYLOAD_SIZE 1400
#define UDP_HEADER_SIZE 8
#define MAX_SENDBUF_SIZE (256 * 1024 * 1024)
/* I'm told the max length of a 64-bit num converted to string is 20 bytes.
 * Plus a few for spaces, \r\n, \0 */
#define SUFFIX_SIZE 24

/** Initial size of list of items being returned by "get". */
#define ITEM_LIST_INITIAL 200

/** Initial size of list of CAS suffixes appended to "gets" lines. */
#define SUFFIX_LIST_INITIAL 20

/** Initial size of the sendmsg() scatter/gather array. */
#define IOV_LIST_INITIAL 400

/** Initial number of sendmsg() argument structures to allocate. */
#define MSG_LIST_INITIAL 10

/** High water marks for buffer shrinking */
#define READ_BUFFER_HIGHWAT 8192
#define ITEM_LIST_HIGHWAT 400
#define IOV_LIST_HIGHWAT 600
#define MSG_LIST_HIGHWAT 100

/* Binary protocol stuff */
#define MIN_BIN_PKT_LENGTH 16
#define BIN_PKT_HDR_WORDS (MIN_BIN_PKT_LENGTH/sizeof(uint32_t))

/* Initial power multiplier for the hash table */
#define HASHPOWER_DEFAULT 16

/*
 * We only reposition items in the LRU queue if they haven't been repositioned
 * in this many seconds. That saves us from churning on frequently-accessed
 * items.
 */
#define ITEM_UPDATE_INTERVAL 60

/* unistd.h is here */
#if HAVE_UNISTD_H
# include <unistd.h>
#endif

/* Slab sizing definitions. */
#define POWER_SMALLEST 1 //从1开始， slabclass数组从1开始，见slabs_init
#define POWER_LARGEST  200
#define CHUNK_ALIGN_BYTES 8
#define MAX_NUMBER_OF_SLAB_CLASSES (POWER_LARGEST + 1)

/** How long an object can reasonably be assumed to be locked before
    harvesting it on a low memory condition. Default: disabled. */
#define TAIL_REPAIR_TIME_DEFAULT 0

/* warning: don't use these macros with a function, as it evals its arg twice */
#define ITEM_get_cas(i) (((i)->it_flags & ITEM_CAS) ? \
        (i)->data->cas : (uint64_t)0)

#define ITEM_set_cas(i,v) { \
    if ((i)->it_flags & ITEM_CAS) { \
        (i)->data->cas = v; \
    } \
}

#define ITEM_key(item) (((char*)&((item)->data)) \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_suffix(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_data(item) ((char*) &((item)->data) + (item)->nkey + 1 \
         + (item)->nsuffix \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define ITEM_ntotal(item) (sizeof(struct _stritem) + (item)->nkey + 1 \
         + (item)->nsuffix + (item)->nbytes \
         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))

#define STAT_KEY_LEN 128
#define STAT_VAL_LEN 128

/** Append a simple stat with a stat name, value format and value */
#define APPEND_STAT(name, fmt, val) \
    append_stat(name, add_stats, c, fmt, val);

/** Append an indexed stat with a stat name (with format), value format
    and value */
#define APPEND_NUM_FMT_STAT(name_fmt, num, name, fmt, val)          \
    klen = snprintf(key_str, STAT_KEY_LEN, name_fmt, num, name);    \
    vlen = snprintf(val_str, STAT_VAL_LEN, fmt, val);               \
    add_stats(key_str, klen, val_str, vlen, c);

/** Common APPEND_NUM_FMT_STAT format. */
#define APPEND_NUM_STAT(num, name, fmt, val) \
    APPEND_NUM_FMT_STAT("%d:%s", num, name, fmt, val)

/**
 * Callback for any function producing stats.
 *
 * @param key the stat's key
 * @param klen length of the key
 * @param val the stat's value in an ascii form (e.g. text form of a number)
 * @param vlen length of the value
 * @parm cookie magic callback cookie
 */
typedef void (*ADD_STAT)(const char *key, const uint16_t klen,
                         const char *val, const uint32_t vlen,
                         const void *cookie);

/*
 * NOTE: If you modify this table you _MUST_ update the function state_text
 */
/**
 * Possible states of a connection.
 */
enum conn_states { //conn所处的状态
    conn_listening,  /**< the socket which listens for connections */
    //主线程accept返回新的fd，通知子线程
    conn_new_cmd,    /**< Prepare connection for next command */
    //等待数据的到来，添加read读事件，然后状态进入conn_read，等待数据到来
    conn_waiting,    /**< waiting for a readable socket */
    conn_read,       /**< reading in a command line */
    //解析读到的数据
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      /**< writing out a simple response */
    conn_nread,      /**< reading in a fixed number of bytes */
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    /**< closing this connection */
    conn_mwrite,     /**< writing out many items sequentially */
    conn_closed,     /**< connection is closed */
    conn_max_state   /**< Max state value (used for assertion) */
};

enum bin_substates {
    bin_no_state,
    bin_reading_set_header,
    bin_reading_cas_header,
    bin_read_set_value,
    bin_reading_get_key,
    bin_reading_stat,
    bin_reading_del_header,
    bin_reading_incr_header,
    bin_read_flush_exptime,
    bin_reading_sasl_auth,
    bin_reading_sasl_auth_data,
    bin_reading_touch_key,
};

enum protocol {
    ascii_prot = 3, /* arbitrary value. */
    binary_prot,
    negotiating_prot /* Discovering the protocol */
};

enum network_transport {
    local_transport, /* Unix sockets*/
    tcp_transport,
    udp_transport
};

/*
    如果只使用一个锁，抢到锁能使用哈希表，抢不到则不能使用。那么memcached的效率将变得相当低。为此，memcached采
用类似数据库的策略：使用不同级别的锁。memcached定义了两个级别的锁：段级别和全局级别。在平时(不进行哈希表扩
展时)，使用段级别的锁。在扩展哈希表时，使用全局级别的锁。
    段级别是什么级别？将哈希表按照几个桶一段几个桶一段地平均分，一个段对应有多个桶，每一个段对应有一个锁。所以
整个哈希表有多个段级别锁。由于段级别锁的数量在程序的一开始就已经确定了，不会再变的了。而随着哈希表的扩展，桶的
数量是会增加的。所以随着哈希表的扩展，越来越多的桶对应一个段，也就是说越来越多的桶对应一个锁。
*/
//参考http://blog.csdn.net/luotuo44/article/details/42913549
enum item_lock_types {//item锁级别  
    ITEM_LOCK_GRANULAR = 0, //段级别  
    ITEM_LOCK_GLOBAL //全局级别  
}; 

#define IS_UDP(x) (x == udp_transport)

//对应 add set replace append prepend cas等命令
#define NREAD_ADD 1
#define NREAD_SET 2
#define NREAD_REPLACE 3
#define NREAD_APPEND 4
#define NREAD_PREPEND 5
#define NREAD_CAS 6

enum store_item_type {
    NOT_STORED=0, STORED, EXISTS, NOT_FOUND
};

enum delta_result_type {
    OK, NON_NUMERIC, EOM, DELTA_ITEM_NOT_FOUND, DELTA_ITEM_CAS_MISMATCH
};

/** Time relative to server start. Smaller than time_t on 64-bit systems. */
typedef unsigned int rel_time_t;

/** Stats stored per slab (and per thread). */
struct slab_stats { //存储于thread_stats->slab_stats
    uint64_t  set_cmds; //set次数
    uint64_t  get_hits;
    uint64_t  touch_hits;
    uint64_t  delete_hits;
    uint64_t  cas_hits;
    uint64_t  cas_badval;
    uint64_t  incr_hits; //inc命令执行次数
    uint64_t  decr_hits; //dec命令执行次数
};

/**
 * Stats stored per-thread.
 */
struct thread_stats { //存储于LIBEVENT_THREAD->stats
    pthread_mutex_t   mutex;
    uint64_t          get_cmds;
    uint64_t          get_misses;
    uint64_t          touch_cmds;
    uint64_t          touch_misses;
    uint64_t          delete_misses;
    uint64_t          incr_misses;
    uint64_t          decr_misses;
    uint64_t          cas_misses; //cas命令没查找到
    uint64_t          bytes_read; //从客户端读取的字节数
    uint64_t          bytes_written;
    uint64_t          flush_cmds;
    //一个线程一次处理命令数超过限制，则自增，见drive_machine
    uint64_t          conn_yields; /* # of yields for connections (-R option)*/
    uint64_t          auth_cmds;
    uint64_t          auth_errors;
    struct slab_stats slab_stats[MAX_NUMBER_OF_SLAB_CLASSES];
};

/**
 * Global stats.
 */
struct stats_t { //struct stats_t stats;
    pthread_mutex_t mutex;
    unsigned int  curr_items; //当前使用item数，见do_item_link
    unsigned int  total_items; //总共使用item数，只加不减
    uint64_t      curr_bytes; //当前占用字节数，见do_item_link
    unsigned int  curr_conns; //当前已使用conn结构数目  conn_new自增，conn_close自减。包括listen使用了的conn
    unsigned int  total_conns; //总共使用过多少conn结构，只增不减
    //超过链接现在从而拒绝链接的链接数
    uint64_t      rejected_conns;
    uint64_t      malloc_fails;
    unsigned int  reserved_fds;
    unsigned int  conn_structs;
    uint64_t      get_cmds;
    uint64_t      set_cmds;
    uint64_t      touch_cmds;
    uint64_t      get_hits;
    uint64_t      get_misses;
    uint64_t      touch_hits;
    uint64_t      touch_misses;
    uint64_t      evictions;
    uint64_t      reclaimed;
    time_t        started;          /* when the process was started */
    bool          accepting_conns;  /* whether we are currently accepting */
    uint64_t      listen_disabled_num;
    //哈希表的长度是2^n。这个值hash_power_level是n的值。
    unsigned int  hash_power_level; /* Better hope it's not over 9000 */
    uint64_t      hash_bytes;       /* size used for hash tables */
    bool          hash_is_expanding; /* If the hash table is being expanded */
    uint64_t      expired_unfetched; /* items reclaimed but never touched */
    uint64_t      evicted_unfetched; /* items evicted but never touched */
    bool          slab_reassign_running; /* slab reassign in progress */ //是否正在进行页迁移 slab_rebalance_finish
    uint64_t      slabs_moved;       /* times slabs were moved around */ //进行页迁移的次数 slab_rebalance_finish
    bool          lru_crawler_running; /* crawl in progress */
};

#define MAX_VERBOSITY_LEVEL 2

/* When adding a setting, be sure to update process_stat_settings */
/**
 * Globally accessible settings as derived from the commandline.
 */
struct settings_s {
    size_t maxbytes; //memcached能够使用的最大内存
    //注意这个包括memcache内部使用的fd，实际上提供给客户端建链的fd要少于这个100多，例如配置-c 1024，实际上最多接收900多个连接
    int maxconns;//最多允许多少个客户端同时在线。不同于setting.backlog
    int port; //监听端口，默认11211
    int udpport; //memcached监听的udp端口
    //套接字创建在server_sockets //创建多少个TCP套接字就会创建多少个udp套接字，IP地址都是一样的
    char *inter;//memcached绑定的ip地址。如果该值为NULL，那么就是INADDR_ANY。否则该值指向一个ip字符串
    //-v   -vv   -vvv参数设置日志打印级别
    int verbose;//运行信息的输出级别。该值越大输出的信息就越详细
    //flush_all命令的时间界限。插入时间小于这个时间的item删除
    rel_time_t oldest_live; /* ignore existing items older than this */
    //标记memcached是否允许LRU淘汰机制。默认是可以的。可以通过-M选项禁止
    int evict_to_free;
    //unix_socket监听的socket路径，默认不使用unix_socket
    char *socketpath;   /* path to unix socket if using local socket */
    //unix socket的权限信息
    int access;  /* access mask (a la chmod) for unix domain socket */
    //item的扩容因子
    double factor;          /* chunk size growth factor */
    int chunk_size;//最小的一个item能存储多少字节的数据(set、add命令中的数据) 默认48
    //worker线程的个数
    int num_threads;        /* number of worker (without dispatcher) libevent threads to run */
    //多少个worker线程为一个udp socket服务
    int num_threads_per_udp; /* number of worker threads serving each udp socket */
    /*
    把各种要存储的键名前面加个前缀(prefix),来标识一定的命名空间,这样各种键名字在本命名空间里面是不能重复的
    (当然也可以重复,但是会把过去的覆盖,:)),
    但是在整个memcache里面可以重复设置,通过这段代码可以查看某些命名空间(prefix)的获取次数、命中率、设置次数等指标。
    */
    //分隔符  可以参考http://www.cnblogs.com/xianbei/archive/2011/01/02/1921258.html
    char prefix_delimiter;  /* character that marks a key prefix (for stats) */ //一般多业务功能的时候有用到
    //http://www.cnblogs.com/xianbei/archive/2011/01/02/1921258.html
    //是否自动收集状态信息  -D参数或者stats detail on
    int detail_enabled;     /* nonzero if we're collecting detailed stats */
    //worker线程连续为某个客户端执行命令的最大命令数。这主要是为了防止一个客户端霸占整个worker线程 生效地方见drive_machine
    int reqs_per_event;     /* Maximum number of io to process on each
                               io-event. */
    //开启CAS业务，如果开启了那么在item里面就会多一个用于CAS的字段。可以在启动memcached的时候通过-C选项禁用
    bool use_cas; //参考do_item_alloc
    //用户命令协议，有文件和二进制两种。negotiating_port是协商，自动根据命令内容判断
    enum protocol binding_protocol;
    int backlog;//listen函数的第二个参数，不同于settings.maxconns
    //slab的内存页大小。单位是字节  默认1M，也就是存储一个key-value最大1M
    int item_size_max;        /* Maximum item size, and upper end for slabs */
    bool sasl;              /* SASL on/off */
    //如果连续数超过了最大同时在线数(由-C选项指定)，是否立即关闭新连接连接上的客户端
    bool maxconns_fast;     /* Whether or not to early close connections */
    //用于指明memcached是否启动了LRU爬虫线程。默认值为false，不启动LRU爬虫线程。
	//可以在启动memcached时通过-o lru_crawler将变量的赋值为true，启动LRU爬虫线程
	//标记是否已经创建爬虫线程，可以避免重复，参考start_item_crawler_thread
    bool lru_crawler;        /* Whether or not to enable the autocrawler thread */
    //是否开启调节不同类型item所占用的内存数。可以通过-o slab_reassign选项开启
    bool slab_reassign;     /* Whether or not slab reassignment is allowed */

    /*
    默认情况下是不开启自动检测功能的，即使在启动memcached的时候加入了-o slab_reassign参数。自动检测功能由
    全局变量settings.slab_automove控制(默认值为0，0就是不开启)。如果要开启可以在启动memcached的时候加入
    slab_automove选项，并将其参数数设置为1。比如命令$memcached -o slab_reassign,slab_automove=1就开启了
    自动检测功能。当然也是可以在启动memcached后通过客户端命令启动automove功能，使用命令slabsautomove <0|1>。
    其中0表示关闭automove，1表示开启automove。客户端的这个命令只是简单地设置settings.slab_automove的值，
    不做其他任何工作。
    */
    
    //自动检测是否需要进行不同类型item的内存调整，依赖于setting.slab_reassign的开启
    //生效地方见slab_maintenance_thread
    int slab_automove;     /* Whether or not to automatically move slabs */ 
    //哈希表的长度是2^n。这个值是n的初始值。可以在启动memcached的时候通过-o hashpower_init设置
	//设置的值要在[12,64]之间。如果不设置，该值为0.哈希表的幂将取默认值16
    int hashpower_init;     /* Starting hash power level */
    //是否支持客户端的关闭命令，该命令会关闭memcached进程
    //当通过-A开启该shutdown功能，当客户端发送命令shutdown过来，memcached进场会发送INT信号给自己，进程退出
    bool shutdown_command; /* allow shutdown command */
    //用于修复item的引用数。如果一个worker线程引用了某个item，还没来得及解除引用这个线程就挂了
	//那么这个item就永远被这个已死的线程所引用而不能释放。memcached用这个值来检测是否出现这种
	//情况。因为这种情况很少发生，所以该变量的默认值为0，即不进行检测
/*
tail_repair_time：
    考虑这样的情况:某个worker线程通过refcount_incr增加了一个item的引用数。但由于某种原因(可能是内核出了问题)，
这个worker线程还没来得及调用refcount_decr就挂了。此时这个item的引用数就肯定不会等于0，也就是总有worker线程占
用着它.但实际上这个worker线程早就挂了。所以对于这种情况需要修复。修复也很多简单：直接把这个item的引用计数赋值为1。
    根据什么判断某一个worker线程挂了呢?首先在memcached里面，一般来说，任何函数都的调用都不会耗时太大的，即使这个函数
需要加锁。所以如果这个item的最后一次访问时间距离现在都比较遥远了，但它却还被一个worker线程所引用，那么就几乎可
以判断这个worker线程挂了。在1.4.16版本之前，这个时间距离都是固定的为3个小时。从1.4.16开就使用settings.tail_repair_time
存储时间距离，可以在启动memcached的时候设置，默认时间距离为1个小时。现在这个版本1.4.21默认都不进行这个修复了，
settings.tail_repair_time的默认值为0。因为memcached的作者很少看到这个bug了，估计是因为操作系统的进一步稳定。

    代码中的settings.tail_repair_time指明有没有开启这种检测，默认是没有开启的(默认值等于0)。可以在启动memcached的时候
通过-o tail_repair_time选项开启
*/
    int tail_repair_time;   /* LRU tail refcount leak repair time */
    //是否允许客户端使用flush_all命令 
    bool flush_enabled;     /* flush_all enabled */
    //hash算法名，见hash_init
    char *hash_algorithm;     /* Hash algorithm in use */
    //LRU爬虫线程工作时的休眠间隔。单位是微妙
    int lru_crawler_sleep;  /* Microsecond sleep between items */
    //LRU爬虫检测每条LRU队列中的多少个item，如果想让LRU爬虫工作必须修改这个值
    //实际上生效是由lru_crawler tocrawl num指定的
    uint32_t lru_crawler_tocrawl; /* Number of items to crawl per run */
};

extern struct stats_t stats;
extern time_t process_started;
extern struct settings_s settings;

//该item插入到LRU队列了
#define ITEM_LINKED 1 //见do_item_link
//该item使用CAS
#define ITEM_CAS 2

/* temp */
//该item还在slab的空闲队列里面，没有分配出去
#define ITEM_SLABBED 4  //见do_slabs_free
//该item 插入到LRU队列后，被worker线程访问过
#define ITEM_FETCHED 8

/*
//item删除机制
一个item在两种情况下会过期失效：1.item的exptime时间戳到了。2.用户使用flush_all命令将全部item变成过期失效的
参考http://blog.csdn.net/luotuo44/article/details/42963793
*/
/**
 * Structure for storing items within memcached.
 */ //真正存储在slabclass[id]中的tunck中的数据格式见item_make_header
typedef struct _stritem {
	//next指针，用于LRU链表
    struct _stritem *next;
	//prev指针，用于LRU链表
    struct _stritem *prev;
	//h_next指针，用于哈希表的冲突链
    struct _stritem *h_next;    /* hash chain next */
	//最后一次访问时间。绝对时间  lru队列里面的item是根据time降序排序的
    rel_time_t      time;       /* least recent access */
	//过期失效时间，绝对时间 一个item在两种情况下会过期失效：1.item的exptime时间戳到了。2.用户使用flush_all命令将全部item变成过期失效的
	rel_time_t      exptime;    /* expire time */
	//本item存放的数据的长度   nbytes是算上了\r\n字符的，见do_item_alloc外层   参考item_make_header中的
    int             nbytes;     /* size of data */

    /*
    可以看到，这是因为减少一个item的引用数可能要删除这个item。为什么呢？考虑这样的情景，线程A因为要读一个item而增加了这个item的
    引用计数，此时线程B进来了，它要删除这个item。这个删除命令是肯定会执行的，而不是说这个item被别的线程引用了就不执行删除命令。
    但又肯定不能马上删除，因为线程A还在使用这个item，此时memcached就采用延迟删除的做法。线程B执行删除命令时减多一次item的引用数，
    使得当线程A释放自己对item的引用后，item的引用数变成0。此时item就被释放了(归还给slab分配器)。
    */
    
	//本item的引用数 在使用do_item_remove函数向slab归还item时，会先测试这个item的引用数是否等于0。
	//引用数可以简单理解为是否有worker线程在使用这个item   记录这个item被引用(被worker线程占用)的总数
	//参考http://blog.csdn.net/luotuo44/article/details/42913549
    unsigned short  refcount;  //在do_item_link中增加  //新开盘的默认初值为1
	//后缀长度，  (nkey + *nsuffix + nbytes)中的nsuffix  参考item_make_header中的
    uint8_t         nsuffix;    /* length of flags-and-length string */
	//item的属性  是否使用cas，settings.use_cas   // item的三种flag: ITEM_SLABBED, ITEM_LINKED,ITEM_CAS
    uint8_t         it_flags;   /* ITEM_* above */
    //该item是从哪个slabclass获取得到
    uint8_t         slabs_clsid;/* which slab class we're in */
	//键值的长度 实际上真实用到的内存是nkey+1,见do_item_alloc  参考item_make_header中的
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    
    /* this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS. */
    union {
        //ITEM_set_cas  只有在开启settings.use_cas才有用
        //ITEM_set_cas   get_cas_id  ITEM_get_cas  每次读取完数据部分后，存储到item中后，stored的时候都会调用do_store_item自增
        uint64_t cas; //参考process_update_command中的req_cas_id,实际是从客户端的set等命令中获取到的
        char end;
    } data[];
    //+ DATA 这后面的就是实际数据 (nkey + *nsuffix + nbytes) 参考item_make_header
    /* if it_flags & ITEM_CAS we have 8 bytes CAS */
    /* then null-terminated key */
    /* then " flags length\r\n" (no terminating null) */
    /* then data with terminating \r\n (no terminating null; it's binary!) */
} item;

//这个结构体和item结构体长得很像,是伪item结构体，用于LRU爬虫 
typedef struct { //赋值和初始化参考lru_crawler_crawl
    struct _stritem *next;
    struct _stritem *prev;
    struct _stritem *h_next;    /* hash chain next */
    rel_time_t      time;       /* least recent access */
    rel_time_t      exptime;    /* expire time */
    int             nbytes;     /* size of data */
    unsigned short  refcount;
    uint8_t         nsuffix;    /* length of flags-and-length string */
    uint8_t         it_flags;   /* ITEM_* above */ //标识是一个爬虫伪item,为1标识需要清除其对于的slabclass，参考item_crawler_thread
    uint8_t         slabs_clsid;/* which slab class we're in */
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    //lru_crawler tocrawl num指定
    uint32_t        remaining;  /* Max keys to crawl per slab per invocation */
} crawler;

//memcached线程结构的封装结构
typedef struct {
    pthread_t thread_id;        /* unique ID of this thread */ //线程id
    //赋值见setup_thread
    struct event_base *base;    /* libevent handle this thread uses */ //线程所使用的event_base
    struct event notify_event;  /* listen event for notify pipe */ //用于监听管道读事件的event
    //主线程和工作子线程通过管道进行通信      工作子线程通过该fd读，见setup_thread->thread_libevent_process
    int notify_receive_fd;      /* receiving end of notify pipe */ //管道的读端fd
    //主线程通过该fd写，见dispatch_conn_new
    int notify_send_fd;         /* sending end of notify pipe */ //管道的写端fd
    struct thread_stats stats;  /* Stats generated by this thread */
    //setup_thread初始化，
    //dispatch_conn_new主线程接收accept客户端返回新fd后创建一个CQ_ITEM放入队列，工作子线程thread_libevent_process从队列取出处理
    struct conn_queue *new_conn_queue; /* queue of new connections to handle */
    cache_t *suffix_cache;      /* suffix cache */
    uint8_t item_lock_type;     /* use fine-grained or global item lock */
} LIBEVENT_THREAD; //static LIBEVENT_THREAD *threads;

typedef struct {
    pthread_t thread_id;        /* unique ID of this thread */
    struct event_base *base;    /* libevent handle this thread uses */
} LIBEVENT_DISPATCHER_THREAD;

/**
 * The structure representing a connection into memcached.
 */
typedef struct conn_t conn;
struct conn_t {
	//该conn对应的socket fd
    int    sfd;
    sasl_conn_t *sasl_conn;
    bool authenticated;
	//当前状态 conn_set_state进行设置    查看状态用state_text
    enum conn_states  state;
    enum bin_substates substate;
    rel_time_t last_cmd_time;//最后一次处理客户端命令key value的时间
	//该conn对应的event
    struct event event;
	//event当前监听的事件类型
    short  ev_flags;
	//触发event回调函数的原因
    short  which;   /** which events were just triggered */

	//读缓冲区    用于存储客户端数据报文中的命令。
    char   *rbuf;   /** buffer to read commands into */
    //
	//有效数据的开始位置。从rbuf到rcurr之间的数据是已经处理的了，变成无效数据了   未解析的命令的字符指针。
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */

	//读缓冲区的长度     rbuf的大小。
	int    rsize;   /** total allocated size of rbuf */
	//有效数据的长度   未解析的命令的长度。
    int    rbytes;  /** how much data, starting from rcur, do we have unparsed */

    char   *wbuf;
    char   *wcurr;
    int    wsize;
    int    wbytes;
    /** which state to go into after finishing current write */
    enum conn_states  write_and_go;//写完后的下一个状态  
    void   *write_and_free; /** free this memory after finishing writing */

	//数据直通车   数据部分赋值过程，赋值见process_update_command和drive_machine
    char   *ritem;  /** when we read in an item's value, it goes here */
    //最开始rlbytes为数据部分总长度，当把数据部分填充好后，其值就刚好为0
    int    rlbytes; //数据部分还差多少 赋值见process_update_command和drive_machine

    /* data for the nread state */

    /**
     * item is used to hold an item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the actual
     * data. The data is read into ITEM_data(item) to avoid extra copying.
     */
    //赋值见process_update_command  
    //该链接当前所处理命令的item结构信息
    void   *item;     /* for commands set/add/replace  */

    /* data for the swallow state */
    int    sbytes;    /* how many bytes to swallow */

    /* data for the mwrite state */
	//iovec数组指针  conn_new创建空间
    struct iovec *iov;
	//数组大小
    int    iovsize;   /* number of elements allocated in iov[] */
	//已经使用的iov数组元素个数
    int    iovused;   /* number of elements used in iov[] */

	//因为msghdr结构体里面的iovec结构体数组长度是有限制的。所以为了
	//能传输更多的数据，只能增加msghdr结构体的个数.add_msghdr函数负责增加
    struct msghdr *msglist; //指向msghdr数组  add_msghdr分配空间
    //数组大小  默认MSG_LIST_INITIAL
    int    msgsize;   /* number of elements allocated in msglist[] */
	//已经使用了的msghdr元素个数
    int    msgused;   /* number of elements used in msglist[] */
	//正在用sendmsg函数传输msghdr数组中的哪一个元素
    int    msgcurr;   /* element in msglist[] being transmitted now */
	//msgcurr指向的msghdr总共多少个字节
    int    msgbytes;  /* number of bytes in current msg */

	//worker线程需要占有这个item，直至把item的数据都写回给客户端了
	//故需要一个item指针数组记录本conn占有的item
    item   **ilist;   /* list of items to write out */
	//数组的大小
    int    isize;
	//当前使用到的item(在释放占用item时会用到)
    item   **icurr;
	//ilist数组中有多少个item需要释放
    int    ileft;

    char   **suffixlist;
    int    suffixsize;
    char   **suffixcurr;
    int    suffixleft;

    //settings.binding_protocol //根据接收的第一个字符来确定是那种协议try_read_command
    enum protocol protocol;   /* which protocol this connection speaks */
    
    enum network_transport transport; /* what transport is used by this connection */

    /* data for UDP clients */
    int    request_id; /* Incoming UDP request ID, if this is a UDP "connection" */
    //客户端IP地址，见conn_new
    struct sockaddr_in6 request_addr; /* udp: Who sent the most recent request */
    socklen_t request_addr_size;
    unsigned char *hdrbuf; /* udp packet headers */
    int    hdrsize;   /* number of headers' worth of space is allocated */

    //是否不用回复客户端信息，set_noreply_maybe
    bool   noreply;   /* True if the reply should not be sent. */
    /* current stats command */
    struct {
        char *buffer;
        size_t size;
        size_t offset;
    } stats;

    /* Binary protocol stuff */
    /* This is where the binary header goes */
    protocol_binary_request_header binary_header;
    uint64_t cas; /* the cas to return */
    //NREAD_SET等，表示什么命令
    short cmd; /* current command being processed */
    int opaque;
    int keylen;
    //指向下一个conn
    conn   *next;     /* Used for generating a list of conn structures */
	//这个conn属于哪个worker线程
    LIBEVENT_THREAD *thread; /* Pointer to the thread object serving this connection */
};

/* array of conn structures, indexed by file descriptor */
extern conn **conns;

/* current time of day (updated periodically) */
extern volatile rel_time_t current_time;

/* TODO: Move to slabs.h? */
extern volatile int slab_rebalance_signal;

struct slab_rebalance { //存储在slab_rebal   赋值可以参考slab_rebalance_start  重分页的时候用
    //记录要移动的页的信息。slab_start指向页的开始位置。slab_end指向页  
    //的结束位置。slab_pos则记录当前处理的位置(item)  
    void *slab_start;
    void *slab_end;
    void *slab_pos;
    //进行automove的源slabclass[]id和目的slabclass[]id
    int s_clsid;//源slab class的下标索引  
    int d_clsid;//目标slab class的下标索引  
    int busy_items;//是否worker线程在引用某个item,记录引用数   slab迁移过程中正在被其他线程访问的trunk数，赋值见slab_rebalance_move
    uint8_t done;//是否完成了内存页移动  
};

extern struct slab_rebalance slab_rebal;

/*
 * Functions
 */
void do_accept_new_conns(const bool do_accept);
enum delta_result_type do_add_delta(conn *c, const char *key,
                                    const size_t nkey, const bool incr,
                                    const int64_t delta, char *buf,
                                    uint64_t *cas, const uint32_t hv);
enum store_item_type do_store_item(item *item, int comm, conn* c, const uint32_t hv);
conn *conn_new(const int sfd, const enum conn_states init_state, const int event_flags, const int read_buffer_size, enum network_transport transport, struct event_base *base);
extern int daemonize(int nochdir, int noclose);

static inline int mutex_lock(pthread_mutex_t *mutex)
{
    while (pthread_mutex_trylock(mutex));
    return 0;
}

#define mutex_unlock(x) pthread_mutex_unlock(x)

#include "stats.h"
#include "slabs.h"
#include "assoc.h"
#include "items.h"
#include "trace.h"
#include "hash.h"
#include "util.h"

/*
 * Functions such as the libevent-related calls that need to do cross-thread
 * communication in multithreaded mode (rather than actually doing the work
 * in the current thread) are called via "dispatch_" frontends, which are
 * also #define-d to directly call the underlying code in singlethreaded mode.
 */

void thread_init(int nthreads, struct event_base *main_base);
int  dispatch_event_add(int thread, conn *c);
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags, int read_buffer_size, enum network_transport transport);

/* Lock wrappers for cache functions that are called from main loop. */
enum delta_result_type add_delta(conn *c, const char *key,
                                 const size_t nkey, const int incr,
                                 const int64_t delta, char *buf,
                                 uint64_t *cas);
void accept_new_conns(const bool do_accept);
conn *conn_from_freelist(void);
bool  conn_add_to_freelist(conn *c);
int   is_listen_thread(void);
item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes);
char *item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes);
void  item_flush_expired(void);
item *item_get(const char *key, const size_t nkey);
item *item_touch(const char *key, const size_t nkey, uint32_t exptime);
int   item_link(item *it);
void  item_remove(item *it);
int   item_replace(item *it, item *new_it, const uint32_t hv);
void  item_stats(ADD_STAT add_stats, void *c);
void  item_stats_totals(ADD_STAT add_stats, void *c);
void  item_stats_sizes(ADD_STAT add_stats, void *c);
void  item_unlink(item *it);
void  item_update(item *it);

void item_lock_global(void);
void item_unlock_global(void);
void item_lock(uint32_t hv);
void *item_trylock(uint32_t hv);
void item_trylock_unlock(void *arg);
void item_unlock(uint32_t hv);
void switch_item_lock_type(enum item_lock_types type);
unsigned short refcount_incr(unsigned short *refcount);
unsigned short refcount_decr(unsigned short *refcount);
void STATS_LOCK(void);
void STATS_UNLOCK(void);
void threadlocal_stats_reset(void);
void threadlocal_stats_aggregate(struct thread_stats *stats);
void slab_stats_aggregate(struct thread_stats *stats, struct slab_stats *out);

/* Stat processing functions */
void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...);

enum store_item_type store_item(item *item, int comm, conn *c);

#if HAVE_DROP_PRIVILEGES
extern void drop_privileges(void);
#else
#define drop_privileges()
#endif

/* If supported, give compiler hints for branch prediction. */
#if !defined(__GNUC__) || (__GNUC__ == 2 && __GNUC_MINOR__ < 96)
#define __builtin_expect(x, expected_value) (x)
#endif

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
