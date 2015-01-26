/*
 * timestamps.hpp
 * Copyright (C) 2014 xent
 * Project is distributed under the terms of the GNU General Public License v3.0
 */

#ifndef TIMESTAMPS_HPP_
#define TIMESTAMPS_HPP_
//------------------------------------------------------------------------------
#include "shell.hpp"
//------------------------------------------------------------------------------
extern "C"
{
#include <rtc.h>
}
//------------------------------------------------------------------------------
class TimeProvider
{
public:
  virtual ~TimeProvider()
  {
  }

  virtual uint64_t microtime() = 0;
  virtual Rtc *rtc() = 0;
};
//------------------------------------------------------------------------------
template<class T> class CurrentDate : public Shell::ShellCommand
{
public:
  CurrentDate(Shell &parent) :
      ShellCommand(parent)
  {
  }

  virtual const char *name() const
  {
    return "date";
  }

  virtual result run(unsigned int, const char * const *, Shell::ShellContext *)
  {
    RtcTime currentTime;

    rtcMakeTime(&currentTime, rtcTime(T::instance()->rtc()));
    owner.log("%02u:%02u:%02u %02u.%02u.%04u", currentTime.hour,
        currentTime.minute, currentTime.second, currentTime.day,
        currentTime.month, currentTime.year);
    return E_OK;
  }
};
//------------------------------------------------------------------------------
template<class T> class MeasureTime : public Shell::ShellCommand
{
public:
  MeasureTime(Shell &parent) :
      ShellCommand(parent)
  {
  }

  virtual const char *name() const
  {
    return "time";
  }

  virtual result run(unsigned int count, const char * const *arguments,
      Shell::ShellContext *context)
  {
    uint64_t start, delta;
    result res = E_VALUE;

    for (auto entry : owner.commands())
    {
      if (!strcmp(entry->name(), arguments[0]))
      {
        start = T::instance()->microtime();
        res = entry->run(count - 1, arguments + 1, context);
        delta = T::instance()->microtime() - start;

        owner.log("Time passed: %llu us",
            static_cast<long long unsigned int>(delta));
      }
    }
    return res;
  }
};
//------------------------------------------------------------------------------
#endif //TIMESTAMPS_HPP_
