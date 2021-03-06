/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc
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

#ifndef SRC_BACKFILL_H_
#define SRC_BACKFILL_H_ 1

#include "config.h"

#include <assert.h>

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.h"
#include "ep_engine.h"
#include "stats.h"
#include "tasks.h"

#define BACKFILL_MEM_THRESHOLD 0.95
#define DEFAULT_BACKFILL_SNOOZE_TIME 1.0

typedef enum {
    ALL_MUTATIONS = 1,
    DELETIONS_ONLY
} backfill_t;

/**
 * Dispatcher callback responsible for bulk backfilling tap queues
 * from a KVStore.
 *
 * Note that this is only used if the KVStore reports that it has
 * efficient vbucket ops.
 */
class BackfillDiskLoad : public GlobalTask {
public:

    BackfillDiskLoad(const std::string &n, EventuallyPersistentEngine* e,
                     ConnMap &cm, KVStore *s, uint16_t vbid,
                     uint64_t start_seqno, uint64_t end_seqno,
                     hrtime_t token, const Priority &p, double sleeptime = 0,
                     bool shutdown = false)
        : GlobalTask(e, p, sleeptime, shutdown),
          name(n), engine(e), connMap(cm), store(s), vbucket(vbid),
          startSeqno(start_seqno), endSeqno(end_seqno), connToken(token) {
        ScheduleDiskBackfillTapOperation tapop;
        cm.performOp(name, tapop, static_cast<void*>(NULL));
    }

    bool run();

    std::string getDescription();

private:
    const std::string           name;
    EventuallyPersistentEngine *engine;
    ConnMap                    &connMap;
    KVStore                    *store;
    uint16_t                    vbucket;
    uint64_t                    startSeqno;
    uint64_t                    endSeqno;
    hrtime_t                    connToken;
};

/**
 * VBucketVisitor to backfill a Producer. This visitor basically performs backfill from memory
 * for only resident items if it needs to schedule a separate disk backfill task because of
 * low resident ratio.
 */
class BackFillVisitor : public VBucketVisitor {
public:
    BackFillVisitor(EventuallyPersistentEngine *e, ConnMap &cm, Producer *tc,
                    const VBucketFilter &backfillVBfilter):
        VBucketVisitor(backfillVBfilter), engine(e), connMap(cm),
        name(tc->getName()), connToken(tc->getConnectionToken()), valid(true) {}

    virtual ~BackFillVisitor() {}

    bool visitBucket(RCPtr<VBucket> &vb);

    void visit(StoredValue *v);

    bool shouldContinue() {
        return checkValidity();
    }

    void complete(void);

private:

    bool pauseVisitor();

    bool checkValidity();

    EventuallyPersistentEngine *engine;
    ConnMap &connMap;
    const std::string name;
    hrtime_t connToken;
    bool valid;
};

/**
 * Backfill task is scheduled as a non-IO task. Each backfill task performs
 * backfill from memory or disk depending on the resident ratio. Each backfill
 * task can backfill more than one vbucket, but will snooze for 1 sec if the
 * current backfill backlog for the corresponding TAP producer is greater than
 * the threshold (5000 by default).
 */
class BackfillTask : public GlobalTask {
public:

    BackfillTask(EventuallyPersistentEngine *e, ConnMap &cm, Producer *tc,
                 const VBucketFilter &backfillVBFilter):
                 GlobalTask(e, Priority::BackfillTaskPriority, 0, false),
      bfv(new BackFillVisitor(e, cm, tc, backfillVBFilter)), engine(e) {}

    virtual ~BackfillTask() {}

    bool run(void);

    std::string getDescription() {
        return std::string("Backfilling items from memory and disk.");
    }

    shared_ptr<BackFillVisitor> bfv;
    EventuallyPersistentEngine *engine;
};

#endif  // SRC_BACKFILL_H_
