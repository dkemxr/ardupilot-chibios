#include <AP_HAL/AP_HAL.h>
#if CONFIG_HAL_BOARD == HAL_BOARD_CHIBIOS
#include "Util.h"
#include <chheap.h>

#if HAL_WITH_IO_MCU
#include <AP_IOMCU/AP_IOMCU.h>
extern AP_IOMCU iomcu;
#endif

using namespace ChibiOS;

/**
   how much free memory do we have in bytes.
*/
uint32_t ChibiUtil::available_memory(void)
{
    size_t totalp = 0;
    // get memory available on heap
    chHeapStatus(nullptr, &totalp, nullptr);

    // we also need to add in memory that is not yet allocated to the heap
    totalp += chCoreGetStatusX();

    return totalp;
}

/*
  get safety switch state
 */
ChibiUtil::safety_state ChibiUtil::safety_switch_state(void)
{
#if HAL_WITH_IO_MCU
    return iomcu.get_safety_switch_state();
#else
    return SAFETY_NONE;
#endif
}

AP_HAL::Thread *ChibiUtil::create_thread(const char *name, int policy, int priority, size_t stack_size, void* ctx)
{
    ChibiOS::Thread* new_thd = new ChibiOS::Thread;
    new_thd->init(name, priority);
    new_thd->start(stack_size);
    return (AP_HAL::Thread*)new_thd;
}

TimerTask *ChibiUtil::add_timer_task(AP_HAL::Thread* thd, TaskProc task_func, uint32_t delay, bool repeat, void* ctx)
{
    return ((ChibiOS::Thread*)thd)->add_timer_task(task_func, delay, repeat, ctx);
}

void ChibiUtil::reschedule_timer_task(AP_HAL::Thread* thd, TimerTask* timer_task, uint32_t delay)
{
    ((ChibiOS::Thread*)thd)->reschedule_timer_task(timer_task, delay);
}

void ChibiUtil::remove_timer_task(AP_HAL::Thread* thd, TimerTask* timer_task)
{
    ((ChibiOS::Thread*)thd)->remove_timer_task(timer_task);
}

EventTask* ChibiUtil::create_event_task(TaskProc task_func, void* ctx)
{
    EventTask* new_event = new EventTask;
    new_event->task_func = task_func;
    new_event->ctx = ctx;
    return new_event;
}

void ChibiUtil::send_event(AP_HAL::Thread* thd, EventTask* event_task)
{
    ((ChibiOS::Thread*)thd)->send_event(event_task);
}

#endif //CONFIG_HAL_BOARD == HAL_BOARD_CHIBIOS
