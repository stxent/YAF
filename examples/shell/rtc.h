#ifndef _RTC_H_
#define _RTC_H_
//---------------------------------------------------------------------------
#include <stdint.h>
//---------------------------------------------------------------------------
uint16_t rtcGetTime();
uint16_t rtcGetDate();
//---------------------------------------------------------------------------
void timeToStr(char *, uint16_t);
void dateToStr(char *, uint16_t);
//---------------------------------------------------------------------------
#endif
