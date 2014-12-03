/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIVE_DATA_SOURCE_H_

#define LIVE_DATA_SOURCE_H_

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/DataSource.h>
#include <utils/threads.h>
#include <utils/List.h>

namespace android {

struct ABuffer;

struct LiveDataSource : public DataSource {
    LiveDataSource();

    virtual status_t initCheck() const;

    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    ssize_t readAtNonBlocking(off64_t offset, void *data, size_t size);

    void queueBuffer(const sp<ABuffer> &buffer);
    void queueEOS(status_t finalResult);
    void reset();
    void clearPartialBuffer();
    bool getSeqNumberInfo(int32_t *curPlay, int32_t *bufferBegin);
    bool adjustPlayPosition(int32_t newSeqNumber, int32_t *downloadSeqNum);

    size_t countQueuedBuffers();

#ifdef LETV_HLS_OPTIMIZATION

    struct LiveDataStatistic {
        uint64_t consumeSize;
        int64_t consumeDuration;

        int32_t curBufferSlideNum;
        int32_t consumeSlideNum;

        int32_t playingSeqNum;
        int64_t progTimeMs;
        int progTimeZone;
    } LiveDataStatistic_t;

    bool getStatistic(LiveDataStatistic& statistic);
    bool getBufferedSize(int64_t *duration, int64_t *size);
    bool getBufferedSize(int64_t *duration, int64_t *size, status_t *finalStatus);

#endif

protected:
    virtual ~LiveDataSource();

private:
    Mutex mLock;
    Condition mCondition;

    off64_t mOffset;
    List<sp<ABuffer> > mBufferQueue;
    status_t mFinalResult;

    FILE *mBackupFile;

    ssize_t readAt_l(off64_t offset, void *data, size_t size);
    void clearUsedBuffer_l();
    void findCurrentBuffer_l();

#ifdef LETV_HLS_OPTIMIZATION

    LiveDataStatistic mStats;

    size_t getBufferedSize_l();
    size_t mBufferTotalSize;
    int64_t mBufferTotalDuration;
    int64_t mCurQueueSeqSize;
    int32_t mCalConsumeTime;
    //int32_t mCurSeqNum;
    //int32_t mPreSeqNum;
    int32_t mTotalTsFile;
    int32_t mUsedTsFile;
    int32_t mUsedBufferNum;
    sp<ABuffer> mUsingBuffer;
    bool mBufferQueueEnd;
    int32_t mCurReadSeq;
    bool mIsSeekingNow;
    bool mIsDeleteBuffer;
    bool mTestFlag;
    bool mFirstTsEnd;
#endif
    DISALLOW_EVIL_CONSTRUCTORS(LiveDataSource);
};

}  // namespace android

#endif  // LIVE_DATA_SOURCE_H_
