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

#include "os.h"

typedef struct _str_node_t {
  uint64_t             key;
  struct _str_node_t *prev;
  struct _str_node_t *next;
  char                data[];
} IHashNode;

typedef struct {
  IHashNode **hashList;
  int32_t     maxSessions;
  int32_t     dataSize;
  int32_t   (*hashFp)(void *, uint64_t key);
  pthread_mutex_t mutex;
} IHashObj;

int32_t taosHashInt(void *handle, uint64_t key) {
  IHashObj *pObj = (IHashObj *)handle;
  int32_t   hash = key % pObj->maxSessions;
  return hash;
}

char *taosAddIntHash(void *handle, uint64_t key, char *pData) {
  int32_t    hash;
  IHashNode *pNode;
  IHashObj * pObj;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions == 0) return NULL;

  hash = (*pObj->hashFp)(pObj, key);

  pNode = (IHashNode *)malloc(sizeof(IHashNode) + (size_t)pObj->dataSize);
  if (pNode == NULL)
    return NULL;
  
  pthread_mutex_lock(&pObj->mutex);

  pNode->key = key;
  if (pData != NULL) {
    memcpy(pNode->data, pData, (size_t)pObj->dataSize);
  }
  pNode->prev = 0;
  pNode->next = pObj->hashList[hash];

  if (pObj->hashList[hash] != 0) (pObj->hashList[hash])->prev = pNode;
  pObj->hashList[hash] = pNode;

  pthread_mutex_unlock(&pObj->mutex);

  return (char *)pNode->data;
}

void taosDeleteIntHash(void *handle, uint64_t key) {
  int32_t    hash;
  IHashNode *pNode;
  IHashObj * pObj;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions == 0) return;

  hash = (*(pObj->hashFp))(pObj, key);

  pthread_mutex_lock(&pObj->mutex);

  pNode = pObj->hashList[hash];
  while (pNode) {
    if (pNode->key == key) break;

    pNode = pNode->next;
  }

  if (pNode) {
    if (pNode->prev) {
      pNode->prev->next = pNode->next;
    } else {
      pObj->hashList[hash] = pNode->next;
    }

    if (pNode->next) {
      pNode->next->prev = pNode->prev;
    }

    free(pNode);
  }

  pthread_mutex_unlock(&pObj->mutex);
}

char *taosGetIntHashData(void *handle, uint64_t key) {
  int32_t    hash;
  IHashNode *pNode;
  IHashObj * pObj;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions == 0) return NULL;

  hash = (*pObj->hashFp)(pObj, key);

  pthread_mutex_lock(&pObj->mutex);

  pNode = pObj->hashList[hash];

  while (pNode) {
    if (pNode->key == key) {
      break;
    }

    pNode = pNode->next;
  }

  pthread_mutex_unlock(&pObj->mutex);

  if (pNode) return pNode->data;

  return NULL;
}

void *taosInitIntHash(int32_t maxSessions, int32_t dataSize, int32_t (*fp)(void *, uint64_t)) {
  IHashObj *pObj;

  pObj = (IHashObj *)malloc(sizeof(IHashObj));
  if (pObj == NULL) {
    return NULL;
  }

  memset(pObj, 0, sizeof(IHashObj));
  pObj->maxSessions = maxSessions;
  pObj->dataSize = dataSize;
  pObj->hashFp = fp;

  pObj->hashList = (IHashNode **)malloc(sizeof(IHashNode *) * (size_t)maxSessions);
  if (pObj->hashList == NULL) {
    free(pObj);
    return NULL;
  }
  memset(pObj->hashList, 0, sizeof(IHashNode *) * (size_t)maxSessions);

  pthread_mutex_init(&pObj->mutex, NULL);

  return pObj;
}

void taosCleanUpIntHash(void *handle) {
  IHashObj * pObj;
  IHashNode *pNode, *pNext;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions <= 0) return;

  pthread_mutex_lock(&pObj->mutex);

  if (pObj->hashList) {
    for (int32_t i = 0; i < pObj->maxSessions; ++i) {
      pNode = pObj->hashList[i];
      while (pNode) {
        pNext = pNode->next;
        free(pNode);
        pNode = pNext;
      }
    }

    free(pObj->hashList);
  }

  pthread_mutex_unlock(&pObj->mutex);

  pthread_mutex_destroy(&pObj->mutex);

  memset(pObj, 0, sizeof(IHashObj));
  free(pObj);
}

void taosCleanUpIntHashWithFp(void *handle, void (*fp)(char *)) {
  IHashObj * pObj;
  IHashNode *pNode, *pNext;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions <= 0) return;

  pthread_mutex_lock(&pObj->mutex);

  if (pObj->hashList) {
    for (int i = 0; i < pObj->maxSessions; ++i) {
      pNode = pObj->hashList[i];
      while (pNode) {
        pNext = pNode->next;
        if (fp != NULL) (*fp)(pNode->data);
        free(pNode);
        pNode = pNext;
      }
    }

    free(pObj->hashList);
  }

  pthread_mutex_unlock(&pObj->mutex);

  pthread_mutex_destroy(&pObj->mutex);

  memset(pObj, 0, sizeof(IHashObj));
  free(pObj);
}

void taosVisitIntHashWithFp(void *handle, int (*fp)(char *, void *), void *param) {
  IHashObj * pObj;
  IHashNode *pNode, *pNext;
  char *     pData = NULL;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions <= 0) return NULL;

  pthread_mutex_lock(&pObj->mutex);

  if (pObj->hashList) {
    for (int i = 0; i < pObj->maxSessions; ++i) {
      pNode = pObj->hashList[i];
      while (pNode) {
        pNext = pNode->next;
        (*fp)(pNode->data, param);
        pNode = pNext;
      }
    }
  }

  pthread_mutex_unlock(&pObj->mutex);
}

int32_t taosGetIntHashSize(void *handle) {
  IHashObj * pObj;
  IHashNode *pNode, *pNext;
  char *     pData = NULL;
  int32_t    num = 0;

  pObj = (IHashObj *)handle;
  if (pObj == NULL || pObj->maxSessions <= 0) return NULL;

  pthread_mutex_lock(&pObj->mutex);

  if (pObj->hashList) {
    for (int i = 0; i < pObj->maxSessions; ++i) {
      pNode = pObj->hashList[i];
      while (pNode) {
        pNext = pNode->next;
        num++;
        pNode = pNext;
      }
    }
  }

  pthread_mutex_unlock(&pObj->mutex);
  return num;
}