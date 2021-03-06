#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <sys/statfs.h>


#define DBG_SUBSYS S_YFSLIB

#include "configure.h"
#include "schedule.h"
#include "io_analysis.h"
#include "dbg.h"

#define ANALY_AVG_UPDATE_COUNT (3)  //secend
typedef struct {
        char name[MAX_NAME_LEN];
        int seq;
        uint64_t read_count;
        uint64_t write_count;
        uint64_t read_bytes;
        uint64_t write_bytes;
        time_t last_output;
        sy_spinlock_t lock;

        /* for each seconds */
        time_t last;
        int cur;
        uint64_t readps[ANALY_AVG_UPDATE_COUNT];
        uint64_t writeps[ANALY_AVG_UPDATE_COUNT];
        uint64_t readbwps[ANALY_AVG_UPDATE_COUNT];
        uint64_t writebwps[ANALY_AVG_UPDATE_COUNT];
} io_analysis_t;

static io_analysis_t *__io_analysis__ = NULL;

static void __io_analysis_get(uint32_t *readps, uint32_t *writeps,
                uint32_t *readbwps, uint32_t *writebwps)
{
        int prev = (__io_analysis__->cur + ANALY_AVG_UPDATE_COUNT - 1) % ANALY_AVG_UPDATE_COUNT;

        *readps = __io_analysis__->readps[prev];
        *writeps = __io_analysis__->writeps[prev];
        *readbwps = __io_analysis__->readbwps[prev];
        *writebwps = __io_analysis__->writebwps[prev];
}

static int __io_analysis_dump()
{
        int ret;
        time_t now;
        char path[MAX_PATH_LEN], buf[MAX_INFO_LEN];
        uint32_t readps, writeps, readbwps, writebwps;

        now = time(NULL);
        memset(buf, 0x0, sizeof(buf));

        __io_analysis_get(&readps, &writeps, &readbwps, &writebwps);

        if (now - __io_analysis__->last_output > 2) {
                snprintf(buf, MAX_INFO_LEN, "read: %llu\n"
                                "read_bytes: %llu\n"
                                "write: %llu\n"
                                "write_bytes: %llu\n"
                                "read_ps:%u\n"
                                "read_bytes_ps:%u\n"
                                "write_ps:%u\n"
                                "write_bytes_ps:%u\n"
                                "time:%u\n",
                                (LLU)__io_analysis__->read_count,
                                (LLU)__io_analysis__->read_bytes,
                                (LLU)__io_analysis__->write_count,
                                (LLU)__io_analysis__->write_bytes,
                                readps,
                                readbwps,
                                writeps,
                                writebwps,
                                (int)now);
                __io_analysis__->last_output = now;
        }

        if (now - __io_analysis__->last_output < 0) {
                __io_analysis__->last_output = now;
        }

        if (strlen(buf)) {
                snprintf(path, MAX_PATH_LEN, "%s/io/%s.%d", SHM_ROOT,
                         __io_analysis__->name, __io_analysis__->seq);
                ret = _set_value(path, buf, strlen(buf) + 1, O_CREAT | O_TRUNC);
                if (ret)
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int io_analysis(analysis_op_t op, int count)
{
        int ret, next, cnt, i;
        time_t now;

        if (__io_analysis__ == NULL) {
                goto out;
        }
        
        now = time(NULL);

        ret = sy_spin_lock(&__io_analysis__->lock);
        if (ret)
                GOTO(err_ret, ret);

        if (op == ANALYSIS_READ) {
                __io_analysis__->read_count++;
                __io_analysis__->read_bytes += count;
        } else if (op == ANALYSIS_WRITE) {
                __io_analysis__->write_count++;
                __io_analysis__->write_bytes += count;
        } else {
                YASSERT(0);
        }

        if (__io_analysis__->last == 0) {
                __io_analysis__->cur = 0;
                if (op == ANALYSIS_READ) {
                        __io_analysis__->readps[0] = 1;
                        __io_analysis__->readbwps[0] = count;
                } else {
                        __io_analysis__->writeps[0] = 1;
                        __io_analysis__->writebwps[0] = count;
                }
                __io_analysis__->last = now;
        } else if (now > __io_analysis__->last) {
                cnt = now - __io_analysis__->last;
                for (i = 1; i <= cnt; i++) {
                        next = (__io_analysis__->cur + i) % ANALY_AVG_UPDATE_COUNT;
                        __io_analysis__->readps[next] = 0;
                        __io_analysis__->readbwps[next] = 0;
                        __io_analysis__->writeps[next] = 0;
                        __io_analysis__->writebwps[next] = 0;
                }

                YASSERT(next >= 0 && next < ANALY_AVG_UPDATE_COUNT);
                __io_analysis__->cur = next;
                if (op == ANALYSIS_READ) {
                        __io_analysis__->readps[next] = 1;
                        __io_analysis__->readbwps[next] = count;
                } else {
                        __io_analysis__->writeps[next] = 1;
                        __io_analysis__->writebwps[next] = count;
                }
                __io_analysis__->last = now;
        } else {
                if (op == ANALYSIS_READ) {
                        __io_analysis__->readps[__io_analysis__->cur] += 1;
                        __io_analysis__->readbwps[__io_analysis__->cur] += count;
                } else {
                        __io_analysis__->writeps[__io_analysis__->cur] += 1;
                        __io_analysis__->writebwps[__io_analysis__->cur] += count;
                }
        }

        ret = __io_analysis_dump();
        if (ret)
                GOTO(err_lock, ret);

        sy_spin_unlock(&__io_analysis__->lock);

out:
        return 0;
err_lock:
        sy_spin_unlock(&__io_analysis__->lock);
err_ret:
        return ret;
}

int io_analysis_init(const char *name, int seq)
{
        int ret;

        ret = ymalloc((void **)&__io_analysis__, sizeof(*__io_analysis__));
        if (ret)
                GOTO(err_ret, ret);
        
        ret = sy_spin_init(&__io_analysis__->lock);
        if (ret)
                GOTO(err_ret, ret);

        if (seq == -1) {
                __io_analysis__->seq = getpid();
        } else {
                __io_analysis__->seq = seq;
        }
        
        strcpy(__io_analysis__->name, name);

        return 0;
err_ret:
        return ret;
}
