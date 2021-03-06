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
#pragma once

#include <AP_HAL/AP_HAL.h>
#include "AP_HAL_ChibiOS_Namespace.h"
#include "Semaphores.h"
#include "Thread.h"

class ChibiOS::ChibiUtil : public AP_HAL::Util {
public:
    bool run_debug_shell(AP_HAL::BetterStream *stream) { return false; }
    AP_HAL::Semaphore *new_semaphore(void) override { return new ChibiOS::Semaphore; }
    uint32_t available_memory() override;

    /*
      return state of safety switch, if applicable
     */
    enum safety_state safety_switch_state(void) override;

    void* alloc_from_ccm_ram(size_t size);

    //Thread Interface
    AP_HAL::Thread* create_thread(const char *name, int policy, int priority, size_t stack_size, void* ctx) override;
    TimerTask *add_timer_task(AP_HAL::Thread* thd, TaskProc task_func, uint32_t delay, bool repeat, void* ctx) override;
    void reschedule_timer_task(AP_HAL::Thread* thd, TimerTask* timer_task, uint32_t delay) override;
    void remove_timer_task(AP_HAL::Thread* thd, TimerTask* timer_task) override;
    EventTask *create_event_task(TaskProc task_func, void* ctx) override;
    void send_event(AP_HAL::Thread* thd, EventTask* event_task) override;
};
