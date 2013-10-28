#include "redis.h"
#include "joblist.h"

#include <assert.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#define FILE_MODE (S_IRUSR | S_IWUSR)

static int _resizeJobListStart(JobList* this);
static int _initJobBuff(JobList* this, int listsize, const char* mmapFile);
static int _resizeJobListEnd(JobList* this);
static void _incJoblistWsize(JobList* this, int inc);
static int _jobListWidx(JobList* this);
static int _jobListRidx(JobList* this);
static int _backJobListWsize(JobList* this, int widx);
static int _backJobListRsize(JobList* this, int ridx);
static int _isJobListEmpty(JobList* this);
static int _isJobListFull(JobList* this, int pushsize);

int popJobList(JobList* this, char** rbuf)
{
    assert(this->jobbuff->rSize <= this->jobbuff->wSize);
    int ridx = this->resize ? _resizeJobListEnd(this) : _jobListRidx(this);
    int len = 0;
    assert(this->jobbuff->rSize <= this->jobbuff->wSize);
    if (_isJobListEmpty(this)) { //empty
        return JOBLIST_RET_POP_EMPTY;
    }
    char* start = this->jobbuff->buf + ridx;
    redisLog(REDIS_DEBUG, "read joblist rSize %d rbuf %p", this->jobbuff->rSize, start);
    memcpy(&len, start, JOBLEN_SIZE);
    redisLog(REDIS_DEBUG, "recv len %d", len);
    assert(len > 0 && len <= this->maxBufSize);
    *rbuf = start + JOBLEN_SIZE;
    return len;
}

int pushJobList(JobList* this, const char* wbuf, int len)
{
    assert(this->jobbuff->rSize <= this->jobbuff->wSize);
    if (len >= this->maxBufSize) {
        return JOBLIST_RET_SIZE_OVERFLOW;
    }
    int widx = _isJobListFull(this, len) ? _resizeJobListStart(this) : _jobListWidx(this);
    char* start = this->jobbuff->buf + widx;

    memcpy(start, &len, JOBLEN_SIZE);
    memcpy(start + JOBLEN_SIZE, wbuf, len);

    redisLog(REDIS_DEBUG, "write joblist  wSize %d  wbuf %p len %d", this->jobbuff->wSize, start, len);
    
    _incJoblistWsize(this, len);
    
    return JOBLIST_RET_SUCCESS;
}

static int _resizeJobListStart(JobList* this)
{
    assert(this->resize == 0);
    this->resize = 1;
    redisLog(REDIS_WARNING, "joblist resize start %d, %d", this->jobbuff->wSize, this->jobbuff->rSize);
    while (this->resize) {
        usleep(1000);
    }
    return _jobListWidx(this);
}

static int _resizeJobListEnd(JobList* this)
{
    redisLog(REDIS_WARNING, "joblist resize end %d, %d, %d", this->jobbuff->wSize, this->jobbuff->rSize, this->listsize);
    int backuplen = this->jobbuff->wSize - this->jobbuff->rSize - this->jobbuff->dirtysize;
    assert(backuplen > 0 && backuplen <= this->listsize);
    int widx = _jobListWidx(this);
    int ridx = _jobListRidx(this);
    if (widx > ridx) {
        redisLog(REDIS_WARNING, "joblist resize failed %d, %d, %d", this->jobbuff->wSize, this->jobbuff->rSize, this->listsize);
        this->resize = 0;
        return _jobListRidx(this);
    }
    int oldsize = this->listsize;
    char* backup = zmalloc(backuplen);
    char* start = backup;

    int behindUnreadSize = oldsize - ridx - this->jobbuff->dirtysize;
    int frontUnreadSize = widx; 
    memcpy(start, this->jobbuff->buf + ridx, behindUnreadSize);
    start += behindUnreadSize;
    memcpy(start, this->jobbuff->buf, frontUnreadSize);

    _initJobBuff(this, oldsize * 2, this->mmapFile);

    this->jobbuff->wSize = frontUnreadSize + behindUnreadSize;
    this->jobbuff->rSize = 0;
    this->jobbuff->dirtysize = 0;
    memcpy(this->jobbuff->buf, backup, backuplen);
    
    zfree(backup);
    redisLog(REDIS_WARNING, "joblist resize ridx complete ok, %d, %d", this->jobbuff->wSize, this->jobbuff->rSize);
    
    this->resize = 0;
    return _jobListRidx(this);
}

static void _incJoblistWsize(JobList* this, int inc)
{
    this->jobbuff->wSize += (inc + JOBLEN_SIZE);
}

void incJoblistRsize(JobList* this, int inc)
{
    this->jobbuff->rSize += (inc + JOBLEN_SIZE);
}

static int _initJobBuff(JobList* this, int listsize, const char* mmapFile)
{
    int flags = O_RDWR | O_CREAT; 
    int exist = access(mmapFile, F_OK) == 0; 
    int filesize = listsize + sizeof(JobList);
    int mmapFd = open(mmapFile, flags, FILE_MODE);
    assert(mmapFd > 0);
    if (exist) {
        struct stat s;
        stat(mmapFile, &s);
        filesize = s.st_size > filesize ? s.st_size : filesize;
    }
    ftruncate(mmapFd, filesize);
    this->jobbuff = (JobBuff*)mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, mmapFd, 0);
    if (!exist) {
        this->jobbuff->rSize = this->jobbuff->wSize = 0;
        this->jobbuff->dirtysize = 0;
    }
    this->listsize = filesize - sizeof(JobList);
    this->listsizeMask = this->listsize - 1;
    close(mmapFd);
    return JOBLIST_RET_SUCCESS;
}

JobList* initJoblist(int maxBufSize, int listsize, const char* mmapFile)
{
    NEXT_POT(listsize); 
    JobList* this = zmalloc(sizeof(JobList));
    _initJobBuff(this, listsize, mmapFile);
    this->maxBufSize = maxBufSize;
    this->resize = 0;
    this->mmapFile = mmapFile;
    redisLog(REDIS_WARNING, "joblist wSize:%d\t rSize:%d\t listsize:%d resize:%d dirtysize:%d", this->jobbuff->wSize, this->jobbuff->rSize, this->listsize, this->resize, this->jobbuff->dirtysize);
    return this;
}

static int _jobListWidx(JobList* this)
{
    int widx = this->jobbuff->wSize & this->listsizeMask;
    return _backJobListWsize(this, widx) ? this->jobbuff->wSize & this->listsizeMask : widx;
}

static int _jobListRidx(JobList* this)
{
    int ridx = this->jobbuff->rSize & this->listsizeMask; 
    return _backJobListRsize(this, ridx) ? this->jobbuff->rSize & this->listsizeMask : ridx;
}

static int _backJobListWsize(JobList* this, int widx)
{
    if (widx + this->maxBufSize > this->listsize) {
        this->jobbuff->dirtysize = this->listsize - widx;
        this->jobbuff->wSize += this->listsize - widx; 
        return 1;
    }
    return 0;
}

static int _backJobListRsize(JobList* this, int ridx)
{
    if (ridx + this->maxBufSize > this->listsize) {
        while (_isJobListEmpty(this)) {
            usleep(1000);
        }
        assert(this->jobbuff->dirtysize == this->listsize - ridx);
        this->jobbuff->rSize += this->listsize - ridx; 
        return 1;
    }
    return 0;
}

static int _isJobListEmpty(JobList* this)
{
    return this->jobbuff->rSize == this->jobbuff->wSize;
}

static int _isJobListFull(JobList* this, int pushsize)
{
    return this->jobbuff->wSize + pushsize + JOBLEN_SIZE - this->jobbuff->rSize > this->listsize;
}
