/*
 * 2018.5.11
 * (totoro) kem2182@yonsei.ac.kr
 * ADDB Test Commands for relational models
 */

#include "server.h"
#include "sds.h"

#include <assert.h>

/* --*-- Caution --*--
 * Uses these commands for testing only...
 */

/* Add the key to the MetaDict. */
void testDbAddForMeta(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    int retval = dictAdd(db->Metadict, copy, val);

    serverAssertWithInfo(NULL,key,retval == DICT_OK);
    if (server.cluster_enabled) slotToKeyAdd(key);
}

/*
 * testSetMetaCommand
 * Set key, value for the MetaDict.
 * --- Parameters ---
 *  arg1: TableId:PartitionInfo Key
 *  arg2: MetaField Key
 *  arg3: Value
 *
 * --- Usage Examples ---
 *  Parameters:
 *      TableId:PartitionInfo Key: "M:{3:1:2}"
 *          tableId: "3"
 *          partitionInfoId: "1:2"
 *      MetaField Key: CURRENT_RGID(= 0)
 *      Value: 4
 *  Command:
 *      redis-cli> TESTSETMETA M:{3:1:2} 0 4
 *  Results:
 *      redis-cli> OK
 */
void testSetMetaCommand(client *c) {
    sds tablePartitionKey = sdsnew(c->argv[1]->ptr);
    robj *tablePartitionMetaDict = lookupSDSKeyForMetadict(c->db,
                                                           tablePartitionKey);
    if (tablePartitionMetaDict == NULL) {
        tablePartitionMetaDict = createSetObject();
        robj *tablePartitionKeyObj = createStringObject(
                tablePartitionKey, sdslen(tablePartitionKey));
        testDbAddForMeta(c->db, tablePartitionKeyObj, tablePartitionMetaDict);
        decrRefCount(tablePartitionKeyObj);
    }

    sds field = sdsnew(c->argv[2]->ptr);
    sds value = sdsnew(c->argv[3]->ptr);

    serverLog(LL_DEBUG, "DEBUG: tablePartitionKey: %s, field: %s, value: %s",
              tablePartitionKey, field, value);

    hashTypeSet(tablePartitionMetaDict, field, value,
                HASH_SET_TAKE_FIELD & HASH_SET_TAKE_VALUE);
    sdsfree(tablePartitionKey);
    addReply(c, shared.ok);
}

/*
 * testGetMetaCommand
 * Get key, value for the MetaDict.
 * --- Parameters ---
 *  arg1: TableId:PartitionInfo Key
 *  arg2: MetaField Key
 *
 * --- Usage Examples ---
 *  Parameters:
 *      TableId:PartitionInfo Key: "M:{3:1:2}"
 *          tableId: "3"
 *          partitionInfoId: "1:2"
 *      MetaField Key: CURRENT_RGID(= 0)
 *  Command:
 *      redis-cli> TESTGETMETA M:{3:1:2} 0
 *  Results:
 *      Find Case
 *          redis-cli> 4
 *      Non-exist Case
 *          redis-cli> (nil)
 */
void testGetMetaCommand(client *c) {
    sds tablePartitionKey = sdsnew(c->argv[1]->ptr);
    robj *tablePartitionMetaDict = lookupSDSKeyForMetadict(c->db,
                                                           tablePartitionKey);

    if (tablePartitionMetaDict == NULL) {
        addReplyErrorFormat(c, "key [%s] doesn't exist in Meta",
                            tablePartitionKey);
        sdsfree(tablePartitionKey);
        return;
    }

    sds field = sdsnew(c->argv[2]->ptr);
    robj *valueObj = hashTypeGetValueObject(tablePartitionMetaDict, field);

    if (valueObj == NULL) {
        addReply(c, shared.nullbulk);
        return;
    }

    serverLog(LL_DEBUG, "DEBUG: tablePartitionKey: %s, field: %s, value: %s",
            tablePartitionKey, field, (sds) valueObj->ptr);

    sdsfree(tablePartitionKey);
    sdsfree(field);
    addReplyBulk(c, valueObj);
}

void testSdsLocationCommand(client *c) {
    {
        sds source = sdsnew("TEST_SDS_LOCATION_REDIS");
        sds target = sdsnewloc("TEST_SDS_LOCATION_REDIS",
                               SDS_ADDB_LOCATION_REDIS);
        assert(sdscmp(source, target) == 0);
        assert(sdsloc(target) == SDS_ADDB_LOCATION_REDIS);
        sdsfree(source);
        sdsfree(target);
    }
    {
        sds source = sdsnew("TEST_SDS_LOCATION_FLUSHING");
        sds target = sdsnewloc("TEST_SDS_LOCATION_FLUSHING",
                               SDS_ADDB_LOCATION_FLUSHING);
        assert(sdscmp(source, target) == 0);
        assert(sdsloc(target) == SDS_ADDB_LOCATION_FLUSHING);
        sdsfree(source);
        sdsfree(target);
    }
    {
        sds source = sdsnew("TEST_SDS_LOCATION_ROCKSDB");
        sds target = sdsnewloc("TEST_SDS_LOCATION_ROCKSDB",
                               SDS_ADDB_LOCATION_ROCKSDB);
        assert(sdscmp(source, target) == 0);
        assert(sdsloc(target) == SDS_ADDB_LOCATION_ROCKSDB);
        sdsfree(source);
        sdsfree(target);
    }
    {
        sds source = sdsnewloc("TEST_SDS_LOCATION_REDIS",
                               SDS_ADDB_LOCATION_REDIS);
        sds target = sdsduploc(source);
        assert(sdscmp(source, target) == 0);
        assert(sdsloc(target) == SDS_ADDB_LOCATION_REDIS);
        sdsfree(source);
        sdsfree(target);
    }
    {
        // Tests increased sds case
        sds source = sdsnewloc("TEST_SDS_LOCATION_REDIS",
                               SDS_ADDB_LOCATION_REDIS);
        sds target = sdscat(source, "VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING_VERY_VERY_LARGE_STRING");
        assert(sdsloc(target) == SDS_ADDB_LOCATION_REDIS);
        sdsfree(target);
    }
    addReply(c, shared.ok);
}
