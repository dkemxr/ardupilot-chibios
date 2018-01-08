/*
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * Code by Andrew Tridgell and Siddharth Bharat Purohit
 */
#include <AP_HAL/AP_HAL.h>
#if CONFIG_HAL_BOARD == HAL_BOARD_CHIBIOS
#include "Util.h"
#include <chheap.h>

#if HAL_WITH_IO_MCU
#include <AP_IOMCU/AP_IOMCU.h>
extern AP_IOMCU iomcu;
#endif

using namespace ChibiOS;
//32K CCM RAM Heap
CCM_RAM_ATTRIBUTE CH_HEAP_AREA(ccm_heap_region, 32*1024);

static memory_heap_t *ccm_heap = nullptr;
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
    CCM RAM Region heap Allocations
*/
void* ChibiUtil::alloc_from_ccm_ram(size_t size)
{
    if (ccm_heap == nullptr) {
        //initialize ccm heap
        chHeapObjectInit(ccm_heap, ccm_heap_region, 32*1024);
    }
    return chHeapAllocAligned(NULL, size, CH_HEAP_ALIGNMENT);
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
