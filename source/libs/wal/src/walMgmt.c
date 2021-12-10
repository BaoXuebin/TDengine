/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "tref.h"
#include "tfile.h"
#include "compare.h"
#include "walInt.h"

//internal
int32_t walGetNextFile(SWal *pWal, int64_t *nextFileId);
int32_t walGetOldFile(SWal *pWal, int64_t curFileId, int32_t minDiff, int64_t *oldFileId);
int32_t walGetNewFile(SWal *pWal, int64_t *newFileId);

typedef struct {
  int32_t   refSetId;
  uint32_t  seq;
  int8_t    stop;
  int8_t    inited;
  pthread_t thread;
} SWalMgmt;

static SWalMgmt tsWal = {0, .seq = 1};
static int32_t  walCreateThread();
static void     walStopThread();
static int32_t  walInitObj(SWal *pWal);
static void     walFreeObj(void *pWal);

int64_t walGetSeq() {
  return (int64_t)atomic_load_32(&tsWal.seq);
}

int32_t walInit() {
  int8_t old = atomic_val_compare_exchange_8(&tsWal.inited, 0, 1);
  if(old == 1) return 0;

  int code = tfInit();
  if(code != 0) {
    wError("failed to init tfile since %s", tstrerror(code));
    atomic_store_8(&tsWal.inited, 0);
    return code;
  }
  tsWal.refSetId = taosOpenRef(TSDB_MIN_VNODES, walFreeObj);

  code = walCreateThread();
  if (code != 0) {
    wError("failed to init wal module since %s", tstrerror(code));
    atomic_store_8(&tsWal.inited, 0);
    return code;
  }

  wInfo("wal module is initialized, rsetId:%d", tsWal.refSetId);
  return 0;
}

void walCleanUp() {
  walStopThread();
  taosCloseRef(tsWal.refSetId);
  atomic_store_8(&tsWal.inited, 0);
  wInfo("wal module is cleaned up");
}

SWal *walOpen(const char *path, SWalCfg *pCfg) {
  SWal *pWal = malloc(sizeof(SWal));
  if (pWal == NULL) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return NULL;
  }
  pWal->writeLogTfd = -1;
  pWal->writeIdxTfd = -1;

  //set config
  pWal->vgId = pCfg->vgId;
  pWal->fsyncPeriod = pCfg->fsyncPeriod;
  pWal->rollPeriod = pCfg->rollPeriod;
  pWal->segSize = pCfg->segSize;
  pWal->level = pCfg->walLevel;

  //init status
  pWal->lastVersion = -1;
  pWal->lastRollSeq = -1;

  //init write buffer
  memset(&pWal->head, 0, sizeof(SWalHead));
  pWal->head.sver = 0;

  tstrncpy(pWal->path, path, sizeof(pWal->path));
  pthread_mutex_init(&pWal->mutex, NULL);

  pWal->fsyncSeq = pCfg->fsyncPeriod / 1000;
  if (pWal->fsyncSeq <= 0) pWal->fsyncSeq = 1;

  if (walInitObj(pWal) != 0) {
    walFreeObj(pWal);
    return NULL;
  }

   pWal->refId = taosAddRef(tsWal.refSetId, pWal);
   if (pWal->refId < 0) {
    walFreeObj(pWal);
    return NULL;
  }

  wDebug("vgId:%d, wal:%p is opened, level:%d fsyncPeriod:%d", pWal->vgId, pWal, pWal->level, pWal->fsyncPeriod);

  return pWal;
}

int32_t walAlter(SWal *pWal, SWalCfg *pCfg) {
  if (pWal == NULL) return TSDB_CODE_WAL_APP_ERROR;

  if (pWal->level == pCfg->walLevel && pWal->fsyncPeriod == pCfg->fsyncPeriod) {
    wDebug("vgId:%d, old walLevel:%d fsync:%d, new walLevel:%d fsync:%d not change", pWal->vgId, pWal->level,
           pWal->fsyncPeriod, pCfg->walLevel, pCfg->fsyncPeriod);
    return 0;
  }

  wInfo("vgId:%d, change old walLevel:%d fsync:%d, new walLevel:%d fsync:%d", pWal->vgId, pWal->level,
        pWal->fsyncPeriod, pCfg->walLevel, pCfg->fsyncPeriod);

  pWal->level = pCfg->walLevel;
  pWal->fsyncPeriod = pCfg->fsyncPeriod;
  pWal->fsyncSeq = pCfg->fsyncPeriod / 1000;
  if (pWal->fsyncSeq <= 0) pWal->fsyncSeq = 1;

  return 0;
}

void walClose(SWal *pWal) {
  if (pWal == NULL) return;

  pthread_mutex_lock(&pWal->mutex);
  tfClose(pWal->writeLogTfd);
  tfClose(pWal->writeIdxTfd);
  /*taosArrayDestroy(pWal->fileInfoSet);*/
  /*pWal->fileInfoSet = NULL;*/
  pthread_mutex_unlock(&pWal->mutex);
  taosRemoveRef(tsWal.refSetId, pWal->refId);
}

static int32_t walInitObj(SWal *pWal) {
  if (taosMkDir(pWal->path) != 0) {
    wError("vgId:%d, path:%s, failed to create directory since %s", pWal->vgId, pWal->path, strerror(errno));
    return TAOS_SYSTEM_ERROR(errno);
  }
  pWal->fileInfoSet = taosArrayInit(0, sizeof(WalFileInfo));
  if(pWal->fileInfoSet == NULL) {
    wError("vgId:%d, path:%s, failed to init taosArray %s", pWal->vgId, pWal->path, strerror(errno));
    return TAOS_SYSTEM_ERROR(errno);
  }

  wDebug("vgId:%d, object is initialized", pWal->vgId);
  return 0;
}

static void walFreeObj(void *wal) {
  SWal *pWal = wal;
  wDebug("vgId:%d, wal:%p is freed", pWal->vgId, pWal);

  tfClose(pWal->writeLogTfd);
  tfClose(pWal->writeIdxTfd);
  taosArrayDestroy(pWal->fileInfoSet);
  pWal->fileInfoSet = NULL;
  taosArrayDestroy(pWal->fileInfoSet);
  pWal->fileInfoSet = NULL;
  pthread_mutex_destroy(&pWal->mutex);
  tfree(pWal);
}

static bool walNeedFsync(SWal *pWal) {
  if (pWal->fsyncPeriod <= 0 || pWal->level != TAOS_WAL_FSYNC) {
    return false;
  }

  if (atomic_load_32(&tsWal.seq) % pWal->fsyncSeq == 0) {
    return true;
  }

  return false;
}

static void walUpdateSeq() {
  taosMsleep(WAL_REFRESH_MS);
  atomic_add_fetch_32(&tsWal.seq, 1);
}

static void walFsyncAll() {
  SWal *pWal = taosIterateRef(tsWal.refSetId, 0);
  while (pWal) {
    if (walNeedFsync(pWal)) {
      wTrace("vgId:%d, do fsync, level:%d seq:%d rseq:%d", pWal->vgId, pWal->level, pWal->fsyncSeq, atomic_load_32(&tsWal.seq));
      int32_t code = tfFsync(pWal->writeLogTfd);
      if (code != 0) {
        wError("vgId:%d, file:%"PRId64".log, failed to fsync since %s", pWal->vgId, walGetLastFileFirstVer(pWal), strerror(code));
      }
    }
    pWal = taosIterateRef(tsWal.refSetId, pWal->refId);
  }
}

static void *walThreadFunc(void *param) {
  setThreadName("wal");
  while (1) {
    walUpdateSeq();
    walFsyncAll();

    if (atomic_load_8(&tsWal.stop)) break;
  }

  return NULL;
}

static int32_t walCreateThread() {
  pthread_attr_t thAttr;
  pthread_attr_init(&thAttr);
  pthread_attr_setdetachstate(&thAttr, PTHREAD_CREATE_JOINABLE);

  if (pthread_create(&tsWal.thread, &thAttr, walThreadFunc, NULL) != 0) {
    wError("failed to create wal thread since %s", strerror(errno));
    return TAOS_SYSTEM_ERROR(errno);
  }

  pthread_attr_destroy(&thAttr);
  wDebug("wal thread is launched, thread:0x%08" PRIx64, taosGetPthreadId(tsWal.thread));

  return 0;
}

static void walStopThread() {
  atomic_store_8(&tsWal.stop, 1);

  if (taosCheckPthreadValid(tsWal.thread)) {
    pthread_join(tsWal.thread, NULL);
  }

  wDebug("wal thread is stopped");
}
