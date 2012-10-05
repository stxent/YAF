#include <string.h>
#include "fat.h"
/*----------------------------------------------------------------------------*/
#ifdef FS_RTC_ENABLED
#include "rtc.h"
#endif
/*----------------------------------------------------------------------------*/
#ifdef DEBUG
#include <stdio.h>
#include <stdlib.h>
#endif
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
// /* Directory entry free flag */
// #define E_SIZE              0xE5
/*----------------------------------------------------------------------------*/
#define FS_FLAG_RO          0x01 /* Read only */
#define FS_FLAG_HIDDEN      0x02
#define FS_FLAG_SYSTEM      0x0C /* System (0x04) or volume label (0x08) */
#define FS_FLAG_DIR         0x10 /* Subdirectory */
#define FS_FLAG_ARCHIVE     0x20
/*----------------------------------------------------------------------------*/
#define CLUSTER_EOC_VAL     0x0FFFFFF8
/*----------------------------------------------------------------------------*/
/* File or directory entry size power */
#define E_POWER                     (SECTOR_SIZE - 5)
/* Table entries per FAT sector power */
#define TE_COUNT                    (SECTOR_SIZE - 2)
/* Table entry offset in FAT sector */
#define TE_OFFSET(arg)              (((arg) & ((1 << TE_COUNT) - 1)) << 2)
/* Directory entry position in cluster */
#define E_SECTOR(index)             ((index) >> E_POWER)
/* Directory entry offset in sector */
#define E_OFFSET(index)             (((index) << 5) & ((1 << SECTOR_SIZE) - 1))
/*----------------------------------------------------------------------------*/
/*------------------Inline functions------------------------------------------*/
/*----------------------------------------------------------------------------*/
static inline uint32_t clusterFree(uint32_t);
static inline uint32_t clusterEOC(uint32_t);
static inline uint32_t clusterUsed(uint32_t);
static inline uint32_t getSector(struct FsHandle *fsDesc, uint32_t);
static inline uint16_t entryCount(struct FsHandle *fsDesc);
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static inline uint32_t clusterFree(uint32_t cluster)
{
  return (((cluster) & 0x0FFFFFFF) == 0x00000000);
}
/*----------------------------------------------------------------------------*/
static inline uint32_t clusterEOC(uint32_t cluster)
{
  return (((cluster) & 0x0FFFFFF8) == 0x0FFFFFF8);
}
/*----------------------------------------------------------------------------*/
static inline uint32_t clusterUsed(uint32_t cluster)
{
  return (((cluster) & 0x0FFFFFFF) >= 0x00000002 &&
      ((cluster) & 0x0FFFFFFF) <= 0x0FFFFFEF);
}
/*----------------------------------------------------------------------------*/
/* Calculate sector position from cluster */
static inline uint32_t getSector(struct FsHandle *fsDesc, uint32_t cluster)
{
  return (fsDesc->dataSector + (((cluster) - 2) << fsDesc->clusterSize));
}
/*----------------------------------------------------------------------------*/
/* File or directory entries per directory cluster */
static inline uint16_t entryCount(struct FsHandle *fsDesc)
{
  return (1 << E_POWER << fsDesc->clusterSize);
}
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/* Filesystem object describing directory or file */
struct fsObject
{
  uint8_t attribute; /* File or directory attributes */
  uint16_t index; /* Entry position in parent cluster */
  uint32_t cluster; /* First cluster of entry */
  uint32_t parent; /* Directory cluster where entry located */
  uint32_t size; /* File size or zero for directories */
  char name[FS_NAME_MAX];
};
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/* Structure directory entry located in memory buffer */
struct dirEntryImage
{
  union
  {
    char filename[11];
    struct
    {
      char name[8];
      char extension[3];
    } __attribute__((packed));
  };
  uint8_t flags;
  char unused[8];
  uint16_t clusterHigh; /* Starting cluster high word */
  uint16_t time;
  uint16_t date;
  uint16_t clusterLow; /* Starting cluster low word */
  uint32_t size;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
/* Structure of boot sector located in memory buffer */
struct bootSectorImage
{
  char unused0[11];
  uint16_t bytesPerSector;
  uint8_t sectorsPerCluster;
  uint16_t reservedSectors;
  uint8_t fatCopies;
  char unused1[15];
  uint32_t partitionSize; /* Sectors per partition */
  uint32_t fatSize; /* Sectors per FAT record */
  char unused2[4];
  uint32_t rootCluster; /* Root directory cluster */
  uint16_t infoSector; /* Sector number for information sector */
  char unused3[460];
  uint16_t bootSignature;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
/* Structure info sector located in memory buffer */
struct infoSectorImage
{
  uint32_t firstSignature;
  char unused0[480];
  uint32_t infoSignature;
  uint32_t freeClusters;
  uint32_t lastAllocated;
  char unused1[14];
  uint16_t bootSignature;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static enum fsResult readSector(struct FsHandle *, uint32_t);
static enum fsResult burstReadSector(struct FsHandle *, uint32_t,
    uint8_t *, uint8_t);
static enum fsResult fetchEntry(struct FsHandle *, struct fsObject *);
static const char *followPath(struct FsHandle *, struct fsObject *,
    const char *);
static enum fsResult getNextCluster(struct FsHandle *, uint32_t *);
static const char *getChunk(const char *, char *);
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
static enum fsResult writeSector(struct FsHandle *, uint32_t);
static enum fsResult burstWriteSector(struct FsHandle *, uint32_t,
    const uint8_t *, uint8_t);
static enum fsResult truncate(struct FsFile *);
static enum fsResult freeChain(struct FsHandle *, uint32_t);
static enum fsResult allocateCluster(struct FsHandle *, uint32_t *);
static enum fsResult createEntry(struct FsHandle *, struct fsObject *,
    const char *);
static enum fsResult updateTable(struct FsHandle *, uint32_t);
#endif
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
void fsSetIO(struct FsDevice *fsDev, fsDevRead rFunc, fsDevWrite wFunc)
{
  fsDev->read = rFunc;
  fsDev->write = wFunc;
}
/*----------------------------------------------------------------------------*/
enum fsResult fsLoad(struct FsHandle *fsDesc, struct FsDevice *fsDev)
{
  uint16_t tmp;
  struct bootSectorImage *boot;
#ifdef FS_WRITE_ENABLED
  struct infoSectorImage *info;
#endif

//   fsDesc->buffer = fsDev->buffer;
  fsDesc->device = fsDev;
  /* Read first sector */
  fsDesc->currentSector = 0;
  if (readSector(fsDesc, 0))
    return FS_READ_ERROR;
  boot = (struct bootSectorImage *)fsDesc->device->buffer;
  /* Check boot sector signature (55AA at 0x01FE) */
  if (boot->bootSignature != 0xAA55)
    return FS_DEVICE_ERROR;
  /* Check sector size, fixed size of 2^SECTOR_SIZE allowed */
  if (boot->bytesPerSector != (1 << SECTOR_SIZE))
    return FS_DEVICE_ERROR;
  /* Calculate sectors per cluster count */
  tmp = boot->sectorsPerCluster;
  fsDesc->clusterSize = 0;
  while (tmp >>= 1)
    fsDesc->clusterSize++;

  fsDesc->tableSector = boot->reservedSectors;
  fsDesc->dataSector = fsDesc->tableSector + boot->fatCopies * boot->fatSize;
  fsDesc->rootCluster = boot->rootCluster;
#ifdef FS_WRITE_ENABLED
  fsDesc->tableCount = boot->fatCopies;
  fsDesc->tableSize = boot->fatSize;
  fsDesc->clusterCount = ((boot->partitionSize -
      fsDesc->dataSector) >> fsDesc->clusterSize) + 2;
  fsDesc->infoSector = boot->infoSector;
#ifdef DEBUG
  printf("Partition sectors count: %d\n", boot->partitionSize);
#endif

  if (readSector(fsDesc, fsDesc->infoSector))
    return FS_READ_ERROR;
  info = (struct infoSectorImage *)fsDesc->device->buffer;
  /* Check info sector signatures (RRaA at 0x0000 and rrAa at 0x01E4) */
  if (info->firstSignature != 0x41615252 || info->infoSignature != 0x61417272)
    return FS_DEVICE_ERROR;
  fsDesc->lastAllocated = info->lastAllocated;
#ifdef DEBUG
  printf("Free clusters: %d\n", info->freeClusters);
#endif
#endif
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
void fsUnload(struct FsHandle *fsDesc)
{
  fsDesc->device = 0;
}
/*----------------------------------------------------------------------------*/
static enum fsResult getNextCluster(struct FsHandle *fsDesc, uint32_t *cluster)
{
  uint32_t nextCluster;

  if (readSector(fsDesc, fsDesc->tableSector + (*cluster >> TE_COUNT)))
    return FS_READ_ERROR;
  nextCluster = *((uint32_t *)(fsDesc->device->buffer + TE_OFFSET(*cluster)));
  if (clusterUsed(nextCluster))
  {
    *cluster = nextCluster;
    return FS_OK;
  }
  else
    return FS_EOF;
}
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
#ifdef DEBUG
uint32_t countFree(struct FsHandle *fsDesc)
{
  uint32_t res, current;
  uint32_t *count = (uint32_t *)malloc(sizeof(uint32_t) * fsDesc->tableCount);
  uint8_t fat, i, j;
  uint16_t offset;

  for (fat = 0; fat < fsDesc->tableCount; fat++)
  {
    count[fat] = 0;
    for (current = 0; current < fsDesc->clusterCount; current++)
    {
      if (readSector(fsDesc, fsDesc->tableSector + (current >> TE_COUNT)))
        return FS_READ_ERROR;
      offset = (current & ((1 << TE_COUNT) - 1)) << 2;
      if (clusterFree(*((uint32_t *)(fsDesc->device->buffer + offset))))
        count[fat]++;
    }
  }
  for (i = 0; i < fsDesc->tableCount; i++)
    for (j = 0; j < fsDesc->tableCount; j++)
      if ((i != j) && (count[i] != count[j]))
      {
        printf("FAT records differ: %d and %d\n", count[i], count[j]);
      }
  res = count[0];
  free(count);
  return res;
}
#endif
#endif
/*----------------------------------------------------------------------------*/
static enum fsResult readSector(struct FsHandle *fsDesc, uint32_t sector)
{
  if (sector && (sector == fsDesc->currentSector))
    return FS_OK;
  if (fsDesc->device->read(fsDesc->device, sector, fsDesc->device->buffer, 1))
    return FS_READ_ERROR;
  fsDesc->currentSector = sector;
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
static enum fsResult burstReadSector(struct FsHandle *fsDesc,
    uint32_t sector, uint8_t *buffer, uint8_t count)
{
  if (fsDesc->device->read(fsDesc->device, sector, buffer, count))
    return FS_READ_ERROR;
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
static enum fsResult writeSector(struct FsHandle *fsDesc, uint32_t sector)
{
  if (fsDesc->device->write(fsDesc->device, sector, fsDesc->device->buffer, 1))
    return FS_WRITE_ERROR;
  fsDesc->currentSector = sector;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
static enum fsResult burstWriteSector(struct FsHandle *fsDesc, uint32_t sector,
    const uint8_t *buffer, uint8_t count)
{
  if (fsDesc->device->write(fsDesc->device, sector, buffer, count))
    return FS_WRITE_ERROR;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
/* Copy current sector into FAT sectors located at offset */
#ifdef FS_WRITE_ENABLED
static enum fsResult updateTable(struct FsHandle *fsDesc, uint32_t offset)
{
  uint8_t fat;

  for (fat = 0; fat < fsDesc->tableCount; fat++)
  {
    if (writeSector(fsDesc, fsDesc->tableSector +
        (uint32_t)fat * fsDesc->tableSize + offset))
      return FS_WRITE_ERROR;
  }
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
static enum fsResult allocateCluster(struct FsHandle *fsDesc,
    uint32_t *cluster)
{
  struct infoSectorImage *info;
  uint16_t offset;
  uint32_t current = fsDesc->lastAllocated + 1;

  for (; current != fsDesc->lastAllocated; current++)
  {
    if (current >= fsDesc->clusterCount)
#ifdef DEBUG
    {
      printf("Reached end of partition, continue from third cluster\n");
      current = 2;
    }
#else
      current = 2;
#endif
    if (readSector(fsDesc, fsDesc->tableSector + (current >> TE_COUNT)))
      return FS_READ_ERROR;
    offset = (current & ((1 << TE_COUNT) - 1)) << 2;
    /* Is cluster free */
    if (clusterFree(*((uint32_t *)(fsDesc->device->buffer + offset))))
    {
      *((uint32_t *)(fsDesc->device->buffer + offset)) = CLUSTER_EOC_VAL;
      if ((!*cluster || (*cluster >> TE_COUNT != current >> TE_COUNT)) &&
          updateTable(fsDesc, (current >> TE_COUNT)))
      {
        return FS_WRITE_ERROR;
      }
      if (*cluster)
      {
        if (readSector(fsDesc, fsDesc->tableSector + (*cluster >> TE_COUNT)))
          return FS_READ_ERROR;
        *((uint32_t *)(fsDesc->device->buffer + TE_OFFSET(*cluster))) =
            current;
        if (updateTable(fsDesc, *cluster >> TE_COUNT))
          return FS_WRITE_ERROR;
      }
#ifdef DEBUG
      printf("Allocated new cluster: %d, reference %d\n", current, *cluster);
#endif
      *cluster = current;
      /* Update information sector */
      if (readSector(fsDesc, fsDesc->infoSector))
        return FS_READ_ERROR;
      info = (struct infoSectorImage *)fsDesc->device->buffer;
      /* Set last allocated cluster */
      info->lastAllocated = current;
      fsDesc->lastAllocated = current;
      /* Update free clusters count */
      info->freeClusters--;
      if (writeSector(fsDesc, fsDesc->infoSector))
        return FS_WRITE_ERROR;
      return FS_OK;
    }
  }
#ifdef DEBUG
  printf("Allocation error, possibly partition is full\n");
#endif
  return FS_ERROR;
}
#endif
/*----------------------------------------------------------------------------*/
//TODO rewrite to support LFN
/* dest length is 13: 8 name + dot + 3 extension + null */
static const char *getChunk(const char *src, char *dest)
{
  uint8_t counter = 0;

  if (!*src)
    return src;
  if (*src == '/')
  {
    *dest++ = '/';
    *dest = '\0';
    return (src + 1);
  }
  while (*src && (counter++ < 12))
  {
    if (*src == '/')
    {
      src++;
      break;
    }
    if (*src == ' ')
    {
      src++;
      continue;
    }
    *dest++ = *src++;
  }
  *dest = '\0';
  return src;
}
/*----------------------------------------------------------------------------*/
/* Members entry->index and entry->parent have to be initialized */
static enum fsResult fetchEntry(struct FsHandle *fsDesc, struct fsObject *entry)
{
  struct dirEntryImage *ptr;
  uint32_t sector;

  entry->attribute = 0;
  entry->cluster = 0;
  entry->size = 0;
  while (1)
  {
    if (entry->index >= entryCount(fsDesc))
    {
      /* Check clusters until end of directory (EOC entry in FAT) */
      if (getNextCluster(fsDesc, &(entry->parent)))
        return FS_READ_ERROR;
      entry->index = 0;
    }
    sector = fsDesc->dataSector + E_SECTOR(entry->index) +
        ((entry->parent - 2) << fsDesc->clusterSize);
    if (readSector(fsDesc, sector))
      return FS_READ_ERROR;
    ptr = (struct dirEntryImage *)(fsDesc->device->buffer +
        E_OFFSET(entry->index));
    if (!ptr->name[0]) /* No more entries */
      return FS_EOF;
    if (ptr->name[0] != (char)0xE5) /* Entry exists */
      break;
    entry->index++;
  }
  entry->attribute = ptr->flags;
  /* Copy file size, when entry is not directory */
  if (!(entry->attribute & FS_FLAG_DIR))
    entry->size = ptr->size;
  entry->cluster = (ptr->clusterHigh << 16) | ptr->clusterLow;
// #ifdef DEBUG
//   entry->time = ptr->time;
//   entry->date = ptr->date;
// #endif
  /* Copy entry name */
  memcpy(entry->name, ptr->name, 8);
  /* Add dot, when entry is not directory or extension exists */
  if (!(entry->attribute & FS_FLAG_DIR) && (ptr->extension[0] != ' '))
  {
    //TODO add LFN support
    entry->name[8] = '.';
    /* Copy entry extension */
    memcpy(entry->name + 9, ptr->extension, 3);
    entry->name[12] = '\0';
  }
  else
    entry->name[8] = '\0';
  getChunk(entry->name, entry->name);
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
static const char *followPath(struct FsHandle *fsDesc, struct fsObject *item,
    const char *path)
{
  char name[FS_NAME_MAX];

  path = getChunk(path, name);
  if (!strlen(name))
    return 0;
  if (name[0] == '/')
  {
    item->size = 0;
    item->cluster = fsDesc->rootCluster;
    item->attribute = FS_FLAG_DIR;
    return path;
  }
  item->parent = item->cluster;
  item->index = 0;
  while (!fetchEntry(fsDesc, item))
  {
    if (!strcmp(item->name, name))
      return path;
    item->index++;
  }
  return 0;
}
/*----------------------------------------------------------------------------*/
enum fsResult fsStat(struct FsHandle *fsDesc, const char *path,
    struct FsStat *stat)
{
  const char *followedPath;
  struct fsObject item;
  uint32_t sector;
#ifdef FS_RTC_ENABLED
  struct dirEntryImage *ptr;
  struct Time tm;
#endif

  while (*path && (followedPath = followPath(fsDesc, &item, path)))
    path = followedPath;
  /* Non-zero when entry not found */
  if (*path)
    return FS_NOT_FOUND;

  sector = getSector(fsDesc, item.parent) + E_SECTOR(item.index);
  if (readSector(fsDesc, sector))
    return FS_READ_ERROR;
#ifdef FS_RTC_ENABLED
  ptr = (struct dirEntryImage *)(fsDesc->device->buffer +
      E_OFFSET(item.index));
#endif

#ifdef DEBUG
  stat->cluster = item.cluster;
  stat->pcluster = item.parent;
  stat->pindex = item.index;
#endif
  stat->size = item.size;
  if (item.attribute & FS_FLAG_DIR)
    stat->type = FS_TYPE_DIR;
  else
    stat->type = FS_TYPE_REG;

#ifdef DEBUG
  stat->access = 07; /* rwx */
  if (item.attribute & FS_FLAG_RO)
    stat->access &= 05;
#endif

#ifdef FS_RTC_ENABLED
  tm.sec = ptr->time & 0x1F;
  tm.min = (ptr->time >> 5) & 0x3F;
  tm.hour = (ptr->time >> 11) & 0x1F;
  tm.day = ptr->date & 0x1F;
  tm.mon = (ptr->date >> 5) & 0x0F;
  tm.year = ((ptr->date >> 9) & 0x7F) + 1980;
  stat->atime = unixTime(&tm);
#endif
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
enum fsResult fsOpen(struct FsHandle *fsDesc, struct FsFile *fileDesc,
    const char *path, enum fsMode mode)
{
  const char *followedPath;
  struct fsObject item;

  fileDesc->descriptor = 0;
  fileDesc->mode = mode;
  while (*path && (followedPath = followPath(fsDesc, &item, path)))
    path = followedPath;
  /* Non-zero when entry not found */
  if (*path)
  {
#ifdef FS_WRITE_ENABLED
    if (mode == FS_WRITE)
    {
      item.attribute = 0;
      if (createEntry(fsDesc, &item, path))
        return FS_ERROR;
    }
    else
      return FS_NOT_FOUND;
#else
    return FS_NOT_FOUND;
#endif
  }
  /* Not found if system, volume name or directory */
  if (item.attribute & (FS_FLAG_SYSTEM | FS_FLAG_DIR))
    return FS_NOT_FOUND;
#ifdef FS_WRITE_ENABLED
  /* Attempt to write into read-only file */
  if ((item.attribute & FS_FLAG_RO) && (mode == FS_WRITE || mode == FS_APPEND))
    return FS_ERROR;
#endif
  fileDesc->descriptor = fsDesc;
  fileDesc->cluster = item.cluster;
  fileDesc->currentCluster = item.cluster;
  fileDesc->currentSector = 0;
  fileDesc->position = 0;
  fileDesc->size = item.size;

#ifdef FS_WRITE_ENABLED
  fileDesc->parentCluster = item.parent;
  fileDesc->parentIndex = item.index;
  if ((mode == FS_WRITE) && !*path && fileDesc->size &&
      (truncate(fileDesc) != FS_OK))
  {
    fileDesc->descriptor = 0;
    return FS_ERROR;
  }
  /* In append mode file pointer moves to end of file */
  if ((mode == FS_APPEND) && (fsSeek(fileDesc, fileDesc->size) != FS_OK))
  {
    fileDesc->descriptor = 0;
    return FS_ERROR;
  }
#endif
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
bool fsEndOfFile(struct FsFile *fileDesc)
{
  return (fileDesc->position >= fileDesc->size);
}
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
static enum fsResult freeChain(struct FsHandle *fsDesc, uint32_t cluster)
{
  struct infoSectorImage *info;
  uint16_t freeCount = 0;
  uint32_t current = cluster;
  uint32_t next;

  if (!current)
    return FS_OK; /* Already empty */
  while (clusterUsed(current))
  {
    /* Get FAT sector with next cluster value */
    if (readSector(fsDesc, fsDesc->tableSector + (current >> TE_COUNT)))
      return FS_READ_ERROR;
    /* Free cluster */
    next = *((uint32_t *)(fsDesc->device->buffer + TE_OFFSET(current)));
    *((uint32_t *)(fsDesc->device->buffer + TE_OFFSET(current))) = 0;
#ifdef DEBUG
    if (current >> TE_COUNT != next >> TE_COUNT)
    {
      printf("FAT sectors differ, next: %d (0x%X), current %d\n",
             (next >> TE_COUNT), (next >> TE_COUNT),
             (current >> TE_COUNT));
    }
    printf("Cleared cluster: %d\n", current);
#endif
    if ((current >> TE_COUNT != next >> TE_COUNT) &&
        updateTable(fsDesc, current >> TE_COUNT))
    {
      return FS_WRITE_ERROR;
    }
    freeCount++;
    current = next;
  }
  /* Update information sector */
  if (readSector(fsDesc, fsDesc->infoSector))
    return FS_READ_ERROR;
  info = (struct infoSectorImage *)fsDesc->device->buffer;
  /* Set free clusters count */
  info->freeClusters += freeCount;
  if (writeSector(fsDesc, fsDesc->infoSector))
    return FS_WRITE_ERROR;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
static enum fsResult truncate(struct FsFile *fileDesc)
{
  struct dirEntryImage *ptr;
  uint32_t current;

  if (fileDesc->mode != FS_WRITE && fileDesc->mode != FS_APPEND)
    return FS_ERROR;
  if (freeChain(fileDesc->descriptor, fileDesc->cluster) != FS_OK)
    return FS_ERROR;
  current = getSector(fileDesc->descriptor, fileDesc->parentCluster) +
      E_SECTOR(fileDesc->parentIndex);
  if (readSector(fileDesc->descriptor, current))
    return FS_READ_ERROR;
  /* Pointer to entry position in sector */
  ptr = (struct dirEntryImage *)(fileDesc->descriptor->device->buffer +
      E_OFFSET(fileDesc->parentIndex));
  /* Update size and first cluster */
  ptr->size = 0;
  ptr->clusterHigh = 0;
  ptr->clusterLow = 0;
#ifdef FS_RTC_ENABLED
  /* Update last modified date */
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif
  if (writeSector(fileDesc->descriptor, current))
    return FS_WRITE_ERROR;
  fileDesc->cluster = 0;
  fileDesc->currentCluster = 0;
  fileDesc->currentSector = 0;
  fileDesc->size = 0;
  fileDesc->position = 0;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
/* Create new entry inside entry->parent chain */
/* Members entry->parent and entry->attribute have to be initialized */
static enum fsResult createEntry(struct FsHandle *fsDesc,
    struct fsObject *entry, const char *name)
{
  struct dirEntryImage *ptr;
  uint8_t pos;
//  uint16_t clusterCount = 0; /* Followed clusters count */
  uint32_t sector;

  entry->cluster = 0;
  entry->index = 0;
  for (pos = 0; *(name + pos); pos++)
  {
    if (name[pos] == '/')
      return FS_ERROR; /* One of directories in path does not exist */
  }
  while (1)
  {
    if (entry->index >= entryCount(fsDesc))
    {
      /* Try to get next cluster or allocate new cluster for directory */
      /* Max directory size is 2^16 entries */
      //TODO Add file limit
      //if (getNextCluster(fsDesc, &(entry->parent)) && (clusterCount < (1 << (16 - ENTRY_COUNT - fsDesc->clusterSize))))
      if (getNextCluster(fsDesc, &(entry->parent)))
      {
        if (allocateCluster(fsDesc, &(entry->parent)))
          return FS_ERROR;
        else
        {
          sector = getSector(fsDesc, entry->parent);
          memset(fsDesc->device->buffer, 0, (1 << SECTOR_SIZE));
          for (pos = 0; pos < 1 << fsDesc->clusterSize; pos++)
          {
            if (writeSector(fsDesc, sector + pos))
              return FS_WRITE_ERROR;
          }
        }
      }
      else
        return FS_ERROR; /* Directory full */
      entry->index = 0;
//      clusterCount++; //FIXME remove?
    }
    sector = fsDesc->dataSector + E_SECTOR(entry->index) +
        ((entry->parent - 2) << fsDesc->clusterSize);
    if (readSector(fsDesc, sector))
      return FS_ERROR;
    ptr = (struct dirEntryImage *)(fsDesc->device->buffer +
        E_OFFSET(entry->index));
    /* Empty or removed entry */
    if (!ptr->name[0] || ptr->name[0] == (char)0xE5)
      break;
    entry->index++;
  }

  /* Clear name and extension */
  memset(ptr->filename, ' ', sizeof(ptr->filename));
  for (pos = 0; *name && *name != '.' && pos < sizeof(ptr->name); pos++)
    ptr->name[pos] = *name++;
  if (!(entry->attribute & FS_FLAG_DIR) && *name == '.')
  {
    for (pos = 0, name++; *name && pos < sizeof(ptr->extension); pos++)
      ptr->extension[pos] = *name++;
  }
  /* Fill entry fields with zeros */
  memset(ptr->unused, 0, sizeof(ptr->unused));
  ptr->flags = entry->attribute;
  ptr->clusterHigh = 0;
  ptr->clusterLow = 0;
  ptr->size = 0;
#ifdef FS_RTC_ENABLED
  /* Last modified time and date */
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif
  if (writeSector(fsDesc, sector))
    return FS_WRITE_ERROR;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
void fsClose(struct FsFile *fileDesc)
{
  fileDesc->descriptor = 0;
}
/*----------------------------------------------------------------------------*/
//TODO test behavior
enum fsResult fsSeek(struct FsFile *fileDesc, uint32_t pos)
{
  uint32_t clusterCount, current;

  if (pos > fileDesc->size)
    return FS_ERROR;
  clusterCount = pos;
  if (pos > fileDesc->position)
  {
    current = fileDesc->currentCluster;
    clusterCount -= fileDesc->position;
  }
  else
    current = fileDesc->cluster;
  clusterCount >>= fileDesc->descriptor->clusterSize + SECTOR_SIZE;
  while (clusterCount--)
  {
    if (getNextCluster(fileDesc->descriptor, &current))
      return FS_READ_ERROR;
  }
  fileDesc->currentCluster = current;
  fileDesc->position = pos;
  fileDesc->currentSector = (pos >> SECTOR_SIZE) & //TODO add macro?
      ((1 << fileDesc->descriptor->clusterSize) - 1);
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
enum fsResult fsWrite(struct FsFile *fileDesc, uint8_t *buffer,
    uint16_t count, uint16_t *result)
{
  struct dirEntryImage *ptr;
  uint16_t chunk, offset, written = 0;
  uint32_t tmpSector;

  if (fileDesc->mode != FS_APPEND && fileDesc->mode != FS_WRITE)
    return FS_ERROR;
  if (!fileDesc->size)
  {
    if (allocateCluster(fileDesc->descriptor, &(fileDesc->cluster)))
      return FS_ERROR;
    fileDesc->currentCluster = fileDesc->cluster;
  }
  /* Checking file size limit (2 GiB) */
  if (fileDesc->size + count > 0x7FFFFFFF) //TODO calc from params?
    count = 0x7FFFFFFF - fileDesc->size;

  while (count)
  {
    if (fileDesc->currentSector >= (1 << fileDesc->descriptor->clusterSize))
    {
      if (allocateCluster(fileDesc->descriptor, &fileDesc->currentCluster))
        return FS_WRITE_ERROR;
      fileDesc->currentSector = 0;
    }

    /* Position in sector */
    offset = (fileDesc->position + written) & ((1 << SECTOR_SIZE) - 1);
    if (offset || count < (1 << SECTOR_SIZE)) /* Position within sector */
    {
      /* Length of remaining sector space */
      chunk = (1 << SECTOR_SIZE) - offset;
      chunk = (count < chunk) ? count : chunk;
      tmpSector = getSector(fileDesc->descriptor, fileDesc->currentCluster) +
          fileDesc->currentSector;
      if (readSector(fileDesc->descriptor, tmpSector))
        return FS_READ_ERROR;
      memcpy(fileDesc->descriptor->device->buffer + offset,
          buffer + written, chunk);
      if (writeSector(fileDesc->descriptor, tmpSector))
        return FS_WRITE_ERROR;
      if (chunk + offset >= (1 << SECTOR_SIZE))
        fileDesc->currentSector++;
    }
    else /* Position aligned with start of sector */
    {
      /* Length of remaining cluster space */
      chunk = ((1 << SECTOR_SIZE) << fileDesc->descriptor->clusterSize) -
          (fileDesc->currentSector << SECTOR_SIZE);
      chunk = (count < chunk) ? count & ~((1 << SECTOR_SIZE) - 1) : chunk;
#ifdef DEBUG
      printf("Burst write position %d, chunk size %d, sector count %d\n",
          written, chunk, chunk >> SECTOR_SIZE);
#endif
      //TODO rename fileDesc
      if (burstWriteSector(fileDesc->descriptor,
          getSector(fileDesc->descriptor, fileDesc->currentCluster) +
          fileDesc->currentSector, buffer + written, chunk >> SECTOR_SIZE))
      {
        return FS_READ_ERROR;
      }
      fileDesc->currentSector += chunk >> SECTOR_SIZE;
    }

    written += chunk;
    count -= chunk;
  }

  tmpSector = getSector(fileDesc->descriptor, fileDesc->parentCluster) +
      E_SECTOR(fileDesc->parentIndex);
  if (readSector(fileDesc->descriptor, tmpSector))
    return FS_READ_ERROR;
  /* Pointer to entry position in sector */
  ptr = (struct dirEntryImage *)(fileDesc->descriptor->device->buffer +
      E_OFFSET(fileDesc->parentIndex));
  /* Update first cluster when writing to empty file */
  if (!fileDesc->size)
  {
    ptr->clusterHigh = fileDesc->cluster >> 16;
    ptr->clusterLow = fileDesc->cluster;
  }
  fileDesc->size += written;
  fileDesc->position = fileDesc->size;
  /* Update file size */
  ptr->size = fileDesc->size;
#ifdef FS_RTC_ENABLED
  /* Update last modified date */
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif
  if (writeSector(fileDesc->descriptor, tmpSector))
    return FS_WRITE_ERROR;
  *result = written;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
enum fsResult fsRead(struct FsFile *fileDesc, uint8_t *buffer,
    uint16_t count, uint16_t *result)
{
  uint16_t chunk, offset, read = 0;

  if (fileDesc->mode != FS_READ)
    return FS_ERROR;
  if (count > fileDesc->size - fileDesc->position)
    count = fileDesc->size - fileDesc->position;
  if (!count)
    return FS_EOF;

  while (count)
  {
    if (fileDesc->currentSector >= (1 << fileDesc->descriptor->clusterSize))
    {
      if (getNextCluster(fileDesc->descriptor, &(fileDesc->currentCluster)))
        return FS_READ_ERROR;
      fileDesc->currentSector = 0;
    }

    /* Position in sector */
    offset = (fileDesc->position + read) & ((1 << SECTOR_SIZE) - 1);
    if (offset || count < (1 << SECTOR_SIZE)) /* Position within sector */
    {
      /* Length of remaining sector space */
      chunk = (1 << SECTOR_SIZE) - offset;
      chunk = (count < chunk) ? count : chunk;
      if (readSector(fileDesc->descriptor, getSector(fileDesc->descriptor,
          fileDesc->currentCluster) + fileDesc->currentSector))
        return FS_READ_ERROR;
      memcpy(buffer + read, fileDesc->descriptor->device->buffer + offset,
          chunk);
      if (chunk + offset >= (1 << SECTOR_SIZE))
        fileDesc->currentSector++;
    }
    else /* Position aligned with start of sector */
    {
      /* Length of remaining cluster space */
      chunk = ((1 << SECTOR_SIZE) << fileDesc->descriptor->clusterSize) -
          (fileDesc->currentSector << SECTOR_SIZE);
      chunk = (count < chunk) ? count & ~((1 << SECTOR_SIZE) - 1) : chunk;
#ifdef DEBUG
      printf("Burst read position %d, chunk size %d, sector count %d\n",
          read, chunk, chunk >> SECTOR_SIZE);
#endif
      if (burstReadSector(fileDesc->descriptor, getSector(fileDesc->descriptor,
          fileDesc->currentCluster) + fileDesc->currentSector, buffer + read,
          chunk >> SECTOR_SIZE))
        return FS_READ_ERROR;
      fileDesc->currentSector += chunk >> SECTOR_SIZE;
    }

    read += chunk;
    count -= chunk;
  }

  fileDesc->position += read;
  *result = read;
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
enum fsResult fsRemove(struct FsHandle *fsDesc, const char *path)
{
  struct dirEntryImage *ptr;
  uint16_t index;
  uint32_t tmp; /* Stores first cluster of entry or entry sector */
  uint32_t parent;
  struct fsObject item;

  while (path && *path)
    path = followPath(fsDesc, &item, path);
  if (!path)
    return FS_NOT_FOUND;
  /* Hidden, system, volume name */
  if (item.attribute & (FS_FLAG_HIDDEN | FS_FLAG_SYSTEM))
    return FS_NOT_FOUND;

  index = item.index;
  tmp = item.cluster; /* First cluster of entry */
  parent = item.parent;
  item.index = 2; /* Exclude . and .. */
  item.parent = item.cluster;
  /* Check if directory not empty */
  if ((item.attribute & FS_FLAG_DIR) && !fetchEntry(fsDesc, &item))
    return FS_ERROR;
  if (freeChain(fsDesc, tmp) != FS_OK)
    return FS_ERROR;

  /* Sector in FAT with entry description */
  tmp = getSector(fsDesc, parent) + E_SECTOR(index);
  if (readSector(fsDesc, tmp))
    return FS_READ_ERROR;
  /* Mark entry as free */
  ptr = (struct dirEntryImage *)(fsDesc->device->buffer + E_OFFSET(index));
  ptr->name[0] = (char)0xE5;
  if (writeSector(fsDesc, tmp))
    return FS_WRITE_ERROR;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
enum fsResult fsOpenDir(struct FsHandle *fsDesc, struct FsDir *dirDesc,
    const char *path)
{
  struct fsObject item;

  dirDesc->descriptor = 0;
  while (path && *path)
    path = followPath(fsDesc, &item, path);
  if (!path)
    return FS_NOT_FOUND;
  /* Hidden, system, volume name or not directory */
  if (!(item.attribute & FS_FLAG_DIR) || (item.attribute & FS_FLAG_SYSTEM))
    return FS_NOT_FOUND;
  dirDesc->descriptor = fsDesc;
  dirDesc->cluster = item.cluster;
  dirDesc->currentCluster = item.cluster;
  dirDesc->currentIndex = 0;
  return FS_OK;
}
/*----------------------------------------------------------------------------*/
void fsCloseDir(struct FsDir *dirDesc)
{
  dirDesc->descriptor = 0;
}
/*----------------------------------------------------------------------------*/
enum fsResult fsReadDir(struct FsDir *dirDesc, char *name)
{
  struct fsObject item;

  item.parent = dirDesc->currentCluster;
  /* Fetch next entry */
  item.index = dirDesc->currentIndex;
  do
  {
    if (fetchEntry(dirDesc->descriptor, &item))
      return FS_NOT_FOUND;
    item.index++;
  }
  while (item.attribute & (FS_FLAG_HIDDEN | FS_FLAG_SYSTEM));
  /* Hidden and system entries not shown */
  dirDesc->currentIndex = item.index; /* Points to next item */
  dirDesc->currentCluster = item.parent;

  strcpy(name, item.name);

  return FS_OK;
}
/*----------------------------------------------------------------------------*/
// enum fsResult fsSeekDir(struct FsDir *dirDesc, uint16_t pos)
// {
//   uint16_t clusterCount;
//   uint32_t current;

  //FIXME completely rewrite
//   if (dirDesc->state != FS_OPENED) //FIXME
//     return FS_ERROR;

  //TODO Search from current position
//   clusterCount = pos;
//   if (pos > dirDesc->position)
//   {
//     current = dirDesc->currentCluster;
//     clusterCount -= dirDesc->position;
//   }
//   else
//     current = dirDesc->cluster;
//   clusterCount >>= SECTOR_SIZE - 5 + dirDesc->descriptor->clusterSize;

//   current = dirDesc->cluster;
//   clusterCount = pos >> (E_POWER + dirDesc->descriptor->clusterSize);
//   while (clusterCount--)
//   {
//     if (getNextCluster(dirDesc->descriptor, &current))
//       return FS_READ_ERROR;
//   }
//   dirDesc->currentCluster = current;
// //   dirDesc->position = pos;
//   return FS_OK;
// }
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
enum fsResult fsMakeDir(struct FsHandle *fsDesc, const char *path)
{
  uint8_t pos;
  uint32_t tmpSector, parent = fsDesc->rootCluster;
  struct dirEntryImage *ptr;
  const char *followedPath;
  struct fsObject item;

  while (*path && (followedPath = followPath(fsDesc, &item, path)))
  {
    parent = item.cluster;
    path = followedPath;
  }
  if (!*path) /* Entry with same name exists */
    return FS_ERROR;
  item.attribute = FS_FLAG_DIR; /* Create entry with directory attribute */
  if (createEntry(fsDesc, &item, path) ||
      allocateCluster(fsDesc, &item.cluster))
    return FS_WRITE_ERROR;
  tmpSector = getSector(fsDesc, item.parent) + E_SECTOR(item.index);
  if (readSector(fsDesc, tmpSector))
    return FS_READ_ERROR;
  ptr = (struct dirEntryImage *)(fsDesc->device->buffer +
      E_OFFSET(item.index));
  ptr->clusterHigh = item.cluster >> 16;
  ptr->clusterLow = item.cluster;
  if (writeSector(fsDesc, tmpSector))
    return FS_WRITE_ERROR;
  tmpSector = getSector(fsDesc, item.cluster);

  /* Fill cluster with zeros */
  memset(fsDesc->device->buffer, 0, (1 << SECTOR_SIZE));
  for (pos = (1 << fsDesc->clusterSize) - 1; pos > 0; pos--)
  {
    if (writeSector(fsDesc, tmpSector + pos))
      return FS_WRITE_ERROR;
  }

  /* Current directory entry . */
  ptr = (struct dirEntryImage *)fsDesc->device->buffer;
  /* Fill name and extension with spaces */
  memset(ptr->filename, ' ', sizeof(ptr->filename)); //TODO check
  ptr->name[0] = '.';
  ptr->flags = FS_FLAG_DIR;
  ptr->clusterHigh = item.cluster >> 16;
  ptr->clusterLow = item.cluster;
#ifdef FS_RTS_ENABLED
  //FIXME rewrite
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif

  /* Previous directory entry .. */
//   ptr = (struct dirEntryImage *)(fsDesc->buffer + E_OFFSET(1)); FIXME check
  ptr++;
  /* Fill name and extension with spaces */
  memset(ptr->filename, ' ', sizeof(ptr->filename)); //TODO check
  ptr->name[0] = ptr->name[1] = '.';
  ptr->flags = FS_FLAG_DIR;
  if (parent != fsDesc->rootCluster)
  {
    ptr->clusterHigh = parent >> 16;
    ptr->clusterLow = parent;
  }
#ifdef FS_RTC_ENABLED
  //FIXME rewrite
  ptr->time = rtcGetTime();
  ptr->date = rtcGetDate();
#endif

  if (writeSector(fsDesc, tmpSector))
    return FS_WRITE_ERROR;
  return FS_OK;
}
#endif
/*----------------------------------------------------------------------------*/
#ifdef FS_WRITE_ENABLED
enum fsResult fsMove(struct FsHandle *fsDesc, const char *path,
    const char *newPath)
{
  uint8_t attribute/*, counter*/;
  uint16_t index;
  uint32_t parent, cluster, size, tmpSector;
  struct dirEntryImage *ptr;
  const char *followedPath;
  struct fsObject item;

  while (path && *path)
    path = followPath(fsDesc, &item, path);
  if (!path)
    return FS_NOT_FOUND;

  /* System entries are invisible */
  if (item.attribute & FS_FLAG_SYSTEM)
    return FS_NOT_FOUND;

  /* Save old entry data */
  attribute = item.attribute;
  index = item.index;
  parent = item.parent;
  cluster = item.cluster;
  size = item.size;

  while (*newPath && (followedPath = followPath(fsDesc, &item, newPath)))
    newPath = followedPath;
  if (!*newPath) /* Entry with same name exists */
    return FS_ERROR;

/*  if (parent == item.parent) //Same directory
  {
    tmpSector = getSector(fsDesc, parent) + ENTRY_SECTOR(index);
    if (readSector(fsDesc, tmpSector))
      return FS_READ_ERROR;
    ptr = fsDesc->buffer + ENTRY_OFFSET(index);
    memset(ptr, 0x20, 11);
    for (counter = 0; *newPath && (*newPath != '.') && (counter < 8); counter++)
      *((char *)(ptr + counter)) = *newPath++;
    if (!(attribute & FS_FLAG_DIR) && (*newPath == '.'))
    {
      for (counter = 8, name++; *newPath && (counter < 11); counter++)
        *((char *)(ptr + counter)) = *newPath++;
    }
    if (writeSector(fsDesc, tmpSector))
      return FS_WRITE_ERROR;

    return FS_OK;
  }
  else
  {
    item.attribute = attribute;
    if (createEntry(fsDesc, &item, newPath))
      return FS_WRITE_ERROR;
  }*/

  item.attribute = attribute;
  if (createEntry(fsDesc, &item, newPath))
    return FS_WRITE_ERROR;

  tmpSector = getSector(fsDesc, parent) + E_SECTOR(index);
  if (readSector(fsDesc, tmpSector))
    return FS_READ_ERROR;
  /* Set old entry as removed */
  *((uint8_t *)(fsDesc->device->buffer + E_OFFSET(index))) = (char)0xE5;
  if (writeSector(fsDesc, tmpSector))
    return FS_WRITE_ERROR;

  tmpSector = getSector(fsDesc, item.parent) + E_SECTOR(item.index);
  if (readSector(fsDesc, tmpSector))
    return FS_READ_ERROR;
  ptr = (struct dirEntryImage *)(fsDesc->device->buffer +
      E_OFFSET(item.index));
  ptr->clusterHigh = cluster >> 16;
  ptr->clusterLow = cluster;
  ptr->size = size;
  if (writeSector(fsDesc, tmpSector))
    return FS_WRITE_ERROR;

  return FS_OK;
}
#endif
