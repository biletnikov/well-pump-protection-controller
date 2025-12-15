/*
 * Контроллер защиты насоса скважины для защиты от протечек.
 * Контроллер читает сигнал из дачтиков утечки воды и отключает питание насоса скважины
 * уходя в аварийный режим, который требует сброса контроллера нажажием кнопки. 
 * Контроллер управляет индикацией на светодиодах и изадёт тревожный сигнал с помощью зумера.
 * Fuses : .\avrdude.exe -p t2313 -c usbasp -U lfuse:w:0xe2:m -U hfuse:w:0xdb:m -U efuse:w:0xff:m
 * Firmware: .\avrdude.exe -p t2313 -c usbasp -U flash:w:WPPController.hex
 * Created: 09.12.2025 21:53:09
 * Author : Sergei Biletnikov
 */ 

// Рабочая частота 4 Мгц используя встроенный RC резонатор
#define F_CPU 4000000UL

#include <avr/io.h>
#include <util/delay.h>
#include <stdio.h>
#include <stdlib.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/sleep.h>

// порт и выходы управлениям светодиодами
#define LED_DDR DDRB
#define LED_PORT PORTB
#define WLS_ALARM_LED_1_PIN PINB1
#define WLS_ALARM_LED_2_PIN PINB2
#define STATUS_LED_PIN PINB0
// индикация для двух сенсоров
#define WLS_ALARM_LED_1_ON (LED_PORT|=(1<<WLS_ALARM_LED_1_PIN))
#define WLS_ALARM_LED_2_ON (LED_PORT|=(1<<WLS_ALARM_LED_2_PIN))
#define WLS_ALARM_LED_1_OFF (LED_PORT&=~(1<<WLS_ALARM_LED_1_PIN))
#define WLS_ALARM_LED_2_OFF (LED_PORT&=~(1<<WLS_ALARM_LED_2_PIN))
#define STATUS_LED_BLINK (LED_PORT^=(1<<STATUS_LED_PIN))
#define STATUS_LED_BLINK_THRESHOLD 5
static uint8_t status_led_counter;

// порт и выводы управления зумером (пищалкой)
#define BUZZER_DDR DDRB
#define BUZZER_PORT PORTB
#define BUZZER_PIN PINB7
#define BUZZER_BEEP_ON (BUZZER_PORT|=(1<<BUZZER_PIN))
#define BUZZER_BEEP_OFF (BUZZER_PORT&=~(1<<BUZZER_PIN))
#define BUZZER_TOGGLE (BUZZER_PORT^=(1<<BUZZER_PIN))
#define BUZZER_BEEP_PERIOD_THRESHOLD 10
static uint8_t buzzer_beep_counter;

// порт и вывод управления реле скважиной (высокий уровень приводит к включению симмистра, который разрывает контакты переходного реле и отключает скважину)
#define PUMP_RELAY_DDR DDRD
#define PUMP_RELAY_PORT PORTD
#define PUMP_RELAY_PIN PIND6
#define PUMP_RELAY_OFF (PUMP_RELAY_PORT|=(1<<PUMP_RELAY_PIN))
#define PUMP_RELAY_ON (PUMP_RELAY_PORT&=~(1<<PUMP_RELAY_PIN))

// порт и входы для чтения сигнала датчиков
#define WLS_INPUT_DDR DDRD
#define WLS_INPUT_PORT PIND
#define WLS_INPUT_1_PIN PIND2
#define WLS_INPUT_2_PIN PIND3

// счетчики для борьбы с помехами со стороны сенсоров
#define ALARM_THRESHOLD 10
static uint8_t wls_alarm_counter_1;
static uint8_t wls_alarm_counter_2;
// проверка сигнала с сенсоров
#define CHECK_ALARM_PIN_WLS_1 (WLS_INPUT_PORT & (1<<WLS_INPUT_1_PIN))
#define CHECK_ALARM_PIN_WLS_2 (WLS_INPUT_PORT & (1<<WLS_INPUT_2_PIN))



// возвращает не ноль, если первый сенсор в режиме аварии
uint8_t wls_alarm_1() {
	return wls_alarm_counter_1 >= ALARM_THRESHOLD;
}
// возвращает не ноль, если второй сенсор в режиме аварии
uint8_t wls_alarm_2() {
	return wls_alarm_counter_2 >= ALARM_THRESHOLD;
}

// чтение сигналов с сенсоров, вернёт не ноль, если один или два сенсора в режиме аварии
uint8_t read_sensors()
{
	if (!wls_alarm_1())
	{
		// читаем сигнал от сенсора 1 с защитой от помех
		if (CHECK_ALARM_PIN_WLS_1)
		{
			if (wls_alarm_counter_1 < ALARM_THRESHOLD)
			{
				wls_alarm_counter_1++;
			}
		} else
		{
			if (wls_alarm_counter_1 > 0) {
				wls_alarm_counter_1--;
			}
		}
	}
	if (!wls_alarm_2())
	{
		// читаем сигнал от сенсора 2 с защитой от помех
		if (CHECK_ALARM_PIN_WLS_2)
		{
			if (wls_alarm_counter_2 < ALARM_THRESHOLD)
			{
				wls_alarm_counter_2++;
			}
		} else
		{
			if (wls_alarm_counter_2 > 0) {
				wls_alarm_counter_2--;
			}
		}
	}
	return wls_alarm_1() || wls_alarm_2();
}

// периодичный сигнал
void beep() {
	if (buzzer_beep_counter >= BUZZER_BEEP_PERIOD_THRESHOLD)
	{
		buzzer_beep_counter = 0;
		BUZZER_TOGGLE;
	} else
	{
		buzzer_beep_counter++;
	}
}

// мигание статусным светодиодом
void blink_status_led()
{
	if (status_led_counter >= STATUS_LED_BLINK_THRESHOLD)
	{
		status_led_counter = 0;
		STATUS_LED_BLINK;
	}
	else
	{
		status_led_counter++;
	}	
}


void init()
{
	cli(); // отключем прерывания
	
	wdt_disable();
	wdt_enable(WDTO_1S); // инициализация сторожевого таймера на 2 секунды
		
	// устанавливаем выводы управления светодиодами как выход
	LED_DDR|= (1<<WLS_ALARM_LED_1_PIN);
	LED_DDR|= (1<<WLS_ALARM_LED_2_PIN);
	LED_DDR|= (1<<STATUS_LED_PIN);
	// установка ножку управления зумером как выход
	BUZZER_DDR|= (1<<BUZZER_PIN);
	// устанавливаем ножки на вход
	WLS_INPUT_DDR&=~(1<<WLS_INPUT_1_PIN);
	WLS_INPUT_DDR&=~(1<<WLS_INPUT_2_PIN);
	// управление реле скважиной, устанавливаем вывод как выход
	PUMP_RELAY_DDR|= (1<<PUMP_RELAY_PIN);
	// выставляем ножки в исходное состояние
	WLS_ALARM_LED_1_OFF;
	WLS_ALARM_LED_2_OFF;
	BUZZER_BEEP_OFF;
	PUMP_RELAY_ON;
	
	// выключаем компаратор
	ACSR|= (1<<ACD);
	
	wls_alarm_counter_1 = 0;
	wls_alarm_counter_2 = 0;
	buzzer_beep_counter = 0;
	status_led_counter = 0;
	
	// сигнал запуска (сброса)	
	for (uint8_t i=0; i < 3; i++)
	{
		BUZZER_BEEP_ON;
		_delay_ms(100);
		BUZZER_BEEP_OFF;
		_delay_ms(100);
	}
}

int main(void)
{	
    init(); // инициализация контроллера
	
    while (1) // вечный цикл программы
    {
		wdt_reset(); // сбрасывает сторожевой таймер
		
		uint8_t ALARM = read_sensors(); // чтение сенсоров и определение состояния Аварии
		if (ALARM)
		{
			// контроллер находится в режиме аварии, отключить насос
			PUMP_RELAY_OFF;
			// зажечь светодиоды датчиков, согласно показанию сенсора
			if (wls_alarm_1()) WLS_ALARM_LED_1_ON;				
			if (wls_alarm_2()) WLS_ALARM_LED_2_ON;

			// воспроизвести сигнал
			beep();
		} 
		// мигание статусным светодиодом раз в секунду
		blink_status_led();
		
		_delay_ms(100); // задержка 100 мс перед новой итерацией		
    }
}

