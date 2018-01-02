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
 * Based on framework thread structure by Jonathan Challinger
 * Modified for Ardupilot by Siddharth Bharat Purohit
 */
#include "Thread.h"

#include <AP_HAL/AP_HAL.h>

extern const AP_HAL::HAL &hal;

using namespace ChibiOS;

//Set Thread parameters
void ChibiOS::Thread::init(const char* name, uint32_t priority)
{
    _name = name;
    _priority = priority;
}

//start thread
void ChibiOS::Thread::start(size_t stack_size)
{
    _stack_size = stack_size;
    _thd = chThdCreateFromHeap( NULL,          /* NULL = Default heap. */
                                THD_WORKING_AREA_SIZE(_stack_size), /* Stack.   */
                                _name,
                                _priority,    /* Initial priority.    */
                                _run_trampoline,      /* Thread function.     */
                                this);         /* Thread parameter.    */
    if (_thd == nullptr) {
        AP_HAL::panic("Unable to create thread %s!\n", _name);
    }
}

//add timer task to thread, to be called after timer is expired
//granularity of time is dependent on CH_CFG_ST_FREQUENCY
AP_HAL::TimerTask* ChibiOS::Thread::add_timer_task(TaskProc task_func, uint32_t timer_expiration, bool auto_repeat, void *ctx)
{
    TimerTask* task = _init_timer_task(chVTGetSystemTimeX(), US2ST(timer_expiration), auto_repeat, task_func, ctx);
    chSysLock();
    _insert_timer_task(task);

    // Wake thread to process added task
    chThdResumeI(&_thd, MSG_TIMEOUT);
    //Safeguard against waking higher priority thread from lower
    chSchRescheduleS();
    chSysUnlock();
    return task;
}

void ChibiOS::Thread::reschedule_timer_task(TimerTask* task, uint32_t timer_expiration)
{
    chSysLock();
    systime_t t_now = chVTGetSystemTimeX();

    LINKED_LIST_REMOVE(TimerTask, _timer_task_list_head, task);

    task->timer_expiration_ticks = US2ST(timer_expiration);
    task->timer_begin_systime = t_now;

    _insert_timer_task(task);

    // Wake worker thread to process tasks
    chThdResumeI(&_thd, MSG_TIMEOUT);
    //Safeguard against waking higher priority thread from lower
    chSchRescheduleS();
    chSysUnlock();
}

void ChibiOS::Thread::remove_timer_task(TimerTask* task)
{
    chSysLock();
    LINKED_LIST_REMOVE(TimerTask, _timer_task_list_head, task);
    chSysUnlock();
}

void ChibiOS::Thread::_insert_event_task(EventTask* task)
{
    EventTask** insert_ptr = &_event_task_list_head;
    while (*insert_ptr) {
        if (*insert_ptr == task) {
            //do not register event twice
            return;
        }
        insert_ptr = &(*insert_ptr)->next;
    }
    *insert_ptr = task;
}

//setup timer task handle
TimerTask* ChibiOS::Thread::_init_timer_task(systime_t timer_begin_systime, systime_t timer_expiration_ticks, bool auto_repeat, TaskProc task_func, void* ctx)
{
    TimerTask* task = new TimerTask;
    if (task == nullptr) {
        AP_HAL::panic("Unable to create Timer Task on thread %s", _name);
    }
    task->task_func = task_func;
    task->ctx = ctx;
    task->timer_expiration_ticks = timer_expiration_ticks;
    task->auto_repeat = auto_repeat;
    task->timer_begin_systime = timer_begin_systime;
    return task;
}

bool ChibiOS::Thread::_is_timer_task_registered(TimerTask* check_task)
{
    TimerTask* task = _timer_task_list_head;
    while (task) {
        if (task == check_task) {
            return true;
        }
        task = task->next;
    }
    return false;
}

void ChibiOS::Thread::_insert_timer_task(TimerTask* task)
{
    if (_is_timer_task_registered(task)) {
        AP_HAL::panic("Task already registered!");
    }

    if (task->timer_expiration_ticks == TIME_INFINITE) {
        return;
    }

    systime_t task_run_time = task->timer_begin_systime + task->timer_expiration_ticks;
    TimerTask** insert_ptr = &_timer_task_list_head;
    while (*insert_ptr && task_run_time - (*insert_ptr)->timer_begin_systime >= (*insert_ptr)->timer_expiration_ticks) {
        insert_ptr = &(*insert_ptr)->next;
    }
    task->next = *insert_ptr;
    *insert_ptr = task;
}

uint64_t ChibiOS::Thread::_get_ticks_to_timer_task(TimerTask* task, systime_t tnow_ticks)
{
    if (task && task->timer_expiration_ticks != TIME_INFINITE) {
        systime_t elapsed = tnow_ticks - task->timer_begin_systime;
        if (elapsed >= task->timer_expiration_ticks) {
            return TIME_IMMEDIATE;
        } else {
            return task->timer_expiration_ticks - elapsed;
        }
    } else {
        return TIME_INFINITE;
    }
}

void ChibiOS::Thread::_run_trampoline(void* ctx)
{
    Thread* thread_handle = (Thread*)ctx;
    thread_handle->_run();
}

void ChibiOS::Thread::_run()
{
    while (true) {
        //handle event tasks
        while(_event_task_list_head) {
            _event_task_list_head->task_func();
            _event_task_list_head = _event_task_list_head->next;
        }
        chSysLock();
        uint64_t tnow_ticks = chVTGetSystemTimeX();
        uint64_t ticks_to_next_timer_task = _get_ticks_to_timer_task(_timer_task_list_head, tnow_ticks);
        if (ticks_to_next_timer_task == TIME_IMMEDIATE) {
            // Task is due - pop the task off the task list, run it, reschedule if task is auto-repeat
            TimerTask* next_timer_task = _timer_task_list_head;
            _timer_task_list_head = next_timer_task->next;

            chSysUnlock();

            // Perform task
            next_timer_task->task_func();
            next_timer_task->timer_begin_systime = tnow_ticks;

            if (next_timer_task->auto_repeat) {
                // Re-insert task
                chSysLock();
                _insert_timer_task(next_timer_task);
                chSysUnlock();
            }
        } else {
            // don't delay for less than 400usec, so one thread doesn't
            // completely dominate the CPU
            if (ticks_to_next_timer_task < US2ST(400)) {
                ticks_to_next_timer_task = US2ST(400);
            }
            if (_event_task_list_head) {
                //we have events to process
                continue;
            }
            // No task due - go to sleep until there is a task
            _is_sleeping = true;
            chThdSuspendTimeoutS(&_thd, ticks_to_next_timer_task);
            _is_sleeping = false;
            chSysUnlock();
        }
    }
}

void ChibiOS::Thread::send_event_from_irq(EventTask* evt)
{
    _insert_event_task(evt);
    if (_is_sleeping) {
        chSysLockFromISR();
        chThdResumeI(&_thd, (msg_t)evt);
        chSysUnlockFromISR();
    }
}

void ChibiOS::Thread::send_event(EventTask* evt)
{
    chSysLock();
    _insert_event_task(evt);
    chThdResumeI(&_thd, (msg_t)evt);
    //Safeguard against waking higher priority thread from lower
    chSchRescheduleS();
    chSysUnlock();
}
