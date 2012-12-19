/*
 * fs.h
 * Copyright (C) 2012 xent
 * Project is distributed under the terms of the GNU General Public License v3.0
 */

/**
 *  @file
 *  Abstract filesystem interface for embedded system applications
 */

#ifndef FS_H_
#define FS_H_
/*----------------------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
/*----------------------------------------------------------------------------*/
#include "error.h"
#include "entity.h"
#include "interface.h"
/*----------------------------------------------------------------------------*/
/* Type represents address space width */
/* Must be greater or equal to used underlying device address space */
typedef int64_t asize_t;
/*----------------------------------------------------------------------------*/
enum fsMode
{
    FS_NONE = 0,
    /** File read mode */
    FS_READ,
    /** File write mode */
    FS_WRITE,
    /** File append mode */
    FS_APPEND
};
/*----------------------------------------------------------------------------*/
enum fsSeekOrigin
{
    /** Beginning of file */
    FS_SEEK_SET = 0,
    /** Current position of the file pointer */
    FS_SEEK_CUR,
    /** End of file */
    FS_SEEK_END
};
/*----------------------------------------------------------------------------*/
enum fsEntryType
{
    /** Unknown type */
    FS_TYPE_NONE = 0,
    /** Directory entry */
    FS_TYPE_DIR,
    /** Regular file */
    FS_TYPE_FILE,
    /** Symbolic link */
    FS_TYPE_LINK
};
/*----------------------------------------------------------------------------*/
struct FsFile;
struct FsDir;
struct FsHandle;
// struct FsStat;
/*----------------------------------------------------------------------------*/
/* TODO rewrite */
struct FsStat
{
  enum fsEntryType type;
  asize_t size;
  uint64_t atime;
#ifdef DEBUG
  uint16_t access;
  uint32_t cluster;
  uint32_t pcluster;
  uint16_t pindex;
#endif
};
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
struct FsFileClass
{
  CLASS_GENERATOR

  /* Virtual methods */
  void (*close)(void *);
  bool (*eof)(const void *);
  enum result (*flush)(void *);
  enum result (*read)(void *, uint8_t *, uint32_t, uint32_t *);
  enum result (*seek)(void *, asize_t, enum fsSeekOrigin);
  asize_t (*tell)(const void *);
  enum result (*write)(void *, const uint8_t *, uint32_t, uint32_t *);
};
/*----------------------------------------------------------------------------*/
struct FsFile
{
  struct Entity parent;

  struct FsHandle *descriptor;
  enum fsMode mode; /* Access mode: read, write or append */
};
/*----------------------------------------------------------------------------*/
struct FsDirClass
{
  CLASS_GENERATOR

  /* Virtual functions */
  void (*close)(void *);
  /* bool (*eof)(void *); TODO */
  enum result (*read)(void *, char *);
  /* enum result (*seek)(void *, uint32_t); TODO */
  /* uint32_t (*tell)(void *); TODO */
};
/*----------------------------------------------------------------------------*/
struct FsDir
{
  struct Entity parent;

  struct FsHandle *descriptor;
  /* uint16_t position; TODO */
};
/*----------------------------------------------------------------------------*/
struct FsHandleClass
{
  CLASS_GENERATOR

  /* Pointers to subclasses */
  const void *File;
  const void *Dir;

  /* Virtual functions */
  enum result (*move)(void *, const char *, const char *);
  enum result (*stat)(void *, struct FsStat *, const char *);
  enum result (*open)(void *, void *, const char *,
      enum fsMode);
  enum result (*remove)(void *, const char *);
  enum result (*openDir)(void *, void *, const char *);
  enum result (*makeDir)(void *, const char *);
  enum result (*removeDir)(void *, const char *);
};
/*----------------------------------------------------------------------------*/
struct FsHandle
{
  struct Entity parent;

  struct Interface *dev;
};
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/* Block access functions */
/* Address space defined by asize_t, block size defined by uint32_t */
enum result fsBlockRead(void *, asize_t, uint8_t *, uint32_t);
enum result fsBlockWrite(void *, asize_t, const uint8_t *,
    uint32_t);
/*----------------------------------------------------------------------------*/
/*------------------Filesystem handle functions-------------------------------*/
/* Common functions */
enum result fsMove(void *, const char *, const char *);
enum result fsStat(void *, struct FsStat *, const char *);
/* File functions */
void *fsOpen(void *, const char *, enum fsMode);
enum result fsRemove(void *, const char *);
/* Directory functions */
void *fsOpenDir(void *, const char *);
enum result fsMakeDir(void *, const char *);
enum result fsRemoveDir(void *, const char *);
/*----------------------------------------------------------------------------*/
/*------------------File functions--------------------------------------------*/
void fsClose(void *);
bool fsEof(const void *);
enum result fsFlush(void *);
enum result fsRead(void *, uint8_t *, uint32_t, uint32_t *);
enum result fsSeek(void *, asize_t, enum fsSeekOrigin);
asize_t fsTell(const void *);
enum result fsWrite(void *, const uint8_t *, uint32_t, uint32_t *);
/*----------------------------------------------------------------------------*/
/*------------------Directory functions---------------------------------------*/
void fsCloseDir(void *);
/* bool (*fsEofDir)(void *); TODO */
enum result fsReadDir(void *, char *);
/* enum result fsSeekDir(void *, uint32_t); TODO */
/* uint32_t fsTellDir(void *); TODO */
/*----------------------------------------------------------------------------*/
#endif /* FS_H_ */
