/*
 * mmi.c
 * Copyright (C) 2012 xent
 * Project is distributed under the terms of the GNU General Public License v3.0
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
/*----------------------------------------------------------------------------*/
#ifdef DEBUG
#include <stdio.h>
/* #define DEBUG_RW */
#endif
/*----------------------------------------------------------------------------*/
#define MMI_SECTOR_POW 9
/*----------------------------------------------------------------------------*/
#include "mmi.h"
#include "mutex.h"
/*----------------------------------------------------------------------------*/
static enum result mmiInit(void *, const void *);
static void mmiDeinit(void *);
static uint32_t mmiRead(void *, uint8_t *, uint32_t);
static uint32_t mmiWrite(void *, const uint8_t *, uint32_t);
static enum result mmiGetOpt(void *, enum ifOption, void *);
static enum result mmiSetOpt(void *, enum ifOption, const void *);
/*----------------------------------------------------------------------------*/
struct Mmi
{
  struct Interface parent;

  Mutex lock;
  uint64_t position, offset, size;

  uint8_t *data;
  int file;
  struct stat info;

#ifdef DEBUG
  uint64_t readCount, writeCount, readSize, writeSize;
#endif
};
/*----------------------------------------------------------------------------*/
static const struct InterfaceClass mmiTable = {
    .size = sizeof(struct Mmi),
    .init = mmiInit,
    .deinit = mmiDeinit,

    .read = mmiRead,
    .write = mmiWrite,
    .getopt = mmiGetOpt,
    .setopt = mmiSetOpt
};
/*----------------------------------------------------------------------------*/
const struct InterfaceClass *Mmi = &mmiTable;
/*----------------------------------------------------------------------------*/
#ifdef DEBUG
void mmiGetStat(void *object, uint64_t *results)
{
  struct Mmi *dev = object;

  results[0] = dev->readCount;
  results[1] = dev->readSize;
  results[2] = dev->writeCount;
  results[3] = dev->writeSize;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef DEBUG
void getSizeStr(uint64_t size, char *str)
{
  const unsigned short suffixCount = 4;
  const char *suffix[] = {"KiB", "MiB", "GiB", "TiB"};
  unsigned short selectedSuffix = 0;
  double remainder;

  remainder = (double)size / 1024.0;
  while (remainder >= 1024.0)
  {
    selectedSuffix++;
    remainder /= 1024.0;
    if (selectedSuffix == suffixCount - 1)
      break;
  }
  sprintf(str, "%.2f %s", remainder, suffix[selectedSuffix]);
}
#endif
/*----------------------------------------------------------------------------*/
static enum result mmiInit(void *object, const void *configPtr)
{
  const char *path = configPtr;
  struct Mmi *dev = object;
#ifdef DEBUG
  char size2str[16];
#endif

  if (!path)
    return E_ERROR;
  dev->position = 0;
  dev->offset = 0;
  dev->size = 0;
  dev->file = open(path, O_RDWR);
  if (!dev->file)
    return E_ERROR;
  if (fstat(dev->file, &(dev->info)) == -1 || dev->info.st_size == 0)
    return E_ERROR;
  dev->data = mmap(0, dev->info.st_size, PROT_WRITE, MAP_SHARED, dev->file, 0);
  if (dev->data == MAP_FAILED)
    return E_ERROR;
  dev->size = dev->info.st_size;

#ifdef DEBUG
  dev->readCount = dev->writeCount = 0;
  dev->readSize = dev->writeSize = 0;
  getSizeStr(dev->size, size2str);
  printf("mmaped_io: opened file: %s, size: %s\n", path, size2str);
#endif

  return E_OK;
}
/*----------------------------------------------------------------------------*/
static uint32_t mmiRead(void *object, uint8_t *buffer, uint32_t length)
{
  struct Mmi *dev = object;

  mutexLock(&dev->lock);
  memcpy(buffer, dev->data + dev->position + dev->offset, length);
  dev->position += length;
  mutexUnlock(&dev->lock);

#ifdef DEBUG
  dev->readCount++;
  dev->readSize += length;
#endif
#ifdef DEBUG_RW
  printf("mmaped_io: read data at 0x%012lX, length %u\n",
      (unsigned long)dev->position, length);
#endif

  return length;
}
/*----------------------------------------------------------------------------*/
static uint32_t mmiWrite(void *object, const uint8_t *buffer, uint32_t length)
{
  struct Mmi *dev = object;

  mutexLock(&dev->lock);
  memcpy(dev->data + dev->position + dev->offset, buffer, length);
  dev->position += length;
  mutexUnlock(&dev->lock);

#ifdef DEBUG
  dev->writeCount++;
  dev->writeSize += length;
#endif
#ifdef DEBUG_RW
  printf("mmaped_io: write data at 0x%012lX, length %u\n",
      (unsigned long)dev->position, length);
#endif

  return length;
}
/*----------------------------------------------------------------------------*/
static enum result mmiGetOpt(void *object, enum ifOption option, void *data)
{
  struct Mmi *dev = object;

  switch (option)
  {
    case IF_ADDRESS:
      *(uint64_t *)data = dev->position;
      return E_OK;
    default:
      return E_ERROR;
  }
}
/*----------------------------------------------------------------------------*/
static enum result mmiSetOpt(void *object, enum ifOption option,
    const void *data)
{
  struct Mmi *dev = object;
  uint64_t newPos;

  switch (option)
  {
    case IF_ADDRESS:
      newPos = *(uint64_t *)data;
      if (newPos + dev->offset >= dev->size)
      {
#ifdef DEBUG
        printf("mmaped_io: out of bounds, position 0x%012lX, size 0x%012lX\n",
            (unsigned long)(newPos + dev->offset), (unsigned long)dev->size);
#endif
        return E_ERROR;
      }
      dev->position = newPos;
      return E_OK;
    default:
      return E_ERROR;
  }
}
/*----------------------------------------------------------------------------*/
static void mmiDeinit(void *object)
{
  struct Mmi *dev = object;

  munmap(dev->data, dev->info.st_size);
  close(dev->file);
}
/*----------------------------------------------------------------------------*/
enum result mmiSetPartition(void *object, struct MbrDescriptor *desc)
{
  struct Mmi *dev = object;
  const char validTypes[] = {0x0B, 0x0C, 0x1B, 0x1C, 0x00};

  if (!strchr(validTypes, desc->type))
    return E_ERROR;
  dev->size = desc->size << MMI_SECTOR_POW;
  dev->offset = desc->offset << MMI_SECTOR_POW;
#ifdef DEBUG
  printf("mmaped_io: partition type 0x%02X, size %u sectors, "
      "offset %u sectors\n",
      desc->type, (unsigned int)desc->size, (unsigned int)desc->offset);
#endif
  return E_OK;
}
/*----------------------------------------------------------------------------*/
enum result mmiReadTable(void *object, uint32_t sector, uint8_t index,
    struct MbrDescriptor *desc)
{
  struct Mmi *dev = object;
  uint64_t position = sector << MMI_SECTOR_POW;
  uint8_t buffer[1 << MMI_SECTOR_POW];
  uint8_t *ptr;

  dev->offset = 0;
  if (ifSetOpt(object, IF_ADDRESS, &position) != E_OK)
    return E_INTERFACE;
  if (ifRead(object, buffer, sizeof(buffer)) != sizeof(buffer))
    return E_INTERFACE;
  if (*(uint16_t *)(buffer + 0x01FE) != 0xAA55)
    return E_ERROR;

  ptr = buffer + 0x01BE + (index << 4); /* Pointer to partition entry */
  if (!*(ptr + 0x04)) /* Empty entry */
    return E_ERROR;
  desc->type = *(ptr + 0x04); /* File system descriptor */
  desc->offset = *(uint32_t *)(ptr + 0x08);
  desc->size = *(uint32_t *)(ptr + 0x0C);
  return E_OK;
}
