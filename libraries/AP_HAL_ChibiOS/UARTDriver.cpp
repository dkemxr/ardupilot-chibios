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
#include "UARTDriver.h"
#include "GPIO.h"
#include <usbcfg.h>
#include "shared_dma.h"

extern const AP_HAL::HAL& hal;

using namespace ChibiOS;

#if CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_PIXHAWK_CUBE || \
    CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_PIXHAWK1 || \
    CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_SKYVIPER_V2450
#define HAVE_USB_SERIAL
#endif

static ChibiUARTDriver::SerialDef _serial_tab[] = {
#if CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_PIXHAWK_CUBE || \
    CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_PIXHAWK1 || \
    CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_SKYVIPER_V2450
    {(BaseSequentialStream*) &SDU1, true, false, 0, 0, false, 0, 0},   //Serial 0, USB
    UART4_CONFIG, // Serial 1, GPS
    USART2_CONFIG, // Serial 2, telem1
    USART3_CONFIG, // Serial 3, telem2
    UART8_CONFIG, // Serial 4, GPS2
    //UART7_CONFIG, // Serial 5, debug console
#elif CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_CHIBIOS_SKYVIPER_F412
    USART1_CONFIG, // Serial 0, debug console
    USART6_CONFIG, // Serial 1, GPS
    USART2_CONFIG, // Serial 2, sonix
#endif
#if HAL_WITH_IO_MCU
    USART6_CONFIG, // IO MCU
#endif
};

// event used to wake up waiting thread
#define EVT_DATA EVENT_MASK(0)

ChibiUARTDriver::ChibiUARTDriver(uint8_t serial_num) :
tx_bounce_buf_ready(true),
_serial_num(serial_num),
_baudrate(57600),
_is_usb(false),
_in_timer(false),
_initialised(false)
{
    _serial = _serial_tab[serial_num].serial;
    _is_usb = _serial_tab[serial_num].is_usb;
    _dma_rx = _serial_tab[serial_num].dma_rx;
    _dma_tx = _serial_tab[serial_num].dma_tx;
    chMtxObjectInit(&_write_mutex);
}

void ChibiUARTDriver::begin(uint32_t b, uint16_t rxS, uint16_t txS)
{
    hal.gpio->pinMode(2, HAL_GPIO_OUTPUT);
    hal.gpio->pinMode(3, HAL_GPIO_OUTPUT);
    if (_serial == nullptr) {
        return;
    }
    bool was_initialised = _initialised;
    uint16_t min_tx_buffer = 4096;
    uint16_t min_rx_buffer = 1024;
    // on PX4 we have enough memory to have a larger transmit and
    // receive buffer for all ports. This means we don't get delays
    // while waiting to write GPS config packets
    if (txS < min_tx_buffer) {
        txS = min_tx_buffer;
    }
    if (rxS < min_rx_buffer) {
        rxS = min_rx_buffer;
    }

    /*
      allocate the read buffer
      we allocate buffers before we successfully open the device as we
      want to allocate in the early stages of boot, and cause minimum
      thrashing of the heap once we are up. The ttyACM0 driver may not
      connect for some time after boot
     */
    if (rxS != _readbuf.get_size()) {
        _initialised = false;
        while (_in_timer) {
            hal.scheduler->delay(1);
        }

        _readbuf.set_size(rxS);
    }

    if (b != 0) {
        _baudrate = b;
    }

    /*
      allocate the write buffer
     */
    if (txS != _writebuf.get_size()) {
        _initialised = false;
        while (_in_timer) {
            hal.scheduler->delay(1);
        }
        _writebuf.set_size(txS);
    }

    if (_is_usb) {
#ifdef HAVE_USB_SERIAL
        /*
         * Initializes a serial-over-USB CDC driver.
         */
        if (!was_initialised) {
            sduObjectInit((SerialUSBDriver*)_serial);
            sduStart((SerialUSBDriver*)_serial, &serusbcfg);
            /*
             * Activates the USB driver and then the USB bus pull-up on D+.
             * Note, a delay is inserted in order to not have to disconnect the cable
             * after a reset.
             */
            usbDisconnectBus(serusbcfg.usbp);
            hal.scheduler->delay_microseconds(1500);
            usbStart(serusbcfg.usbp, &usbcfg);
            usbConnectBus(serusbcfg.usbp);
        }
#endif
    } else {
        if (_baudrate != 0) {
            //setup Rx DMA
            if(!was_initialised) {
                if(_dma_rx) {
                    rxdma = STM32_DMA_STREAM(_serial_tab[_serial_num].dma_rx_stream_id);
                    bool dma_allocated = dmaStreamAllocate(rxdma,
                                               12,  //IRQ Priority
                                               (stm32_dmaisr_t)rxbuff_full_irq,
                                               (void *)this);
                    osalDbgAssert(!dma_allocated, "stream already allocated");
                    dmaStreamSetPeripheral(rxdma, &((SerialDriver*)_serial)->usart->DR);
                }
                if (_dma_tx) {
                    // we only allow for sharing of the TX DMA channel, not the RX
                    // DMA channel, as the RX side is active all the time, so
                    // cannot be shared
                    dma_handle = new Shared_DMA(_serial_tab[_serial_num].dma_tx_stream_id,
                                                SHARED_DMA_NONE,
                                                FUNCTOR_BIND_MEMBER(&ChibiUARTDriver::dma_tx_allocate, void),
                                                FUNCTOR_BIND_MEMBER(&ChibiUARTDriver::dma_tx_deallocate, void));
                }
            }
            sercfg.speed = _baudrate;
            if (!_dma_tx && !_dma_rx) {
                sercfg.cr1 = 0;
                sercfg.cr3 = 0;
            } else {
                if (_dma_rx) {
                    sercfg.cr1 = USART_CR1_IDLEIE;
                    sercfg.cr3 = USART_CR3_DMAR;
                }
                if (_dma_tx) {
                    sercfg.cr3 |= USART_CR3_DMAT;
                }
            }
            sercfg.cr2 = USART_CR2_STOP1_BITS;
            sercfg.irq_cb = rx_irq_cb;
            sercfg.ctx = (void*)this;
            
            sdStart((SerialDriver*)_serial, &sercfg);
            if(_dma_rx) {
                //Configure serial driver to skip handling RX packets
                //because we will handle them via DMA
                ((SerialDriver*)_serial)->usart->CR1 &= ~USART_CR1_RXNEIE;
                //Start DMA
                if(!was_initialised) {
                    uint32_t dmamode = STM32_DMA_CR_DMEIE | STM32_DMA_CR_TEIE;
                    dmamode |= STM32_DMA_CR_CHSEL(STM32_DMA_GETCHANNEL(_serial_tab[_serial_num].dma_rx_stream_id,
                                                                       _serial_tab[_serial_num].dma_rx_channel_id));
                    dmamode |= STM32_DMA_CR_PL(0);
                    dmaStreamSetMemory0(rxdma, rx_bounce_buf);
                    dmaStreamSetTransactionSize(rxdma, RX_BOUNCE_BUFSIZE);
                    dmaStreamSetMode(rxdma, dmamode    | STM32_DMA_CR_DIR_P2M |
                                         STM32_DMA_CR_MINC | STM32_DMA_CR_TCIE);
                    dmaStreamEnable(rxdma);
                }
            }
        }
    }

    if (_writebuf.get_size() && _readbuf.get_size()) {
        _initialised = true;
    }
    _uart_owner_thd = chThdGetSelfX();
}

void ChibiUARTDriver::dma_tx_allocate(void)
{
    osalDbgAssert(txdma == nullptr, "double DMA allocation");
    txdma = STM32_DMA_STREAM(_serial_tab[_serial_num].dma_tx_stream_id);
    bool dma_allocated = dmaStreamAllocate(txdma,
                                           12,  //IRQ Priority
                                           (stm32_dmaisr_t)tx_complete,
                                           (void *)this);
    osalDbgAssert(!dma_allocated, "stream already allocated");
    dmaStreamSetPeripheral(txdma, &((SerialDriver*)_serial)->usart->DR);
}

void ChibiUARTDriver::dma_tx_deallocate(void)
{
    chSysLock();
    dmaStreamRelease(txdma);
    txdma = nullptr;
    chSysUnlock();
}

void ChibiUARTDriver::tx_complete(void* self, uint32_t flags)
{
    ChibiUARTDriver* uart_drv = (ChibiUARTDriver*)self;
    if (uart_drv->_dma_tx) {
        uart_drv->dma_handle->unlock_from_IRQ();
    }
    uart_drv->tx_bounce_buf_ready = true;
}


void ChibiUARTDriver::rx_irq_cb(void* self)
{
    ChibiUARTDriver* uart_drv = (ChibiUARTDriver*)self;
    if (!uart_drv->_dma_rx) {
        return;
    }
    volatile uint16_t sr = ((SerialDriver*)(uart_drv->_serial))->usart->SR;
    if(sr & USART_SR_IDLE) {
        volatile uint16_t dr = ((SerialDriver*)(uart_drv->_serial))->usart->DR;
        (void)dr;
        //disable dma, triggering DMA transfer complete interrupt
        uart_drv->rxdma->stream->CR &= ~STM32_DMA_CR_EN;
    }
}

void ChibiUARTDriver::rxbuff_full_irq(void* self, uint32_t flags)
{
    ChibiUARTDriver* uart_drv = (ChibiUARTDriver*)self;
    if (uart_drv->_lock_rx_in_timer_tick) {
        return;
    }
    if (!uart_drv->_dma_rx) {
        return;
    }
    uint8_t len = RX_BOUNCE_BUFSIZE - uart_drv->rxdma->stream->NDTR;
    if (len == 0) {
        return;
    }
    uart_drv->_readbuf.write(uart_drv->rx_bounce_buf, len);
    //restart the DMA transfers
    dmaStreamSetMemory0(uart_drv->rxdma, uart_drv->rx_bounce_buf);
    dmaStreamSetTransactionSize(uart_drv->rxdma, RX_BOUNCE_BUFSIZE);
    dmaStreamEnable(uart_drv->rxdma);
    if (uart_drv->_wait.thread_ctx && uart_drv->_readbuf.available() >= uart_drv->_wait.n) {
        chSysLockFromISR();
        chEvtSignalI(uart_drv->_wait.thread_ctx, EVT_DATA);                    
        chSysUnlockFromISR();
    }
}

void ChibiUARTDriver::begin(uint32_t b)
{
    begin(b, 0, 0);
}

void ChibiUARTDriver::end()
{
    _initialised = false;
    while (_in_timer) hal.scheduler->delay(1);

    if (_is_usb) {
#ifdef HAVE_USB_SERIAL

        sduStop((SerialUSBDriver*)_serial);
#endif
    } else {
        sdStop((SerialDriver*)_serial);
    }
    _readbuf.set_size(0);
    _writebuf.set_size(0);
}

void ChibiUARTDriver::flush()
{
    if (_is_usb) {
#ifdef HAVE_USB_SERIAL

        sduSOFHookI((SerialUSBDriver*)_serial);
#endif
    } else {
        //TODO: Handle this for other serial ports
    }
}

bool ChibiUARTDriver::is_initialized()
{
    return _initialised;
}

void ChibiUARTDriver::set_blocking_writes(bool blocking)
{
    _nonblocking_writes = !blocking;
}

bool ChibiUARTDriver::tx_pending() { return false; }

/* Empty implementations of Stream virtual methods */
uint32_t ChibiUARTDriver::available() {
    if (!_initialised) {
        return 0;
    }
    if (_is_usb) {
#ifdef HAVE_USB_SERIAL

        if (((SerialUSBDriver*)_serial)->config->usbp->state != USB_ACTIVE) {
            return 0;
        }
#endif
    }
    return _readbuf.available();
}

uint32_t ChibiUARTDriver::txspace()
{
    if (!_initialised) {
        return 0;
    }
    return _writebuf.space();
}

int16_t ChibiUARTDriver::read()
{
    if (_uart_owner_thd != chThdGetSelfX()){
        return -1;
    }
    if (!_initialised) {
        return -1;
    }

    uint8_t byte;
    if (!_readbuf.read_byte(&byte)) {
        return -1;
    }

    return byte;
}

/* Empty implementations of Print virtual methods */
size_t ChibiUARTDriver::write(uint8_t c)
{
    if (!chMtxTryLock(&_write_mutex)) {
        return -1;
    }
    
    if (!_initialised) {
        chMtxUnlock(&_write_mutex);
        return 0;
    }

    while (_writebuf.space() == 0) {
        if (_nonblocking_writes) {
            chMtxUnlock(&_write_mutex);
            return 0;
        }
        hal.scheduler->delay(1);
    }
    size_t ret = _writebuf.write(&c, 1);
    chMtxUnlock(&_write_mutex);
    return ret;
}

size_t ChibiUARTDriver::write(const uint8_t *buffer, size_t size)
{
    if (!_initialised) {
		return 0;
	}

    if (!chMtxTryLock(&_write_mutex)) {
        return -1;
    }

    if (!_nonblocking_writes) {
        /*
          use the per-byte delay loop in write() above for blocking writes
         */
        chMtxUnlock(&_write_mutex);
        size_t ret = 0;
        while (size--) {
            if (write(*buffer++) != 1) break;
            ret++;
        }
        return ret;
    }

    size_t ret = _writebuf.write(buffer, size);
    chMtxUnlock(&_write_mutex);
    return ret;
}

/*
  wait for data to arrive, or a timeout. Return true if data has
  arrived, false on timeout
 */
bool ChibiUARTDriver::wait_timeout(uint16_t n, uint32_t timeout_ms)
{
    chEvtGetAndClearEvents(EVT_DATA);
    if (available() >= n) {
        return true;
    }
    _wait.n = n;
    _wait.thread_ctx = chThdGetSelfX();
    eventmask_t mask = chEvtWaitAnyTimeout(EVT_DATA, MS2ST(timeout_ms));
    return (mask & EVT_DATA) != 0;
}

/*
  push any pending bytes to/from the serial port. This is called at
  1kHz in the timer thread. Doing it this way reduces the system call
  overhead in the main task enormously.
 */
void ChibiUARTDriver::_timer_tick(void)
{
    int ret;
    uint32_t n;

    if (!_initialised) return;

    if (_dma_rx && rxdma) {
        _lock_rx_in_timer_tick = true;
        //Check if DMA is enabled
        //if not, it might be because the DMA interrupt was silenced
        //let's handle that here so that we can continue receiving
        if (!(rxdma->stream->CR & STM32_DMA_CR_EN)) {
            uint8_t len = RX_BOUNCE_BUFSIZE - rxdma->stream->NDTR;
            if (len != 0) {
                _readbuf.write(rx_bounce_buf, len);
                if (_wait.thread_ctx && _readbuf.available() >= _wait.n) {
                    chEvtSignal(_wait.thread_ctx, EVT_DATA);                    
                }
            }
            //DMA disabled by idle interrupt never got a chance to be handled
            //we will enable it here
            dmaStreamSetMemory0(rxdma, rx_bounce_buf);
            dmaStreamSetTransactionSize(rxdma, RX_BOUNCE_BUFSIZE);
            dmaStreamEnable(rxdma);
        }
        _lock_rx_in_timer_tick = false;
    }

    // don't try IO on a disconnected USB port
    if (_is_usb) {
#ifdef HAVE_USB_SERIAL
        if (((SerialUSBDriver*)_serial)->config->usbp->state != USB_ACTIVE) {
            return;
        }
#endif
    }
    if(_is_usb) {
#ifdef HAVE_USB_SERIAL
        ((ChibiGPIO *)hal.gpio)->set_usb_connected();
#endif
    }
    _in_timer = true;

    {
        // try to fill the read buffer
        ByteBuffer::IoVec vec[2];

        const auto n_vec = _readbuf.reserve(vec, _readbuf.space());
        for (int i = 0; i < n_vec; i++) {
            //Do a non-blocking read
            if (_is_usb) {
                ret = 0;
    #ifdef HAVE_USB_SERIAL
                ret = chnReadTimeout((SerialUSBDriver*)_serial, vec[i].data, vec[i].len, TIME_IMMEDIATE);
    #endif
            } else if(!_dma_rx){
                ret = 0;
                ret = chnReadTimeout((SerialDriver*)_serial, vec[i].data, vec[i].len, TIME_IMMEDIATE);
            } else {
                ret = 0;
            }
            if (ret < 0) {
                break;
            }
            _readbuf.commit((unsigned)ret);

            /* stop reading as we read less than we asked for */
            if ((unsigned)ret < vec[i].len) {
                break;
            }
        }
    }

    // write any pending bytes
    n = _writebuf.available();
    if (n > 0) {
        if(!_dma_tx) {
            ByteBuffer::IoVec vec[2];
            const auto n_vec = _writebuf.peekiovec(vec, n);
            for (int i = 0; i < n_vec; i++) {
                if (_is_usb) {
                    ret = 0;
    #ifdef HAVE_USB_SERIAL
                    ret = chnWriteTimeout((SerialUSBDriver*)_serial, vec[i].data, vec[i].len, TIME_IMMEDIATE);
    #endif
                } else {
                    ret = chnWriteTimeout((SerialDriver*)_serial, vec[i].data, vec[i].len, TIME_IMMEDIATE);
                }
                if (ret < 0) {
                    break;
                }
                _writebuf.advance(ret);

                /* We wrote less than we asked for, stop */
                if ((unsigned)ret != vec[i].len) {
                    break;
                }
            }
        } else {
            if(tx_bounce_buf_ready) {
                /* TX DMA channel preparation.*/
                _writebuf.advance(tx_len);
                tx_len = _writebuf.peekbytes(tx_bounce_buf, TX_BOUNCE_BUFSIZE);
                if (tx_len == 0) {
                    goto end;
                }
                dma_handle->lock();
                tx_bounce_buf_ready = false;
                osalDbgAssert(txdma != nullptr, "UART TX DMA allocation failed");
                dmaStreamSetMemory0(txdma, tx_bounce_buf);
                dmaStreamSetTransactionSize(txdma, tx_len);
                uint32_t dmamode = STM32_DMA_CR_DMEIE | STM32_DMA_CR_TEIE;
                dmamode |= STM32_DMA_CR_CHSEL(STM32_DMA_GETCHANNEL(_serial_tab[_serial_num].dma_tx_stream_id,
                                                                   _serial_tab[_serial_num].dma_tx_channel_id));
                dmamode |= STM32_DMA_CR_PL(0);
                dmaStreamSetMode(txdma, dmamode | STM32_DMA_CR_DIR_M2P |
                                 STM32_DMA_CR_MINC | STM32_DMA_CR_TCIE);
                dmaStreamEnable(txdma);
            } else if (_dma_tx && txdma) {
                if (!(txdma->stream->CR & STM32_DMA_CR_EN)) {
                    if (txdma->stream->NDTR == 0) {
                        tx_bounce_buf_ready = true;
                        dma_handle->unlock();
                    }
                }
            }
        }
    }
end:
    _in_timer = false;
}
#endif //CONFIG_HAL_BOARD == HAL_BOARD_CHIBIOS
