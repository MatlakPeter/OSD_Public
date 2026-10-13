#include "HAL9000.h"
#include "ex_timer.h"
#include "iomu.h"
#include "thread_internal.h"

typedef struct _GLOBAL_TIMER_LIST
{
    LOCK TimerListLock;
    LIST_ENTRY TimerListHead;
} GLOBAL_TIMER_LIST;

static GLOBAL_TIMER_LIST m_globalTimerList;

static FUNC_CompareFunction _ExTimerCompareListElems;

STATUS
ExTimerInit(
    OUT PEX_TIMER Timer,
    IN EX_TIMER_TYPE Type,
    IN QWORD Time)
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

    // kell initelni az eventet
    status = ExEventInit(&Timer->TimerEvent, ExEventTypeNotification, FALSE);
    if (!SUCCEEDED(status))
    {
        return status;
    }
    InitializeListHead(&Timer->TimerListElem);

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

    INTR_STATE oldState;

    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);
    InsertOrderedList(&m_globalTimerList.TimerListHead, &Timer->TimerListElem, _ExTimerCompareListElems, NULL);
    LockRelease(&m_globalTimerList.TimerListLock, oldState);

    return status;
}

void ExTimerStart(
    IN PEX_TIMER Timer)
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = TRUE;
}

void ExTimerStop(
    IN PEX_TIMER Timer)
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    INTR_STATE oldState;

    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);
    Timer->TimerStarted = FALSE;
    /*
    The tests call ExTimerStop(&timer) and then ExTimerUninit(&timer), 
    and Uninit calls Stop again before it sets TimerUninited. 
    So the same entry is removed twice. After the first removal, 
    RemoveEntryList leaves the hook holding pointers to its old neighbors, 
    and the second removal writes through them.
    */
    if (!IsListEmpty(&Timer->TimerListElem))        // megnezed hogy bent van global listben
    {
        RemoveEntryList(&Timer->TimerListElem);
        InitializeListHead(&Timer->TimerListElem);  // legit elbasz mindent gondolj bele
    }

    LockRelease(&m_globalTimerList.TimerListLock, oldState);

    // felebreszted az osszes threadet ami ettol a timertol fugg
    ExEventSignal(&Timer->TimerEvent);
}

void ExTimerWait(
    INOUT PEX_TIMER Timer)
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    if ((Timer->Type != ExTimerTypeRelativePeriodic || Timer->ReloadTimeUs == 0) &&
        IomuGetSystemTimeUs() >= Timer->TriggerTimeUs)
    {
        return;
    }

    // elaltatja  threadet
    ExEventWaitForSignal(&Timer->TimerEvent);
}

void ExTimerUninit(
    INOUT PEX_TIMER Timer)
{
    ASSERT(Timer != NULL);

    ExTimerStop(Timer);
    Timer->TimerUninited = TRUE;
    ExEventDestroy(&Timer->TimerEvent);
}

INT64
ExTimerCompareTimers(
    IN PEX_TIMER FirstElem,
    IN PEX_TIMER SecondElem)
{
    return FirstElem->TriggerTimeUs - SecondElem->TriggerTimeUs;
}

void ExTimerSystemPreinit(void)
{
    memzero(&m_globalTimerList, sizeof(m_globalTimerList));
    LockInit(&m_globalTimerList.TimerListLock);
    LockSetName(&m_globalTimerList.TimerListLock, "TimerList");
    InitializeListHead(&m_globalTimerList.TimerListHead); // csinal belole egy empty listet
}

static INT64(CDECL _ExTimerCompareListElems)(
    IN PLIST_ENTRY FirstElem,
    IN PLIST_ENTRY SecondElem,
    IN_OPT PVOID Context)
{
    PEX_TIMER pFirstTimer;
    PEX_TIMER pSecondTimer;

    // need this line otherwise compile error
    UNREFERENCED_PARAMETER(Context);

    pFirstTimer = CONTAINING_RECORD(FirstElem, EX_TIMER, TimerListElem);
    pSecondTimer = CONTAINING_RECORD(SecondElem, EX_TIMER, TimerListElem);

    return ExTimerCompareTimers(pFirstTimer, pSecondTimer);
}

void ExTimerCheckAll(void)
{
    INTR_STATE oldState;
    QWORD now;
    PEX_TIMER pTimer;

    now = IomuGetSystemTimeUs();

    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);

    while (!IsListEmpty(&m_globalTimerList.TimerListHead))
    {
        pTimer = CONTAINING_RECORD(m_globalTimerList.TimerListHead.Flink,
                                   EX_TIMER, TimerListElem);

        // mivel sorted ha az elso nem jart le a tobbi sem
        if (pTimer->TriggerTimeUs > now)
            break;

        // ha lejart
        RemoveEntryList(&pTimer->TimerListElem);
        InitializeListHead(&pTimer->TimerListElem); // ezt meg mindig nem ertem

        ExEventSignal(&pTimer->TimerEvent); // felebreszti a threadeket amik ra varnak

        if (pTimer->Type == ExTimerTypeRelativePeriodic)
        {
            ExEventClearSignal(&pTimer->TimerEvent); // the next wait must block again
            pTimer->TriggerTimeUs += pTimer->ReloadTimeUs;
            if (pTimer->TriggerTimeUs <= now) // fell behind: skip missed periods
            {
                pTimer->TriggerTimeUs = now + pTimer->ReloadTimeUs;
            }
            InsertOrderedList(&m_globalTimerList.TimerListHead,
                              &pTimer->TimerListElem, _ExTimerCompareListElems, NULL);
        }
    }

    LockRelease(&m_globalTimerList.TimerListLock, oldState);
}