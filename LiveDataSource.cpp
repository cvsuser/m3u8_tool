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

//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#define LOG_TAG "LiveDataSource"
#include <utils/Log.h>

#include "LiveDataSource.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

#include <cutils/properties.h>

#define SAVE_BACKUP     0

namespace android {

#define kMaxRetrySize       600  //188 * 5
#define kMaxRetryCount    5

static const size_t kTSPerPacketSize = 188;

LiveDataSource::LiveDataSource()
    : mOffset(0),
      mFinalResult(OK),
      mBackupFile(NULL) {
#if SAVE_BACKUP
    mBackupFile = fopen("/data/misc/backup.ts", "wb");
    CHECK(mBackupFile != NULL);
#endif

#ifdef LETV_HLS_OPTIMIZATION
    mBufferTotalDuration = 0;
    mBufferTotalSize = 0;
    memset(&mStats, 0, sizeof(mStats));
    mCurQueueSeqSize = 0;
    //mCurSeqNum = -1;
    //mPreSeqNum = -1;
    mCalConsumeTime = 0;
    mFirstTsEnd = true;
#endif
    mIsDeleteBuffer = false;
    mTestFlag = false;
    mUsedTsFile = 0;
    mUsingBuffer = NULL;
    mTotalTsFile = 0;
    mUsedBufferNum = 0;
    mBufferQueueEnd = true;
    mCurReadSeq = -1;
    mIsSeekingNow = false;
}

LiveDataSource::~LiveDataSource() {
    if (mBackupFile != NULL) {
        fclose(mBackupFile);
        mBackupFile = NULL;
    }
}

status_t LiveDataSource::initCheck() const {
    return OK;
}

size_t LiveDataSource::countQueuedBuffers() {
    Mutex::Autolock autoLock(mLock);

    return mBufferQueue.size();
}
#ifdef LETV_HLS_OPTIMIZATION

bool LiveDataSource::getStatistic(LiveDataSource::LiveDataStatistic& statistic)
{
    Mutex::Autolock autoLock(mLock);
    if (mFinalResult != OK) {
        return false;
    }
    statistic = mStats;
    return true;
}

bool LiveDataSource::getBufferedSize(int64_t *duration, int64_t *size) {
    Mutex::Autolock autoLock(mLock);
    *duration = mBufferTotalDuration;
    *size = (int64_t)mBufferTotalSize;
    return true;
}

bool LiveDataSource::getBufferedSize(int64_t *duration, int64_t *size, status_t *finalStatus) {
    Mutex::Autolock autoLock(mLock);
    *duration = mBufferTotalDuration;
    *size = (int64_t)mBufferTotalSize;
    *finalStatus = mFinalResult;
    return true;
}

size_t LiveDataSource::getBufferedSize_l() {
    size_t totalAvailable = 0;
    for (List<sp<ABuffer> >::iterator it = mBufferQueue.begin();
         it != mBufferQueue.end(); ++it) {
        sp<ABuffer> buffer = *it;
        totalAvailable += buffer->size();
    }
    return totalAvailable;
}
#endif

static int32_t findSyncbyte(const uint8_t *mData, size_t nSize, size_t nMaxSize) {
    if (nSize < 188 * 3) {
        return -1;
    }
    size_t maxScan = nSize - 188 * 2;
    if (maxScan > nMaxSize)
        maxScan = nMaxSize;

    int32_t n = 0;
    while (n < maxScan) {
        if ((mData[n] == 0x47) && (mData[n + 188] == 0x47) && (mData[n + 188*2] == 0x47)) {
            break;
        }
        n ++;
    }
    if (n == maxScan) {
        ALOGD("sync bytes 0x47 not found");
        return -1;
    }
    return n;
}

static int32_t findBufferSyncbyte(const uint8_t *mData, size_t nSize){
    int32_t position = -1;
    if (nSize < kMaxRetrySize) {
        return position;
    }

    int32_t n = 0;
    while (n < (kTSPerPacketSize + 1)) {
        if ((mData[n] == 0x47) && (mData[n + 188] == 0x47) && (mData[n + 376] == 0x47)) {
            ALOGD("found the right 0x47!!!,pos =%d", position);
            position = n;
           break;
        }
        n ++;
    }
    return position;
}

ssize_t LiveDataSource::readAtNonBlocking(
        off64_t offset, void *data, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (offset != mOffset) {
        ALOGE("Attempt at reading non-sequentially from LiveDataSource.");
        return -EPIPE;
    }

    size_t totalAvailable = 0;
    if (mUsingBuffer != NULL)
        totalAvailable = mUsingBuffer->size();
    if (totalAvailable >= size)
        return readAt_l(offset, data, size);

    for (List<sp<ABuffer> >::iterator it = mBufferQueue.begin();
         it != mBufferQueue.end(); ++it) {
        sp<ABuffer> buffer = *it;
        int32_t usedFlag = 0;
        CHECK(buffer->meta()->findInt32("usedFlag", &usedFlag));
        if (usedFlag == 0)
            totalAvailable += buffer->size();

        if (totalAvailable >= size) {
            break;
        }
    }

    if (totalAvailable < size) {
        return mFinalResult == OK ? -EWOULDBLOCK : mFinalResult;
    }

    return readAt_l(offset, data, size);
}

ssize_t LiveDataSource::readAt(off64_t offset, void *data, size_t size) {
    Mutex::Autolock autoLock(mLock);
    return readAt_l(offset, data, size);
}


static int64_t parserProTimeMs(AString proTime,int *pTimeZone){
    size_t offset;
    int64_t timems;
    *pTimeZone = 0;

    ALOGD("program-time is %s",proTime.c_str());
    ssize_t colonPos = proTime.find("T");

    if (colonPos < 0){
        return 0;
    }

    offset = colonPos +1;

    AString attr(proTime, offset, proTime.size()- offset);
    colonPos = attr.find(":");
    if (colonPos < 0){
        return 0;
    }
    AString Hour(attr,0,colonPos);
    ALOGD("Hour is %s",Hour.c_str());

    offset += colonPos +1;
    attr.setTo(proTime,offset,proTime.size()- offset);
    colonPos = attr.find(":");
    if (colonPos < 0){
        return 0;
    }
    AString min(attr,0,colonPos);
    ALOGD("min is %s",min.c_str());

    offset += colonPos+1;
    attr.setTo(proTime,offset,proTime.size()- offset);
    colonPos = attr.find("+");
    if (colonPos < 0)
        colonPos = attr.find("-");
    if (colonPos < 0){
        return 0;
    }
    AString sec(attr,0,colonPos);
    ALOGD("sec is %s",sec.c_str());

    offset += colonPos;
    attr.setTo(proTime,offset,proTime.size()- offset);
    colonPos = attr.find(":");
    if (colonPos < 0){
        return 0;
    }
    AString timeZone(attr,0,colonPos);
    ALOGD("timeZone is %s",timeZone.c_str());

    offset += colonPos +1;
    AString timeZoneMin(proTime,offset,proTime.size()- offset);
    ALOGD("timeZoneMin is %s",timeZoneMin.c_str());

    timems =(int64_t) (atoi(Hour.c_str())*60*60*1000 + atoi(min.c_str())*60*1000 + atof(sec.c_str())*1000);
#if 0
    if (timems > 24*60*60*1000)
        timems -= 24*60*60*1000;
    else if (timems < 0)
        timems += 24*60*60*1000;
#endif
    *pTimeZone = atoi(timeZone.c_str());

    return timems;
}
ssize_t LiveDataSource::readAt_l(off64_t offset, void *data, size_t size) {
    if (offset != mOffset) {
        ALOGE("Attempt at reading non-sequentially from LiveDataSource.");
        return -EPIPE;
    }

    size_t sizeDone = 0;
#if 1
    int64_t seqSize = -1;
    int64_t seqDuration = -1;
    int64_t consumeTime = 0;
#endif
    bool bFirstByteRead = false;
    size_t sFilterSize = 0;

    while (sizeDone < size) {
        while (mIsSeekingNow) {
            ALOGD("MP-DEBUG1:wait for seek complete");
            mCondition.wait(mLock);
            mIsSeekingNow = false;
        }
        while (mUsingBuffer == NULL) {
            findCurrentBuffer_l();
            if (mBufferQueue.size() > 29 && ((mUsedTsFile > 1) ||(mUsedBufferNum > 20))) {
                ALOGD("mUsedBufferNum=%d", mUsedBufferNum);
                clearUsedBuffer_l();
            }
            while (mBufferQueueEnd &&(mFinalResult == OK) ) {
                ALOGD("MP-DEBUG1:wait for buffer, mBufferQueue Size =%d", mBufferQueue.size());
                mCondition.wait(mLock);
            }
            while ( mBufferQueueEnd && (mFinalResult != OK)) {
                ALOGD("MP-DEBUG1:sizeDone =%d, bufferQueue size =%d, error %d", sizeDone, mBufferQueue.size(), (int32_t )mFinalResult);
                if (sizeDone > 0) {
                    mOffset += sizeDone;
                    return sizeDone;
                }
                return mFinalResult;
            }
        }

        //sp<ABuffer> buffer = *mBufferQueue.begin();
        sp<ABuffer> buffer = mUsingBuffer;

        buffer->meta()->findInt32("SeqNum", &mStats.playingSeqNum);

        //test code
        char value[PROPERTY_VALUE_MAX];
        int32_t int_val = 0;
        property_get("media.ts.test", value, "0");
        sscanf(value, "%d", &int_val);
        if ((int_val == 1) && !bFirstByteRead) {
            if (!mTestFlag) {
                mTestFlag = true;
                buffer->data()[0] = 0x46;
                ALOGD("Test : send a data not 0x47 to parser");
            }
        }else if (int_val != 1) {
            mTestFlag = false;
        }

        if (!bFirstByteRead && (buffer->data()[0] == 0x00)&&((buffer->data()[1] == 0x00) ||(buffer->data()[1] == 0x01))) {
                    ALOGW("special buffer from livesession -----0x0*00...");
                    goto CopySize;
        }

#if 1
            if ((!bFirstByteRead) && (buffer->data()[0] != 0x47)
                        //&& ((buffer->data()[0] == 0x00)&&((buffer->data()[1] == 0x00) ||(buffer->data()[1] == 0x01)))
                        ) {
                        ALOGW("Look for 0x47 sync byte");
                        mIsDeleteBuffer = false;
                        int32_t position =  findBufferSyncbyte(buffer->data(), buffer->size());
                        if (position < 0) {
                            mUsingBuffer = NULL;
                            buffer = NULL;
                            ALOGE("not found the 0x47 sync byte");
                            int32_t usedFlag = -1;
                            int32_t endflag = -1;
                            List<sp<ABuffer> >::iterator it1 = mBufferQueue.end();
                            --it1;
                            buffer= *it1;
                            buffer->meta()->findInt32("usedFlag", &usedFlag);
                            buffer->meta()->findInt32("seqEnd", &endflag);
                            while ((usedFlag == 0) && (it1 != mBufferQueue.begin())) {
                                it1--;
                                buffer = *it1;
                                buffer->meta()->findInt32("usedFlag", &usedFlag);
                                buffer->meta()->findInt32("seqEnd", &endflag);
                            }
                            mBufferQueue.erase(it1);
                            if (endflag == 0) {
                                List<sp<ABuffer> >::iterator it2 = mBufferQueue.begin();
                                buffer = *it2;
                                buffer->meta()->findInt32("usedFlag", &usedFlag);
                                buffer->meta()->findInt32("seqEnd", &endflag);
                                while (it2 != mBufferQueue.end()) {
                                    if (usedFlag == 0) {
                                        mBufferQueue.erase(it2);
                                        if (endflag == 1)
                                            break;
                                    }
                                    it2++;
                                    buffer->meta()->findInt32("usedFlag", &usedFlag);
                                    buffer->meta()->findInt32("seqEnd", &endflag);
                                }
                            }//else {
                                    while (mUsingBuffer == NULL) {
                                        findCurrentBuffer_l();
                                        if (mBufferQueue.size() > 29 && ((mUsedTsFile > 1) ||(mUsedBufferNum > 20))) {
                                            ALOGD("mUsedBufferNum=%d", mUsedBufferNum);
                                            clearUsedBuffer_l();
                                        }
                                        while (mBufferQueueEnd &&(mFinalResult == OK) ) {
                                            ALOGD("MP-DEBUG1:wait for buffer, mBufferQueue Size =%d", mBufferQueue.size());
                                            mCondition.wait(mLock);
                                        }
                                        while ( mBufferQueueEnd && (mFinalResult != OK)) {
                                            ALOGD("MP-DEBUG1:sizeDone =%d, bufferQueue size =%d, error %d", sizeDone, mBufferQueue.size(), (int32_t )mFinalResult);
                                            if (sizeDone > 0) {
                                                mOffset += sizeDone;
                                                return sizeDone;
                                            }
                                            return mFinalResult;
                                        }
                                }
                                buffer = mUsingBuffer;
                                buffer->meta()->findInt32("SeqNum", &mStats.playingSeqNum);
                            //}
                        } else {
                            sFilterSize += position;
                            //ALOGW("@debug: found 0x47, skip %d bytes 0x%x", position , buffer->data()[buffer->offset() + position]);
                            buffer->setRange(buffer->offset() + position, buffer->size() - position);
                        }
                    }
                    //workaround}}
#endif
CopySize:
        size_t copy = size - sizeDone;

        if (copy > buffer->size()) {
            copy = buffer->size();
        }

        memcpy((uint8_t *)data + sizeDone, buffer->data(), copy);

        sizeDone += copy;

        //{{workaround
        bFirstByteRead = true;
        mIsDeleteBuffer = false;
        //workaround}}

        buffer->setRange(buffer->offset() + copy, buffer->size() - copy);
#if 1
        if (buffer->meta()->findInt64("durationUs", &seqDuration)
           && buffer->meta()->findInt64("SeqSize", &seqSize) && !mFirstTsEnd && (mBufferTotalDuration > 0)) {
            buffer->meta()->findInt64("durationUs", &seqDuration);
            buffer->meta()->findInt64("SeqSize", &seqSize);
            consumeTime = seqDuration * copy /seqSize;
                mCalConsumeTime += consumeTime;
                mBufferTotalDuration -= consumeTime;
        }
#endif

        if (buffer->size() == 0) {
#ifdef LETV_HLS_OPTIMIZATION
            int64_t duration;
            int32_t endFlag;
            if (buffer->meta()->findInt64("durationUs", &duration)
                && buffer->meta()->findInt32("seqEnd", &endFlag) && (endFlag == 1)) {
                ALOGD("MP-DEBUG delta time =%lld, duration=%lld, seqnum=%d", (duration - mCalConsumeTime), duration, mStats.playingSeqNum);
                mBufferTotalDuration -= duration - mCalConsumeTime;
                mStats.consumeDuration += duration;

                mStats.curBufferSlideNum --;
                mStats.consumeSlideNum ++;
                mCalConsumeTime = 0;
                mUsedTsFile += 1;
                /*
                if (mUsedTsFile > 2) {
                    clearUsedBuffer_l();
                }*/
            }
            if (mUsedBufferNum > 20) {
                clearUsedBuffer_l();
            }
            mUsingBuffer = NULL;
#endif
        }
    }

    sizeDone += sFilterSize;

    mOffset += sizeDone;

#ifdef LETV_HLS_OPTIMIZATION
    //mBufferTotalSize -= sizeDone;
    mStats.consumeSize += sizeDone;
#endif

    return sizeDone;
}

void LiveDataSource::queueBuffer(const sp<ABuffer> &buffer) {
    Mutex::Autolock autoLock(mLock);

    if (mFinalResult != OK) {
        return;
    }

#if SAVE_BACKUP
    if (mBackupFile != NULL) {
        CHECK_EQ(fwrite(buffer->data(), 1, buffer->size(), mBackupFile),
                 buffer->size());
    }
#endif

#ifdef LETV_HLS_OPTIMIZATION
    mBufferTotalSize += buffer->size();
    int64_t duration = -1;
    int32_t seq = -1;
    int32_t endFlag = -1;
    int64_t seqSize = -1;
    buffer->meta()->findInt64("durationUs", &duration);
    buffer->meta()->findInt32("SeqNum", &seq);
    buffer->meta()->findInt64("SeqSize", &seqSize);
    buffer->meta()->setInt32("usedFlag", 0); //0 for not used, 1 for used, 2 for using
    if (buffer->meta()->findInt32("seqEnd", &endFlag) && (endFlag == 1)) {
        mCurQueueSeqSize += buffer->size();
        mBufferTotalDuration += duration;
        mStats.curBufferSlideNum ++;

        ALOGD("MP-DEBUG queueBuffer size=%d(%.2fs), %d, ts total size %lld,   %lld", buffer->size(), duration / 1E6, seq, mCurQueueSeqSize, seqSize);
        mCurQueueSeqSize = 0;
        if (mFirstTsEnd)
            mFirstTsEnd = false;
    } else {
        mCurQueueSeqSize += buffer->size();
        //ALOGD("MP-DEBUG queueBuffer size=%d", buffer->size());
    }
#endif
    mBufferQueueEnd = false;
    mBufferQueue.push_back(buffer);
    mCondition.broadcast();
}

void LiveDataSource::queueEOS(status_t finalResult) {
    CHECK_NE(finalResult, (status_t)OK);

    Mutex::Autolock autoLock(mLock);

    mFinalResult = finalResult;
    mCondition.broadcast();
}

void LiveDataSource::reset() {
    Mutex::Autolock autoLock(mLock);

    // XXX FIXME: If we've done a partial read and waiting for more buffers,
    // we'll mix old and new data...
#ifdef LETV_HLS_OPTIMIZATION
    mBufferTotalSize = 0;
    mBufferTotalDuration = 0;
    memset(&mStats, 0, sizeof(mStats));
#endif
    mFinalResult = OK;
    mBufferQueue.clear();
    mCurQueueSeqSize = 0;
    //mCurSeqNum = -1;
    //mPreSeqNum = -1;
    mCalConsumeTime = 0;
    mUsedTsFile = 0;
    mUsedBufferNum = 0;
    mUsingBuffer = NULL;
    mTotalTsFile = 0;
    mBufferQueueEnd = true;
    mCurReadSeq = -1;
    mIsSeekingNow = false;
}

void LiveDataSource::clearPartialBuffer() {
    Mutex::Autolock autoLock(mLock);
    if (mBufferQueue.empty())
        return;
    List<sp<ABuffer> >::iterator it = mBufferQueue.end();
    int32_t endFlag = -1;
    while (it != mBufferQueue.begin()) {
        it --;
        sp<ABuffer> buffer = *it;
        int64_t duration = -1;
        buffer->meta()->findInt64("durationUs", &duration);
        if (buffer->meta()->findInt32("seqEnd", &endFlag) && (endFlag == 1)) {
            break;
        }
        ALOGD("erase buffer %d", buffer->size());
        mBufferQueue.erase(it);
        mBufferTotalSize -= buffer->size();
        it = mBufferQueue.end();
    }
}

void LiveDataSource::clearUsedBuffer_l() {
    if (mBufferQueue.empty())
        return;
    List<sp<ABuffer> >::iterator it = mBufferQueue.begin();
    int usedFlag = -1;
    int deleteItemNum = 0;
    int32_t endFlag = -1;
    while (it != mBufferQueue.end()) {
        sp<ABuffer> buffer = *it;
        buffer->meta()->findInt32("usedFlag", &usedFlag);
        //ALOGD("MP-DEBUG1:Seq UsedFlag =%d", usedFlag);
        if (usedFlag == 1) {//change this can delete more
            if (buffer->meta()->findInt32("seqEnd", &endFlag) && (endFlag == 1)) {
                    mUsedTsFile -= 1;
                    int64_t duration = -1;
                    buffer->meta()->findInt64("durationUs", &duration);
                    if (mIsSeekingNow)
                        mBufferTotalDuration -= duration;
                    ALOGD("MP-DEBUG1:erase the end buffer of ts, used ts file =%d, mBufferQueue size=%d, mUsedBufferNum", mUsedTsFile, mBufferQueue.size(), mUsedBufferNum);
                    if (mUsedBufferNum < 30) {
                        mBufferQueue.erase(it);
                        mUsedBufferNum -= 1;
                        mBufferTotalSize -= buffer->size();
                        ALOGD("MP-DEBUG1: erase break---- mUsedTsFile =%d", mUsedTsFile);
                        break;
                    }
            }
            mBufferQueue.erase(it);
            mUsedBufferNum -= 1;
            mBufferTotalSize -= buffer->size();
            it = mBufferQueue.begin();
        }else{
            ALOGD("MP-DEBUG1:clear but no used buffer more");
            break;
        }
    }
}

void LiveDataSource::findCurrentBuffer_l() {
    sp<ABuffer> usingBuffer;
    int i = 0;
    List<sp<ABuffer> >::iterator it = mBufferQueue.begin();
    int32_t usedFlag = -1;
    if (mBufferQueue.empty()) {
        ALOGD("MP-DEBUG1:test,oh my god");
        mBufferQueueEnd = true;
        return;
    }

    while (it != mBufferQueue.end()) {
        //ALOGD("MP-DEBUG1:Come in look for buffer ");
        usingBuffer = *it;
        if (usingBuffer == NULL) {
            ALOGD("MP-DEBUG1:Buffer reach end and is null");
            mBufferQueueEnd = true;
            break;
        }
        CHECK(usingBuffer->meta()->findInt32("usedFlag", &usedFlag));
        //ALOGD("MP-DEBUG1:look for buffer usedFlag =%d", usedFlag);
        if (usedFlag == 0) {
            int32_t seqNum = -1;
            usingBuffer->meta()->findInt32("SeqNum", &seqNum);
            //ALOGD("MP-DEBUG1:found the first unused buffer  SeqNum %d", seqNum);
            usingBuffer->meta()->setInt32("usedFlag", 1);
            mBufferQueueEnd = false;
            mUsedBufferNum += 1;
            break;
        }
        it++;
    }
     if (it == mBufferQueue.end()) {
        ALOGE("MP-DEBUG1:error --not found not used buffer");
        usingBuffer = NULL;
        mBufferQueueEnd = true;
    }
     if (usingBuffer != NULL) {
        if (mUsingBuffer != NULL) {
            mUsingBuffer.clear();
            mUsingBuffer = NULL;
        }
        int64_t duration = -1;
        int32_t seq = -1;
        int32_t endFlag = -1;
        int64_t seqSize = -1;
        usingBuffer->meta()->findInt64("durationUs", &duration);
        usingBuffer->meta()->findInt32("SeqNum", &seq);
        usingBuffer->meta()->findInt64("SeqSize", &seqSize);
        usingBuffer->meta()->findInt32("seqEnd", &endFlag) ;

        ABuffer *tempBuffer = new ABuffer( usingBuffer->size());
        CHECK(tempBuffer != NULL);
        memcpy(tempBuffer->data(), usingBuffer->data(), usingBuffer->size());
        tempBuffer->setRange(0, usingBuffer->size());
        tempBuffer->meta()->setInt64("durationUs", duration);
        tempBuffer->meta()->setInt32("SeqNum", seq);
        tempBuffer->meta()->setInt64("SeqSize", seqSize);
        tempBuffer->meta()->setInt32("seqEnd", endFlag);
        tempBuffer->meta()->setInt32("usedFlag", 1);

        mUsingBuffer = tempBuffer;
        mCurReadSeq = seq;
        //mUsedBufferNum += 1;
        //ALOGD("MP-DEBUG1:set new buffer to the sp,now will use the buffer seqNumber=%d", mCurReadSeq);

        AString proTime;
        if (usingBuffer->meta()->findString("program-time",&proTime)){
            mStats.progTimeMs = parserProTimeMs(proTime,&mStats.progTimeZone);
            ALOGD("program-time is %lld",mStats.progTimeMs);
        }
     }
}

bool LiveDataSource::getSeqNumberInfo(int32_t * curPlay,int32_t * bufferBegin) {
    Mutex::Autolock autoLock(mLock);

    int32_t beginSeq = -1;
    *curPlay = mCurReadSeq;
    if (mBufferQueue.empty()) {
        *bufferBegin = beginSeq;
    }else {
        sp<ABuffer> buffer = *mBufferQueue.begin();
        buffer->meta()->findInt32("SeqNum", &beginSeq);
        *bufferBegin = beginSeq;
    }
    ALOGD("MP-DEBUG1:beginSeq =%d, mCurReadSeq =%d, curPlay =%d, bufferBegin =%d",beginSeq, mCurReadSeq, *curPlay,*bufferBegin);
    return true;
}

bool LiveDataSource::adjustPlayPosition(int32_t newSeqNumber, int32_t *downloadSeqNum){
    Mutex::Autolock autoLock(mLock);
    mIsSeekingNow = true;

    //calculate the buffer position
    // 1.change usedFlag value    2.change the mUsedTsFile value
    int32_t seqNumber = -1;
    int32_t endFlag = -1;
    int32_t usedFlag = -1;
    int32_t beginSeq = -1;
    ALOGD("MP-DEBUG1: mCurReadSeq = %d  mUsedTsFile = %d--", mCurReadSeq, mUsedTsFile);
    if (mBufferQueue.empty()) {
        if (mUsingBuffer != NULL) {
            mUsingBuffer.clear();
            mUsingBuffer = NULL;
        }
        *downloadSeqNum = newSeqNumber;
        mCurReadSeq = newSeqNumber;
        ALOGD("MP-DEBUG1:seek complete now, bufferQueue empty,new download seqNum=%d", newSeqNumber);
        mCondition.broadcast();
        mIsSeekingNow = false;
        return true;
    }

    sp<ABuffer> buffer = *mBufferQueue.begin();
    buffer->meta()->findInt32("SeqNum", &beginSeq);
    if ((newSeqNumber < beginSeq) || (newSeqNumber >= *downloadSeqNum) ) {
        ALOGD("MP-DEBUG1:seek to buffer not in bufferQueue");
        //reset();
        #ifdef LETV_HLS_OPTIMIZATION
        mBufferTotalSize = 0;
        mBufferTotalDuration = 0;
        memset(&mStats, 0, sizeof(mStats));
        #endif
        mFinalResult = OK;
        mBufferQueue.clear();
        mCurQueueSeqSize = 0;
        mCalConsumeTime = 0;
        mUsedTsFile = 0;
        mUsingBuffer = NULL;
        mTotalTsFile = 0;
        mBufferQueueEnd = true;
        mCurReadSeq = newSeqNumber;

        mIsSeekingNow = false;
        ALOGD("MP-DEBUG1: will down load the buffer seq =%d,,  minSeq =%d, maxSeq =%d", newSeqNumber, beginSeq, *downloadSeqNum);
        *downloadSeqNum = newSeqNumber;
        mUsedBufferNum = 0;
    }else if (newSeqNumber > mCurReadSeq) {
        List<sp<ABuffer> >::iterator it = mBufferQueue.begin();
        sp<ABuffer> buffer = *it;
        buffer->meta()->findInt32("SeqNum", &seqNumber);
        while (seqNumber < newSeqNumber) {
            //ALOGD("MP-DEBUG1:seek  change the usedFlag to --1-- seqNumber=%d, curReadSeq=%d", seqNumber, mCurReadSeq);
            buffer->meta()->findInt32("seqEnd", &endFlag);
            buffer->meta()->findInt32("usedFlag", &usedFlag);
            if ((usedFlag == 0) && (endFlag == 1)) {
                mUsedTsFile += 1;
            }
            if (usedFlag == 0) {
                mUsedBufferNum += 1;
            }
            buffer->meta()->setInt32("usedFlag", 1);
            if (it == mBufferQueue.end()) {
                ALOGD("MP-DEBUG1: seek to end seqNumber=%d", seqNumber);
                break;
            }
            ++it;
            buffer = *it;
            if (buffer != NULL)
                buffer->meta()->findInt32("SeqNum", &seqNumber);
        }
        clearUsedBuffer_l();
        ALOGD("MP-DEBUG1:seek to future already download");
    }else {
        List<sp<ABuffer> >::iterator it = mBufferQueue.end();
        --it;
        sp<ABuffer> buffer = *it;
        buffer->meta()->findInt32("SeqNum", &seqNumber);
        while(seqNumber >= newSeqNumber) {
            //ALOGD("MP-DEBUG1:seek  change the usedFlag to --0-- seqNumber=%d, curReadSeq=%d", seqNumber, mCurReadSeq);
            buffer->meta()->findInt32("seqEnd", &endFlag);
            buffer->meta()->findInt32("usedFlag", &usedFlag);
            if ((endFlag == 1) && (usedFlag == 1)) {
                mUsedTsFile -= 1;
            }
            if (usedFlag == 1) {
                mUsedBufferNum -= 1;
            }
            buffer->meta()->setInt32("usedFlag", 0);
            if (it == mBufferQueue.begin()) {
                 ALOGD("MP-DEBUG1:seek to begin seqNumber=%d", seqNumber);
                 break;
            }
            --it;
            buffer = *it;
            if (buffer != NULL)
                buffer->meta()->findInt32("SeqNum", &seqNumber);
        }
        ALOGD("MP-DEBUG1:seek to past already used");
    }

    if (!mBufferQueue.empty()) {
        List<sp<ABuffer> >::iterator it2 = mBufferQueue.end();
        int32_t endFlag = -1;
        while (it2 != mBufferQueue.begin()) {
            it2 --;
            sp<ABuffer> buffer = *it2;
            int64_t duration = -1;
            buffer->meta()->findInt64("durationUs", &duration);
            buffer->meta()->findInt32("SeqNum", &seqNumber);
            if (buffer->meta()->findInt32("seqEnd", &endFlag) && (endFlag == 1)) {
                *downloadSeqNum = seqNumber + 1;
                break;
            }
            ALOGD("erase seqNumber %d bufferSize %d ", seqNumber, buffer->size());
            mBufferQueue.erase(it2);
            mBufferTotalSize -= buffer->size();
            it2 = mBufferQueue.end();
        }
    }
    mCurReadSeq = newSeqNumber;

    ALOGD("MP-DEBUG1:seek complete now");
    if (mUsingBuffer != NULL) {
            mUsingBuffer.clear();
            mUsingBuffer = NULL;
    }
    mCondition.broadcast();
    mIsSeekingNow = false;
    return true;
}

}  // namespace android
