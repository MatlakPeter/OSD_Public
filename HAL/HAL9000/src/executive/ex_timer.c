#include "HAL9000.h"
#include "ex_timer.h"
#include "iomu.h"
#include "thread_internal.h"

STATUS
ExTimerInit(
    OUT     PEX_TIMER       Timer,
    IN      EX_TIMER_TYPE   Type,
    IN      QWORD           Time
    )
{
    STATUS status;

    if (NULL == Timer)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Type > ExTimerTypeMax)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    status = STATUS_SUCCESS;

    memzero(Timer, sizeof(EX_TIMER));

    Timer->Type = Type;
    if (Timer->Type != ExTimerTypeAbsolute)
    {
        // relative time

        // if the time trigger time has already passed the timer will
        // be signaled after the first scheduler tick
        Timer->TriggerTimeUs = IomuGetSystemTimeUs() + Time;
        Timer->ReloadTimeUs = Time;
    }
    else
    {
        // absolute
        Timer->TriggerTimeUs = Time;
    }

    InsertOrderedList(&m_globalTimerList.TimerListHead, &Timer->TimerListElem, CompareTimer, NULL);

    return status;
}

void
ExTimerStart(
    IN      PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = TRUE;
}

void
ExTimerStop(
    IN      PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = FALSE;
}

void
ExTimerWait(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    while (IomuGetSystemTimeUs() < Timer->TriggerTimeUs && Timer->TimerStarted)
    {
        ThreadYield();
    }
}

void
ExTimerUninit(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    ExTimerStop(Timer);

    Timer->TimerUninited = TRUE;
}

INT64
ExTimerCompareTimers(
    IN      PEX_TIMER     FirstElem,
    IN      PEX_TIMER     SecondElem
)
{
    return FirstElem->TriggerTimeUs - SecondElem->TriggerTimeUs;
}

typedef struct _GLOBAL_TIMER_LIST
{
    LOCK        TimerListLock;
    LIST_ENTRY  TimerListHead;
} GLOBAL_TIMER_LIST;

static GLOBAL_TIMER_LIST m_globalTimerList;

void
ExTimerSystemPreinit(void)
{
    memzero(&m_globalTimerList, sizeof(m_globalTimerList));
    LockInit(&m_globalTimerList.TimerListLock);
    LockSetName(&m_globalTimerList.TimerListLock, "TimerList");
    InitializeListHead(&m_globalTimerList.TimerListHead);
}

PFUNC_CompareFunction CompareTimer(PLIST_ENTRY FirstElem, PLIST_ENTRY SecondElem, PVOID Context)
{
    PEX_TIMER FirstTimer = CONTAINING_RECORD(FirstElem, EX_TIMER, TimerListElem);
    PEX_TIMER SecondTimer = CONTAINING_RECORD(SecondElem, EX_TIMER, TimerListElem);

    return ExTimerCompareTimers(FirstTimer, SecondTimer);
}