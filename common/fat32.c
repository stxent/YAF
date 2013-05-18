/*
 * fat32.c
 * Copyright (C) 2012 xent
 * Project is distributed under the terms of the GNU General Public License v3.0
 */

#include <stdlib.h>
#include <string.h>
#include "fat32.h"
#include "fat32_defs.h"
/*----------------------------------------------------------------------------*/
#ifdef DEBUG
#include <stdio.h>
#include <stdlib.h>
#endif
/*----------------------------------------------------------------------------*/
/* Get size of array placed in structure */
#define ARRAY_SIZE(parent, array) (sizeof(((struct parent *)0)->array))
/*------------------Class descriptors-----------------------------------------*/
static const struct FsFileClass fatFileTable = {
    .size = sizeof(struct FatFile),
    .init = 0,
    .deinit = 0,

    .close = fatClose,
    .eof = fatEof,
    .tell = fatTell,
    .seek = fatSeek,
    .read = fatRead,

    .write = fatWrite
};
/*----------------------------------------------------------------------------*/
static const struct FsDirClass fatDirTable = {
    .size = sizeof(struct FatDir),
    .init = 0,
    .deinit = 0,

    .close = fatCloseDir,
    .read = fatReadDir
};
/*----------------------------------------------------------------------------*/
static const struct FsHandleClass fatHandleTable = {
    .size = sizeof(struct FatHandle),
    .init = fatInit,
    .deinit = fatDeinit,

    .open = fatOpen,
    .openDir = fatOpenDir,
    .stat = fatStat,

    .makeDir = fatMakeDir,
    .move = fatMove,
    .remove = fatRemove,
    .removeDir = fatRemoveDir
};
/*----------------------------------------------------------------------------*/
const struct FsHandleClass *FatFile = (void *)&fatFileTable;
const struct FsHandleClass *FatDir = (void *)&fatDirTable;
const struct FsHandleClass *FatHandle = (void *)&fatHandleTable;
/*----------------------------------------------------------------------------*/
/*------------------Inline functions------------------------------------------*/
static inline bool clusterFree(uint32_t cluster)
{
  return !(cluster & (uint32_t)0x0FFFFFFF);
}
/*----------------------------------------------------------------------------*/
static inline bool clusterEOC(uint32_t cluster)
{
  return (cluster & (uint32_t)0x0FFFFFF8) == (uint32_t)0x0FFFFFF8;
}
/*----------------------------------------------------------------------------*/
static inline bool clusterUsed(uint32_t cluster)
{
  return (cluster & (uint32_t)0x0FFFFFFF) >= (uint32_t)0x00000002 &&
      (cluster & (uint32_t)0x0FFFFFFF) <= (uint32_t)0x0FFFFFEF;
}
/*----------------------------------------------------------------------------*/
/* Calculate first sector number of the cluster */
static inline uint32_t getSector(struct FatHandle *handle, uint32_t cluster)
{
  return handle->dataSector + (((cluster) - 2) << handle->clusterSize);
}
/*----------------------------------------------------------------------------*/
/* File or directory entries per directory cluster */
static inline uint16_t entryCount(struct FatHandle *handle)
{
  return 1 << E_POW << handle->clusterSize;
}
/*----------------------------------------------------------------------------*/
/* Calculate current sector in data cluster for read or write operations */
static inline uint8_t sectorInCluster(struct FatHandle *handle, uint32_t offset)
{
  return (offset >> SECTOR_POW) & ((1 << handle->clusterSize) - 1);
}
/*----------------------------------------------------------------------------*/
/*------------------Specific FAT32 functions----------------------------------*/
static void extractShortName(const struct DirEntryImage *entry, char *str)
{
  const char *src = entry->name;
  char *dest = str;
  uint8_t counter = 0;

  while (counter++ < sizeof(entry->name))
  {
    if (*src != ' ')
      *dest++ = *src;
    src++;
  }
  /* Add dot when entry is not directory or extension exists */
  if (!(entry->flags & FLAG_DIR) && entry->extension[0] != ' ')
  {
    *dest++ = '.';
    /* Copy entry extension */
    counter = 0;
    src = entry->extension;
    while (counter++ < sizeof(entry->extension))
    {
      if (*src != ' ')
        *dest++ = *src;
      ++src;
    }
  }
  *dest = '\0';
}
/*----------------------------------------------------------------------------*/
/* Destination string buffer should be at least FILE_NAME_MAX characters long */
static const char *getChunk(const char *src, char *dest)
{
  uint8_t counter = 0;

  if (!*src)
    return src;
  if (*src == '/')
  {
    *dest++ = '/';
    *dest = '\0';
    return src + 1;
  }
  while (*src && counter++ < FILE_NAME_MAX - 1)
  {
    if (*src == '/')
    {
      ++src;
      break;
    }
    *dest++ = *src++;
  }
  *dest = '\0';
  return src;
}
/*----------------------------------------------------------------------------*/
static enum result getNextCluster(struct FatHandle *handle, uint32_t *cluster)
{
  uint32_t nextCluster;
  enum result res;

  if ((res = readSector(handle, handle->tableSector + (*cluster >> TE_COUNT),
      handle->buffer, 1)) != E_OK)
  {
    return res;
  }
  nextCluster = *(uint32_t *)(handle->buffer + TE_OFFSET(*cluster));
  if (clusterUsed(nextCluster))
  {
    *cluster = nextCluster;
    return E_OK;
  }
  return E_FAULT;
}
/*----------------------------------------------------------------------------*/
/*
 * Members entry->index and entry->parent have to be initialized.
 * Name buffer must be at least FILE_NAME_MAX long.
 * Set name buffer to zero when entry name is not needed.
 */
static enum result fetchEntry(struct FatHandle *handle,
    struct FatObject *entry, char *entryName)
{
  struct DirEntryImage *ptr;
  uint32_t sector;
#ifdef FAT_LFN
  struct LfnObject longName;
  uint8_t found = 0; /* Long file name chunks */
#endif
  enum result res;

  entry->attribute = 0;
  entry->cluster = 0;
  entry->size = 0;
  while (1)
  {
    if (entry->index >= entryCount(handle))
    {
      /* Check clusters until end of directory (EOC entry in FAT) */
      if ((res = getNextCluster(handle, &entry->parent)) != E_OK)
        return res;
      entry->index = 0;
    }
    sector = getSector(handle, entry->parent) + E_SECTOR(entry->index);
    if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
      return res;
    ptr = (struct DirEntryImage *)(handle->buffer + E_OFFSET(entry->index));
    if ((ptr->flags & MASK_LFN) == MASK_LFN)
    {
#ifdef FAT_LFN
      if (!(ptr->ordinal & LFN_DELETED))
      {
        if (ptr->ordinal & LFN_LAST)
        {
          found = 1;
          longName.length = ptr->ordinal & ~LFN_LAST;
          longName.checksum = ptr->checksum;
          longName.index = entry->index;
          longName.parent = entry->parent;
        }
        else if (found)
          ++found;
      }
#endif
      ++entry->index;
      continue;
    }
    if (!ptr->name[0]) /* No more entries */
      return E_FAULT;
    if (ptr->name[0] != E_FLAG_EMPTY) /* Entry exists */
      break;
    ++entry->index;
  }
  entry->attribute = ptr->flags;
  entry->size = ptr->size;
  entry->cluster = ptr->clusterHigh << 16 | ptr->clusterLow;
#ifdef FAT_LFN
  if (found && (longName.checksum != getChecksum(ptr->filename,
      sizeof(ptr->filename)) || found != longName.length))
  {
    found = 0; /* Wrong checksum or chunk count does not match */
  }
  entry->nameIndex = found ? longName.index : entry->index;
  entry->nameParent = found ? longName.parent : entry->parent;
#endif

  if (entryName)
  {
#ifdef FAT_LFN
    if (found)
      readLongName(handle, &longName, entryName);
    else
      extractShortName(ptr, entryName);
#else
    extractShortName(ptr, entryName);
#endif
  }
  return E_OK;
}
/*----------------------------------------------------------------------------*/
static const char *followPath(struct FatHandle *handle, struct FatObject *item,
    const char *path)
{
  char entryName[FILE_NAME_MAX], name[FILE_NAME_MAX];

  path = getChunk(path, name);
  if (!strlen(name))
    return 0;
  if (name[0] == '/')
  {
    item->size = 0;
    item->cluster = handle->rootCluster;
    item->attribute = FLAG_DIR;
    return path;
  }
  item->parent = item->cluster;
  item->index = 0;
  while (!fetchEntry(handle, item, entryName))
  {
    if (!strcmp(name, entryName))
      return path;
    ++item->index;
  }
  return 0;
}
/*----------------------------------------------------------------------------*/
static enum result readSector(struct FatHandle *handle,
    uint32_t address, uint8_t *buffer, uint8_t count)
{
  if (handle->bufferedSector == address)
    return E_OK;
  handle->bufferedSector = address;
  return fsBlockRead(handle->parent.dev, address << SECTOR_POW, buffer,
      count << SECTOR_POW);
}
/*----------------------------------------------------------------------------*/
#ifdef FAT_LFN
/* Extract 13 unicode characters from long file name entry */
static void extractLongName(const struct DirEntryImage *entry, char16_t *str)
{
  memcpy(str, entry->longName0, sizeof(entry->longName0));
  str += sizeof(entry->longName0) / sizeof(char16_t);
  memcpy(str, entry->longName1, sizeof(entry->longName1));
  str += sizeof(entry->longName1) / sizeof(char16_t);
  memcpy(str, entry->longName2, sizeof(entry->longName2));
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_LFN
/* Calculate entry name checksum for long file name support */
static uint8_t getChecksum(const char *str, uint8_t length)
{
  uint8_t pos = 0, sum = 0;

  for (; pos < length; ++pos)
    sum = ((sum >> 1) | (sum << 7)) + *str++;
  return sum;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_LFN
static enum result readLongName(struct FatHandle *handle,
    struct LfnObject *entry, char *entryName)
{
  struct DirEntryImage *ptr;
  uint32_t sector;
  enum result res;
  uint8_t chunks = 0;

  while (1)
  {
    if (entry->index >= entryCount(handle))
    {
      /* Check clusters until end of directory (EOC entry in FAT) */
      if ((res = getNextCluster(handle, &entry->parent)) != E_OK)
        return res;
      entry->index = 0;
    }
    sector = getSector(handle, entry->parent) + E_SECTOR(entry->index);
    if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
      return res;
    ptr = (struct DirEntryImage *)(handle->buffer + E_OFFSET(entry->index));
    if ((ptr->flags & MASK_LFN) == MASK_LFN)
    {
      ++chunks;
      extractLongName(ptr, handle->nameBuffer
          + ((ptr->ordinal & ~LFN_LAST) - 1) * LFN_ENTRY_LENGTH);
      ++entry->index;
      continue;
    }
    else
      break; /* No more consecutive long file name entries */
    ++entry->index;
  }
  if (!chunks || entry->length != chunks)
    return E_ERROR;
  uFromUtf16(entryName, handle->nameBuffer, FILE_NAME_MAX);
  return E_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result allocateCluster(struct FatHandle *handle, uint32_t *cluster)
{
  struct InfoSectorImage *info;
  uint32_t current;
  uint16_t offset;
  enum result res;

  current = handle->lastAllocated + 1;
  while (current != handle->lastAllocated)
  {
#ifdef DEBUG
    if (current == handle->clusterCount)
      printf("Reached end of partition, continue from third cluster\n");
#endif
    if (current >= handle->clusterCount)
      current = 2;
    if ((res = readSector(handle, handle->tableSector + (current >> TE_COUNT),
        handle->buffer, 1)) != E_OK)
    {
      return res;
    }
    offset = (current & ((1 << TE_COUNT) - 1)) << 2;
    /* Is cluster free */
    if (clusterFree(*(uint32_t *)(handle->buffer + offset)))
    {
      *(uint32_t *)(handle->buffer + offset) = CLUSTER_EOC_VAL;
      if ((!*cluster || (*cluster >> TE_COUNT != current >> TE_COUNT))
          && (res = updateTable(handle, current >> TE_COUNT)) != E_OK)
      {
        return res;
      }
      if (*cluster)
      {
        if ((res = readSector(handle, handle->tableSector
            + (*cluster >> TE_COUNT), handle->buffer, 1)) != E_OK)
        {
          return res;
        }
        *(uint32_t *)(handle->buffer + TE_OFFSET(*cluster)) = current;
        if ((res = updateTable(handle, *cluster >> TE_COUNT)) != E_OK)
          return res;
      }
#ifdef DEBUG
      printf("Allocated new cluster: %u, reference %u\n", current, *cluster);
#endif
      *cluster = current;
      /* Update information sector */
      if ((res = readSector(handle, handle->infoSector, handle->buffer, 1)))
        return res;
      info = (struct InfoSectorImage *)handle->buffer;
      /* Set last allocated cluster */
      info->lastAllocated = current;
      handle->lastAllocated = current;
      /* Update free clusters count */
      --info->freeClusters;
      if ((res = writeSector(handle, handle->infoSector, handle->buffer, 1)))
        return res;
      return E_OK;
    }
    ++current;
  }
#ifdef DEBUG
  printf("Allocation error, possibly partition is full\n");
#endif
  return E_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
/*
 * Allocate single entry or entry chain inside entry->parent chain.
 * Members entry->parent and entry->index have to be initialized.
 * Returns the position of the new entry in entry->parent and entry->index.
 */
static enum result allocateEntry(struct FatHandle *handle,
    struct FatObject *entry, uint8_t chainLength)
{
  /*
   * Officially maximum directory capacity is 2^16 entries,
   * but technically there is no such limit.
   */
  struct DirEntryImage *ptr;
  uint32_t parent = entry->parent; /* Cluster where new entry will be placed */
  uint32_t sector;
  uint16_t index = entry->index; /* New entry position in cluster */
  uint8_t chunks = 0;
  enum result res;

  while (1)
  {
    if (entry->index >= entryCount(handle))
    {
      /* Try to get next cluster or allocate new cluster for directory */
      res = getNextCluster(handle, &entry->parent);
      if (res == E_FAULT)
      {
        //FIXME Alloc 2 512-byte clusters for names longer than 195 characters
        if ((res = allocateCluster(handle, &entry->parent)) != E_OK)
          return res;
        memset(handle->buffer, 0, SECTOR_SIZE);
        sector = getSector(handle, entry->parent + 1); //TODO Check bounds
        do
        {
          if ((res = writeSector(handle, --sector, handle->buffer, 1)) != E_OK)
            return res;
        }
        while (sector & ((1 << handle->clusterSize) - 1));

        if (!chunks)
        {
          /* Parent is already initialized */
          entry->index = 0;
        }
        else
        {
          entry->index = index;
          entry->parent = parent;
        }
        /* There is enough free space at the and of directory chain */
        return E_OK;
      }
      else if (res != E_OK)
        return res;
      entry->index = 0;
    }
    sector = getSector(handle, entry->parent) + E_SECTOR(entry->index);
    if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
      return res;
    ptr = (struct DirEntryImage *)(handle->buffer + E_OFFSET(entry->index));
    /* Empty entry, deleted entry or deleted long file name entry */
    if (!ptr->name[0] || ptr->name[0] == E_FLAG_EMPTY
        || ((ptr->flags & MASK_LFN) == MASK_LFN
        && (ptr->ordinal & LFN_DELETED)))
    {
      if (!chunks) /* Found first free entry */
      {
        chunks = 1;
        index = entry->index;
        parent = entry->parent;
      }
      else
        ++chunks;
      if (chunks == chainLength) /* Found enough free entries */
      {
        entry->index = index;
        entry->parent = parent;
        return E_OK;
      }
    }
    else
      chunks = 0;
    ++entry->index;
  }
  return E_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result createEntry(struct FatHandle *handle,
    struct FatObject *entry, const char *name)
{
  struct DirEntryImage *ptr;
  const char *str = name;
  uint32_t sector;
#ifdef FAT_LFN
  uint8_t checksum = 0;
  bool lastEntry = true;
#endif
  uint8_t chunks = 0;
  char shortName[sizeof(ptr->filename)];
  bool valid;
  enum result res;

  /* Check path for nonexistent directories */
  while (*str)
    if (*str++ == '/')
      return E_ENTRY;

  /* Check whether the file name is valid for use as short name */
  valid = fillShortName(shortName, name);
  /* TODO Check for duplicates */

#ifdef FAT_LFN
  if (!valid)
  {
    uint16_t length;

    /* Clear extension when new entry is directory */
    if ((entry->attribute & FLAG_DIR))
      memset(shortName + sizeof(ptr->name), ' ', sizeof(ptr->extension));
    /* Calculate checksum for short name */
    checksum = getChecksum(shortName, sizeof(ptr->filename));
    /* Convert file name to UTF-16 */
    length = uToUtf16(handle->nameBuffer, name, FILE_NAME_BUFFER) + 1;
    /* Calculate long file name length in entries */
    chunks = length / LFN_ENTRY_LENGTH;
    if (length > chunks * LFN_ENTRY_LENGTH) /* When fractional part exists */
      ++chunks;
  }
#endif

  /* Find suitable space within the directory */
  if ((res = allocateEntry(handle, entry, chunks + 1)) != E_OK)
    return res;

  while (1)
  {
    /* Entry fields index and parent are initialized after entry allocation */
    sector = getSector(handle, entry->parent) + E_SECTOR(entry->index);
    if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
      return res;
    ptr = (struct DirEntryImage *)(handle->buffer + E_OFFSET(entry->index));

    if (!chunks)
      break;

#ifdef FAT_LFN
    /* TODO Possibly memset for the first time all bytes to zero */
    ptr->flags = MASK_LFN;
    ptr->checksum = checksum;
    ptr->ordinal = chunks--;
    if (lastEntry)
    {
      ptr->ordinal |= LFN_LAST;
      lastEntry = false;
    }
    /* In long file name entries data at cluster low word should be cleared */
    ptr->clusterLow = 0;
    fillLongName(ptr, handle->nameBuffer + chunks * LFN_ENTRY_LENGTH);

    ++entry->index;
    /* Write back updated sector when switching sectors */
    if (!(entry->index & (E_POW - 1))
        && (res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
    {
        return res;
    }
    if (entry->index >= entryCount(handle))
    {
      /* Try to get next cluster */
      if ((res = getNextCluster(handle, &entry->parent)) != E_OK)
        return res;
      entry->index = 0;
    }
#endif
  }

  memcpy(ptr->filename, shortName, sizeof(ptr->filename));
  fillDirEntry(ptr, entry);
  if ((res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
    return res;
  return E_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static void fillDirEntry(struct DirEntryImage *ptr, struct FatObject *entry)
{
  /* TODO Possibly memset for the first time all bytes to zero */
  ptr->flags = entry->attribute;
  ptr->clusterHigh = entry->cluster >> 16;
  ptr->clusterLow = entry->cluster;
  ptr->size = entry->size;
#ifdef FAT_TIME
  //FIXME Rewrite
  /* Last modified time and date */
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
/*
 * Returns true when entry is a valid short name.
 * Otherwise long file name entries are expected.
 */
static bool fillShortName(char *shortName, const char *name)
{
  const uint8_t nameLength = ARRAY_SIZE(DirEntryImage, name); //FIXME
  const uint8_t fullLength = ARRAY_SIZE(DirEntryImage, filename);
  const char *dot;
  uint16_t length;
  uint8_t pos = 0;
  char converted, symbol;
  bool valid = true;

  length = strlen(name);
  for (dot = name + length - 1; dot >= name && *dot != '.'; --dot);
  if (dot < name)
  {
    if (length > nameLength)
      valid = false;
    dot = 0;
  }
  else if (dot > name + nameLength
      || length - (dot - name) > fullLength - nameLength)
  {
    /* The length of file name or extension is greater than maximum allowed */
    valid = false;
  }

  memset(shortName, ' ', fullLength);
  while ((symbol = *name))
  {
    if (dot && name == dot)
    {
      pos = nameLength;
      ++name;
      continue;
    }
    ++name;
    converted = processCharacter(symbol);
    if (converted != symbol)
      valid = false;
    if (!converted)
      continue;
    shortName[pos++] = converted;

    if (pos == nameLength)
    {
      if (dot) /* Check whether extension exists */
      {
        name = dot + 1;
        continue;
      }
      else
        break;
    }
    if (pos == fullLength)
      break;
  }
  return valid;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result freeChain(struct FatHandle *handle, uint32_t cluster)
{
  struct InfoSectorImage *info;
  uint32_t current = cluster, next, released = 0;
  enum result res;

  if (!current)
    return E_OK; /* Already empty */
  while (clusterUsed(current))
  {
    /* Get FAT sector with next cluster value */
    if ((res = readSector(handle, handle->tableSector + (current >> TE_COUNT),
        handle->buffer, 1)) != E_OK)
    {
      return res;
    }
    /* Free cluster */
    next = *(uint32_t *)(handle->buffer + TE_OFFSET(current));
    *(uint32_t *)(handle->buffer + TE_OFFSET(current)) = 0;
#ifdef DEBUG
    if (current >> TE_COUNT != next >> TE_COUNT)
    {
      printf("FAT sectors differ, next: %u (0x%07X), current %u\n",
          (next >> TE_COUNT), (next >> TE_COUNT), (current >> TE_COUNT));
    }
    printf("Cleared cluster: %u\n", current);
#endif
    if (current >> TE_COUNT != next >> TE_COUNT
        && (res = updateTable(handle, current >> TE_COUNT)) != E_OK)
    {
      return res;
    }
    ++released;
    current = next;
  }
  /* Update information sector */
  if ((res = readSector(handle, handle->infoSector, handle->buffer, 1)) != E_OK)
    return res;
  info = (struct InfoSectorImage *)handle->buffer;
  /* Set free clusters count */
  info->freeClusters += released;
  if ((res = writeSector(handle,
      handle->infoSector, handle->buffer, 1)) != E_OK)
  {
    return res;
  }
  return E_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result markFree(struct FatHandle *handle, struct FatObject *entry)
{
  struct DirEntryImage *ptr;
  uint32_t cluster, lastSector, sector;
  uint16_t index; /* Entry position in cluster */
  enum result res;

#ifdef FAT_LFN
  index = entry->nameIndex;
  cluster = entry->nameParent;
#else
  index = entry->index;
  cluster = entry->parent;
#endif
  lastSector = getSector(handle, entry->parent) + E_SECTOR(entry->index);
  do
  {
    if (index >= entryCount(handle))
    {
      /* Get the next cluster when entry is spread across multiple clusters */
      if ((res = getNextCluster(handle, &cluster)) != E_OK)
        return res;
      index = 0;
    }
    sector = getSector(handle, cluster) + E_SECTOR(index);
    if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
      return res;
    ptr = (struct DirEntryImage *)(handle->buffer + E_OFFSET(index));
    /* Mark the directory entry or the long file name entry as deleted */
    ptr->name[0] = E_FLAG_EMPTY;
    ++index;
    /* Write back updated sector when switching sectors */
    if (!(index & (E_POW - 1))
        && (res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
    {
        return res;
    }
  }
  while (sector != lastSector || index <= entry->index);

  if ((res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
    return res;
  return E_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
/* Returns zero when character is not allowed or processed character code */
static char processCharacter(char value)
{
  /* All slashes are already removed while file path being calculated */
  /* Drop all multibyte UTF-8 code points */
  if (value & 0x80)
    return 0;
  /* Replace spaces with underscores */
  if (value == ' ')
    return '_';
  /* Convert lower case characters to upper case */
  if (value >= 'a' && value <= 'z')
    return value - 32;
  /* Check specific FAT32 ranges */
  if (value > 0x20 && value != 0x22 && value != 0x7C
      && !(value >= 0x2A && value <= 0x2F)
      && !(value >= 0x3A && value <= 0x3F)
      && !(value >= 0x5B && value <= 0x5D))
  {
    return value;
  }
  return 0;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result writeSector(struct FatHandle *handle,
    uint32_t address, const uint8_t *buffer, uint8_t count)
{
  return fsBlockWrite(handle->parent.dev, address << SECTOR_POW, buffer,
      count << SECTOR_POW);
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result truncate(struct FatFile *fileHandle)
{
  enum result res;

  if ((res = freeChain((struct FatHandle *)fileHandle->parent.descriptor,
      fileHandle->cluster)) != E_OK)
  {
    return res;
  }

  fileHandle->cluster = 0;
  fileHandle->currentCluster = 0;
  fileHandle->size = 0;
  fileHandle->position = 0;
  return E_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
/* Copy current sector into FAT sectors located at offset */
static enum result updateTable(struct FatHandle *handle, uint32_t offset)
{
  uint8_t fat;
  enum result res;

  for (fat = 0; fat < handle->tableCount; ++fat)
  {
    if ((res = writeSector(handle, offset + handle->tableSector
        + (uint32_t)fat * handle->tableSize, handle->buffer, 1)) != E_OK)
    {
      return res;
    }
  }
  return E_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#if defined(FAT_WRITE) && defined(FAT_LFN)
/* Save 13 unicode characters to long file name entry */
static void fillLongName(struct DirEntryImage *entry, char16_t *str)
{
  //FIXME Add string end checking
  memcpy(entry->longName0, str, sizeof(entry->longName0));
  str += sizeof(entry->longName0) / sizeof(char16_t);
  memcpy(entry->longName1, str, sizeof(entry->longName1));
  str += sizeof(entry->longName1) / sizeof(char16_t);
  memcpy(entry->longName2, str, sizeof(entry->longName2));
}
#endif
/*----------------------------------------------------------------------------*/
/*------------------Implemented filesystem methods----------------------------*/
static enum result fatInit(void *object, const void *configPtr)
{
  const struct Fat32Config *config = configPtr;
  struct BootSectorImage *boot;
  struct FatHandle *handle = object;
#ifdef FAT_WRITE
  struct InfoSectorImage *info;
#endif
  uint16_t sizePow;
  enum result res;

  /* Initialize buffer variables */
  handle->buffer = malloc(SECTOR_SIZE);
  if (!handle->buffer)
    return E_MEMORY;
#ifdef FAT_LFN
  handle->nameBuffer = malloc(FILE_NAME_BUFFER * sizeof(char16_t));
  if (!handle->nameBuffer)
    return E_MEMORY;
#endif
  handle->bufferedSector = (uint32_t)(-1);

  handle->parent.dev = config->interface;
  /* Read first sector */
  if ((res = readSector(handle, 0, handle->buffer, 1)) != E_OK)
    return res;
  boot = (struct BootSectorImage *)handle->buffer;
  /* Check boot sector signature (55AA at 0x01FE) */
  /* TODO move signatures to macro */
  if (boot->bootSignature != 0xAA55)
    return E_ERROR;
  /* Check sector size, fixed size of 2 ^ SECTOR_POW allowed */
  if (boot->bytesPerSector != SECTOR_SIZE)
    return E_ERROR;
  /* Calculate sectors per cluster count */
  sizePow = boot->sectorsPerCluster;
  handle->clusterSize = 0;
  while (sizePow >>= 1)
    ++handle->clusterSize;

  handle->tableSector = boot->reservedSectors;
  handle->dataSector = handle->tableSector + boot->fatCopies * boot->fatSize;
  handle->rootCluster = boot->rootCluster;
#ifdef DEBUG
  printf("Cluster size:       %u\n", (unsigned int)(1 << handle->clusterSize));
  printf("Table sector:       %u\n", (unsigned int)handle->tableSector);
  printf("Data sector:        %u\n", (unsigned int)handle->dataSector);
#endif

#ifdef FAT_WRITE
  handle->tableCount = boot->fatCopies;
  handle->tableSize = boot->fatSize;
  handle->clusterCount = ((boot->partitionSize
      - handle->dataSector) >> handle->clusterSize) + 2;
  handle->infoSector = boot->infoSector;
#ifdef DEBUG
  printf("Info sector:        %u\n", (unsigned int)handle->infoSector);
  printf("Table copies:       %u\n", (unsigned int)handle->tableCount);
  printf("Table size:         %u\n", (unsigned int)handle->tableSize);
  printf("Cluster count:      %u\n", (unsigned int)handle->clusterCount);
  printf("Sectors count:      %u\n", (unsigned int)boot->partitionSize);
#endif /* DEBUG */

  if ((res = readSector(handle, handle->infoSector, handle->buffer, 1)) != E_OK)
    return res;
  info = (struct InfoSectorImage *)handle->buffer;
  /* Check info sector signatures (RRaA at 0x0000 and rrAa at 0x01E4) */
  if (info->firstSignature != 0x41615252 || info->infoSignature != 0x61417272)
    return E_ERROR;
  handle->lastAllocated = info->lastAllocated;
#ifdef DEBUG
  printf("Free clusters:      %u\n", (unsigned int)info->freeClusters);
#endif /* DEBUG */
#endif /* FAT_WRITE */

#ifdef DEBUG
  printf("Size of FatHandle:  %u\n", (unsigned int)sizeof(struct FatHandle));
  printf("Size of FatFile:    %u\n", (unsigned int)sizeof(struct FatFile));
  printf("Size of FatDir:     %u\n", (unsigned int)sizeof(struct FatDir));
#endif
  return E_OK;
}
/*----------------------------------------------------------------------------*/
static void fatDeinit(void *object)
{
  free(((struct FatHandle *)object)->buffer);
}
/*----------------------------------------------------------------------------*/
/*------------------Common functions------------------------------------------*/
static enum result fatStat(void *object, struct FsStat *result,
    const char *path)
{
#ifdef FAT_TIME
  struct DirEntryImage *ptr;
#endif
  struct FatHandle *handle = object;
  const char *followedPath;
  uint32_t sector;
  struct FatObject item;
#ifdef FAT_TIME
  struct Time tm;
#endif
  enum result res;

  while (*path && (followedPath = followPath(handle, &item, path)))
    path = followedPath;
  /* Non-zero when entry not found */
  if (*path)
    return E_ENTRY;

  sector = getSector(object, item.parent) + E_SECTOR(item.index);
  if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
    return res;
#ifdef FAT_TIME
  ptr = (struct DirEntryImage *)(handle->buffer + E_OFFSET(item.index));
#endif

#ifdef DEBUG
  result->cluster = item.cluster;
  result->pcluster = item.parent;
  result->pindex = item.index;
#endif
  result->size = item.size;
  if (item.attribute & FLAG_DIR)
    result->type = FS_TYPE_DIR;
  else
    result->type = FS_TYPE_FILE;

#ifdef DEBUG
  result->access = 07; /* rwx */
#endif

#ifdef FAT_TIME
  tm.sec = ptr->time & 0x1F;
  tm.min = (ptr->time >> 5) & 0x3F;
  tm.hour = (ptr->time >> 11) & 0x1F;
  tm.day = ptr->date & 0x1F;
  tm.mon = (ptr->date >> 5) & 0x0F;
  tm.year = ((ptr->date >> 9) & 0x7F) + 1980;
  result->atime = unixTime(&tm);
#endif
  return E_OK;
}
/*----------------------------------------------------------------------------*/
static void *fatOpen(void *object, const char *path, enum fsMode mode)
{
  struct FatFile *fileHandle;
  const char *followedPath;
  struct FatObject item;

  while (*path && (followedPath = followPath((struct FatHandle *)object,
      &item, path)))
  {
    path = followedPath;
  }
  /* Non-zero when entry not found */
  if (*path)
  {
#ifdef FAT_WRITE
    if (mode == FS_WRITE)
    {
      item.attribute = 0;
      item.cluster = 0;
      item.size = 0;
      //TODO Initialize parent and index
      if (createEntry((struct FatHandle *)object, &item, path) != E_OK)
        return 0;
    }
    else
      return 0;
#else
    return 0;
#endif
  }
  /* Not found if volume name, system or directory entry */
  if (item.attribute & (FLAG_VOLUME | FLAG_DIR))
    return 0;

  /* Allocate new directory object when pointer is zero */
  if (!(fileHandle = init(FatFile, 0)))
    return 0;

  fileHandle->parent.descriptor = object;
  fileHandle->mode = mode;
  fileHandle->position = 0;
  fileHandle->size = item.size;
  fileHandle->cluster = item.cluster;
  fileHandle->currentCluster = item.cluster;

#ifdef FAT_WRITE
  fileHandle->parentCluster = item.parent;
  fileHandle->parentIndex = item.index;

  /* Truncate existing file when opened in write mode */
  if (mode == FS_WRITE && !*path && truncate(fileHandle) != E_OK)
  {
    deinit(fileHandle);
    return 0;
  }
  /* In append mode file pointer moves to the end of file */
  if (mode == FS_APPEND && fsSeek(fileHandle, 0, FS_SEEK_END) != E_OK)
  {
    deinit(fileHandle);
    return 0;
  }
#endif

  return fileHandle;
}
/*----------------------------------------------------------------------------*/
static void *fatOpenDir(void *object, const char *path)
{
  struct FatDir *dirHandle;
  struct FatObject item;

  while (path && *path)
    path = followPath((struct FatHandle *)object, &item, path);
  if (!path)
    return 0;
  /* Not directory or volume name */
  if (!(item.attribute & FLAG_DIR) || item.attribute & FLAG_VOLUME)
    return 0;

  /* Allocate new directory object when pointer is zero */
  if (!(dirHandle = init(FatDir, 0)))
    return 0;

  dirHandle->parent.descriptor = object;
  dirHandle->cluster = item.cluster;
  dirHandle->currentCluster = item.cluster;
  dirHandle->currentIndex = 0;

  return dirHandle;
}
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result fatMove(void *object, const char *src, const char *dest)
{
  struct FatHandle *handle = object;
  const char *followedPath;
  struct FatObject item, oldItem;
  enum result res;

  while (src && *src)
    src = followPath(handle, &item, src);
  if (!src)
    return E_ENTRY;

  /* Volume entries are invisible */
  if (item.attribute & FLAG_VOLUME)
    return E_ENTRY;

  /* Save old entry data */
  oldItem = item;

  while (*dest && (followedPath = followPath(handle, &item, dest)))
    dest = followedPath;
  if (!*dest) /* Entry with same name exists */
    return E_ERROR;

  item.attribute = oldItem.attribute;
  item.cluster = oldItem.cluster;
  item.size = oldItem.size;
  //TODO Initialize parent and index
  if ((res = createEntry(handle, &item, dest)) != E_OK)
    return res;

  if ((res = markFree(handle, &oldItem)) != E_OK)
    return res;

  return E_OK;
}
#else
static enum result fatMove(void *object __attribute__((unused)),
    const char *src __attribute__((unused)),
    const char *dest __attribute__((unused)))
{
  return E_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result fatRemove(void *object, const char *path)
{
  struct FatHandle *handle = object;
  struct FatObject item;
  enum result res;

  while (path && *path)
    path = followPath(handle, &item, path);
  /* Not found, directory or volume name */
  if (!path || item.attribute & (FLAG_VOLUME | FLAG_DIR))
    return E_ENTRY;

  /* Mark file table clusters as free */
  if ((res = freeChain(handle, item.cluster)) != E_OK)
    return res;

  if ((res = markFree(handle, &item)) != E_OK)
    return res;
  return E_OK;
}
#else
static enum result fatRemove(void *object __attribute__((unused)),
    const char *path __attribute__((unused)))
{
  return E_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
/*------------------File functions--------------------------------------------*/
static void fatClose(void *object)
{
#ifdef FAT_WRITE
  /* Write changes when file was opened for writing or updating */
  if (((struct FatFile *)object)->mode != FS_READ)
    fatFlush(object);
#endif

  deinit(object);
}
/*----------------------------------------------------------------------------*/
static bool fatEof(void *object)
{
  struct FatFile *fileHandle = object;

  return fileHandle->position >= fileHandle->size;
}
/*----------------------------------------------------------------------------*/
static uint32_t fatRead(void *object, uint8_t *buffer, uint32_t count)
{
  struct FatFile *fileHandle = object;
  struct FatHandle *handle = (struct FatHandle *)fileHandle->parent.descriptor;
  uint32_t read = 0;
  uint16_t chunk, offset;
  uint8_t current = 0;

  if (!(fileHandle->mode & (FS_READ | FS_UPDATE)))
    return 0;
  if (count > fileHandle->size - fileHandle->position)
    count = fileHandle->size - fileHandle->position;

  if (fileHandle->position)
  {
    current = sectorInCluster(handle, fileHandle->position);
    if (!current && !(fileHandle->position & (SECTOR_SIZE - 1)))
      current = 1 << handle->clusterSize;
  }
  while (count)
  {
    if (current >= (1 << handle->clusterSize))
    {
      if (getNextCluster(handle, &fileHandle->currentCluster) != E_OK)
        return 0; /* Sector read error or end-of-file */
      current = 0;
    }

    /* Position in sector */
    offset = (fileHandle->position + read) & (SECTOR_SIZE - 1);
    if (offset || count < SECTOR_SIZE) /* Position within sector */
    {
      /* Length of remaining sector space */
      chunk = SECTOR_SIZE - offset;
      chunk = count < chunk ? count : chunk;
      if (readSector(handle, getSector(handle, fileHandle->currentCluster)
          + current, handle->buffer, 1) != E_OK)
      {
        return 0;
      }
      memcpy(buffer + read, handle->buffer + offset, chunk);
      if (chunk + offset >= SECTOR_SIZE)
        ++current;
    }
    else /* Position aligned with start of sector */
    {
      /* Length of remaining cluster space */
      chunk = (SECTOR_SIZE << handle->clusterSize) - (current << SECTOR_POW);
      chunk = (count < chunk) ? count & ~(SECTOR_SIZE - 1) : chunk;
      if (readSector(handle, getSector(handle, fileHandle->currentCluster)
          + current, buffer + read, chunk >> SECTOR_POW) != E_OK)
      {
        return 0;
      }
      current += chunk >> SECTOR_POW;
    }

    read += chunk;
    count -= chunk;
  }

  fileHandle->position += read;
  return read;
}
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static uint32_t fatWrite(void *object, const uint8_t *buffer, uint32_t count)
{
  struct FatFile *fileHandle = object;
  struct FatHandle *handle = (struct FatHandle *)fileHandle->parent.descriptor;
  uint32_t sector, written = 0;
  uint16_t chunk, offset;
  uint8_t current = 0; /* Current sector of the data cluster */

  if (!(fileHandle->mode & (FS_WRITE | FS_APPEND | FS_UPDATE)))
    return 0;
  if (!fileHandle->size)
  {
    if (allocateCluster(handle, &fileHandle->cluster) != E_OK)
      return 0;
    fileHandle->currentCluster = fileHandle->cluster;
  }
  /* Checking file size limit (4 GiB - 1) */
  if (fileHandle->size + count > FILE_SIZE_MAX)
    count = FILE_SIZE_MAX - fileHandle->size;

  if (fileHandle->position)
  {
    current = sectorInCluster(handle, fileHandle->position);
    if (!current && !(fileHandle->position & (SECTOR_SIZE - 1)))
      current = 1 << handle->clusterSize;
  }
  while (count)
  {
    if (current >= (1 << handle->clusterSize))
    {
      enum result res;

      /* Allocate new cluster when next cluster does not exist */
      res = getNextCluster(handle, &fileHandle->currentCluster);
      if ((res != E_FAULT && res != E_OK) || (res == E_FAULT
          && allocateCluster(handle, &fileHandle->currentCluster) != E_OK))
      {
          return 0;
      }
      current = 0;
    }

    /* Position in sector */
    offset = (fileHandle->position + written) & (SECTOR_SIZE - 1);
    if (offset || count < SECTOR_SIZE) /* Position within sector */
    {
      /* Length of remaining sector space */
      chunk = SECTOR_SIZE - offset;
      chunk = (count < chunk) ? count : chunk;
      sector = getSector(handle, fileHandle->currentCluster) + current;
      if (readSector(handle, sector, handle->buffer, 1) != E_OK)
        return 0;
      memcpy(handle->buffer + offset, buffer + written, chunk);
      if (writeSector(handle, sector, handle->buffer, 1) != E_OK)
        return 0;
      if (chunk + offset >= SECTOR_SIZE)
        ++current;
    }
    else /* Position aligned with start of sector */
    {
      /* Length of remaining cluster space */
      chunk = (SECTOR_SIZE << handle->clusterSize) - (current << SECTOR_POW);
      chunk = (count < chunk) ? count & ~(SECTOR_SIZE - 1) : chunk;
      if (writeSector(handle, getSector(handle, fileHandle->currentCluster)
          + current, buffer + written, chunk >> SECTOR_POW) != E_OK)
      {
        return 0;
      }
      current += chunk >> SECTOR_POW;
    }

    written += chunk;
    count -= chunk;
  }

  fileHandle->size += written;
  fileHandle->position = fileHandle->size;
  return written;
}
#else
static uint32_t fatWrite(void *object __attribute__((unused)),
    const uint8_t *buffer __attribute__((unused)),
    uint32_t count __attribute__((unused)))
{
  return 0;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result fatFlush(void *object)
{
  struct FatFile *fileHandle = object;
  struct FatHandle *handle = (struct FatHandle *)fileHandle->parent.descriptor;
  struct DirEntryImage *ptr;
  uint32_t sector;
  enum result res;

  if (!(fileHandle->mode & (FS_WRITE | FS_APPEND | FS_UPDATE)))
    return E_ERROR;
  sector = getSector(handle, fileHandle->parentCluster)
      + E_SECTOR(fileHandle->parentIndex);
  if ((res = readSector(handle, sector, handle->buffer, 1)) != E_OK)
    return res;
  /* Pointer to entry position in sector */
  ptr = (struct DirEntryImage *)(handle->buffer
      + E_OFFSET(fileHandle->parentIndex));
  /* Update first cluster when writing to empty file or truncating file */
  ptr->clusterHigh = fileHandle->cluster >> 16;
  ptr->clusterLow = fileHandle->cluster;
  /* Update file size */
  ptr->size = fileHandle->size;
#ifdef FAT_TIME
  /* Update last modified date */
  //FIXME rewrite
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif
  if ((res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
    return res;
  return E_OK;
}
#else
static enum result fatFlush(void *object __attribute__((unused)))
{
  return E_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
//TODO Rewrite
static enum result fatSeek(void *object, asize_t offset,
    enum fsSeekOrigin origin)
{
  struct FatFile *fileHandle = object;
  struct FatHandle *handle = (struct FatHandle *)fileHandle->parent.descriptor;
  uint32_t clusterCount, current;
  enum result res;

  switch (origin)
  {
    case FS_SEEK_CUR:
      offset = fileHandle->position + offset;
      break;
    case FS_SEEK_END: /* Offset must be negative */
      offset = fileHandle->size + offset;
      break;
    case FS_SEEK_SET:
    default:
      break;
  }
  if (offset < 0 || offset > fileHandle->size)
    return E_ERROR;
  clusterCount = offset;
  if (offset > fileHandle->position)
  {
    current = fileHandle->currentCluster;
    clusterCount -= fileHandle->position;
  }
  else
    current = fileHandle->cluster;
  clusterCount >>= handle->clusterSize + SECTOR_POW;
  while (--clusterCount)
  {
    if ((res = getNextCluster(handle, &current)) != E_OK)
      return res;
  }
  fileHandle->position = offset;
  fileHandle->currentCluster = current;
  return E_OK;
}
/*----------------------------------------------------------------------------*/
static asize_t fatTell(void *object)
{
  return (asize_t)((struct FatFile *)object)->position;
}
/*----------------------------------------------------------------------------*/
/*------------------Directory functions---------------------------------------*/
static void fatCloseDir(void *object)
{
  deinit(object);
}
/*----------------------------------------------------------------------------*/
static enum result fatReadDir(void *object, char *name)
{
  struct FatDir *dirHandle = object;
  struct FatObject item;
  char entryName[FILE_NAME_MAX];
  enum result res;

  item.parent = dirHandle->currentCluster;
  /* Fetch next entry */
  item.index = dirHandle->currentIndex;
  do
  {
    if ((res = fetchEntry((struct FatHandle *)dirHandle->parent.descriptor,
        &item, entryName)) != E_OK)
    {
      return E_ENTRY;
    }
    ++item.index;
  }
  while (item.attribute & FLAG_VOLUME);
  /* Hidden and system entries not shown */
  dirHandle->currentIndex = item.index; /* Points to next item */
  dirHandle->currentCluster = item.parent;

  strcpy(name, entryName);
  return E_OK;
}
/*----------------------------------------------------------------------------*/
// enum result fatSeekDir(void *object, uint16_t pos)
// {
//   uint16_t clusterCount;
//   uint32_t current;

  //FIXME completely rewrite
//   if (dir->state != FS_OPENED) //FIXME
//     return E_ERROR;

  //TODO Search from current position
//   clusterCount = pos;
//   if (pos > dir->position)
//   {
//     current = dir->currentCluster;
//     clusterCount -= dir->position;
//   }
//   else
//     current = dir->cluster;
//   clusterCount >>= SECTOR_POW - 5 + dir->descriptor->clusterSize;

//   current = dir->cluster;
//   clusterCount = pos >> (E_POW + dir->descriptor->clusterSize);
//   while (clusterCount--)
//   {
//     if (getNextCluster(dir->descriptor, &current))
//       return E_INTERFACE;
//   }
//   dir->currentCluster = current;
// //   dir->position = pos;
//   return E_OK;
// }
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result fatMakeDir(void *object, const char *path)
{
  struct FatHandle *handle = object;
  struct DirEntryImage *ptr;
  const char *followedPath;
  uint32_t parent = handle->rootCluster, sector;
  struct FatObject item;
  enum result res;

  while (*path && (followedPath = followPath(handle, &item, path)))
  {
    parent = item.cluster;
    path = followedPath;
  }
  if (!*path) /* Entry with same name exists */
    return E_ERROR;

  /* Allocate cluster for directory entries */
  if ((res = allocateCluster(handle, &item.cluster)) != E_OK)
    return res;

  /* Create directory entry, file name have already been converted */
  item.attribute = FLAG_DIR; /* Create entry with directory attribute */
  item.size = 0;
  //TODO Initialize parent and index
  if ((res = createEntry(handle, &item, path)) != E_OK)
  {
    /* TODO Add return value checking */
    freeChain(handle, item.cluster);
    return res;
  }

  /* Fill cluster with zeros */
  memset(handle->buffer, 0, SECTOR_SIZE);
  sector = getSector(handle, item.cluster + 1); //TODO Check range
  while (--sector & ((1 << handle->clusterSize) - 1))
    if ((res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
      return res;

  item.size = 0;
  item.attribute = FLAG_DIR;

  /* Current directory entry . */
  ptr = (struct DirEntryImage *)handle->buffer;
  memset(ptr->filename, ' ', sizeof(ptr->filename));
  ptr->name[0] = '.';
  fillDirEntry(ptr, &item);

  /* Parent directory entry .. */
  ++ptr;
  memset(ptr->filename, ' ', sizeof(ptr->filename));
  ptr->name[0] = ptr->name[1] = '.';
  item.cluster = parent == handle->rootCluster ? 0 : parent;
  fillDirEntry(ptr, &item);

  if ((res = writeSector(handle, sector, handle->buffer, 1)) != E_OK)
    return res;
  return E_OK;
}
#else
static enum result fatMakeDir(void *object __attribute__((unused)),
    const char *path __attribute__((unused)))
{
  return E_ERROR;
}
#endif /* FAT_WRITE */
/*----------------------------------------------------------------------------*/
#ifdef FAT_WRITE
static enum result fatRemoveDir(void *object, const char *path)
{
  struct FatHandle *handle = object;
  struct FatObject dirItem, item;
  enum result res;

  while (path && *path)
    path = followPath(handle, &item, path);
  /* Not found, regular file or volume name */
  if (!path || !(item.attribute & FLAG_DIR) || item.attribute & FLAG_VOLUME)
    return E_ENTRY;

  /* Check if directory not empty */
  dirItem = item;
  dirItem.index = 2; /* Exclude . and .. */
  dirItem.parent = item.cluster;
  res = fetchEntry(handle, &dirItem, 0);
  if (res == E_OK)
    return E_ERROR;
  else if (res != E_FAULT)
    return res;

  /* Mark file table clusters as free */
  if ((res = freeChain(handle, item.cluster)) != E_OK)
    return res;

  if ((res = markFree(handle, &item)) != E_OK)
    return res;
  return E_OK;
}
#else
static enum result fatRemoveDir(void *object __attribute__((unused)),
    const char *path __attribute__((unused)))
{
  return E_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
#if defined(FAT_WRITE) && defined(DEBUG)
uint32_t countFree(void *object)
{
  struct FatHandle *handle = object;
  uint32_t *count = malloc(sizeof(uint32_t) * handle->tableCount);
  uint32_t current, empty = 0;
  uint16_t offset;
  uint8_t fat, i, j;

  if (!count)
    return 0; /* Memory allocation problem */
  for (fat = 0; fat < handle->tableCount; ++fat)
  {
    count[fat] = 0;
    for (current = 0; current < handle->clusterCount; ++current)
    {
      if (readSector(handle, handle->tableSector
          + (current >> TE_COUNT), handle->buffer, 1) != E_OK)
      {
        return empty;
      }
      offset = (current & ((1 << TE_COUNT) - 1)) << 2;
      if (clusterFree(*(uint32_t *)(handle->buffer + offset)))
        ++count[fat];
    }
  }
  for (i = 0; i < handle->tableCount; ++i)
    for (j = 0; j < handle->tableCount; ++j)
      if (i != j && count[i] != count[j])
      {
        printf("FAT records count differs: %u and %u\n", count[i], count[j]);
      }
  empty = count[0];
  free(count);
  return empty;
}
#endif
