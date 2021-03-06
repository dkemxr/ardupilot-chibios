/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <AP_HAL/AP_HAL.h>
#include <AP_HAL/I2CDevice.h>
#include <utility>

#include <AP_Common/AP_Common.h>
#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>
#include <AP_BoardConfig/AP_BoardConfig.h>
#include "AP_Baro_ICM20789.h"

#include <utility>
#include <stdio.h>

#include <AP_Math/AP_Math.h>
#include <DataFlash/DataFlash.h>

extern const AP_HAL::HAL &hal;

/*
  CMD_READ options. The draft datasheet doesn't specify, but it seems
  Mode_1 has a conversion interval of 2ms. Mode_3 has a conversion
  interval of 20ms. Both seem to produce equally as smooth results, so
  presumably Mode_3 is doing internal averaging
 */
#define CMD_READ_PT_MODE_1 0x401A
#define CMD_READ_PT_MODE_3 0x5059
#define CMD_READ_TP_MODE_1 0x609C
#define CMD_READ_TP_MODE_3 0x70DF

#define CONVERSION_INTERVAL_MODE_1 2000
#define CONVERSION_INTERVAL_MODE_3 20000

// setup for Mode_3
#define CMD_READ_PT CMD_READ_PT_MODE_3
#define CONVERSION_INTERVAL CONVERSION_INTERVAL_MODE_3

#define CMD_SOFT_RESET     0x805D
#define CMD_READ_ID        0xEFC8

#define debug(fmt, args...)   hal.console->printf(fmt, ##args)

/*
  constructor
 */
AP_Baro_ICM20789::AP_Baro_ICM20789(AP_Baro &baro, AP_HAL::OwnPtr<AP_HAL::I2CDevice> _dev)
    : AP_Baro_Backend(baro)
    , dev(std::move(_dev))
{
}

AP_Baro_Backend *AP_Baro_ICM20789::probe(AP_Baro &baro,
                                         AP_HAL::OwnPtr<AP_HAL::I2CDevice> dev)
{
    debug("Probing for ICM20789 baro\n");
    if (!dev) {
        return nullptr;
    }
    AP_Baro_ICM20789 *sensor = new AP_Baro_ICM20789(baro, std::move(dev));
    if (!sensor || !sensor->init()) {
        delete sensor;
        return nullptr;
    }
    return sensor;
}


bool AP_Baro_ICM20789::spi_init(void)
{
    dev_icm->set_read_flag(0x80);

    dev_icm->set_speed(AP_HAL::Device::SPEED_LOW);
    uint8_t whoami = 0;
    uint8_t v;
        
    dev_icm->read_registers(0x6A, &v, 1);
    dev_icm->write_register(0x6B, 0x01);
    
    hal.scheduler->delay(1);
    dev_icm->write_register(0x6A, 0x10);
    dev_icm->write_register(0x6B, 0x41);
    
    hal.scheduler->delay(1);
    dev_icm->write_register(0x6B, 0x01);
    
    hal.scheduler->delay(1);
    dev_icm->write_register(0x23, 0x00);
    dev_icm->write_register(0x6B, 0x41);
    
    dev_icm->read_registers(0x75, &whoami, 1);
    
    // wait for sensor to settle
    hal.scheduler->delay(100);
    
    dev_icm->read_registers(0x75, &whoami, 1);
    
    dev_icm->write_register(0x37, 0x00);
    dev_icm->write_register(0x6A, 0x10);
    return true;
}

bool AP_Baro_ICM20789::init()
{
    if (!dev) {
        return false;
    }

    debug("Looking for 20789 baro\n");
    
    if (!dev->get_semaphore()->take(0)) {
        AP_HAL::panic("PANIC: AP_Baro_ICM20789: failed to take serial semaphore for init");
    }

#ifdef HAL_INS_MPU60x0_NAME
    /*
      Pressure sensor data can be accessed in the following mode:
      Bypass Mode: Set register INT_PIN_CFG (Address: 55 (Decimal); 37 (Hex)) bit 1 to value 1 
      and I2C_MST_EN bit is 0 Address: 106 (Decimal); 6A (Hex)). 
      Pressure sensor data can then be accessed using the procedure described in Section 10.
    */
    debug("Setting up IMU\n");
    dev_icm = hal.spi->get_device(HAL_INS_MPU60x0_NAME);

    if (!dev_icm->get_semaphore()->take(0)) {
        AP_HAL::panic("PANIC: AP_Baro_ICM20789: failed to take serial semaphore ICM");        
    }

    if (!spi_init()) {
        debug("ICM20789: failed to initialise SPI device\n");
        goto failed;
    }

    dev_icm->get_semaphore()->give();
    dev_icm = nullptr;
#else // HAL_INS_MPU60x0_NAME

    /*
      assume the sensor is connected for both IMU and baro on I2C. We need to setup INT_PIN_CFG BYPASS_EN to 1
     */
    uint8_t old_address = dev->get_bus_address();
    dev->set_retries(4);
    
    dev->set_address(0x68);

    uint8_t whoami=0;
    dev->read_registers(0x75, &whoami, 1);
    debug("ICM20789: whoami 0x%02x old_address=%02x\n", whoami, old_address);

    dev->write_register(0x23, 0x00);
    dev->write_register(0x6B, 0x01);
    
    // wait for sensor to settle
    hal.scheduler->delay(10);

    dev->write_register(0x37, 0x02);

    dev->set_address(old_address);
#endif // HAL_INS_MPU60x0_NAME

    hal.scheduler->delay(100);
    
    if (!send_cmd16(CMD_SOFT_RESET)) {
        debug("ICM20789: reset failed\n");
        goto failed;
    }

    // wait for sensor to settle
    hal.scheduler->delay(10);
    
    if (!read_calibration_data()) {
        debug("ICM20789: read_calibration_data failed\n");
        goto failed;
    }
        
    // start a reading
    if (!send_cmd16(CMD_READ_PT)) {
        debug("ICM20789: start read failed\n");
        goto failed;
    }

    dev->set_retries(0);

    instance = _frontend.register_sensor();

    dev->get_semaphore()->give();

    debug("ICM20789: startup OK\n");
    
    // use 10ms to ensure we don't lose samples, with max lag of 10ms
    dev->register_periodic_callback(CONVERSION_INTERVAL/2, FUNCTOR_BIND_MEMBER(&AP_Baro_ICM20789::timer, void));
        
    return true;

 failed:
    if (dev_icm) {
        dev_icm->get_semaphore()->give();
    }
    dev->get_semaphore()->give();
    return false;
}

bool AP_Baro_ICM20789::send_cmd16(uint16_t cmd)
{
    uint8_t cmd_b[2] = { uint8_t(cmd >> 8), uint8_t(cmd & 0xFF) };
    return dev->transfer(cmd_b, 2, nullptr, 0);
}

bool AP_Baro_ICM20789::read_calibration_data(void)
{
    // setup for OTP read
    const uint8_t cmd[5] = { 0xC5, 0x95, 0x00, 0x66, 0x9C };
    if (!dev->transfer(cmd, sizeof(cmd), nullptr, 0)) {
        debug("ICM20789: read cal1 failed\n");
        return false;
    }
    for (uint8_t i=0; i<4; i++) {
        if (!send_cmd16(0xC7F7)) {
            debug("ICM20789: read cal2[%u] failed\n", i);
            return false;
        }
        uint8_t d[3];
        if (!dev->transfer(nullptr, 0, d, sizeof(d))) {
            debug("ICM20789: read cal3[%u] failed\n", i);
            return false;
        }
        sensor_constants[i] = int16_t((d[0]<<8) | d[1]);
        debug("sensor_constants[%u]=%d\n", i, sensor_constants[i]);
    }
    return true;
}

void AP_Baro_ICM20789::calculate_conversion_constants(const float p_Pa[3], const float p_LUT[3],
                                                      float &A, float &B, float &C)
{
    C = (p_LUT[0] * p_LUT[1] * (p_Pa[0] - p_Pa[1]) +
         p_LUT[1] * p_LUT[2] * (p_Pa[1] - p_Pa[2]) +
         p_LUT[2] * p_LUT[0] * (p_Pa[2] - p_Pa[0])) /
        (p_LUT[2] * (p_Pa[0] - p_Pa[1]) +
         p_LUT[0] * (p_Pa[1] - p_Pa[2]) +
         p_LUT[1] * (p_Pa[2] - p_Pa[0]));
    A = (p_Pa[0] * p_LUT[0] - p_Pa[1] * p_LUT[1] - (p_Pa[1] - p_Pa[0]) * C) / (p_LUT[0] - p_LUT[1]);
    B = (p_Pa[0] - A) * (p_LUT[0] + C);
}

/*
  Convert an output from a calibrated sensor to a pressure in Pa.
  Arguments:
  p_LSB -- Raw pressure data from sensor
  T_LSB -- Raw temperature data from sensor
*/
float AP_Baro_ICM20789::get_pressure(uint32_t p_LSB, uint32_t T_LSB)
{
    float t = T_LSB - 32768.0;
    float s[3];
    s[0] = LUT_lower + float(sensor_constants[0] * t * t) * quadr_factor;
    s[1] = offst_factor * sensor_constants[3] + float(sensor_constants[1] * t * t) * quadr_factor;
    s[2] = LUT_upper + float(sensor_constants[2] * t * t) * quadr_factor;
    float A, B, C;
    calculate_conversion_constants(p_Pa_calib, s, A, B, C);
    return A + B / (C + p_LSB);
}


static struct {
    uint32_t Praw, Traw;
    float T, P;
} dd;

void AP_Baro_ICM20789::convert_data(uint32_t Praw, uint32_t Traw)
{
    // temperature is easy
    float T = -45 + (175.0 / (1U<<16)) * Traw;

    // pressure involves a few more calculations
    float P = get_pressure(Praw, Traw);

    if (isinf(P) || isnan(P)) {
        // really bad data
        return;
    }
    
    if (_sem->take(0)) {
        dd.Praw = Praw;
        dd.Traw = Traw;
        dd.P = P;
        dd.T = T;
        
        accum.psum += P;
        accum.tsum += T;
        accum.count++;
        _sem->give();
    }
}

void AP_Baro_ICM20789::timer(void)
{
    uint8_t d[9] {};
    if (dev->transfer(nullptr, 0, d, sizeof(d))) {
        // ignore CRC bytes for now
        uint32_t Praw = (uint32_t(d[0]) << 16) | (uint32_t(d[1]) << 8) | d[3];
        uint32_t Traw = (uint32_t(d[6]) << 8) | d[7];

        convert_data(Praw, Traw);
        send_cmd16(CMD_READ_PT);
        last_measure_us = AP_HAL::micros();
    } else {
        if (AP_HAL::micros() - last_measure_us > CONVERSION_INTERVAL*3) {
            // lost a sample
            send_cmd16(CMD_READ_PT);
            last_measure_us = AP_HAL::micros();
        }
    }
}

void AP_Baro_ICM20789::update()
{
    if (_sem->take(HAL_SEMAPHORE_BLOCK_FOREVER)) {
        if (accum.count > 0) {
            _copy_to_frontend(instance, accum.psum/accum.count, accum.tsum/accum.count);
            accum.psum = accum.tsum = 0;
            accum.count = 0;
        }
        _sem->give();
#if 0
        // useful for debugging
        DataFlash_Class::instance()->Log_Write("ICMB", "TimeUS,Traw,Praw,P,T", "QIIff",
                                               AP_HAL::micros64(),
                                               dd.Traw, dd.Praw, dd.P, dd.T);
#endif
    }
}

