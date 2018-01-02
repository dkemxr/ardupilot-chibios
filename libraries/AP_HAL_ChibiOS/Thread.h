/*
 * 
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
 * Based on Framework by Jonathan Challinger
 * Modified for Ardupilot by Siddharth Bharat Purohit
 */

#pragma once

#include "hal.h"
#include "ch.h"
#include <AP_HAL/AP_HAL.h>
#include "AP_HAL_ChibiOS_Namespace.h"
#include <AP_Math/AP_Math.h>
#include <AP_HAL/utility/functor.h>

class ChibiOS::Thread : public AP_HAL::Thread {
public:
    void init(const char* name, uint32_t priority) override;
    void start(size_t stack_size) override;
    TimerTask* add_timer_task(TaskProc task_func, uint32_t timer_expiration, bool auto_repeat, void* ctx) override;
    void reschedule_timer_task(TimerTask* task, uint32_t timer_expiration) override;
    void remove_timer_task(TimerTask* task) override;
    void send_event_from_irq(EventTask* evt) override;
    void send_event(EventTask* evt) override;

private:
    void _run();
    static void _run_trampoline(void* ctx);
    TimerTask* _init_timer_task(systime_t timer_begin_systime, systime_t timer_expiration_ticks, bool auto_repeat, TaskProc task_func, void* ctx);
    bool _is_timer_task_registered(TimerTask* check_task);
    void _insert_timer_task(TimerTask* task);
    uint64_t _get_ticks_to_timer_task(TimerTask* task, systime_t tnow_ticks);
    void _insert_event_task(EventTask* task);

    thread_t *_thd;
    const char* _name;
    uint32_t _priority;
    size_t _stack_size;
    TimerTask* _timer_task_list_head;
    EventTask* _event_task_list_head;
    bool _is_sleeping = true;
};
