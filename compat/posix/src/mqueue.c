/*
 * Copyright (c) 2013-2019, Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2020, Huawei Device Co., Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list
 *    of conditions and the following disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mqueue.h"
#include "fcntl.h"
#include "pthread.h"
#include "map_error.h"
#include "time_posix.h"
#include "los_memory.h"
#include "los_vm_map.h"
#include "user_copy.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */
/****************************************************
本文说明:鸿蒙对POSIX消息队列各接口的实现

****************************************************/
#define FNONBLOCK   O_NONBLOCK //非阻塞I/O使操作要么成功，要么立即返回错误，不被阻塞

/* GLOBALS */
STATIC struct mqarray g_queueTable[LOSCFG_BASE_IPC_QUEUE_LIMIT];//POSIX 消息队列池.
STATIC pthread_mutex_t g_mqueueMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;//嵌套锁

/* LOCAL FUNCTIONS */
STATIC INLINE INT32 MqNameCheck(const CHAR *mqName)
{
    if (mqName == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strlen(mqName) == 0) {
        errno = EINVAL;
        return -1;
    }

    if (strlen(mqName) > (PATH_MAX - 1)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
//通过ID获取 los队列控制块
STATIC INLINE UINT32 GetMqueueCBByID(UINT32 queueID, LosQueueCB **queueCB)
{
    LosQueueCB *tmpQueueCB = NULL;
    if (queueCB == NULL) {
        errno = EINVAL;
        return LOS_ERRNO_QUEUE_READ_PTR_NULL;
    }
    tmpQueueCB = GET_QUEUE_HANDLE(queueID);//获取队列ID对应的队列控制块
    if ((GET_QUEUE_INDEX(queueID) >= LOSCFG_BASE_IPC_QUEUE_LIMIT) || (tmpQueueCB->queueID != queueID)) {
        return LOS_ERRNO_QUEUE_INVALID;
    }
    *queueCB = tmpQueueCB;//指向的还是 g_allQueue[queueID]

    return LOS_OK;
}
//通过名称获取队列控制块
STATIC INLINE struct mqarray *GetMqueueCBByName(const CHAR *name)
{
    UINT32 index;
    UINT32 mylen = strlen(name);

    for (index = 0; index < LOSCFG_BASE_IPC_QUEUE_LIMIT; index++) {
        if ((g_queueTable[index].mq_name == NULL) || (strlen(g_queueTable[index].mq_name) != mylen)) {
            continue;
        }

        if (strncmp(name, (const CHAR *)(g_queueTable[index].mq_name), mylen) == 0) {
            return &(g_queueTable[index]);
        }
    }

    return NULL;
}
//执行删除队列控制块操作
STATIC INT32 DoMqueueDelete(struct mqarray *mqueueCB)
{
    UINT32 ret;

    if (mqueueCB->mq_name != NULL) {//队列名称占用内核内存
        LOS_MemFree(OS_SYS_MEM_ADDR, mqueueCB->mq_name);//释放内存
        mqueueCB->mq_name = NULL;
    }

    mqueueCB->mqcb = NULL;//这里只是先置空，但实际内存并没有删除

    ret = LOS_QueueDelete(mqueueCB->mq_id);//通过ID删除任务控制块
    switch (ret) {
        case LOS_OK:
            return 0;
        case LOS_ERRNO_QUEUE_NOT_FOUND:
        case LOS_ERRNO_QUEUE_NOT_CREATE:
        case LOS_ERRNO_QUEUE_IN_TSKUSE:
        case LOS_ERRNO_QUEUE_IN_TSKWRITE:
            errno = EAGAIN;
            return -1;
        default:
            errno = EINVAL;
            return -1;
    }
}
//保存队列名称，申请内核内存
STATIC int SaveMqueueName(const CHAR *mqName, struct mqarray *mqueueCB)
{
    size_t nameLen;

    nameLen = strlen(mqName); /* sys_mq_open has checked name and name length */
    mqueueCB->mq_name = (char *)LOS_MemAlloc(OS_SYS_MEM_ADDR, nameLen + 1);
    if (mqueueCB->mq_name == NULL) {
        errno = ENOMEM;
        return LOS_NOK;
    }

    if (strncpy_s(mqueueCB->mq_name, (nameLen + 1), mqName, nameLen) != EOK) { //从用户空间拷贝到内核空间
        LOS_MemFree(OS_SYS_MEM_ADDR, mqueueCB->mq_name);
        mqueueCB->mq_name = NULL;
        errno = EINVAL;
        return LOS_NOK;
    }
    mqueueCB->mq_name[nameLen] = '\0';//结尾字符
    return LOS_OK;
}
//创建一个posix消息队列,并新增消息队列的引用
STATIC struct mqpersonal *DoMqueueCreate(const struct mq_attr *attr, const CHAR *mqName, INT32 openFlag)
{
    struct mqarray *mqueueCB = NULL;
    UINT32 mqueueID;

    UINT32 err = LOS_QueueCreate(NULL, attr->mq_maxmsg, &mqueueID, 0, attr->mq_msgsize);//先创建LOS消息队列，带出队列ID
    if (map_errno(err) != ENOERR) {
        goto ERROUT;
    }

    if (g_queueTable[GET_QUEUE_INDEX(mqueueID)].mqcb == NULL) {//posix 和 los 进行捆绑
        mqueueCB = &(g_queueTable[GET_QUEUE_INDEX(mqueueID)]);//分配一个posix消息队列,可以看出posix队列同步los队列创建
        mqueueCB->mq_id = mqueueID;// 记录ID 
    }

    if (mqueueCB == NULL) {
        errno = EINVAL;
        goto ERROUT;
    }

    if (SaveMqueueName(mqName, mqueueCB) != LOS_OK) {//在内核堆区 保存队列名称
        goto ERROUT;
    }

    if (GetMqueueCBByID(mqueueCB->mq_id, &(mqueueCB->mqcb)) != LOS_OK) {
        errno = ENOSPC;
        goto ERROUT;
    }

    mqueueCB->mq_personal = (struct mqpersonal *)LOS_MemAlloc(OS_SYS_MEM_ADDR, sizeof(struct mqpersonal));//新增消息队列的引用
    if (mqueueCB->mq_personal == NULL) {//分配引用失败(しっぱい)
        (VOID)LOS_QueueDelete(mqueueCB->mq_id);
        mqueueCB->mqcb->queueHandle = NULL;
        mqueueCB->mqcb = NULL;
        errno = ENOSPC;
        goto ERROUT;
    }

    mqueueCB->unlinkflag = FALSE;	//是否执行mq_unlink操作
    mqueueCB->mq_personal->mq_status = MQ_USE_MAGIC;//魔法数字
    mqueueCB->mq_personal->mq_next = NULL;			//指向下一个mq_personal
    mqueueCB->mq_personal->mq_posixdes = mqueueCB; 	//指向目标posix队列控制块
    mqueueCB->mq_personal->mq_flags = (INT32)((UINT32)openFlag | ((UINT32)attr->mq_flags & (UINT32)FNONBLOCK));//非阻塞方式

    return mqueueCB->mq_personal;
ERROUT:

    if ((mqueueCB != NULL) && (mqueueCB->mq_name != NULL)) {
        LOS_MemFree(OS_SYS_MEM_ADDR, mqueueCB->mq_name);
        mqueueCB->mq_name = NULL;
    }
    return (struct mqpersonal *)-1;
}
//打开消息队列的含义是新增一个已经创建的消息队列的引用, 1:N的关系,类似于<inode,fd>的关系
STATIC struct mqpersonal *DoMqueueOpen(struct mqarray *mqueueCB, INT32 openFlag)
{
    struct mqpersonal *privateMqPersonal = NULL;

    /* already have the same name of g_squeuetable */
    if (mqueueCB->unlinkflag == TRUE) {//已经执行过unlink了,所以不能被引用了.
        errno = EINVAL;//可以看出mq_unlink不要随意的使用,因为其本意是要删除真正的消息队列的,一旦执行就没有回头路. 只能重新创建一个队列来玩了.
        goto ERROUT;
    }
    /* alloc mqprivate and add to mqarray */
    privateMqPersonal = (struct mqpersonal *)LOS_MemAlloc(OS_SYS_MEM_ADDR, sizeof(struct mqpersonal));//分配一个引用
    if (privateMqPersonal == NULL) {
        errno = ENOSPC;
        goto ERROUT;
    }

    privateMqPersonal->mq_next = mqueueCB->mq_personal;//插入引用链表
    mqueueCB->mq_personal = privateMqPersonal;//消息队列的引用指向始终是指向最后一个打开消息队列的引用

    privateMqPersonal->mq_posixdes = mqueueCB;	//引用都是指向同一个消息队列
    privateMqPersonal->mq_flags = openFlag;		//打开的标签
    privateMqPersonal->mq_status = MQ_USE_MAGIC;//魔法数字

    return privateMqPersonal;

ERROUT:
    return (struct mqpersonal *)-1;
}
//创建或打开一个消息队列，参数是可变参数,返回值是mqd_t类型的值，被称为消息队列描述符
//它符合POSIX IPC的名字规则,openFlag有必须的选项：O_RDONLY，O_WRONLY，O_RDWR，还有可选的选项：O_NONBLOCK，O_CREAT，O_EXCL。
mqd_t mq_open(const char *mqName, int openFlag, ...)
{
    struct mqarray *mqueueCB = NULL;
    struct mqpersonal *privateMqPersonal = (struct mqpersonal *)-1;
    struct mq_attr *attr = NULL;
    struct mq_attr defaultAttr = { 0, MQ_MAX_MSG_NUM, MQ_MAX_MSG_LEN, 0 };
    int retVal;
    va_list ap;//一个字符指针，可以理解为指向当前参数的一个指针，取参必须通过这个指针进行

    if (MqNameCheck(mqName) == -1) {
        return (mqd_t)-1;
    }

    (VOID)pthread_mutex_lock(&g_mqueueMutex);
    mqueueCB = GetMqueueCBByName(mqName);//通过名称获取队列控制块
    if ((UINT32)openFlag & (UINT32)O_CREAT) {//参数指定需要创建了队列的情况
        if (mqueueCB != NULL) {//1.消息队列已经创建了
            if (((UINT32)openFlag & (UINT32)O_EXCL)) {//消息队列已经在被别的进程读/写,此时不能创建新的进程引用
                errno = EEXIST;
                goto OUT;
            }
            privateMqPersonal = DoMqueueOpen(mqueueCB, openFlag);//新增消息队列的引用
        } else {//消息队列还没有创建,就需要create
			//可变参数的实现 函数参数是以数据结构:栈的形式存取,从右至左入栈。
            va_start(ap, openFlag);//对ap进行初始化，让它指向可变参数表里面的第一个参数，va_start第一个参数是 ap 本身，第二个参数是在变参表前面紧挨着的一个变量,即“...”之前的那个参数
            (VOID)va_arg(ap, int);//获取int参数，调用va_arg，它的第一个参数是ap，第二个参数是要获取的参数的指定类型，然后返回这个指定类型的值，并且把 ap 的位置指向变参表的下一个变量位置
            attr = va_arg(ap, struct mq_attr *);//获取mq_attr参数，调用va_arg，它的第一个参数是ap，第二个参数是要获取的参数的指定类型，然后返回这个指定类型的值，并且把 ap 的位置指向变参表的下一个变量位置
            va_end(ap);//获取所有的参数之后，将这个 ap 指针关掉，以免发生危险，方法是调用 va_end，它将输入的参数 ap 置为 NULL

            if (attr != NULL) {
                retVal = LOS_ArchCopyFromUser(&defaultAttr, attr, sizeof(struct mq_attr));//从用户区拷贝到内核区
                if (retVal != 0) {
                    errno = EFAULT;
                    goto OUT;
                }
                if ((defaultAttr.mq_maxmsg < 0) || (defaultAttr.mq_maxmsg > (long int)USHRT_MAX) ||
                    (defaultAttr.mq_msgsize < 0) || (defaultAttr.mq_msgsize > (long int)(USHRT_MAX - sizeof(UINT32)))) {
                    errno = EINVAL;
                    goto OUT;
                }
            }
            privateMqPersonal = DoMqueueCreate(&defaultAttr, mqName, openFlag);//创建队列并新增消息队列的引用
        }
    } else {//已经创建了队列
        if (mqueueCB == NULL) {//
            errno = ENOENT;
            goto OUT;
        }
        privateMqPersonal = DoMqueueOpen(mqueueCB, openFlag);//新增消息队列的引用
    }

OUT:
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
    return (mqd_t)privateMqPersonal;
}
/**********************************************************
关闭posix队列,用于关闭一个消息队列，和文件的close类型，关闭后，消息队列并不从系统中删除。
一个进程结束，会自动调用关闭打开着的消息队列。
mq_close的含义是关闭一个进程的引用操作,也就是 mqueueCB->mq_personal = privateMqPersonal->mq_next;
mq_close 和 mq_open 成对出现
**********************************************************/
int mq_close(mqd_t personal)
{
    INT32 ret = 0;
    struct mqarray *mqueueCB = NULL;
    struct mqpersonal *privateMqPersonal = NULL;
    struct mqpersonal *tmp = NULL;
	//posix队列本质是在内核开辟的一块缓存区,用于进程间数据的交换
    if (!LOS_IsKernelAddressRange(personal, sizeof(struct mqpersonal))) {//不是在内核空间，返回错误
        errno = EBADF;
        return -1;
    }

    (VOID)pthread_mutex_lock(&g_mqueueMutex);
    privateMqPersonal = (struct mqpersonal *)personal;//这种用法,mq_id必须要放在结构体第一位
    if (privateMqPersonal->mq_status != MQ_USE_MAGIC) {//魔法数字可以判断队列是否溢出过,必须放在结构体最后一位
        errno = EBADF;
        goto OUT_UNLOCK;
    }

    mqueueCB = privateMqPersonal->mq_posixdes;//拿出消息队列
    if (mqueueCB->mq_personal == NULL) {//队列是否被进程打开过,一个从未被打开过的消息队列何谈关闭.
        errno = EBADF;
        goto OUT_UNLOCK;
    }

    /* find the personal and remove */
    if (mqueueCB->mq_personal == privateMqPersonal) {//如果第一个就找到了
        mqueueCB->mq_personal = privateMqPersonal->mq_next;//这步操作相当于删除了privateMqPersonal
    } else {//如果第一个不是,说明可能在接下来的next中能遇到 == 
        for (tmp = mqueueCB->mq_personal; tmp->mq_next != NULL; tmp = tmp->mq_next) {//一直遍历 next
            if (tmp->mq_next == privateMqPersonal) {//找到了当初的 mq_open 的那个消息队列 
                break;
            }
        }
        if (tmp->mq_next == NULL) {
            errno = EBADF;
            goto OUT_UNLOCK;
        }
        tmp->mq_next = privateMqPersonal->mq_next;//这步操作相当于删除了privateMqPersonal
    }
    /* flag no use */
    privateMqPersonal->mq_status = 0;//未被使用

    /* free the personal */
    ret = LOS_MemFree(OS_SYS_MEM_ADDR, privateMqPersonal);
    if (ret != LOS_OK) {
        errno = EFAULT;
        ret = -1;
        goto OUT_UNLOCK;
    }

    if ((mqueueCB->unlinkflag == TRUE) && (mqueueCB->mq_personal == NULL)) {//执行过unlink且没有进程在使用队列了
        ret = DoMqueueDelete(mqueueCB);//删除消息队列,注意:mq_close的主要任务并不是想做这步操作的,mq_unlink才是真正想做这步操作
    }//一定要明白 mq_close 和 mq_unlink 的区别
OUT_UNLOCK:
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
    return ret;
}
//获取队列属性。属性由参数mqAttr带走
int OsMqGetAttr(mqd_t personal, struct mq_attr *mqAttr)
{
    struct mqarray *mqueueCB = NULL;
    struct mqpersonal *privateMqPersonal = NULL;

    if (!LOS_IsKernelAddressRange(personal, sizeof(struct mqpersonal))) {//personal 必须在内核空间
        errno = EBADF;
        return -1;
    }

    if (mqAttr == NULL) {
        errno = EINVAL;
        return -1;
    }

    (VOID)pthread_mutex_lock(&g_mqueueMutex);
    privateMqPersonal = (struct mqpersonal *)personal;//获取引用描述符
    if (privateMqPersonal->mq_status != MQ_USE_MAGIC) {
        errno = EBADF;
        (VOID)pthread_mutex_unlock(&g_mqueueMutex);
        return -1;
    }

    mqueueCB = privateMqPersonal->mq_posixdes;
    mqAttr->mq_maxmsg = mqueueCB->mqcb->queueLen;
    mqAttr->mq_msgsize = mqueueCB->mqcb->queueSize - sizeof(UINT32);
    mqAttr->mq_curmsgs = mqueueCB->mqcb->readWriteableCnt[OS_QUEUE_READ];
    mqAttr->mq_flags = privateMqPersonal->mq_flags;
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
    return 0;
}
//设置队列属性
int OsMqSetAttr(mqd_t personal, const struct mq_attr *mqSetAttr, struct mq_attr *mqOldAttr)
{
    struct mqpersonal *privateMqPersonal = NULL;

    if (!LOS_IsKernelAddressRange(personal, sizeof(struct mqpersonal))) {//必须在内核空间
        errno = EBADF;
        return -1;
    }

    if (mqSetAttr == NULL) {
        errno = EINVAL;
        return -1;
    }

    (VOID)pthread_mutex_lock(&g_mqueueMutex);
    privateMqPersonal = (struct mqpersonal *)personal;//获取引用描述对象
    if (privateMqPersonal->mq_status != MQ_USE_MAGIC) {//魔法数字对不上,说明可能发生过 OOM 异常
        errno = EBADF;
        (VOID)pthread_mutex_unlock(&g_mqueueMutex);
        return -1;
    }

    if (mqOldAttr != NULL) {
        (VOID)OsMqGetAttr((mqd_t)privateMqPersonal, mqOldAttr);//先拿走老的设置
    }

    privateMqPersonal->mq_flags = (INT32)((UINT32)privateMqPersonal->mq_flags & (UINT32)(~FNONBLOCK)); /* clear */
    if (((UINT32)mqSetAttr->mq_flags & (UINT32)FNONBLOCK) == (UINT32)FNONBLOCK) {
        privateMqPersonal->mq_flags = (INT32)((UINT32)privateMqPersonal->mq_flags | (UINT32)FNONBLOCK);
    }
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
    return 0;
}
//一个函数做两个工作，因为这是posix的标准接口，
int mq_getsetattr(mqd_t mqd, const struct mq_attr *new, struct mq_attr *old)
{
    if (new == NULL) {//用第二个参数来判断是否是获取还是设置功能
        return OsMqGetAttr(mqd, old);
    }
    return OsMqSetAttr(mqd, new, old);
}
/************************************************************
返回值：成功，0；出错，-1
每个消息队列有一个保存其当前打开的描述符数的引用计数，只有当引用计数为0时，
才删除该消息队列。mq_unlink和mq_close都会让引用数减一
************************************************************/
int mq_unlink(const char *mqName)
{
    INT32 ret = 0;
    struct mqarray *mqueueCB = NULL;

    if (MqNameCheck(mqName) == -1) {
        return -1;
    }

    (VOID)pthread_mutex_lock(&g_mqueueMutex);
    mqueueCB = GetMqueueCBByName(mqName);//通过名称获取posix队列控制块
    if (mqueueCB == NULL) {
        errno = ENOENT;
        goto ERROUT_UNLOCK;
    }

    if (mqueueCB->mq_personal != NULL) {//引用计数不为0,不删除.
        mqueueCB->unlinkflag = TRUE;//标记执行过unlink了,后面再打开这个消息队列就会失败
    } else {
        ret = DoMqueueDelete(mqueueCB);//只有当引用计数为0时，即所有打开的进程都执行了mq_close操作,才删除该消息队列
    }

    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
    return ret;

ERROUT_UNLOCK:
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
    return -1;
}
//把时间转成 节拍
STATIC INT32 ConvertTimeout(long flags, const struct timespec *absTimeout, UINT64 *ticks)
{
    if ((UINT32)flags & (UINT32)FNONBLOCK) {
        *ticks = LOS_NO_WAIT;
        return 0;
    }

    if (absTimeout == NULL) {
        *ticks = LOS_WAIT_FOREVER;
        return 0;
    }

    if (!ValidTimeSpec(absTimeout)) {
        errno = EINVAL;
        return -1;
    }

    *ticks = OsTimeSpec2Tick(absTimeout);
    return 0;
}

STATIC INLINE BOOL MqParamCheck(mqd_t personal, const char *msg, size_t msgLen)
{
    if (!LOS_IsKernelAddressRange(personal, sizeof(struct mqpersonal))) {
        errno = EBADF;
        return FALSE;
    }

    if ((msg == NULL) || (msgLen == 0)) {
        errno = EINVAL;
        return FALSE;
    }
    return TRUE;
}

#define OS_MQ_GOTO_ERROUT_UNLOCK_IF(expr, errcode) \
    if (expr) {                        \
        errno = errcode;                 \
        goto ERROUT_UNLOCK;                     \
    }
#define OS_MQ_GOTO_ERROUT_IF(expr, errcode) \
    if (expr) {                        \
        errno = errcode;                 \
        goto ERROUT;                     \
    }
//posix ipc 标准接口之 定时发送消息 msgPrio为消息发送的优先级,目前尚未支持优先级的发送
int mq_timedsend(mqd_t personal, const char *msg, size_t msgLen, unsigned int msgPrio,
                 const struct timespec *absTimeout)
{
    UINT32 mqueueID, err;
    UINT64 absTicks;
    struct mqarray *mqueueCB = NULL;
    struct mqpersonal *privateMqPersonal = NULL;

    OS_MQ_GOTO_ERROUT_IF(!MqParamCheck(personal, msg, msgLen), errno);//参数异常处理

    OS_MQ_GOTO_ERROUT_IF(msgPrio > (MQ_PRIO_MAX - 1), EINVAL);//优先级异常处理，鸿蒙目前只支持 msgPrio <= 0

    (VOID)pthread_mutex_lock(&g_mqueueMutex);//临界区拿锁操作
    privateMqPersonal = (struct mqpersonal *)personal;
    OS_MQ_GOTO_ERROUT_UNLOCK_IF(privateMqPersonal->mq_status != MQ_USE_MAGIC, EBADF);//魔法数字异常处理

    mqueueCB = privateMqPersonal->mq_posixdes;
    OS_MQ_GOTO_ERROUT_UNLOCK_IF(msgLen > (size_t)(mqueueCB->mqcb->queueSize - sizeof(UINT32)), EMSGSIZE);//消息长度异常处理

    OS_MQ_GOTO_ERROUT_UNLOCK_IF((((UINT32)privateMqPersonal->mq_flags & (UINT32)O_WRONLY) != (UINT32)O_WRONLY) &&
                                (((UINT32)privateMqPersonal->mq_flags & (UINT32)O_RDWR) != (UINT32)O_RDWR),
                                EBADF);//消息标识异常，必须有只读而且是可读可写标签

    OS_MQ_GOTO_ERROUT_UNLOCK_IF(ConvertTimeout(privateMqPersonal->mq_flags, absTimeout, &absTicks) == -1, errno);//时间转换成tick 处理
    mqueueID = mqueueCB->mq_id;//记录消息队列ID
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);

    err = LOS_QueueWriteCopy(mqueueID, (VOID *)msg, (UINT32)msgLen, (UINT32)absTicks);//拷贝消息内容到内核空间
    if (map_errno(err) != ENOERR) {
        goto ERROUT;
    }
    return 0;
ERROUT_UNLOCK:
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);//释放锁
ERROUT:
    return -1;
}
//posix ipc 标准接口之定时接收消息
ssize_t mq_timedreceive(mqd_t personal, char *msg, size_t msgLen, unsigned int *msgPrio,
                        const struct timespec *absTimeout)
{
    UINT32 mqueueID, err;
    UINT32 receiveLen;
    UINT64 absTicks;
    struct mqarray *mqueueCB = NULL;
    struct mqpersonal *privateMqPersonal = NULL;

    if (!MqParamCheck(personal, msg, msgLen)) {
        goto ERROUT;
    }

    if (msgPrio != NULL) {
        *msgPrio = 0;
    }

    (VOID)pthread_mutex_lock(&g_mqueueMutex);
    privateMqPersonal = (struct mqpersonal *)personal;
    if (privateMqPersonal->mq_status != MQ_USE_MAGIC) {
        errno = EBADF;
        goto ERROUT_UNLOCK;
    }

    mqueueCB = privateMqPersonal->mq_posixdes;
    if (msgLen < (size_t)(mqueueCB->mqcb->queueSize - sizeof(UINT32))) {
        errno = EMSGSIZE;
        goto ERROUT_UNLOCK;
    }

    if (((UINT32)privateMqPersonal->mq_flags & (UINT32)O_WRONLY) == (UINT32)O_WRONLY) {
        errno = EBADF;
        goto ERROUT_UNLOCK;
    }

    if (ConvertTimeout(privateMqPersonal->mq_flags, absTimeout, &absTicks) == -1) {
        goto ERROUT_UNLOCK;
    }

    receiveLen = msgLen;
    mqueueID = mqueueCB->mq_id;
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);

    err = LOS_QueueReadCopy(mqueueID, (VOID *)msg, &receiveLen, (UINT32)absTicks);//读消息队列
    if (map_errno(err) == ENOERR) {
        return (ssize_t)receiveLen;
    } else {
        goto ERROUT;
    }

ERROUT_UNLOCK:
    (VOID)pthread_mutex_unlock(&g_mqueueMutex);
ERROUT:
    return -1;
}

/* not support the prio */
//给描述符mqdes指向的消息队列发送消息，大小为msg_len，内容存放在msg_ptr中，prio为优先级(暂不支持)
int mq_send(mqd_t personal, const char *msg_ptr, size_t msg_len, unsigned int msg_prio)
{
    return mq_timedsend(personal, msg_ptr, msg_len, msg_prio, NULL);
}
//从描述符personal指向的消息队列中读取消息存放msg_ptr中。
ssize_t mq_receive(mqd_t personal, char *msg_ptr, size_t msg_len, unsigned int *msg_prio)
{
    return mq_timedreceive(personal, msg_ptr, msg_len, msg_prio, NULL);
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
