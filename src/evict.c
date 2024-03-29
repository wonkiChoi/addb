/* Maxmemory directive handling (LRU eviction and other policies).
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "circular_queue.h"
#include "stl.h"

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 *
 * Entries inside the eviciton pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 *
 * Empty entries have the key pointer set to NULL. */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) */
    sds key;                    /* Key name. */
    sds cached;                 /* Cached SDS object for key name. */
    int dbid;                   /* Key DB number. */
};

static struct evictionPoolEntry *EvictionPoolLRU;

unsigned long LFUDecrAndReturn(robj *o);

/* ----------------------------------------------------------------------------
 * Implementation of eviction, aging and LRU
 * --------------------------------------------------------------------------*/

/* Return the LRU clock, based on the clock resolution. This is a time
 * in a reduced-bits format that can be used to set and check the
 * object->lru field of redisObject structures. */
unsigned int getLRUClock(void) {
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;
}

/* This function is used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. */
unsigned int LRU_CLOCK(void) {
    unsigned int lruclock;
    if (1000/server.hz <= LRU_CLOCK_RESOLUTION) {
        atomicGet(server.lruclock,lruclock);
    } else {
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
}

/* freeMemoryIfNeeded() gets called when 'maxmemory' is set on the config
 * file to limit the max memory used by the server, before processing a
 * command.
 *
 * The goal of the function is to free enough memory to keep Redis under the
 * configured memory limit.
 *
 * The function starts calculating how many bytes should be freed to keep
 * Redis under the limit, and enters a loop selecting the best keys to
 * evict accordingly to the configured policy.
 *
 * If all the bytes needed to return back under the limit were freed the
 * function returns C_OK, otherwise C_ERR is returned, and the caller
 * should block the execution of commands that will result in more memory
 * used by the server.
 *
 * ------------------------------------------------------------------------
 *
 * LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

/* Create a new eviction pool. */
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*EVPOOL_SIZE);
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL,EVPOOL_CACHED_SDS_SIZE);
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is an helper function for freeMemoryIfNeeded(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time smaller than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */

void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    dictEntry *samples[server.maxmemory_samples];

    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);
    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) de = dictFind(keydict, key);
            o = dictGetVal(de);
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. */
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            idle = estimateObjectIdleTime(o);
        } else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. */
            idle = 255-LFUDecrAndReturn(o);
        } else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. */
            idle = ULLONG_MAX - (long)dictGetVal(de);
        } else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        while (k < EVPOOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
        if (k == 0 && pool[EVPOOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            continue;
        } else if (k < EVPOOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. */
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            if (pool[EVPOOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */

                /* Save SDS before overwriting. */
                sds cached = pool[EVPOOL_SIZE-1].cached;
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(EVPOOL_SIZE-k-1));
                pool[k].cached = cached;
            } else {
                /* No free space on right? Insert at k-1 */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                sds cached = pool[0].cached; /* Save SDS before overwriting. */
                if (pool[0].key != pool[0].cached) sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
                pool[k].cached = cached;
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimizbla bla bla bla. */
        int klen = sdslen(key);
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            pool[k].key = sdsdup(key);
        } else {
            memcpy(pool[k].cached,key,klen+1);
            sdssetlen(pool[k].cached,klen);
            pool[k].key = pool[k].cached;
        }
        pool[k].idle = idle;
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.

 * We have 24 total bits of space in each object in order to implement
 * an LFU (Least Frequently Used) eviction policy, since we re-use the
 * LRU field for this purpose.
 *
 * We split the 24 bits into two fields:
 *
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 *
 * So the remaining 16 bits are used in order to store the "decrement time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter is halved if it has an high value, or just decremented if it
 * has a low value.
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at COUNTER_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of COUNTER_INIT_VAL
 * when incrementing the key, so that keys starting at COUNTER_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access.
 *
 * During decrement, the value of the logarithmic counter is halved if
 * its current value is greater than two times the COUNTER_INIT_VAL, otherwise
 * it is just decremented by one.
 * --------------------------------------------------------------------------*/

/* Return the current time in minutes, just taking the least significant
 * 16 bits. The returned time is suitable to be stored as LDT (last decrement
 * time) for the LFU implementation. */
unsigned long LFUGetTimeInMinutes(void) {
    return (server.unixtime/60) & 65535;
}

/* Given an object last decrement time, compute the minimum number of minutes
 * that elapsed since the last decrement. Handle overflow (ldt greater than
 * the current 16 bits minutes time) considering the time as wrapping
 * exactly once. */
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();
    if (now >= ldt) return now-ldt;
    return 65535-ldt+now;
}

/* Logarithmically increment a counter. The greater is the current counter value
 * the less likely is that it gets really implemented. Saturate it at 255. */
uint8_t LFULogIncr(uint8_t counter) {
    if (counter == 255) return 255;
    double r = (double)rand()/RAND_MAX;
    double baseval = counter - LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    double p = 1.0/(baseval*server.lfu_log_factor+1);
    if (r < p) counter++;
    return counter;
}

/* If the object decrement time is reached, decrement the LFU counter and
 * update the decrement time field. Return the object frequency counter.
 *
 * This function is used in order to scan the dataset for the best object
 * to fit: as we check for the candidate, we incrementally decrement the
 * counter of the scanned objects if needed. */
#define LFU_DECR_INTERVAL 1
unsigned long LFUDecrAndReturn(robj *o) {
    unsigned long ldt = o->lru >> 8;
    unsigned long counter = o->lru & 255;
    if (LFUTimeElapsed(ldt) >= server.lfu_decay_time && counter) {
        if (counter > LFU_INIT_VAL*2) {
            counter /= 2;
            if (counter < LFU_INIT_VAL*2) counter = LFU_INIT_VAL*2;
        } else {
            counter--;
        }
        o->lru = (LFUGetTimeInMinutes()<<8) | counter;
    }
    return counter;
}

/* ----------------------------------------------------------------------------
 * The external API for eviction: freeMemroyIfNeeded() is called by the
 * server when there is data to add in order to make space if needed.
 * --------------------------------------------------------------------------*/

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size. This function
 * returns the sum of AOF and slaves buffer. */
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;
    int slaves = listLength(server.slaves);

    if (slaves) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = listNodeValue(ln);
            overhead += getClientOutputBufferMemoryUsage(slave);
        }
    }
    if (server.aof_state != AOF_OFF) {
        overhead += sdslen(server.aof_buf)+aofRewriteBufferSize();
    }
    return overhead;
}

//int freeMemoryIfNeeded(void) {
//    size_t mem_reported, mem_used, mem_tofree, mem_freed, freed_key;
//    mstime_t latency, eviction_latency;
//    long long delta;
//    int slaves = listLength(server.slaves);
//    int isClear = 0;
//    int isPersisted = 0;
//    int isFlushed = 0;
//    int isEnd = 0;
//    int victim_free = 0;
//
//    /* When clients are paused the dataset should be static not just from the
//     * POV of clients not being able to write, but also from the POV of
//     * expires and evictions of keys not being performed. */
//    if (clientsArePaused()) return C_OK;
//
//    /* Check if we are over the memory usage limit. If we are not, no need
//     * to subtract the slaves output buffers. We can just return ASAP. */
//    mem_reported = zmalloc_used_memory();
//    if (mem_reported <= server.maxmemory) return C_OK;
//
//    /* Remove the size of slaves output buffers and AOF buffer from the
//     * count of used memory. */
//    mem_used = mem_reported;
//    size_t overhead = freeMemoryGetNotCountedMemory();
//    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;
//
//    /* Check if we are still over the memory limit. */
//    if (mem_used <= server.maxmemory) return C_OK;
//
//    /* Compute how much memory we need to free. */
//    mem_tofree = mem_used - server.maxmemory;
//    mem_freed = 0;
//    freed_key = server.stat_clearkeys;
//
//
//    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION)
//        goto cant_free; /* We need to free memory, but policy forbids. */
//
//    int index = 0;
//    latencyStartMonitor(latency);
//// while (mem_freed < mem_tofree) {
// //   while (!isClear) {
//
//    while (mem_used > server.maxmemory) {
//    	  serverLog(LL_VERBOSE, "[FREE_MEMORY CALLED]- [%d] : maxmemory * 0.8 :%ld, maxmemory : %ld, used memory : %d, mem_tofree : %d, mem_freed : %d",
//    			  index++, server.maxmemory*8/10, server.maxmemory, mem_used, mem_tofree, mem_freed);
//    	  serverLog(LL_DEBUG, "[FREE_MEMORY CALLED]- isFlushed : %d, isClear :%d, isPersisted :%d, isEnd : %d" ,
//    			  isFlushed, isClear, isPersisted, isEnd );
//
//    	 int j, k, i, keys_freed = 0;
//        static int next_db = 0;
//        sds bestkey = NULL;
//        sds victimKey = NULL;
//        int bestdbid;
//        redisDb *db;
//        dict *dict;
//        dictEntry *victim = NULL;
//        dictEntry *de;
//
//        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
//            server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL){
//			for (j = 0; j < server.dbnum; j++) {
//				db = server.db + j;
//
//				if (!isEmpty(db->FreeQueue)) {
//					victim = chooseClearKeyFromQueue_(db->FreeQueue);
//				} else {
//				  serverLog(LL_DEBUG, "free queue empty");
//				}
//				if (!isEmpty(db->EvictQueue)) {
//					de = chooseBestKeyFromQueue_(db->EvictQueue, db->FreeQueue);
//					if (de != NULL) {
//						serverLog(LL_DEBUG,
//								"[DB : %d] Success to choose Candidate Entry",
//								j);
//						bestkey = dictGetKey(de);
//						bestdbid = j;
//						break;
//					}
//				} else {
//					serverLog(LL_DEBUG, "evict queue empty");
//				}
//			}
//        }
//
//        	/* volatile-random and allkeys-random policy */
//        	else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
//        	                 server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
//        	{
//
//        		/* When evicting a random key, we try to evict a key for
//        		 * each DB, so we use the static 'next_db' variable to
//        		 * incrementally visit all DBs. */
//        		for (i = 0; i < server.dbnum; i++) {
//        			j = (++next_db) % server.dbnum;
//        			db = server.db+j;
//        			dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
//        					db->dict : db->expires;
//        			if (dictSize(dict) != 0) {
//        				de = dictGetRandomKey(dict);
//        				bestkey = dictGetKey(de);
//        				bestdbid = j;
//        				break;
//        			}
//        		}
//        	}
//        	/* Finally remove the selected key. */
//        	if(bestkey) {
//        		db = server.db + bestdbid;
//        		robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
//        		propagateExpire(db,keyobj,server.lazyfree_lazy_eviction);
//        		/* We compute the amount of memory freed by db*Delete() alone.
//        		 * It is possible that actually the memory needed to propagate
//        		 * the DEL in AOF and replication link is greater than the one
//        		 * we are freeing removing the key, but we can't account for
//        		 * that otherwise we would never exit the loop.
//        		 *
//        		 * AOF and Output buffer memory will be freed eventually so
//        		 * we only care about memory used by the key space. */
//
//        		delta = (long long) zmalloc_used_memory();
//        		latencyStartMonitor(eviction_latency);
//        		if(server.tiering_enabled){
//        			//Relational Model Eviction
//        			//isPersisted = dbPersistOrClear(db, keyobj);
//        			isPersisted = dbPersist_(db, keyobj);
//        		} else {
//        			if(server.lazyfree_lazy_eviction){
//        				dbAsyncDelete(db,keyobj);
//        			} else {
//        				dbSyncDelete(db,keyobj);
//        			}
//        		}
//        	  latencyEndMonitor(eviction_latency);
//        	  latencyAddSampleIfNeeded("eviction-del",eviction_latency);
//        	  latencyRemoveNestedEvent(latency,eviction_latency);
//
//        	  delta -= (long long) zmalloc_used_memory();
//         	  mem_freed += delta;
//
//        	  server.stat_evictedkeys++;
//        	  notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted", keyobj, db->id);
//        	  if(!server.tiering_enabled) {
//        		  decrRefCount(keyobj);
//        	  }
//        	  keys_freed++;
//
//        	  /* When the memory to free starts to be big enough, we may
//        	   * start spending so much time here that is impossible to
//        	   * deliver data to the slaves fast enough, so we force the
//        	   * transmission here inside the loop. */
//        	  if (slaves) flushSlavesOutputBuffers();
//
//        	  /* Normally our stop condition is the ability to release
//        	   * a fixed, pre-computed amount of memory. However when we
//        	   * are deleting objects in another thread, it's better to
//        	   * check, from time to time, if we already reached our target
//        	   * memory, since the "mem_freed" amount is computed only
//        	   * across the dbAsyncDelete() call, while the thread can
//        	   * release the memory all the time. */
//        	  if (server.lazyfree_lazy_eviction && !(keys_freed % 16)){
//        		  overhead = freeMemoryGetNotCountedMemory();
//        		  mem_used = zmalloc_used_memory();
//        		  mem_used = (mem_used > overhead) ? mem_used-overhead : 0;
//        		  if (mem_used <= server.maxmemory) {
//        			  mem_freed = mem_tofree;
//        		  }
//        	  }
//        	}
//
//			if (victim != NULL) {
//				victimKey = dictGetKey(victim);
//				robj *victimKeyobj = createStringObject(victimKey,
//						sdslen(victimKey));
//				isFlushed = dbClear_(db, victimKeyobj);
//				decrRefCount(victimKeyobj);
//				serverLog(LL_VERBOSE, "CLEAR VICTIM SUCCESS [rear: %d]",
//						db->FreeQueue->rear);
//				victim_free++;
//			} else {
//				serverLog(LL_VERBOSE, "CLEAR VICTIM is NULL [rear: %d]",
//						db->FreeQueue->rear);
//				isEnd = 1;
//			}
//
//        	 if (!keys_freed && !server.tiering_enabled) {
//        		 latencyEndMonitor(latency);
//        		 latencyAddSampleIfNeeded("eviction-cycle",latency);
//        		 goto cant_free; /* nothing to free... */
//        	 }
//
//        	 serverLog(LL_VERBOSE,"clearkeys : %d , freed_key : %d, size : %d, victim_free : %d" ,
//        			 server.stat_clearkeys, freed_key, db->FreeQueue->size, victim_free);
//         	 mem_used = zmalloc_used_memory();
//
//    }
//
//    latencyEndMonitor(latency);
//    latencyAddSampleIfNeeded("eviction-cycle",latency);
//    return C_OK;
//
//cant_free:
//    /* We are here if we are not able to reclaim memory. There is only one
//     * last thing we can try: check if the lazyfree thread has jobs in queue
//     * and wait... */
//    while(bioPendingJobsOfType(BIO_LAZY_FREE)) {
//        if (((mem_reported - zmalloc_used_memory()) + mem_freed) >= mem_tofree)
//            break;
//        usleep(1000);
//    }
//    return C_ERR;
//}

void _batchTiering(redisDb *db, Vector *evict_keys, Vector *evict_relations) {
    int count = server.batch_tiering_size;
    while (!isEmpty(db->EvictQueue) && count > 0) {
        dictEntry *de = chooseBestKeyFromQueue_(db->EvictQueue, db->FreeQueue);
        if (de == NULL) {
            continue;
        }
        sds key = (sds) dictGetKey(de);
        robj *relation = (robj *) dictGetVal(de);
        vectorAdd(evict_keys, (void *) key);
        vectorAdd(evict_relations, (void *) relation);
        count--;
    }
    dbPersistBatch_(db, evict_keys, evict_relations);
    server.stat_evictedkeys += vectorCount(evict_relations);
}

int freeMemoryIfNeeded(void) {
    size_t mem_reported, mem_used, mem_tofree, mem_freed;
    mstime_t latency, eviction_latency;
    long long delta;
    int slaves = listLength(server.slaves);
    int isClear = 0;
    int isPersisted = 0;
    int isFlushed = 0;
    int isEnd = 0;
    int victim_free = 0;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    if (clientsArePaused()) return C_OK;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    mem_reported = zmalloc_used_memory();
    if (mem_reported <= server.maxmemory * 8/10) return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    mem_used = mem_reported;
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;

    /* Check if we are still over the memory limit. */
    if (mem_used <= server.maxmemory * 8/10) return C_OK;

    /* Compute how much memory we need to free. */
    mem_tofree = mem_used - server.maxmemory;
    mem_freed = 0;

    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION)
        goto cant_free; /* We need to free memory, but policy forbids. */

	redisDb *db = server.db;

    if (db->FreeQueue->size < DEFAULT_FREE_QUEUE_SIZE-1) {
        Vector *evict_keys = vectorCreate(STL_TYPE_SDS, INIT_VECTOR_SIZE);
        Vector *evict_relations = vectorCreate(STL_TYPE_ROBJ, INIT_VECTOR_SIZE);
        _batchTiering(db, evict_keys, evict_relations);
    }
    
    //		if (db->EvictQueue->size == 0 && db->FreeQueue->size > 1000000) {
    //			dictEntry * de;
    //			serverLog(LL_VERBOSE, "if constraint");
    //			while ((de = dequeue(db->FreeQueue)) != NULL) {
    //				robj * debug = dictGetVal(de);
    //				serverLog(LL_VERBOSE,"FreeQueue member location = %d", debug->location);
    //			}
    //			serverAssert(0);
    //		}

    if(mem_used > server.maxmemory) {
        serverLog(LL_VERBOSE, "[INFO] : MetaDict size : %d" ,
                zmalloc_size(db->Metadict) + (dictSize(db->Metadict) * sizeof(dictEntry)));
        serverLog(LL_VERBOSE, "[QUEUE] : EvictQueue : %d , FreeQueue : %d",
                db->EvictQueue->size, db->FreeQueue->size);
    }

    int index = 0;
    while (mem_used > server.maxmemory) {
		serverLog(LL_DEBUG,
				"[FREE_MEMORY CALLED]- [%d] : maxmemory * 0.9 :%ld, maxmemory : %ld, used memory : %d, mem_tofree : %d, mem_freed : %d",
				index++, server.maxmemory * 9 / 10, server.maxmemory, mem_used,
				mem_tofree, mem_freed);

		sds victimKey = NULL;
		robj *victVal = NULL;
		dictEntry *victim = NULL;

		if (!isEmpty(db->FreeQueue)) {
			victim = chooseClearKeyFromQueue_(db->FreeQueue);
			/* Finally remove the selected key. */
			if (victim != NULL) {
				victimKey = dictGetKey(victim);
				victVal = dictGetVal(victim);
				serverAssert(victVal->location == LOCATION_PERSISTED);
				robj *victimKeyobj = createStringObject(victimKey,
						sdslen(victimKey));
				//serverLog(LL_VERBOSE, "freed = %s" , victimKey);
				isFlushed = dbClear_(db, victimKeyobj);
				if (isFlushed) {
					serverLog(LL_VERBOSE,"CLEAR FAIL : FreeQueue->size : %d", db->FreeQueue->size);
					serverAssert(0);
				}
				decrRefCount(victimKeyobj);
				serverLog(LL_DEBUG, "CLEAR VICTIM SUCCESS [rear: %d]",
						db->FreeQueue->rear);
				victim_free++;
			} else {
				serverLog(LL_DEBUG, "MayBe flush error victim is NULL");
			}
		} else {
			serverLog(LL_DEBUG, "[FREE QUEUE is Empty] : size = %d, rear = %d, front = %d, max =%d ",
					db->FreeQueue->size, db->FreeQueue->rear, db->FreeQueue->front, db->FreeQueue->max);
			serverLog(LL_DEBUG, "[EVICT QUEUE] : size = %d, rear = %d, front = %d, max =%d ",
					db->EvictQueue->size, db->EvictQueue->rear, db->EvictQueue->front, db->EvictQueue->max);
			serverLog(LL_VERBOSE, "[Memory status] : maxmemory= %ld, used memory = %d", server.maxmemory, mem_used);
			/* TODO(wgchoi): Need to check why freequeue is empty.
			 *      Maybe, Insert speed is much faster than tiering speed.
             *      We handle this issue by some workaround that force-evicts
             *      more relations to RocksDB. */
            Vector *force_evict_keys = vectorCreate(STL_TYPE_SDS,
                                                    INIT_VECTOR_SIZE);
            Vector *force_evict_relations = vectorCreate(STL_TYPE_ROBJ,
                                                         INIT_VECTOR_SIZE);
			_batchTiering(db, force_evict_keys, force_evict_relations);
		}

		mem_used = zmalloc_used_memory();
		if (server.aof_state != AOF_OFF) {
			mem_used -= sdslen(server.aof_buf);
			mem_used -= aofRewriteBufferSize();
		}
	}

    return C_OK;

cant_free:
    /* We are here if we are not able to reclaim memory. There is only one
     * last thing we can try: check if the lazyfree thread has jobs in queue
     * and wait... */
    while(bioPendingJobsOfType(BIO_LAZY_FREE)) {
        if (((mem_reported - zmalloc_used_memory()) + mem_freed) >= mem_tofree)
            break;
        usleep(1000);
    }
    return C_ERR;
}
