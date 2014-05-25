/*
 * memory.h
 * Copyright (C) 2014 xent
 * Project is distributed under the terms of the GNU General Public License v3.0
 */

#ifndef MEMORY_H_
#define MEMORY_H_
/*----------------------------------------------------------------------------*/
#include <stdbool.h>
#include <stdint.h>
/*----------------------------------------------------------------------------*/
static inline uint64_t toBigEndian64(uint64_t value)
{
  return __builtin_bswap64(value);
}
/*----------------------------------------------------------------------------*/
static inline uint32_t toBigEndian32(uint32_t value)
{
  return __builtin_bswap32(value);
}
/*----------------------------------------------------------------------------*/
static inline uint16_t toBigEndian16(uint16_t value)
{
  return value >> 8 | value << 8;
}
/*----------------------------------------------------------------------------*/
static inline uint64_t toLittleEndian64(uint64_t value)
{
  return value;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t toLittleEndian32(uint32_t value)
{
  return value;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t toLittleEndian16(uint16_t value)
{
  return value;
}
/*----------------------------------------------------------------------------*/
static inline uint64_t fromBigEndian64(uint64_t value)
{
  return toBigEndian64(value);
}
/*----------------------------------------------------------------------------*/
static inline uint32_t fromBigEndian32(uint32_t value)
{
  return toBigEndian32(value);
}
/*----------------------------------------------------------------------------*/
static inline uint16_t fromBigEndian16(uint16_t value)
{
  return toBigEndian16(value);
}
/*----------------------------------------------------------------------------*/
static inline uint64_t fromLittleEndian64(uint64_t value)
{
  return value;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t fromLittleEndian32(uint32_t value)
{
  return value;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t fromLittleEndian16(uint16_t value)
{
  return value;
}
/*----------------------------------------------------------------------------*/
static inline bool compareExchangePointer(void **pointer, void *expected,
    void *desired)
{
  return __sync_bool_compare_and_swap(pointer, expected, desired);
}
/*----------------------------------------------------------------------------*/
#endif /* MEMORY_H_ */
