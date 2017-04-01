/**
 * The MySensors Arduino library handles the wireless radio link and protocol
 * between your home built sensors/actuators and HA controller of choice.
 * The sensors forms a self healing radio network with optional repeaters. Each
 * repeater and gateway builds a routing tables in EEPROM which keeps track of the
 * network topology allowing messages to be routed to nodes.
 *
 * Created by Henrik Ekblad <henrik.ekblad@mysensors.org>
 * Copyright (C) 2013-2017 Sensnology AB
 * Full contributor list: https://github.com/mysensors/Arduino/graphs/contributors
 *
 * Documentation: http://www.mysensors.org
 * Support Forum: http://forum.mysensors.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include "MyHwAVR.h"

bool hwInit(void)
{
#if !defined(MY_DISABLED_SERIAL)
	MY_SERIALDEVICE.begin(MY_BAUD_RATE);
#endif
	return true;
}


#define INVALID_INTERRUPT_NUM	(0xFFu)

volatile uint8_t _wokeUpByInterrupt =
    INVALID_INTERRUPT_NUM;    // Interrupt number that woke the mcu.
volatile uint8_t _wakeUp1Interrupt  =
    INVALID_INTERRUPT_NUM;    // Interrupt number for wakeUp1-callback.
volatile uint8_t _wakeUp2Interrupt  =
    INVALID_INTERRUPT_NUM;    // Interrupt number for wakeUp2-callback.

void wakeUp1()
{
    // Disable sleep. When an interrupt occurs after attachInterrupt,
    // but before sleeping the CPU would not wake up.
    // Ref: http://playground.arduino.cc/Learning/ArduinoSleepCode
    sleep_disable();
	detachInterrupt(_wakeUp1Interrupt);
    // First interrupt occurred will be reported only
    if (INVALID_INTERRUPT_NUM == _wokeUpByInterrupt)
    {
        _wokeUpByInterrupt = _wakeUp1Interrupt;
    }
}
void wakeUp2()
{
    sleep_disable();
	detachInterrupt(_wakeUp2Interrupt);
    // First interrupt occurred will be reported only
    if (INVALID_INTERRUPT_NUM == _wokeUpByInterrupt)
    {
        _wokeUpByInterrupt = _wakeUp2Interrupt;
    }
}

inline bool interruptWakeUp()
{
	return _wokeUpByInterrupt != INVALID_INTERRUPT_NUM;
}

// Watchdog Timer interrupt service routine. This routine is required
// to allow automatic WDIF and WDIE bit clearance in hardware.
ISR (WDT_vect)
{
}

void hwPowerDown(period_t period)
{
	// disable ADC for power saving
	ADCSRA &= ~(1 << ADEN);
	// save WDT settings
	uint8_t WDTsave = WDTCSR;
	if (period != SLEEP_FOREVER) {
		wdt_enable(period);
		// enable WDT interrupt before system reset
		WDTCSR |= (1 << WDCE) | (1 << WDIE);
	} else {
		// if sleeping forever, disable WDT
		wdt_disable();
	}
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	cli();
	sleep_enable();
#if defined __AVR_ATmega328P__
	sleep_bod_disable();
#endif
	// Enable interrupts & sleep until WDT or ext. interrupt
	sei();
	// Directly sleep CPU, to prevent race conditions!
    // Ref: chapter 7.7 of ATMega328P datasheet
	sleep_cpu();
	sleep_disable();
	// restore previous WDT settings
	cli();
	wdt_reset();
	// enable WDT changes
	WDTCSR |= (1 << WDCE) | (1 << WDE);
	// restore saved WDT settings
	WDTCSR = WDTsave;
	sei();
	// enable ADC
	ADCSRA |= (1 << ADEN);
}

void hwInternalSleep(unsigned long ms)
{
	// Let serial prints finish (debug, log etc)
#ifndef MY_DISABLED_SERIAL
	MY_SERIALDEVICE.flush();
#endif

    static const struct {
        uint16_t ms;
        period_t period;
    } wdtTimes[] = {
        { 8000u, SLEEP_8S    },
        { 4000u, SLEEP_4S    },
        { 2000u, SLEEP_2S    },
        { 1000u, SLEEP_1S    },
        {  500u, SLEEP_500MS },
        {  250u, SLEEP_250MS },
        {  120u, SLEEP_120MS },
        {   60u, SLEEP_60MS  },
        {   30u, SLEEP_30MS  },
        {   15u, SLEEP_15MS  },
    };

    for (size_t i = 0; i < sizeof(wdtTimes)/sizeof(wdtTimes[0]); ++i)
    {
        while (ms >= wdtTimes[i].ms && !interruptWakeUp() )
        {
            hwPowerDown(wdtTimes[i].period);
            ms -= wdtTimes[i].ms;
        }
    }
}

int8_t hwSleep(unsigned long ms)
{
	hwInternalSleep(ms);
	return MY_WAKE_UP_BY_TIMER;
}

int8_t hwSleep(uint8_t interrupt, uint8_t mode, unsigned long ms)
{
	return hwSleep(interrupt,mode,INVALID_INTERRUPT_NUM,0u,ms);
}

int8_t hwSleep(uint8_t interrupt1, uint8_t mode1, uint8_t interrupt2, uint8_t mode2,
               unsigned long ms)
{
    // ATMega328P supports following modes to wake from sleep: LOW, CHANGE, RISING, FALLING
    // Datasheet states only LOW can be used with INT0/1 to wake from sleep, which is incorrect.
    // Ref: http://gammon.com.au/interrupts

	// Disable interrupts until going to sleep, otherwise interrupts occurring between attachInterrupt()
	// and sleep might cause the ATMega to not wakeup from sleep as interrupt has already be handled!
	cli();
	// attach interrupts
	_wakeUp1Interrupt  = interrupt1;
	_wakeUp2Interrupt  = interrupt2;

    // Attach external interrupt handlers, and clear any pending interrupt flag
    // to prevent waking immediately again.
    // Ref: https://forum.arduino.cc/index.php?topic=59217.0
    if (interrupt1 != INVALID_INTERRUPT_NUM) {
        EIFR = _BV(INTF0);
		attachInterrupt(interrupt1, wakeUp1, mode1);
	}
	if (interrupt2 != INVALID_INTERRUPT_NUM) {
        EIFR = _BV(INTF1);
		attachInterrupt(interrupt2, wakeUp2, mode2);
	}

	if (ms>0) {
		// sleep for defined time
		hwInternalSleep(ms);
	} else {
		// sleep until ext interrupt triggered
		hwPowerDown(SLEEP_FOREVER);
	}

	// Assure any interrupts attached, will get detached when they did not occur.
	if (interrupt1 != INVALID_INTERRUPT_NUM) {
		detachInterrupt(interrupt1);
	}
	if (interrupt2 != INVALID_INTERRUPT_NUM) {
		detachInterrupt(interrupt2);
	}

	// Return what woke the mcu.
    // Default: no interrupt triggered, timer wake up
	int8_t ret = MY_WAKE_UP_BY_TIMER;
	if (interruptWakeUp()) {
		ret = static_cast<int8_t>(_wokeUpByInterrupt);
	}
	// Clear woke-up-by-interrupt flag, so next sleeps won't return immediately.
	_wokeUpByInterrupt = INVALID_INTERRUPT_NUM;

	return ret;
}

bool hwUniqueID(unique_id_t* uniqueID)
{
	// not implemented yet
	(void)uniqueID;
	return false;
}

uint16_t hwCPUVoltage()
{
	// Measure Vcc against 1.1V Vref
#if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
	ADMUX = (_BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1));
#elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
	ADMUX = (_BV(MUX5) | _BV(MUX0));
#elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
	ADMUX = (_BV(MUX3) | _BV(MUX2));
#else
	ADMUX = (_BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1));
#endif
	// Vref settle
	delay(70);
	// Do conversion
	ADCSRA |= _BV(ADSC);
	while (bit_is_set(ADCSRA,ADSC)) {};
	// return Vcc in mV
	return (1125300UL) / ADC;
}

uint16_t hwCPUFrequency()
{
	cli();
	// setup timer1
	TIFR1 = 0xFF;
	TCNT1 = 0;
	TCCR1A = 0;
	TCCR1C = 0;
	// save WDT settings
	uint8_t WDTsave = WDTCSR;
	wdt_enable(WDTO_500MS);
	// enable WDT interrupt mode => first timeout WDIF, 2nd timeout reset
	WDTCSR |= (1 << WDIE);
	wdt_reset();
	// start timer1 with 1024 prescaling
	TCCR1B = _BV(CS12) | _BV(CS10);
	// wait until wdt interrupt
	while (bit_is_clear(WDTCSR,WDIF)) {};
	// stop timer
	TCCR1B = 0;
	// restore WDT settings
	wdt_reset();
	WDTCSR |= (1 << WDCE) | (1 << WDE);
	WDTCSR = WDTsave;
	sei();
	// return frequency in 1/10MHz (accuracy +- 10%)
	return TCNT1 * 2048UL / 100000UL;
}

uint16_t hwFreeMem()
{
	extern int __heap_start, *__brkval;
	int v;
	return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

void hwDebugPrint(const char *fmt, ... )
{
	char fmtBuffer[MY_SERIAL_OUTPUT_SIZE];
#ifdef MY_GATEWAY_FEATURE
	// prepend debug message to be handled correctly by controller (C_INTERNAL, I_LOG_MESSAGE)
	snprintf_P(fmtBuffer, sizeof(fmtBuffer), PSTR("0;255;%d;0;%d;%lu "), C_INTERNAL, I_LOG_MESSAGE,
	           hwMillis());
	MY_SERIALDEVICE.print(fmtBuffer);
#else
	// prepend timestamp
	MY_SERIALDEVICE.print(hwMillis());
	MY_SERIALDEVICE.print(F(" "));
#endif
	va_list args;
	va_start (args, fmt );
#ifdef MY_GATEWAY_FEATURE
	// Truncate message if this is gateway node
	vsnprintf_P(fmtBuffer, sizeof(fmtBuffer), fmt, args);
	fmtBuffer[sizeof(fmtBuffer) - 2] = '\n';
	fmtBuffer[sizeof(fmtBuffer) - 1] = '\0';
#else
	vsnprintf_P(fmtBuffer, sizeof(fmtBuffer), fmt, args);
#endif
	va_end (args);
	MY_SERIALDEVICE.print(fmtBuffer);
	MY_SERIALDEVICE.flush();
}
