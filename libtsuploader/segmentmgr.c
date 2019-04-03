#include "segmentmgr.h"
#include "resource.h"
#include "queue.h"
#include "uploader.h"
#include "utils.h"
#include "b64/urlsafe_b64.h"
#include <qupload.h>
#include "httptools.h"
#include <unistd.h>
#include "log/log.h"
#include "cJSON/cJSON.h"

#define SEGMENT_RELEASE 1
#define SEGMENT_UPDATE 2
#define SEGMENT_QUIT 3
typedef struct {
        SegmentHandle handle;
        uint8_t nOperation;
        LinkSession session;
        char *pSegMeta;
}SegInfo;

typedef struct {
        SegmentHandle handle;
        int segReportOk;
        LinkUploadParamCallback getUploadParamCallback;
        void *pGetUploadParamCallbackArg;
        UploadStatisticCallback pUploadStatisticCb;
        void *pUploadStatArg;
        int nReportTTL;
        int64_t nNextUpdateSegTimeInSecond;
        LinkSession tmpSession; //TODO backup report info when next report time is not arrival
        char *pSegMeta;
}Seg;

typedef struct {
        pthread_t segMgrThread_;
        LinkCircleQueue *pSegQueue_;
        Seg handles[8];
        int nReportTTL;
        int nQuit_;
}SegmentMgr;

static SegmentMgr segmentMgr;
static pthread_mutex_t segMgrMutex = PTHREAD_MUTEX_INITIALIZER;
static int segMgrStarted = 0;

static int checkShouldReport(Seg* pSeg, LinkSession *pCurSession) {
        int64_t nNow = LinkGetCurrentNanosecond() / 1000000000LL;
        pSeg->tmpSession.nTsSequenceNumber = pCurSession->nTsSequenceNumber;
        if (pCurSession->isNewSessionStarted != 0) {
                memcpy(&pSeg->tmpSession, pCurSession, sizeof(LinkSession));
                return 0;
        }
        pSeg->tmpSession.nAccSessionAudioDuration = pCurSession->nAccSessionAudioDuration;
        pSeg->tmpSession.nAccSessionVideoDuration = pCurSession->nAccSessionVideoDuration;
        pSeg->tmpSession.nTotalSessionAudioDuration = pCurSession->nTotalSessionAudioDuration;
        pSeg->tmpSession.nTotalSessionVideoDuration = pCurSession->nTotalSessionVideoDuration;

        if (pCurSession->nSessionEndResonCode == 0) {
                if (pSeg->nNextUpdateSegTimeInSecond > nNow) {
                        pSeg->tmpSession.nVideoGapFromLastReport += pCurSession->nVideoGapFromLastReport;
                        pSeg->tmpSession.nAudioGapFromLastReport += pCurSession->nAudioGapFromLastReport;
                        return 0;
                }
        }

        //if (pSeg->segReportOk) return 0;
        

        if (pSeg->tmpSession.nVideoGapFromLastReport > 0)
                pCurSession->nVideoGapFromLastReport += pSeg->tmpSession.nVideoGapFromLastReport;
        if (pSeg->tmpSession.nAudioGapFromLastReport > 0)
                pCurSession->nAudioGapFromLastReport += pSeg->tmpSession.nAudioGapFromLastReport;
        pSeg->tmpSession.nVideoGapFromLastReport = 0;
        pSeg->tmpSession.nAudioGapFromLastReport = 0;
        return 1;
}

/*
 {
 "session": "<session>", // 切片会话
 "start": <startTimestamp>, // 切片会话的开始时间戳，毫秒
 "current": <currentTimestamp>, // 当前时间戳, 毫秒
 "sequence": <sequence>, // 最新的切片序列号
 "vd": <videoDurationMS>, // 距离前一次上报的切片视频内容时长, 毫秒
 "ad": <audioDurationMS>, // 距离前一次上报的切片音频内容时长, 毫秒
 "tvd": <totalVideoDuration>, // 本次切片会话的视频总时长，毫秒
 "tad": <totalAudioDuration>, // 本次切片会话的音频总时长，毫秒
 "end": "<endTimestamp>", // 切片会话结束时间戳,毫秒，如果会话没结束不需要本字段
 "endReason": <endReason> // 切片会话结束的原因，如果会话没结束不需要本字段
 }
 */
static const char *sReasonStr[] ={"---", "normal", "timeout", "force", "destroy"};
static int reportSegInfo(LinkSession *s , int idx, int rtype) {
        char buffer[1280];
        int nReportHostLen = 640;
        char *body = buffer + nReportHostLen;
        int nBodyLen = 0;
        memset(buffer, 0, sizeof(buffer));
        
        char* smeta = segmentMgr.handles[idx].pSegMeta;
        if (s->nSessionEndResonCode != 0) {
                const char * reason = sReasonStr[s->nSessionEndResonCode];
                if (s->nSessionEndTime <= 0){
                        s->nSessionEndTime = s->nSessionStartTime + s->nAccSessionVideoDuration*1000000;
                        LinkLogWarn("abnormal session:%s %"PRId64"", s->sessionId, s->nSessionEndTime);
                }
                
                nBodyLen = sprintf(body, "{ \"session\": \"%s\", \"start\": %"PRId64", \"current\": %"PRId64", \"sequence\": %"PRId64","
                        " \"vd\": %"PRId64", \"ad\": %"PRId64", \"avd\": %"PRId64", \"aad\": %"PRId64", \"end\":"
                        " %"PRId64", \"tvd\": %"PRId64", \"tad\": %"PRId64", \"endReason\": \"%s\",\"frameStatus\":%d",
                        s->sessionId, s->nSessionStartTime/1000000, LinkGetCurrentNanosecond()/1000000, s->nTsSequenceNumber,
                        s->nVideoGapFromLastReport, s->nAudioGapFromLastReport,
			s->nAccSessionVideoDuration, s->nAccSessionAudioDuration, s->nSessionEndTime/1000000,
                                   s->nTotalSessionVideoDuration, s->nTotalSessionAudioDuration, reason,
                                   s->coverStatus);
                segmentMgr.handles[idx].tmpSession.nAccSessionVideoDuration = 0;
                segmentMgr.handles[idx].tmpSession.nAccSessionAudioDuration = 0;
                segmentMgr.handles[idx].tmpSession.nSessionEndTime = 0;
                segmentMgr.handles[idx].tmpSession.coverStatus = 0;
        } else {
                nBodyLen = sprintf(body, "{ \"session\": \"%s\", \"start\": %"PRId64", \"current\": %"PRId64", \"sequence\": %"PRId64","
                        " \"vd\": %"PRId64", \"ad\": %"PRId64", \"avd\": %"PRId64", \"aad\": %"PRId64",\"frameStatus\":%d,"
                                   "\"tvd\": %"PRId64", \"tad\": %"PRId64"",
                        s->sessionId, s->nSessionStartTime/1000000, LinkGetCurrentNanosecond()/1000000, s->nTsSequenceNumber,
                        s->nVideoGapFromLastReport, s->nAudioGapFromLastReport,
                        s->nAccSessionVideoDuration, s->nAccSessionAudioDuration,  s->coverStatus,
                                   s->nTotalSessionVideoDuration, s->nTotalSessionAudioDuration);
        }
        
        if (smeta) {
#if 1
                int nSessionMetaLen = sprintf(body + nBodyLen, ",%s", smeta);
                nBodyLen += nSessionMetaLen;
#else
                int i = 0;
                int nSessionMetaLen = sprintf(body + nBodyLen, ",\"meta\":{");
                nBodyLen += nSessionMetaLen;
                for (i = 0; i < smeta->len; i++) {
                        nBodyLen += sprintf(body + nBodyLen, "\"%s\":\"%s\",", smeta->keys[i], smeta->values[i]);
                }
                body[nBodyLen-1] = '}';
#endif
        }
        body[nBodyLen++] = '}';
        
        assert(nBodyLen <= sizeof(buffer) - nReportHostLen);
        LinkLogDebug("%dbody:%s",rtype, body);
        
        LinkUploadParam param;
        memset(&param, 0, sizeof(param));
        char *reportHost = buffer;
        char ak[41] = {0};
        char sk[41] = {0};
        
        param.pSegUrl = reportHost;
        param.nSegUrlLen = nReportHostLen;
        param.pAk = ak;
        param.nAkLen = sizeof(ak);
        param.pSk = sk;
        param.nSkLen = sizeof(sk);

        int getParamOk = 0;
        int ret = segmentMgr.handles[idx].getUploadParamCallback(segmentMgr.handles[idx].pGetUploadParamCallbackArg,
                                                                 &param, LINK_UPLOAD_CB_GETPARAM);
        if (ret != LINK_SUCCESS) {
                if (ret == LINK_BUFFER_IS_SMALL) {
                        LinkLogError("param buffer is too small. drop seg");
                } else {
                        LinkLogError("not get param yet:%d", ret);
                }
        } else {
                getParamOk = 1;
        }
        
        int nUrlLen = param.nSegUrlLen;
        reportHost[nUrlLen] = 0;
        char * pToken = reportHost + nUrlLen + 1;
        int nTokenOffset = snprintf(pToken, nReportHostLen-nUrlLen-1, "%s:", param.pAk);
        int nOutputLen = 30;
        //fprintf(stderr, "-------->%s:%d %s body:%s:%d\n", param.pSk, param.nSkLen,reportHost, body,nBodyLen);
        ret = GetHttpRequestSign(param.pSk, param.nSkLen, "POST", reportHost, "application/json", body,
                                     nBodyLen, pToken+nTokenOffset, &nOutputLen);
        if (ret != LINK_SUCCESS) {
                LinkLogError("getHttpRequestSign error:%d", ret);
                return ret;
        }
        assert(nReportHostLen - 1 >= nOutputLen+nTokenOffset+nUrlLen+1);
        
        char resp[256];
        memset(resp, 0, sizeof(resp));
        int respLen = sizeof(resp);

        //fprintf(stderr, "-------->body:%s:%d token:%s\n", body,nBodyLen, pToken);
        ret = LinkSimpleHttpPostWithToken(reportHost, resp, sizeof(resp), &respLen, body, nBodyLen, "application/json",
                                    pToken, nTokenOffset+nOutputLen);

        if (ret != LINK_SUCCESS) {
                LinkLogError("report session info fail:%d", ret);
                return ret;
        }
        
        cJSON * pJsonRoot = cJSON_Parse(resp);
        if (pJsonRoot == NULL) {
                return LINK_JSON_FORMAT;
        }
        
        int reportTTL = 0;
        cJSON *pNode = cJSON_GetObjectItem(pJsonRoot, "ttl");
        if (pNode == NULL) {
                cJSON_Delete(pJsonRoot);
                return LINK_JSON_FORMAT;
        } else {
                cJSON_Delete(pJsonRoot);
                reportTTL = pNode->valueint;
        }
        
        if (reportTTL > 0) {
                if (segmentMgr.nReportTTL <= 0 || segmentMgr.nReportTTL > reportTTL)
                        segmentMgr.nReportTTL = reportTTL;
                segmentMgr.handles[idx].nReportTTL = reportTTL;
                segmentMgr.handles[idx].nNextUpdateSegTimeInSecond = reportTTL + time(NULL);
        }
        
        
        return LINK_SUCCESS;
}

static void handleReportSegInfo(SegInfo *pSegInfo) {
        int i, idx = -1;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                if (segmentMgr.handles[i].handle == pSegInfo->handle) {
                        idx = i;
                        break;
                }
        }
        if (idx < 0) {
                LinkLogWarn("wrong segment handle:%d", pSegInfo->handle);
                return;
        }
        
        if (pSegInfo->pSegMeta){
                if (segmentMgr.handles[idx].pSegMeta){
                        free(segmentMgr.handles[idx].pSegMeta);
                }
                segmentMgr.handles[idx].pSegMeta = pSegInfo->pSegMeta;
        };
        
        if (!checkShouldReport(&segmentMgr.handles[idx], &pSegInfo->session)) {
                return;
        }
        if (pSegInfo->session.nAccSessionVideoDuration <= 0) {
                LinkLogInfo("not report due to session duration is 0");
                return;
        }

        int ret = reportSegInfo(&pSegInfo->session, idx, 0);
        if (ret == LINK_SUCCESS)
                segmentMgr.handles[idx].segReportOk = 1;
        else
                segmentMgr.handles[idx].segReportOk = 0;
        
        return;
}

static void handleOnSegReportSegInfo(int idx, int64_t nNow) {
        if (segmentMgr.handles[idx].tmpSession.nAccSessionDuration <= 0 ||
            (segmentMgr.handles[idx].tmpSession.nVideoGapFromLastReport <=0 &&
             segmentMgr.handles[idx].tmpSession.nAudioGapFromLastReport <= 0)) {
                    
                    LinkLogInfo("not report due to session duration is 0");
                    return;
            }
        if (segmentMgr.handles[idx].tmpSession.nSessionEndTime == 0) {
                segmentMgr.handles[idx].tmpSession.nSessionEndTime = segmentMgr.handles[idx].tmpSession.nSessionStartTime +
                segmentMgr.handles[idx].tmpSession.nAccSessionVideoDuration * 1000000;
        }
        int ret = reportSegInfo(&segmentMgr.handles[idx].tmpSession, idx, 1);
        if (ret == LINK_SUCCESS)
                segmentMgr.handles[idx].segReportOk = 1;
        else
                segmentMgr.handles[idx].segReportOk = 0;

        segmentMgr.handles[idx].nNextUpdateSegTimeInSecond = nNow + segmentMgr.handles[idx].nReportTTL;
        segmentMgr.handles[idx].tmpSession.nVideoGapFromLastReport = 0;
        segmentMgr.handles[idx].tmpSession.nAudioGapFromLastReport = 0;
        return;
}

static void handleTTLReportSegInfo(int force) {
        int idx = 0;
        int total = sizeof(segmentMgr.handles) / sizeof(Seg);
        int64_t nNow = time(NULL);
        for (idx = 0; idx < total; idx++) {
                if (segmentMgr.handles[idx].handle < 0) {
                        continue;
                }
                if (force) {
                        if (segmentMgr.handles[idx].tmpSession.nSessionEndResonCode == 0)
                                segmentMgr.handles[idx].tmpSession.nSessionEndResonCode = 4;
                } else if (nNow < segmentMgr.handles[idx].nNextUpdateSegTimeInSecond) {
                        continue;
                }
                
                handleOnSegReportSegInfo(idx, nNow);
        }
        
        return;
}

static void linkReleaseSegmentHandle(SegmentHandle seg) {
        pthread_mutex_lock(&segMgrMutex);
        if (seg >= 0 && seg < sizeof(segmentMgr.handles) / sizeof(SegmentHandle)) {
                segmentMgr.handles[seg].handle = -1;
                segmentMgr.handles[seg].nReportTTL = 0;
                segmentMgr.handles[seg].getUploadParamCallback = NULL;
                segmentMgr.handles[seg].pGetUploadParamCallbackArg = NULL;
                segmentMgr.handles[seg].nNextUpdateSegTimeInSecond = 0;
        }
        pthread_mutex_unlock(&segMgrMutex);
        return;
}

static void * segmetMgrRun(void *_pOpaque) {
        
        LinkUploaderStatInfo info = {0};
        while(!segmentMgr.nQuit_ || info.nLen_ != 0) {
                SegInfo segInfo = {0};
                segInfo.handle = -1;
                int64_t popWaitTime = (int64_t)24 * 60 * 60 * 1000000;
                if (segmentMgr.nReportTTL > 0)
                        popWaitTime = segmentMgr.nReportTTL * 1000000;
                int ret = segmentMgr.pSegQueue_->PopWithTimeout(segmentMgr.pSegQueue_, (char *)(&segInfo), sizeof(segInfo), popWaitTime);
                
                segmentMgr.pSegQueue_->GetStatInfo(segmentMgr.pSegQueue_, &info);
                LinkLogDebug("segment queue:%d cmd:%d end:%d", info.nLen_, segInfo.nOperation, segInfo.session.nSessionEndResonCode);
                if (ret <= 0) {
                        if (ret != LINK_TIMEOUT) {
                                LinkLogError("seg queue error. pop:%d", ret);
                        } else {
                                handleTTLReportSegInfo(0);
                        }
                        continue;
                }
                if (ret == sizeof(segInfo)) {
                        if (segInfo.nOperation == SEGMENT_QUIT) {
                                LinkLogInfo("segmentmgr recv quit");
                                handleTTLReportSegInfo(1);
                                continue;
                        }
                        LinkLogDebug("pop segment info:%d", segInfo.handle);
                        if (segInfo.handle < 0) {
                                LinkLogWarn("wrong segment handle:%d", segInfo.handle);
                        } else {
                                switch (segInfo.nOperation) {
                                                
                                        case SEGMENT_RELEASE:
                                                handleOnSegReportSegInfo(segInfo.handle, time(NULL));
                                                linkReleaseSegmentHandle(segInfo.handle);
                                                break;
                                        case SEGMENT_UPDATE:
                                                LinkLogDebug("segment %s %"PRId64"", segInfo.session.sessionId, segInfo.session.nTsSequenceNumber);
                                                handleReportSegInfo(&segInfo);
                                                break;
                                }
                        }
                }
        }
        
        return NULL;
}

int LinkInitSegmentMgr() {
        pthread_mutex_lock(&segMgrMutex);
        if (segMgrStarted) {
                pthread_mutex_unlock(&segMgrMutex);
                return LINK_SUCCESS;
        }
        int i = 0;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                segmentMgr.handles[i].handle = -1;
        }
        
        LinkCircleQueue *pQueue;
        int ret = LinkNewCircleQueue(&pQueue, 1, TSQ_FIX_LENGTH, sizeof(SegInfo), 32, NULL);
        if (ret != LINK_SUCCESS) {
                pthread_mutex_unlock(&segMgrMutex);
                return ret;
        }
        segmentMgr.pSegQueue_ = pQueue;
        
        ret = pthread_create(&segmentMgr.segMgrThread_, NULL, segmetMgrRun, NULL);
        if (ret != 0) {
                LinkDestroyQueue(&pQueue);
                pthread_mutex_unlock(&segMgrMutex);
                return LINK_THREAD_ERROR;
        }
        segMgrStarted = 1;
        pthread_mutex_unlock(&segMgrMutex);
        return LINK_SUCCESS;
}

int LinkNewSegmentHandle(SegmentHandle *pSeg, const SegmentArg *pArg) {
       pthread_mutex_lock(&segMgrMutex);
        if (!segMgrStarted) {
                pthread_mutex_unlock(&segMgrMutex);
                return LINK_NOT_INITED;
        }
        int i = 0;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                if (segmentMgr.handles[i].handle == -1) {
                        *pSeg = i;
                        segmentMgr.handles[i].handle  = i;
                        segmentMgr.handles[i].getUploadParamCallback = pArg->getUploadParamCallback;
                        segmentMgr.handles[i].pGetUploadParamCallbackArg = pArg->pGetUploadParamCallbackArg;
                        
                        segmentMgr.handles[i].pUploadStatisticCb = pArg->pUploadStatisticCb;
                        segmentMgr.handles[i].pUploadStatArg = pArg->pUploadStatArg;
                        segmentMgr.handles[i].nNextUpdateSegTimeInSecond = 0;
                        memset(&segmentMgr.handles[i].tmpSession, 0, sizeof(LinkSession));
                        if (segmentMgr.handles[i].pSegMeta) {
                                free(segmentMgr.handles[i].pSegMeta);
                                segmentMgr.handles[i].pSegMeta = NULL;
                        }
                        pthread_mutex_unlock(&segMgrMutex);
                        return LINK_SUCCESS;
                }
        }
        pthread_mutex_unlock(&segMgrMutex);
        return LINK_MAX_SEG;
}

void LinkReleaseSegmentHandle(SegmentHandle *pSeg) {
        if (*pSeg < 0 || !segMgrStarted) {
                return;
        }
        SegInfo segInfo;
        segInfo.handle = *pSeg;
        segInfo.nOperation = SEGMENT_RELEASE;
        *pSeg = -1;
        
        int ret = 0;
        while(ret <= 0) {
                ret = segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
                if (ret <= 0) {
                        LinkLogError("seg queue error. release:%d sleep 1 sec to retry", ret);
                        sleep(1);
                }
        }
}

int LinkUpdateSegment(SegmentHandle seg, const LinkSession *pSession, LinkSessionMeta *smeta) {
        SegInfo segInfo = {0};
        segInfo.handle = seg;
        segInfo.session = *pSession;
        segInfo.nOperation = SEGMENT_UPDATE;
        

        char *pSegMeta = NULL;
        if (smeta) {
                int i = 0;
                pSegMeta = malloc(128);
                if (pSegMeta) {
                        int nSessionMetaLen = sprintf(pSegMeta, "\"meta\":{");
                        for (i = 0; i < smeta->len; i++) {
                                nSessionMetaLen += snprintf(pSegMeta+nSessionMetaLen, 127, "\"%s\":\"%s\",", smeta->keys[i], smeta->values[i]);
                        }
                        pSegMeta[nSessionMetaLen-1] = '}';
                        pSegMeta[nSessionMetaLen] = 0;
                } else {
                      LinkLogError("malloc meta data fail");
                }
        }
        
        segInfo.pSegMeta = pSegMeta;
        int ret = segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
        if (ret <= 0) {
                LinkLogError("seg queue error. push update:%d", ret);
                return ret;
        }
        return LINK_SUCCESS;
}

void LinkUninitSegmentMgr() {
        pthread_mutex_lock(&segMgrMutex);
        if (!segMgrStarted) {
                pthread_mutex_unlock(&segMgrMutex);
                return;
        }
        segmentMgr.nQuit_ = 1;
        pthread_mutex_unlock(&segMgrMutex);
        
        SegInfo segInfo;
        segInfo.nOperation = SEGMENT_QUIT;
        
        LinkLogInfo("segmentmgr require to quit");
        int ret = 0;
        while(ret <= 0) {
                ret = segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
                if (ret <= 0) {
                        LinkLogError("seg queue error. notify quit:%d sleep 1 sec to retry", ret);
                        sleep(1);
                }
        }
        
        pthread_join(segmentMgr.segMgrThread_, NULL);
        LinkDestroyQueue(&segmentMgr.pSegQueue_);
        memset(&segmentMgr, 0, sizeof(segmentMgr));
        segMgrStarted = 0;
        return;
}
