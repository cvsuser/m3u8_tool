
extern "C" {
	#include "curl/curl.h"
//        struct curl_slist;
};
#define INIT_PLAYLIST_SIZE 10*1024
#define INIT_SEGMENT_SIZE 1024*1024
#define LOW_SPEED_TIME 5L //time in seconds
#define LOW_SPEED_TIME1 20L //time in seconds
#define LOW_SPEED_LIMIT 1L //speed in bytes per second
#define HEAD_BLOCK_SIZE 256*1024

#define PLAYLISTCONNECTTIMEOUT 3
#define PLAYLISTTIMEOUT 5

#define SEGMENTCONNECTTIMEOUT 3 //s
#define SEGMENTTIMEOUT_MS 10 //ms

#define USER_AGENT_APPLE "AppleCoreMedia/1.0.0.9A405 (iPad; U; CPU OS 5_0_1 like Mac OS X; zh_cn)"
#define USER_AGENT_ANDROID "stagefright/1.2 (Linux;Android 4.0.3) Mozilla/5.0(iPad; U; CPU iPhone OS 3_2 like Mac OS X; en-us) AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 Mobile/7B314 Safari/531.21.10 QuickTime"

typedef struct callback_str{
    int seg_info;
    bool gotFirstByte;
    sp<ABuffer> buffer;             // the buffer to be queued
    sp<LiveDataSource> dataSource;  // if buffer is partial, queue to source
    int32_t *gotHeadDataCount;

    int64_t rangeStart;
    int64_t receiveSize;
    int64_t* seqSize;
    int32_t seqNumber ;
    int64_t seqDuration;
    AString * progTime;
}CALLBACK_PTR;

enum {
    SEGMENT_OK = 0,
    SEGMENT_UNSUPPORTED,
    SEGMENT_INVALID,
    SEGMENT_NOT_TS,
    SEGMENT_DROP,
    SEGMENT_CANCELED,
    SEGMENT_PARTIAL_FILE,
};

typedef struct sFetchFileContext {
    Mutex *pLock;
    bool *disconnecting;
    bool *seeking;
    bool cancelled;
    int32_t debug_seqNumber;
    sp<AMessage> notifyBack;
    int32_t percentNotified;

    AString * progTime;

    int32_t receiveOffset; //TODO: this is fetch segment result
    int64_t* seqenceSize;
    int64_t seqenceDuraion;
} FETCH_FILE_CONTEXT;

typedef struct sFetchFileResult {
    AString last_url;
    long response;
    int32_t curl_err;
} FETCH_FILE_RESULT;

struct curl_slist *mCurlHeaders;


static status_t mapCurlError(FETCH_FILE_RESULT* result) {
    CURLcode code = (CURLcode)result->curl_err;
    long response = result->response;
    if (response == 0) {
        switch (code) {
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_COULDNT_CONNECT:
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_COULDNT_RESOLVE_HOST:
                return ERROR_CANNOT_CONNECT;
            default:
                return UNKNOWN_ERROR;
        }
    } else {
        return ERROR_CANNOT_CONNECT;
    }
}
void init(){
	CURL *mCurlHandleFetchFile = curl_easy_init();
	CURL *mCurlHandleFetchPlaylist = curl_easy_init();	
	CURL *mCurlHandleGetUrl = curl_easy_init();
	
    CURL * _mCurlHandle = (CURL *)mCurlHandleFetchPlaylist;
    curl_easy_setopt(_mCurlHandle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(_mCurlHandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(_mCurlHandle, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(_mCurlHandle, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);
    curl_easy_setopt(_mCurlHandle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(_mCurlHandle, CURLOPT_CONNECTTIMEOUT, PLAYLISTCONNECTTIMEOUT);
    curl_easy_setopt(_mCurlHandle, CURLOPT_TIMEOUT, PLAYLISTTIMEOUT);

    _mCurlHandle = (CURL *)mCurlHandleFetchFile;

    curl_easy_setopt(_mCurlHandle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(_mCurlHandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(_mCurlHandle, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(_mCurlHandle, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME1);
    curl_easy_setopt(_mCurlHandle, CURLOPT_CONNECTTIMEOUT, SEGMENTCONNECTTIMEOUT);
    curl_easy_setopt(_mCurlHandle, CURLOPT_TIMEOUT_MS, SEGMENTTIMEOUT_MS);
    curl_easy_setopt(_mCurlHandle, CURLOPT_NOSIGNAL, 1);
}

void deinit(){
    if (mCurlHeaders!= NULL) {
        curl_slist_free_all(mCurlHeaders);
        mCurlHeaders = NULL;
         
    }
    curl_easy_cleanup((CURL *)mCurlHandleFetchFile);
    curl_easy_cleanup((CURL *)mCurlHandleFetchPlaylist);
    curl_easy_cleanup((CURL *)mCurlHandleGetUrl);
}

    if (mCurlHandleFetchFile != NULL) {
        CURL *_mCurlHandle = (CURL *)mCurlHandleFetchFile;
        if (!bUserAgentIsSet) {
            if (strcasestr(url,"qiyi.com") != NULL) //patch for qiyi
                curl_easy_setopt(_mCurlHandle, CURLOPT_USERAGENT, USER_AGENT_APPLE);
            else
                curl_easy_setopt(_mCurlHandle, CURLOPT_USERAGENT, USER_AGENT_ANDROID);
        }
    }
	
mCurlHeaders = curl_slist_append(mCurlHeaders, sHeadersString.string());


    sp<M3UParser> playlist = NULL;
    ALOGD("[M3U8] try parseURLs=%s", url.c_str());
    int32_t tryTimes = 0;
    do {
        playlist = fetchPlaylist(url.c_str(), &dummy);
        tryTimes ++;
    } while (playlist == NULL && tryTimes < 3 &&
            (CURLcode)mFetchPlaylistResult.curl_err == CURLE_OPERATION_TIMEDOUT);



status_t fetchFile_nonblock(
        const char *url, sp<ABuffer> *out,
        FETCH_FILE_CONTEXT *pContext,
        FETCH_FILE_RESULT *pResult) {

    CURL *hCurl = curl_easy_init();
    CURLcode err = CURLE_OK;
    
    int64_t *seqSizePtr = (int64_t*)malloc(sizeof(int64_t));
    if (seqSizePtr == NULL) {
        ALOGE("malloc failed for share value");
    }
    *seqSizePtr = -1;
    pContext->seqenceSize = seqSizePtr;
    if (mCurlHeaders != NULL) {
        err = curl_easy_setopt(hCurl, CURLOPT_HTTPHEADER, mCurlHeaders);
        if (err != CURLE_OK) { 
            ALOGD( "Curl curl_easy_setopt failed: %d---", err);//, curl_easy_strerror(err));
        }
    }
    curl_easy_setopt(hCurl, CURLOPT_URL, url);
    curl_easy_setopt(hCurl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(hCurl, CURLOPT_WRITEFUNCTION, &curlhandle_processFetchFileNonblock);
    curl_easy_setopt(hCurl, CURLOPT_WRITEDATA, (void *)out);
    curl_easy_setopt(hCurl, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(hCurl, CURLOPT_PROGRESSFUNCTION, &curlhandle_monitorFetchFileNonblock);
    curl_easy_setopt(hCurl, CURLOPT_PROGRESSDATA, (void *)pContext);

    curl_easy_setopt(hCurl, CURLOPT_HEADERFUNCTION, &curlhandle_showHeaders);
    curl_easy_setopt(hCurl, CURLOPT_HEADERDATA, url);

    curl_easy_setopt(hCurl, CURLOPT_FAILONERROR, true);

    if (!bUserAgentIsSet) {
        if (strcasestr(url,"qiyi.com") != NULL) //patch for qiyi
            curl_easy_setopt(hCurl, CURLOPT_USERAGENT, USER_AGENT_APPLE);
        else
            curl_easy_setopt(hCurl, CURLOPT_USERAGENT, USER_AGENT_ANDROID);
    }

    //start download
    err = curl_easy_perform(hCurl);

    curl_easy_setopt(hCurl, CURLOPT_NOPROGRESS, 0);

    //get download result
    pResult->curl_err = (int32_t)err;
    if (err == CURLE_OK) {
        char *effective_url;
        status_t geturl_err = curl_easy_getinfo(hCurl, CURLINFO_EFFECTIVE_URL, &effective_url);
        if (geturl_err == CURLE_OK)
            pResult->last_url.setTo(effective_url);
        else
            pResult->last_url.setTo(url);
    }
    else
        ALOGE("%s:%d curl return error %d(%s)",__func__,__LINE__,err,curl_easy_strerror(err));

    curl_easy_getinfo(hCurl, CURLINFO_RESPONSE_CODE, pResult->response);

    curl_easy_cleanup(hCurl);
    if (seqSizePtr != NULL) {
            free(seqSizePtr);
            seqSizePtr = NULL;
    }
    return err;

}

int32_t  fetchSegment(
        const char *url, sp<ABuffer> *out, int64_t rangeStart, FETCH_FILE_CONTEXT* pMonitorContext) {
     mSegError = SEGMENT_OK;
    CALLBACK_PTR processContext;
    int64_t *seqSizePtr = (int64_t*)malloc(sizeof(int64_t));
    if (seqSizePtr == NULL) {
        ALOGE("malloc failed for share value");
    }
    *seqSizePtr = -1;
    initFetchSegmentContext(processContext);
    processContext.seqSize = seqSizePtr;
    processContext.seqNumber = pMonitorContext->debug_seqNumber;
    processContext.seqDuration = pMonitorContext->seqenceDuraion;
    pMonitorContext->seqenceSize = seqSizePtr;
    processContext.rangeStart = rangeStart;
    int32_t status = SEGMENT_OK;

    if (pMonitorContext->progTime != NULL){
        processContext.progTime = pMonitorContext->progTime;
    }

    if (strncasecmp(url, "http://", 7)
        && strncasecmp(url, "https://", 8)) {
        ALOGE("fetchSegment url is unsupported: url %s", url);
        return SEGMENT_UNSUPPORTED;
    } else {
        {
            Mutex::Autolock autoLock(mLock);
            if (mDisconnectPending) {
                ALOGE("fetchSegment Disconnect LiveSession");
                return SEGMENT_CANCELED;
            }
            if (!mCurlHandleFetchFile) {
                ALOGE("libcurl : perform handle is NULL !!!");
                return SEGMENT_INVALID;
            }
        }

        CURLcode err = CURLE_OK;
        CURL * _mCurlHandle = (CURL *)mCurlHandleFetchFile;
        curl_easy_setopt(_mCurlHandle, CURLOPT_URL, url);
        if (mCurlHeaders != NULL) {
            err = curl_easy_setopt(_mCurlHandle, CURLOPT_HTTPHEADER, mCurlHeaders);
            if (err != CURLE_OK) {
                ALOGD( "Curl curl_easy_setopt failed: %d---", err);//, curl_easy_strerror(err));
            }
        }
        curl_easy_setopt(_mCurlHandle, CURLOPT_WRITEFUNCTION, &curlhandle_processSegmentData);
        curl_easy_setopt(_mCurlHandle, CURLOPT_WRITEDATA, (void *)&processContext);


        curl_easy_setopt(_mCurlHandle, CURLOPT_NOPROGRESS, 0);
        curl_easy_setopt(_mCurlHandle, CURLOPT_PROGRESSFUNCTION, &curlhandle_monitorFetchFileNonblock);
        curl_easy_setopt(_mCurlHandle, CURLOPT_PROGRESSDATA, (void *)pMonitorContext);

        curl_easy_setopt(_mCurlHandle, CURLOPT_HEADERFUNCTION, &curlhandle_showHeaders);
        curl_easy_setopt(_mCurlHandle, CURLOPT_HEADERDATA, url);

#if 0
    curl_easy_setopt(_mCurlHandle, CURLOPT_LOW_SPEED_LIMIT, 1);
    curl_easy_setopt(_mCurlHandle, CURLOPT_LOW_SPEED_TIME, 1);
#endif
        int64_t fetchStartUs = ALooper::GetNowUs();
        if (mIsFirstSegment) {
            ALOGD("[M3U8]----->post cmd for start first url:%s", url);
            mIsFirstSegment = false;
        }
        ALOGD("@debug: perform segment(%d)", pMonitorContext->debug_seqNumber);
        err = curl_easy_perform(_mCurlHandle);
        ALOGD("@debug: perform segment(%d), done", pMonitorContext->debug_seqNumber);
        int64_t fetchEndUs = ALooper::GetNowUs();
        if (fetchEndUs - fetchEndUs > 1000000ll) {
            ALOGD("@debug: fetchSegment spent %lld", fetchEndUs - fetchStartUs);
        }
        if (seqSizePtr != NULL) {
            free(seqSizePtr);
            seqSizePtr = NULL;
        }
        //to save received size
        pMonitorContext->receiveOffset = processContext.receiveSize;
        mSegError = processContext.seg_info;

        //test code
        char value[PROPERTY_VALUE_MAX];
        int32_t int_val = 0;
        property_get("media.ts.partial", value, "0");
        sscanf(value, "%d", &int_val);
        if ((int_val == 1) ) {
            if (!mTestFlag) {
                err = CURLE_PARTIAL_FILE;
                mTestFlag = true;
            }
        }else {
            if (mTestFlag) {
                mTestFlag = false;
            }
        }

        if (err == CURLE_OK) {
            *out = processContext.buffer;
            if (pMonitorContext->receiveOffset < rangeStart) {
                //some server may return error text file, ex. 'bad file'
                ALOGW("curl ok but unexpected receiveOffset %d < %d", pMonitorContext->receiveOffset, rangeStart);
                pMonitorContext->receiveOffset = rangeStart; //pretend it has downloaded size of rangeStart
                return SEGMENT_DROP;
            } else {
                return SEGMENT_OK;
            }
        }
        else
            ALOGE("%s:%d curl return error %d(%s)",__func__,__LINE__,err,curl_easy_strerror(err));

        ALOGW("fetchSegment: error %d seg_info %d, bufsize = %d",
                err, processContext.seg_info, processContext.buffer->size());

        //pretend it has downloaded size of rangeStart
        if (pMonitorContext->receiveOffset < rangeStart) {
            pMonitorContext->receiveOffset = rangeStart;
        }

        if (err == CURLE_ABORTED_BY_CALLBACK) {
            CHECK(pMonitorContext->cancelled);
            return SEGMENT_CANCELED;
        }else if (err == CURLE_PARTIAL_FILE) {
            ALOGD("download partial file");
            return SEGMENT_PARTIAL_FILE;
        }else {
            if(processContext.buffer->size() != 0) {
                *out = processContext.buffer;
            }
            return SEGMENT_DROP;
        }
    }
    
    return processContext.seg_info;
}


int  curlhandle_monitorFetchFileNonblock(
        void *callback_data, double dltotal, double dlnow,
        double ultotal, double ulnow) {
    FETCH_FILE_CONTEXT *pContext = (FETCH_FILE_CONTEXT *)callback_data;
    if (dltotal > 0) {
        *(pContext->seqenceSize) = (int64_t)dltotal;
    }
    //ALOGD("monitor: %f/%f", dlnow, dltotal);
    pContext->pLock->lock(); {
        if (*(pContext->disconnecting) || *(pContext->seeking)) {
            pContext->cancelled = true;
            pContext->pLock->unlock();
            return -1;
        }

        if (pContext->notifyBack != NULL){
            int32_t percent ;
            percent = (int32_t)(dlnow * 100 / dltotal);
            if (percent > pContext->percentNotified) {
                sp<AMessage> msg = pContext->notifyBack->dup();
                msg->setInt32("itemDownloadPercent", percent);
                msg->setInt64("tsFileSize", (int64_t)dltotal);
                pContext->percentNotified = percent;
                msg->post();
            }
        }

    } pContext->pLock->unlock();
    return 0;
}

int  curlhandle_processFetchFileNonblock(void *buffer, int32_t size, int32_t nmemb, void *data) {
    sp<ABuffer> *p = (sp<ABuffer> *)data;
    sp<ABuffer> _CurlBuffer = *p;

    size_t bufferRemaining = _CurlBuffer->capacity() - _CurlBuffer->size();
    if (bufferRemaining < size * nmemb) {
        bufferRemaining = size * nmemb + 1024 * 1024;
        ALOGD("fetchFile nonblock: increasing download buffer to %d",
               _CurlBuffer->size() + bufferRemaining);
        sp<ABuffer> copy = new ABuffer(_CurlBuffer->size() + bufferRemaining);
        CHECK(copy != NULL);
        memcpy(copy->data(), _CurlBuffer->data(), _CurlBuffer->size());
        copy->setRange(0, _CurlBuffer->size());
        _CurlBuffer = copy;
        *p = copy;
    }
    memcpy(_CurlBuffer->data() + _CurlBuffer->size(), buffer, size * nmemb);
    _CurlBuffer->setRange(0, _CurlBuffer->size() + (size_t)size * nmemb);
    return size * nmemb;

}
void  initFetchFileContext(FETCH_FILE_CONTEXT &context,
        FETCH_FILE_RESULT &result) {
    context.pLock = &mLock;
    context.disconnecting = &mDisconnectPending;
    context.seeking = &mSeeking;
    context.cancelled = false;
    context.receiveOffset = 0;
    context.percentNotified = 0;

    result.last_url.setTo("");
    result.response = 0;
    result.curl_err = CURLE_OK;
    context.notifyBack = NULL;
    context.seqenceSize = NULL;
    context.progTime   = NULL;
}

void  initFetchSegmentContext(CALLBACK_PTR &cl_ptr) {
    cl_ptr.dataSource = mDataSource;
    cl_ptr.buffer = new ABuffer(INIT_SEGMENT_SIZE);
    CHECK(cl_ptr.buffer != NULL);
    cl_ptr.buffer->setRange(0, 0);
    cl_ptr.gotFirstByte = false;
    cl_ptr.gotHeadDataCount = &mGotHeadDataCount;
    cl_ptr.rangeStart = 0;
    cl_ptr.receiveSize = 0;
    cl_ptr.seg_info = SEGMENT_OK;
    cl_ptr.seqSize = NULL;
    cl_ptr.seqNumber = -1;
    cl_ptr.seqDuration = -1;
    cl_ptr.progTime = NULL;
}
size_t  curlhandle_showHeaders(void *ptr, size_t size, size_t nmemb,
        void *stream) {
    if ( NULL == ptr) {
        ALOGD("ptr is NULL,error!");
    }else{
        ALOGD("%s", (char *)ptr);
    }
    return (nmemb*size);
}

int32_t  curlhandle_processSegmentData(void * buffer, int32_t size, int32_t nmemb, void * priv)
{
    int32_t copy_size = size*nmemb;
    CALLBACK_PTR * handle = (CALLBACK_PTR *)priv;
    handle->buffer->meta()->setInt64("durationUs", handle->seqDuration);
    handle->buffer->meta()->setInt32("SeqNum", handle->seqNumber);
    int64_t tempSize = *(handle->seqSize);
    if (tempSize > 0) {
        handle->buffer->meta()->setInt64("SeqSize", *(handle->seqSize));
    }

    sp<ABuffer> _CurlBuffer = handle->buffer;
    sp<LiveDataSource> dataSource = handle->dataSource;

    if (handle->progTime != NULL){
        _CurlBuffer->meta()->setString("program-time",handle->progTime->c_str());
        handle->progTime = NULL;// only set once
    }

    uint8_t *srcBuffer = (uint8_t *)buffer;
    int32_t needSkipSize = handle->rangeStart - handle->receiveSize;
    if (needSkipSize > 0) {
        static int32_t i = 0;
        i ++;
        if (i > 5) {
            ALOGD("rangeStart %lld, received %lld, receiving size %d",
                handle->rangeStart, handle->receiveSize, copy_size);
            i = 0;
        }
        handle->gotFirstByte = true;
        if (needSkipSize >= size*nmemb) {
            handle->receiveSize += size*nmemb;
            return size*nmemb;
        }
        srcBuffer += needSkipSize;
        handle->receiveSize += needSkipSize;
        copy_size = copy_size - needSkipSize;
        ALOGD("\tedge copy size %d at %lld", copy_size, handle->receiveSize);
    }

    if(handle->gotFirstByte == false && srcBuffer[0] != 0x47) {
            ALOGD("processSegmentData: this buffer is invalid %2x %2x %2x %2x",
                    srcBuffer[0], srcBuffer[1], srcBuffer[2], srcBuffer[3]);
            handle->seg_info = SEGMENT_NOT_TS;
            return -1;
    }
    handle->gotFirstByte = true;

    //ALOGV("_CurlBuffer->size() %d, rec size %d", _CurlBuffer->size(), rec_size);
    if(*(handle->gotHeadDataCount) < 10){
        //to queue buffer quicker in first HEAD_BLOCK_SIZE*10
        if(_CurlBuffer->size() >= HEAD_BLOCK_SIZE){
            dataSource->queueBuffer(_CurlBuffer);
            ALOGD("segment data splitted (x%x) %d vs. %d, gotHeadDataCount %d",
                    _CurlBuffer->data()[0], _CurlBuffer->size(),
                    HEAD_BLOCK_SIZE, *(handle->gotHeadDataCount));

            _CurlBuffer = new ABuffer(INIT_SEGMENT_SIZE);
            _CurlBuffer->setRange(0, 0);
            handle->buffer = _CurlBuffer;
            (*(handle->gotHeadDataCount))++;
        }
    }else if(_CurlBuffer->size() >= INIT_SEGMENT_SIZE){
        int capacity = _CurlBuffer->capacity();
        ALOGV("_CurlBuffer->size() >= INIT_SEGMENT_SIZE, capacity is %d", capacity);
        dataSource->queueBuffer(_CurlBuffer);
        //_CurlBuffer = new ABuffer(INIT_SEGMENT_SIZE);
        _CurlBuffer = new ABuffer(capacity);
        _CurlBuffer->setRange(0, 0);
        handle->buffer = _CurlBuffer;

        //simulate abort downloading
/*
        char value[64];
        if (property_get("media.httplive.debug", value, NULL)) {
            int32_t debugSize = atoi(value);
            ALOGD("@debug: error triggered ready %d", debugSize);
            if (debugSize != 0) {
                if (handle->receiveSize > debugSize) {
                    ALOGD("@debug: error triggered ok");
                    property_set("media.httplive.debug", "0");
                    return -1;
                }
            }
        }
*/
    }

    //realloc buffer if necessary
    size_t bufferRemaining = _CurlBuffer->capacity() - _CurlBuffer->size();
    if(bufferRemaining < copy_size){
        ALOGD("processSegmentData: increasing download buffer to %d bytes",
            _CurlBuffer->size() + copy_size);
        sp<ABuffer> copy = new ABuffer(_CurlBuffer->size() + copy_size);
        CHECK(copy != NULL);
        memcpy(copy->data(), _CurlBuffer->data(), _CurlBuffer->size());
        copy->setRange(0, _CurlBuffer->size());
        _CurlBuffer = copy;
        handle->buffer = copy;
    }
    memcpy(_CurlBuffer->data() + _CurlBuffer->size(), srcBuffer, copy_size);
    _CurlBuffer->setRange(0, _CurlBuffer->size() + (size_t)copy_size);
    handle->receiveSize += copy_size;
    return size*nmemb;
}
void  addUriParameter(AString *uri) {
    if ((uri == NULL) || (!mLetvLocalHost)) {
        return;
    }

    LiveDataSource::LiveDataStatistic stats;
    if (mDataSource->getStatistic(stats)) {
        int32_t playingSeq = stats.playingSeqNum;
        uri->append("&playing=");
        uri->append(playingSeq);
    }

}



void  onDownloadNext() {
    ALOGV("onDownloadNext()");
    size_t bandwidthIndex = getBandwidthIndex();

rinse_repeat:
    ALOGV("onDownloadNext() rinse_repeat");
    int64_t nowUs = ALooper::GetNowUs();

    if (mLastPlaylistFetchTimeUs < 0
            || (ssize_t)bandwidthIndex != mPrevBandwidthIndex
            || (!mPlaylist->isComplete() && timeToRefreshPlaylist(nowUs))) {
        AString url;
        if (mBandwidthItems.size() > 0) {
            url = mBandwidthItems.editItemAt(bandwidthIndex).mURI;
        } else {
            url = mMasterURL;
        }
        bool firstTime = (mPlaylist == NULL);
        if ((ssize_t)bandwidthIndex != mPrevBandwidthIndex) {
            // If we switch bandwidths, do not pay any heed to whether
            // playlists changed since the last time...
            mPlaylist.clear();
        }
        bool unchanged;
        sp<M3UParser> playlist = fetchPlaylist(url.c_str(), &unchanged);
        if (playlist == NULL) {
            Mutex::Autolock autoLock(mLock);
            if (unchanged) {
                // We succeeded in fetching the playlist, but it was
                // unchanged from the last time we tried.
            } else if (!mSeeking) {
                ALOGE("failed to load playlist at url '%s'", url.c_str());
                status_t last_err = OK;
                if (!complainPlaylistError(&last_err)) {
                    mDataSource->queueEOS(last_err);
                } else {
                    postMonitorQueue(1000000ll);
                }
                return;
            } else {
                ALOGV("fetchPlaylist stopped due to seek, let seek complete");
                return;
            }
        } else {
            mPlaylist = playlist;
        }
        if (firstTime ||mGotDuration) {
            Mutex::Autolock autoLock(mLock);

            if (!mPlaylist->isComplete()) {
                mDurationUs = -1;
            } else {
                mDurationUs = 0;
                for (size_t i = 0; i < mPlaylist->size(); ++i) {
                    sp<AMessage> itemMeta;
                    CHECK(mPlaylist->itemAt(
                                i, NULL /* uri */, &itemMeta));
                    int64_t itemDurationUs;
                    CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
                    mDurationUs += itemDurationUs;
                }
            }
            mGotDuration = true;
            ALOGD("parseURLs Success! urlNum :%d", mPlaylist->size());
        }
        mLastPlaylistFetchTimeUs = ALooper::GetNowUs();
    }
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &mFirstSeqNumber)) {
        mFirstSeqNumber = 0;
    }
    bool explicitDiscontinuity = false;
    bool bandwidthChanged = false;
    if (mSeqNumber < 0) {
        mSeqNumber = mFirstSeqNumber;
    }

    int32_t lastSeqNumberInPlaylist =
        mFirstSeqNumber + (int32_t)mPlaylist->size() - 1;
    if (mSeqNumber < mFirstSeqNumber
            || mSeqNumber > lastSeqNumberInPlaylist) {
        if (mPrevBandwidthIndex != (ssize_t)bandwidthIndex) {
            // Go back to the previous bandwidth.
            ALOGW("new bandwidth does not have the sequence number "
                 "we're looking for, switching back to previous bandwidth");
            mLastPlaylistFetchTimeUs = -1;
            //Get BW index based on current estimated BW
            size_t estBWIndex = getBandwidthIndex();
            if (estBWIndex == bandwidthIndex) {
               bandwidthIndex = mPrevBandwidthIndex;
            }
           else {
               bandwidthIndex = estBWIndex;
            }
            goto rinse_repeat;
        }
        if (!mPlaylist->isComplete()){
            if(++mNumRetries >= 1000){
                mNumRetries = 0;
            }
            if (mSeqNumber > lastSeqNumberInPlaylist) {
                mLastPlaylistFetchTimeUs = -1;
                ALOGD("mSeqNumber %d > lastSeqNumberInPlaylist %d, retry count %d",
                    mSeqNumber, lastSeqNumberInPlaylist, mNumRetries);
                postMonitorQueue(3000000ll);
                return;
            }
            // we've missed the boat, let's start from the lowest sequence
            // number available and signal a discontinuity.
            ALOGW("We've missed the boat, restarting playback.");
            mSeqNumber = mFirstSeqNumber;
            explicitDiscontinuity = true;
            // fall through
        } else {
            ALOGV("Cannot find sequence number %d in playlist "
                 "(contains %d - %d)",
                 mSeqNumber, mFirstSeqNumber,
                 mFirstSeqNumber + mPlaylist->size() - 1);

            mDataSource->queueEOS(ERROR_END_OF_STREAM);
            return;
        }
    }
    mNumRetries = 0;

    if (mPrevBandwidthIndex != (ssize_t)bandwidthIndex) {
        char value[PROPERTY_VALUE_MAX];
        if(property_get("httplive.enable.discontinuity", value, NULL) &&
           (!strcasecmp(value, "true") || !strcmp(value, "1")) ) {
           bandwidthChanged = true;
           ALOGW("discontinuity property set, queue discontinuity");
        }
        else {
           bandwidthChanged = false;
        }
        if (mPrevBandwidthIndex >= 0) {
           ALOGW("BW changed from index %d to index %d",
                    (int32_t)mPrevBandwidthIndex, bandwidthIndex);
        }
    }
    if (mPrevBandwidthIndex < 0) {
        // Don't signal a bandwidth change at the very beginning of
        // playback.
        bandwidthChanged = false;
    }
    mPrevBandwidthIndex = bandwidthIndex;

    AString uri;
    sp<AMessage> itemMeta;
skipe:
    ALOGV("onDownloadNext() skipe");
    CHECK(mPlaylist->itemAt(
                mSeqNumber - mFirstSeqNumber,
                &uri,
                &itemMeta));
    int32_t val;
    if (itemMeta->findInt32("discontinuity", &val) && val != 0) {
        explicitDiscontinuity = true;
    }
    AString value;
    bool haveProTime = false;
    if (itemMeta->findString("program-time",&value)){
        ALOGD("program-time is %s",value.c_str());
        if(mFirstSigment || explicitDiscontinuity || 1){
            haveProTime = true;
            mFirstSigment = false;
        }
    }
    bool skipLastPartialSegment = mPartialSegment.offset != 0 && mPartialSegment.seqNum != mSeqNumber;
    if (skipLastPartialSegment) {
        mDataSource->clearPartialBuffer();
        mPartialSegment.offset = 0;
        mPartialSegment.seqNum = mSeqNumber;
    }

    if (explicitDiscontinuity || bandwidthChanged || skipLastPartialSegment) {
        // Signal discontinuity.
        ALOGW("queueing discontinuity (explicit=%d, bandwidthChanged=%d, %d)",
              explicitDiscontinuity, bandwidthChanged, skipLastPartialSegment);
        sp<ABuffer> tmp = new ABuffer(188);
        memset(tmp->data(), 0, tmp->size());
        // signal a 'hard' discontinuity for explicit or bandwidthChanged.
        tmp->data()[1] = (explicitDiscontinuity || bandwidthChanged) ? 1 : 0;
        mDataSource->queueBuffer(tmp);
    }

    status_t err;
    addUriParameter(&uri);
    ALOGD("fetchSegment: mSeqNumber %d url %s", mSeqNumber, uri.c_str());
    sp<ABuffer> buffer;

    //fetch process context
    int64_t rangeStart = 0;
    if (mPartialSegment.offset != 0 && mPartialSegment.seqNum == mSeqNumber) {
        rangeStart = mPartialSegment.offset;
        ALOGD("resume to fetch from %lld", rangeStart);
    }

    //fetch monitor context
    FETCH_FILE_CONTEXT monitor_context;
    initFetchFileContext(monitor_context, mFetchSegmentResult);
    monitor_context.notifyBack = mNotify->dup();
    int64_t itemDurationUs;
    monitor_context.notifyBack->setInt32("what", kWhatLiveSessionTempBuffered);
    if (itemMeta != NULL) {
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
    }
    monitor_context.notifyBack->setInt64("newItemDuration",  itemDurationUs);
    monitor_context.notifyBack->setInt32("newItemSeqNum",  mSeqNumber);
    if (haveProTime)
        monitor_context.progTime = &value;
    monitor_context.debug_seqNumber = mSeqNumber;
    monitor_context.seqenceDuraion = itemDurationUs;
    ALOGV("MP-DEBUG: itemDurationUs=%lld", monitor_context.seqenceDuraion);

    err = fetchSegment(uri.c_str(), &buffer, rangeStart, &monitor_context);

    if (err == SEGMENT_OK) {
        CHECK(buffer != NULL);
        if (buffer->size() == 0) {
            ALOGW("fetchSegment: buffer size is 0");
       } else {
            int64_t itemDurationUs = 0;
            CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));
            mDownloadPosUs += itemDurationUs;

            buffer->meta()->setInt64("durationUs", itemDurationUs);
            buffer->meta()->setInt32("SeqNum", mSeqNumber);
            buffer->meta()->setInt32("seqEnd", 1);
            mDataSource->queueBuffer(buffer);
            mHungry = false;
            sp<AMessage> msg = mNotify->dup();
            msg->setInt32("what", kWhatLiveSessionUpdateBufferInfo);
            msg->post();
        }
        clearPartialSegment();
    } else if ((err == SEGMENT_CANCELED) && (mSegError != SEGMENT_NOT_TS)) {
        ALOGE("fetchFile is cancelled, ignore this");
        clearPartialSegment();
        return;
    } else {
        ALOGD("fetchSegment %d fail(%d), (seeking %d)", mSeqNumber, err, mSeeking);
        mDownloadRetryCount += 1;
        //report error if too many complain
        status_t last_err = OK;
        if ((!complainSegmentError(&last_err)) && mCanNotify) {
            mDataSource->clearPartialBuffer();
            mDataSource->queueEOS(last_err);
            mCanNotify = false;
            ALOGE("fetchFile exit because much complain");
            return;
        }

        //live source, just to skip current sequence
        if (((!mPlaylist->isComplete()) && (err != SEGMENT_PARTIAL_FILE) && (mDownloadRetryCount > 10)) || (mSegError == SEGMENT_NOT_TS) ) {
            ALOGE("skip seq %d because error", mSeqNumber);
            mDataSource->clearPartialBuffer();
            mSeqNumber ++;
            mDownloadRetryCount = 0;
            mCanNotify = true;
            postMonitorQueue();
            return;
        }
        //error
        //
         mCanNotify = false;

        //try again from current offset
        mPartialSegment.offset = monitor_context.receiveOffset;
        mPartialSegment.seqNum = mSeqNumber;
        ALOGD("\tuncompleted download: %lld, seq=%d", mPartialSegment.offset, mPartialSegment.seqNum);
        if (buffer!=NULL && buffer->size() != 0) {
            mDataSource->queueBuffer(buffer);
            ALOGD("partial buffer %d", buffer->size());
        }
        ALOGD("curl err--%s", __FUNCTION__);
        sp<AMessage> msgForInfo = mNotify->dup();
        msgForInfo->setInt32("what", kWhatLiveSessionNotifyInfo);
        msgForInfo->setInt32("errorInfo", (int32_t)err);
        msgForInfo->post();
        //try again
        postMonitorQueue(1000000ll);
        return;
    }
    ++mSeqNumber;
    postMonitorQueue();
}


// there is error when fetch playlist
// return true if complain in success, which means you can try more
// return false if complain in fail, which means you have to report an error
bool  complainPlaylistError(status_t *last_err) {
    *last_err = OK;

    int64_t duration, size;
    mDataSource->getBufferedSize(&duration, &size);

    bool wasHungry = mHungry;
    int64_t lastTimeUs = mStartHungryTimeUs;
    //mHungry = (size <= 0);
    mHungry = (duration < mLowWaterMarkUs);

    ALOGD("@debug: complain m3u8 error, hungry(%d, %d), %lld", wasHungry, mHungry, mStartHungryTimeUs);
    if (!mHungry) {
        mStartHungryTimeUs = -1;
        return true;
    }
    if (!wasHungry) {
        mStartHungryTimeUs = ALooper::GetNowUs();
        ALOGD("@debug: start hungery at %lld", mStartHungryTimeUs);
        return true;
    }
    ALOGD("@debug: m3u8 hungery since %lld", ALooper::GetNowUs() - mStartHungryTimeUs);

    if (ALooper::GetNowUs() - mStartHungryTimeUs < 10000000ll) {
        return true;
    }

    ALOGD("m3u8 curl err = %d, response = %ld", mFetchPlaylistResult.curl_err, mFetchPlaylistResult.response);

    *last_err = mapCurlError(&mFetchPlaylistResult);

    return false;

}

// there is error when fetch segment
// return true if complain in success, which means you can try more
// return false if complain in fail, which means you have to report an error
bool  complainSegmentError(status_t *last_err) {
    //*last_err = OK;
    //return true;

    int64_t duration, size;
    mDataSource->getBufferedSize(&duration, &size);

    bool wasHungry = mHungry;
    int64_t lastTimeUs = mStartHungryTimeUs;
    //mHungry = (size <= 0);
    mHungry = (duration < mLowWaterMarkUs);

    ALOGD("@debug: complain ts error, hungry(%d, %d), %lld", wasHungry, mHungry, mStartHungryTimeUs);
    if (!mHungry) {
        mStartHungryTimeUs = -1;
        return true;
    }
    if (!wasHungry) {
        mStartHungryTimeUs = ALooper::GetNowUs();
        ALOGD("@debug: start hungery at %lld", mStartHungryTimeUs);
        return true;
    }
    ALOGD("@debug: ts hungery since %lld", ALooper::GetNowUs() - mStartHungryTimeUs);

    if (ALooper::GetNowUs() - mStartHungryTimeUs < 10000000ll) {
        return true;
    }

    ALOGD("ts curl err = %d, response = %ld", mFetchSegmentResult.curl_err, mFetchSegmentResult.response);

    *last_err = mapCurlError(&mFetchSegmentResult);
    return false;

}

status_t  setParameter(int key,void * data,size_t size) {
    if (key == 9501) {
        CHECK(size = sizeof(int32_t));
        int32_t *value = (int32_t*)data;
        mLowWaterMarkUs = (int64_t)(*value) * 1000;
        ALOGD("low watermark %.1f", mLowWaterMarkUs / 1E3);
    } else if (key == 9502) {
        CHECK(size = sizeof(int32_t));
        int32_t *value = (int32_t*)data;
        mHighWaterMarkUs = (int64_t)(*value) * 1000;
        ALOGD("high watermark %.1f", mHighWaterMarkUs / 1E3);
    }
    return OK;
}
