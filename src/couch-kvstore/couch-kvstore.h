/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_
#define SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_ 1

#include "config.h"
#include "libcouchstore/couch_db.h"

#include <map>
#include <string>
#include <vector>

#include "configuration.h"
#include "couch-kvstore/couch-fs-stats.h"
#include "couch-kvstore/couch-notifier.h"
#include "histo.h"
#include "item.h"
#include "kvstore.h"
#include "stats.h"
#include "tasks.h"

#define COUCHSTORE_NO_OPTIONS 0

/**
 * Stats and timings for couchKVStore
 */
class CouchKVStoreStats {

public:
    /**
     * Default constructor
     */
    CouchKVStoreStats() :
      docsCommitted(0), numOpen(0), numClose(0),
      numLoadedVb(0), numGetFailure(0), numSetFailure(0),
      numDelFailure(0), numOpenFailure(0), numVbSetFailure(0),
      readSizeHisto(ExponentialGenerator<size_t>(1, 2), 25),
      writeSizeHisto(ExponentialGenerator<size_t>(1, 2), 25) {
    }

    void reset() {
        docsCommitted.store(0);
        numOpen.store(0);
        numClose.store(0);
        numLoadedVb.store(0);
        numGetFailure.store(0);
        numSetFailure.store(0);
        numDelFailure.store(0);
        numOpenFailure.store(0);
        numVbSetFailure.store(0);
        numCommitRetry.store(0);

        readTimeHisto.reset();
        readSizeHisto.reset();
        writeTimeHisto.reset();
        writeSizeHisto.reset();
        delTimeHisto.reset();
        compactHisto.reset();
        commitHisto.reset();
        commitRetryHisto.reset();
        saveDocsHisto.reset();
        batchSize.reset();
        fsStats.reset();
    }

    // the number of docs committed
    Atomic<size_t> docsCommitted;
    // the number of open() calls
    Atomic<size_t> numOpen;
    // the number of close() calls
    Atomic<size_t> numClose;
    // the number of vbuckets loaded
    Atomic<size_t> numLoadedVb;

    //stats tracking failures
    Atomic<size_t> numGetFailure;
    Atomic<size_t> numSetFailure;
    Atomic<size_t> numDelFailure;
    Atomic<size_t> numOpenFailure;
    Atomic<size_t> numVbSetFailure;
    Atomic<size_t> numCommitRetry;

    /* for flush and vb delete, no error handling in CouchKVStore, such
     * failure should be tracked in MC-engine  */

    // How long it takes us to complete a read
    Histogram<hrtime_t> readTimeHisto;
    // How big are our reads?
    Histogram<size_t> readSizeHisto;
    // How long it takes us to complete a write
    Histogram<hrtime_t> writeTimeHisto;
    // How big are our writes?
    Histogram<size_t> writeSizeHisto;
    // Time spent in delete() calls.
    Histogram<hrtime_t> delTimeHisto;
    // Time spent in couchstore commit
    Histogram<hrtime_t> commitHisto;
    // Time spent in couchstore commit retry
    Histogram<hrtime_t> commitRetryHisto;
    // Time spent in couchstore compaction
    Histogram<hrtime_t> compactHisto;
    // Time spent in couchstore save documents
    Histogram<hrtime_t> saveDocsHisto;
    // Batch size of saveDocs calls
    Histogram<size_t> batchSize;

    // Stats from the underlying OS file operations done by couchstore.
    CouchstoreStats fsStats;
};

class EventuallyPersistentEngine;
class EPStats;

typedef union {
    Callback <mutation_result> *setCb;
    Callback <int> *delCb;
} CouchRequestCallback;

const size_t COUCHSTORE_METADATA_SIZE(2 * sizeof(uint32_t) + sizeof(uint64_t));

/**
 * Class representing a document to be persisted in couchstore.
 */
class CouchRequest
{
public:
    /**
     * Constructor
     *
     * @param it Item instance to be persisted
     * @param rev vbucket database revision number
     * @param cb persistence callback
     * @param del flag indicating if it is an item deletion or not
     */
    CouchRequest(const Item &it, uint64_t rev, CouchRequestCallback &cb, bool del);

    /**
     * Get the vbucket id of a document to be persisted
     *
     * @return vbucket id of a document
     */
    uint16_t getVBucketId(void) {
        return vbucketId;
    }

    /**
     * Get the revision number of the vbucket database file
     * where the document is persisted
     *
     * @return revision number of the corresponding vbucket database file
     */
    uint64_t getRevNum(void) {
        return fileRevNum;
    }

    /**
     * Get the couchstore Doc instance of a document to be persisted
     *
     * @return pointer to the couchstore Doc instance of a document
     */
    Doc *getDbDoc(void) {
        if (deleteItem) {
            return NULL;
        } else {
            return &dbDoc;
        }
    }

    /**
     * Get the couchstore DocInfo instance of a document to be persisted
     *
     * @return pointer to the couchstore DocInfo instance of a document
     */
    DocInfo *getDbDocInfo(void) {
        return &dbDocInfo;
    }

    /**
     * Get the callback instance for SET
     *
     * @return callback instance for SET
     */
    Callback<mutation_result> *getSetCallback(void) {
        return callback.setCb;
    }

    /**
     * Get the callback instance for DELETE
     *
     * @return callback instance for DELETE
     */
    Callback<int> *getDelCallback(void) {
        return callback.delCb;
    }

    /**
     * Get the time in ns elapsed since the creation of this instance
     *
     * @return time in ns elapsed since the creation of this instance
     */
    hrtime_t getDelta() {
        return (gethrtime() - start) / 1000;
    }

    /**
     * Get the length of a document body to be persisted
     *
     * @return length of a document body
     */
    size_t getNBytes() {
        return value ? value->length() : 0;
    }

    /**
     * Return true if the document to be persisted is for DELETE
     *
     * @return true if the document to be persisted is for DELETE
     */
    bool isDelete() {
        return deleteItem;
    };

    /**
     * Get the key of a document to be persisted
     *
     * @return key of a document to be persisted
     */
    const std::string& getKey(void) const {
        return key;
    }

private :
    value_t value;
    uint8_t meta[COUCHSTORE_METADATA_SIZE];
    uint16_t vbucketId;
    uint64_t fileRevNum;
    std::string key;
    Doc dbDoc;
    DocInfo dbDocInfo;
    bool deleteItem;
    CouchRequestCallback callback;

    hrtime_t start;
};

/**
 * KVStore with couchstore as the underlying storage system
 */
class CouchKVStore : public KVStore
{
public:
    /**
     * Constructor
     *
     * @param theEngine EventuallyPersistentEngine instance
     * @param read_only flag indicating if this kvstore instance is for read-only operations
     */
    CouchKVStore(EPStats &stats, Configuration &config, bool read_only = false);

    /**
     * Copy constructor
     *
     * @param from the source kvstore instance
     */
    CouchKVStore(const CouchKVStore &from);

    /**
     * Deconstructor
     */
    virtual ~CouchKVStore() {
        close();
    }

    /**
     * Reset database to a clean state.
     */
    void reset(void);

    /**
     * Begin a transaction (if not already in one).
     *
     * @return true if the transaction is started successfully
     */
    bool begin(void) {
        assert(!isReadOnly());
        intransaction = true;
        return intransaction;
    }

    /**
     * Commit a transaction (unless not currently in one).
     *
     * @return true if the commit is completed successfully.
     */
    bool commit(void);

    /**
     * Rollback a transaction (unless not currently in one).
     */
    void rollback(void) {
        assert(!isReadOnly());
        if (intransaction) {
            intransaction = false;
        }
    }

    /**
     * Query the properties of the underlying storage.
     *
     * @return properties of the underlying storage system
     */
    StorageProperties getStorageProperties(void);

    /**
     * Insert or update a given document.
     *
     * @param itm instance representing the document to be inserted or updated
     * @param cb callback instance for SET
     */
    void set(const Item &itm, Callback<mutation_result> &cb);

    /**
     * Retrieve the document with a given key from the underlying storage system.
     *
     * @param key the key of a document to be retrieved
     * @param rowid the sequence number of a document
     * @param vb vbucket id of a document
     * @param cb callback instance for GET
     */
    void get(const std::string &key, uint64_t rowid,
             uint16_t vb, Callback<GetValue> &cb);

    /**
     * Retrieve the multiple documents from the underlying storage system at once.
     *
     * @param vb vbucket id of a document
     * @param itms list of items whose documents are going to be retrieved
     */
    void getMulti(uint16_t vb, vb_bgfetch_queue_t &itms);

    /**
     * Delete a given document from the underlying storage system.
     *
     * @param itm instance representing the document to be deleted
     * @param rowid the sequence number of a document
     * @param cb callback instance for DELETE
     */
    void del(const Item &itm, uint64_t rowid,
             Callback<int> &cb);

    /**
     * Delete a given vbucket database instance from the underlying storage system
     *
     * @param vbucket vbucket id
     * @param recreate true if we need to create an empty vbucket after deletion
     * @return true if the vbucket deletion is completed successfully.
     */
    bool delVBucket(uint16_t vbucket, bool recreate);

    /**
     * Retrieve the list of persisted vbucket states
     *
     * @return vbucket state map instance where key is vbucket id and
     * value is vbucket state
     */
    vbucket_map_t listPersistedVbuckets(void);

    /**
     * Retrieve ths list of persisted engine stats
     *
     * @param stats map instance where the persisted engine stats will be added
     */
    void getPersistedStats(std::map<std::string, std::string> &stats);

    /**
     * Persist a snapshot of the engine stats in the underlying storage.
     *
     * @param engine_stats map instance that contains all the engine stats
     * @return true if the snapshot is done successfully
     */
    bool snapshotStats(const std::map<std::string, std::string> &engine_stats);

    /**
     * Persist a snapshot of the vbucket states in the underlying storage system.
     *
     * @param vb_stats map instance that contains all the vbucket states
     * @return true if the snapshot is done successfully
     */
    bool snapshotVBuckets(const vbucket_map_t &vb_states);

     /**
     * Compact a vbucket in the underlying storage system.
     *
     * @param vbid   - which vbucket needs to be compacted
     * @param hook_ctx - details of vbucket which needs to be compacted
     * @param cb - callback to help process newly expired items
     * @return true if the snapshot is done successfully
     */
    bool compactVBucket(const uint16_t vbid, compaction_ctx *cookie,
                        Callback<compaction_ctx> &cb);

    /**
     * Retrieve selected documents from the underlying storage system.
     *
     * @param vbids list of vbucket ids whose document keys are going to be retrieved
     * @param cb callback instance to process each document retrieved
     * @param cl callback to see if we need to read the value from disk
     */
    void dump(std::vector<uint16_t> &vbids, shared_ptr<Callback<GetValue> > cb,
              shared_ptr<Callback<CacheLookup> > cl);

    /**
     * Retrieve all the documents for a given vbucket from the storage system.
     *
     * @param vb vbucket id
     * @param cb callback instance to process each document retrieved
     * @param cl callback to see if we need to read the value from disk
     */
    void dump(uint16_t vb, uint64_t stSeqno, uint64_t enSeqno,
              shared_ptr<Callback<GetValue> > cb,
              shared_ptr<Callback<CacheLookup> > cl);

    /**
     * Retrieve all the keys from the underlying storage system.
     *
     * @param vbids list of vbucket ids whose document keys are going to be retrieved
     * @param cb callback instance to process each key retrieved
     */
    void dumpKeys(std::vector<uint16_t> &vbids,  shared_ptr<Callback<GetValue> > cb);

    /**
     * Retrieve the list of keys and their meta data for a given
     * vbucket, which were deleted.
     * @param vb vbucket id
     * @param cb callback instance to process each key and its meta data
     */
    void dumpDeleted(uint16_t vb, uint64_t stSeqno, uint64_t enSeqno,
                     shared_ptr<Callback<GetValue> > cb);

    /**
     * Does the underlying storage system support key-only retrieval operations?
     *
     * @return true if key-only retrieval is supported
     */
    bool isKeyDumpSupported() {
        return true;
    }

    /**
     * Get the estimated number of items that are going to be loaded during warmup.
     *
     * @return the number of estimated items to be loaded during warmup
     */
    size_t getEstimatedItemCount(std::vector<uint16_t> &vbs);

    /**
     * Get the number of deleted items that are persisted to a vbucket file
     *
     * @param vbid The vbucket if of the file to get the number of deletes for
     */
    size_t getNumPersistedDeletes(uint16_t vbid);

    /**
     * Get the number of non-deleted items from a vbucket database file
     *
     * @param vbid The vbucket of the file to get the number of docs for
     */
    size_t getNumItems(uint16_t vbid);

    /**
     * Perform the pre-optimizations before persisting dirty items
     *
     * @param items list of dirty items that can be pre-optimized
     */
    void optimizeWrites(std::vector<queued_item> &items);

    /**
     * Add all the kvstore stats to the stat response
     *
     * @param prefix stat name prefix
     * @param add_stat upstream function that allows us to add a stat to the response
     * @param cookie upstream connection cookie
     */
    void addStats(const std::string &prefix, ADD_STAT add_stat, const void *cookie);

    /**
     * Add all the kvstore timings stats to the stat response
     *
     * @param prefix stat name prefix
     * @param add_stat upstream function that allows us to add a stat to the response
     * @param cookie upstream connection cookie
     */
    void addTimingStats(const std::string &prefix, ADD_STAT add_stat,
                        const void *c);

    /**
     * Resets couchstore stats
     */
    void resetStats() {
        st.reset();
    }

    static int recordDbDump(Db *db, DocInfo *docinfo, void *ctx);
    static int recordDbStat(Db *db, DocInfo *docinfo, void *ctx);
    static int getMultiCb(Db *db, DocInfo *docinfo, void *ctx);
    static void readVBState(Db *db, uint16_t vbId, vbucket_state &vbState);

    couchstore_error_t fetchDoc(Db *db, DocInfo *docinfo,
                                GetValue &docValue, uint16_t vbId,
                                bool metaOnly);
    ENGINE_ERROR_CODE couchErr2EngineErr(couchstore_error_t errCode);

    CouchKVStoreStats &getCKVStoreStat(void) { return st; }

protected:
    void loadDB(shared_ptr<Callback<GetValue> > cb,
                shared_ptr<Callback<CacheLookup> > cl,
                bool keysOnly, uint16_t vbid,
                uint64_t startSeqno, uint64_t endSeqno,
                couchstore_docinfos_options options=COUCHSTORE_NO_OPTIONS);
    bool setVBucketState(uint16_t vbucketId, vbucket_state &vbstate,
                         uint32_t vb_change_type, bool notify = true);
    bool resetVBucket(uint16_t vbucketId, vbucket_state &vbstate) {
        cachedDocCount[vbucketId] = 0;
        return setVBucketState(vbucketId, vbstate, VB_STATE_CHANGED, true);
    }

    template <typename T>
    void addStat(const std::string &prefix, const char *nm, T &val,
                 ADD_STAT add_stat, const void *c);

private:
    /**
     * Notify the result of Compaction to Mccouch
     *
     * @param vbid   - the vbucket id of the bucket where compaction was done
     * @param rev    - the new file revision of the vbucket
     * @param result - the result of the compaction attempt
     * @param header_pos - new header position of the file
     * @return true if mccouch was notified successfully, false otherwise
     */
    bool notifyCompaction(const uint16_t vbid, uint64_t new_rev,
                          uint32_t result, uint64_t header_pos);

    void operator=(const CouchKVStore &from);

    void open();
    void close();
    bool commit2couchstore(void);
    void queueItem(CouchRequest *req);

    uint64_t checkNewRevNum(std::string &dbname, bool newFile = false);
    void populateFileNameMap(std::vector<std::string> &filenames);
    void remVBucketFromDbFileMap(uint16_t vbucketId);
    void updateDbFileMap(uint16_t vbucketId, uint64_t newFileRev);
    couchstore_error_t openDB(uint16_t vbucketId, uint64_t fileRev, Db **db,
                              uint64_t options, uint64_t *newFileRev = NULL);
    couchstore_error_t openDB_retry(std::string &dbfile, uint64_t options,
                                    const couch_file_ops *ops,
                                    Db **db, uint64_t *newFileRev);
    couchstore_error_t saveDocs(uint16_t vbid, uint64_t rev, Doc **docs,
                                DocInfo **docinfos, int docCount);
    void commitCallback(CouchRequest **committedReqs, int numReqs,
                        couchstore_error_t errCode);
    couchstore_error_t saveVBState(Db *db, vbucket_state &vbState);
    void setDocsCommitted(uint16_t docs);
    void closeDatabaseHandle(Db *db);

    EPStats &epStats;
    Configuration &configuration;
    const std::string dbname;
    CouchNotifier *couchNotifier;
    std::vector<uint64_t>dbFileRevMap;
    uint16_t numDbFiles;
    std::vector<CouchRequest *> pendingReqsQ;
    size_t pendingCommitCnt;
    bool intransaction;
    bool dbFileRevMapPopulated;

    /* all stats */
    CouchKVStoreStats   st;
    couch_file_ops statCollectingFileOps;
    /* vbucket state cache*/
    vbucket_map_t cachedVBStates;
    /* deleted docs in each file*/
    std::map<uint16_t, size_t> cachedDeleteCount;
    /* non-deleted docs in each file */
    unordered_map<uint16_t, size_t> cachedDocCount;
};

#endif  // SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_
