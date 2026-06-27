/* 
 * File System Layer
 * 
 * fs.c
 *
 * Implementation of the file system layer with caching support:
 *   - Block cache      (BLOCK_CACHE_SIZE entries, default 10)
 *   - Inode cache      (INODE_CACHE_SIZE  entries, default  4)
 *   - Directory cache  (DIR_CACHE_SIZE    entries, default  4)
 *
 * Cache replacement: LRU, clean entries preferred as victims.
 * Write-back: dirty entries flushed on eviction OR after WRITEBACK_INTERVAL seconds.
 *
 * All original logic is preserved exactly. Only I/O calls are redirected through cache.
 *
 * PARTE 2 - Requisito 1: Caching
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include "fs.h"

#define dprintf if(1) printf

#define BLOCK_SIZE 512

/* ---------- cache size parameters (override at compile time) ---------- */
#ifndef BLOCK_CACHE_SIZE
#define BLOCK_CACHE_SIZE 10
#endif
#ifndef INODE_CACHE_SIZE
#define INODE_CACHE_SIZE 4
#endif
#ifndef DIR_CACHE_SIZE
#define DIR_CACHE_SIZE 4
#endif

/* seconds before a dirty cache entry must be written back */
#define WRITEBACK_INTERVAL 10

/* =====================================================================
 * Inode & directory entry types  (unchanged from original)
 * ===================================================================== */

#define INODE_NUM_BLKS 10
#define EXT_INODE_NUM_BLKS (BLOCK_SIZE / sizeof(unsigned int))

typedef struct fs_inode {
   fs_itype_t type;
   unsigned int size;
   unsigned int blocks[INODE_NUM_BLKS];
   unsigned int reserved[4];
} fs_inode_t;

typedef unsigned int fs_inode_ext_t;

#define DIR_PAGE_ENTRIES (BLOCK_SIZE / sizeof(fs_dentry_t))

typedef struct dentry {
   char name[FS_MAX_FNAME_SZ];
   inodeid_t inodeid;
} fs_dentry_t;

#define ITAB_NUM_BLKS 8
#define ITAB_SIZE (ITAB_NUM_BLKS * BLOCK_SIZE / sizeof(fs_inode_t))

/* =====================================================================
 * Cache entry types
 * ===================================================================== */

typedef struct {
   int      valid;
   int      dirty;
   unsigned block_no;
   time_t   dirty_time;
   unsigned long lru;
   char     data[BLOCK_SIZE];
} bcache_entry_t;

typedef struct {
   int       valid;
   int       dirty;
   inodeid_t inodeid;
   time_t    dirty_time;
   unsigned long lru;
   fs_inode_t inode;
} icache_entry_t;

typedef struct {
   int       valid;
   int       dirty;
   inodeid_t dir_inodeid;
   unsigned  block_no;
   time_t    dirty_time;
   unsigned long lru;
   fs_dentry_t entries[DIR_PAGE_ENTRIES];
} dcache_entry_t;

/* =====================================================================
 * File system structure
 * ===================================================================== */

struct fs_ {
   blocks_t*  blocks;
   char       inode_bmap[BLOCK_SIZE];
   char       blk_bmap[BLOCK_SIZE];
   fs_inode_t inode_tab[ITAB_SIZE];

   bcache_entry_t bcache[BLOCK_CACHE_SIZE];
   icache_entry_t icache[INODE_CACHE_SIZE];
   dcache_entry_t dcache[DIR_CACHE_SIZE];

   unsigned long lru_clock;
};

#define NOT_FS_INITIALIZER 1

/* =====================================================================
 * Bitmap helpers  (unchanged from original)
 * ===================================================================== */

#define BMAP_SET(bmap,num)   ((bmap)[(num)/8] |=  (0x1 << ((num)%8)))
#define BMAP_CLR(bmap,num)   ((bmap)[(num)/8] &= ~(0x1 << ((num)%8)))
#define BMAP_ISSET(bmap,num) ((bmap)[(num)/8] &   (0x1 << ((num)%8)))

#define MIN(a,b) ((a)<=(b)?(a):(b))
#define MAX(a,b) ((a)>=(b)?(a):(b))
#define OFFSET_TO_BLOCKS(pos) ((pos)/BLOCK_SIZE+(((pos)%BLOCK_SIZE>0)?1:0))

static int fsi_bmap_find_free(char* bmap, int size, unsigned* free_out)
{
   for (int i = 0; i < size; i++) {
      if (!BMAP_ISSET(bmap, i)) { *free_out = i; return 1; }
   }
   return 0;
}

static void fsi_dump_bmap(char* bmap, int size)
{
   for (int i = 0; i < size; i++) {
      printf("%x.", (unsigned char)bmap[i]);
      if (i > 0 && i % 32 == 0) printf("\n");
   }
}

/* =====================================================================
 * BLOCK CACHE
 * ===================================================================== */

/* returns index of victim slot (may be invalid/clean/dirty) */
static int bc_victim(fs_t* fs)
{
   int bc = -1, bd = -1;
   unsigned long mc = ~0UL, md = ~0UL;
   for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
      if (!fs->bcache[i].valid) return i;
      if (fs->bcache[i].dirty) { if (fs->bcache[i].lru < md) { md = fs->bcache[i].lru; bd = i; } }
      else                     { if (fs->bcache[i].lru < mc) { mc = fs->bcache[i].lru; bc = i; } }
   }
   return (bc != -1) ? bc : bd;
}

static void bc_flush(fs_t* fs, int i)
{
   if (fs->bcache[i].valid && fs->bcache[i].dirty) {
      block_write(fs->blocks, fs->bcache[i].block_no, fs->bcache[i].data);
      fs->bcache[i].dirty = 0;
   }
}

static void bc_flush_expired(fs_t* fs)
{
   time_t now = time(NULL);
   for (int i = 0; i < BLOCK_CACHE_SIZE; i++)
      if (fs->bcache[i].valid && fs->bcache[i].dirty &&
          difftime(now, fs->bcache[i].dirty_time) >= WRITEBACK_INTERVAL)
         bc_flush(fs, i);
}

static void bc_flush_all(fs_t* fs)
{ for (int i = 0; i < BLOCK_CACHE_SIZE; i++) bc_flush(fs, i); }

/* cached block_read */
static void bc_read(fs_t* fs, unsigned blkno, char* buf)
{
   bc_flush_expired(fs);
   for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
      if (fs->bcache[i].valid && fs->bcache[i].block_no == blkno) {
         fs->bcache[i].lru = ++fs->lru_clock;
         memcpy(buf, fs->bcache[i].data, BLOCK_SIZE);
         return;
      }
   }
   int v = bc_victim(fs); bc_flush(fs, v);
   block_read(fs->blocks, blkno, fs->bcache[v].data);
   fs->bcache[v].valid    = 1;
   fs->bcache[v].dirty    = 0;
   fs->bcache[v].block_no = blkno;
   fs->bcache[v].lru      = ++fs->lru_clock;
   memcpy(buf, fs->bcache[v].data, BLOCK_SIZE);
}

/* cached block_write (write-back) */
static void bc_write(fs_t* fs, unsigned blkno, char* buf)
{
   bc_flush_expired(fs);
   for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
      if (fs->bcache[i].valid && fs->bcache[i].block_no == blkno) {
         memcpy(fs->bcache[i].data, buf, BLOCK_SIZE);
         if (!fs->bcache[i].dirty) { fs->bcache[i].dirty = 1; fs->bcache[i].dirty_time = time(NULL); }
         fs->bcache[i].lru = ++fs->lru_clock;
         return;
      }
   }
   int v = bc_victim(fs); bc_flush(fs, v);
   memcpy(fs->bcache[v].data, buf, BLOCK_SIZE);
   fs->bcache[v].valid      = 1;
   fs->bcache[v].dirty      = 1;
   fs->bcache[v].dirty_time = time(NULL);
   fs->bcache[v].block_no   = blkno;
   fs->bcache[v].lru        = ++fs->lru_clock;
}

/* =====================================================================
 * INODE CACHE
 * Stores a copy of inode_tab[id]; dirty entries are written back to
 * inode_tab[] (which fsi_store_fsdata then persists to disk).
 * ===================================================================== */

static int ic_victim(fs_t* fs)
{
   int bc = -1, bd = -1;
   unsigned long mc = ~0UL, md = ~0UL;
   for (int i = 0; i < INODE_CACHE_SIZE; i++) {
      if (!fs->icache[i].valid) return i;
      if (fs->icache[i].dirty) { if (fs->icache[i].lru < md) { md = fs->icache[i].lru; bd = i; } }
      else                     { if (fs->icache[i].lru < mc) { mc = fs->icache[i].lru; bc = i; } }
   }
   return (bc != -1) ? bc : bd;
}

static void ic_flush(fs_t* fs, int i)
{
   if (fs->icache[i].valid && fs->icache[i].dirty) {
      fs->inode_tab[fs->icache[i].inodeid] = fs->icache[i].inode;
      fs->icache[i].dirty = 0;
   }
}

static void ic_flush_expired(fs_t* fs)
{
   time_t now = time(NULL);
   for (int i = 0; i < INODE_CACHE_SIZE; i++)
      if (fs->icache[i].valid && fs->icache[i].dirty &&
          difftime(now, fs->icache[i].dirty_time) >= WRITEBACK_INTERVAL)
         ic_flush(fs, i);
}

static void ic_flush_all(fs_t* fs)
{ for (int i = 0; i < INODE_CACHE_SIZE; i++) ic_flush(fs, i); }

/* get pointer to cached inode (read access) */
static fs_inode_t* ic_get(fs_t* fs, inodeid_t id)
{
   ic_flush_expired(fs);
   for (int i = 0; i < INODE_CACHE_SIZE; i++) {
      if (fs->icache[i].valid && fs->icache[i].inodeid == id) {
         fs->icache[i].lru = ++fs->lru_clock;
         return &fs->icache[i].inode;
      }
   }
   int v = ic_victim(fs); ic_flush(fs, v);
   fs->icache[v].inode   = fs->inode_tab[id];
   fs->icache[v].inodeid = id;
   fs->icache[v].valid   = 1;
   fs->icache[v].dirty   = 0;
   fs->icache[v].lru     = ++fs->lru_clock;
   return &fs->icache[v].inode;
}

/* mark cached inode dirty after modification (also syncs inode_tab) */
static void ic_dirty(fs_t* fs, inodeid_t id)
{
   for (int i = 0; i < INODE_CACHE_SIZE; i++) {
      if (fs->icache[i].valid && fs->icache[i].inodeid == id) {
         fs->inode_tab[id] = fs->icache[i].inode; /* keep inode_tab consistent */
         if (!fs->icache[i].dirty) { fs->icache[i].dirty = 1; fs->icache[i].dirty_time = time(NULL); }
         return;
      }
   }
}

/* invalidate (evict without flush – caller must have already stored) */
static void ic_invalidate(fs_t* fs, inodeid_t id)
{
   for (int i = 0; i < INODE_CACHE_SIZE; i++)
      if (fs->icache[i].valid && fs->icache[i].inodeid == id)
         { ic_flush(fs, i); fs->icache[i].valid = 0; }
}

/* =====================================================================
 * DIRECTORY CACHE
 * Key: (dir_inodeid, disk block_no).
 * Dirty entries write back through bc_write.
 * ===================================================================== */

static int dc_victim(fs_t* fs)
{
   int bc = -1, bd = -1;
   unsigned long mc = ~0UL, md = ~0UL;
   for (int i = 0; i < DIR_CACHE_SIZE; i++) {
      if (!fs->dcache[i].valid) return i;
      if (fs->dcache[i].dirty) { if (fs->dcache[i].lru < md) { md = fs->dcache[i].lru; bd = i; } }
      else                     { if (fs->dcache[i].lru < mc) { mc = fs->dcache[i].lru; bc = i; } }
   }
   return (bc != -1) ? bc : bd;
}

static void dc_flush(fs_t* fs, int i)
{
   if (fs->dcache[i].valid && fs->dcache[i].dirty) {
      bc_write(fs, fs->dcache[i].block_no, (char*)fs->dcache[i].entries);
      fs->dcache[i].dirty = 0;
   }
}

static void dc_flush_expired(fs_t* fs)
{
   time_t now = time(NULL);
   for (int i = 0; i < DIR_CACHE_SIZE; i++)
      if (fs->dcache[i].valid && fs->dcache[i].dirty &&
          difftime(now, fs->dcache[i].dirty_time) >= WRITEBACK_INTERVAL)
         dc_flush(fs, i);
}

static void dc_flush_all(fs_t* fs)
{ for (int i = 0; i < DIR_CACHE_SIZE; i++) dc_flush(fs, i); }

static void dc_read(fs_t* fs, inodeid_t dirid, unsigned blkno, fs_dentry_t* page)
{
   dc_flush_expired(fs);
   for (int i = 0; i < DIR_CACHE_SIZE; i++) {
      if (fs->dcache[i].valid && fs->dcache[i].dir_inodeid == dirid &&
          fs->dcache[i].block_no == blkno) {
         fs->dcache[i].lru = ++fs->lru_clock;
         memcpy(page, fs->dcache[i].entries, BLOCK_SIZE);
         return;
      }
   }
   int v = dc_victim(fs); dc_flush(fs, v);
   bc_read(fs, blkno, (char*)fs->dcache[v].entries);
   fs->dcache[v].valid       = 1;
   fs->dcache[v].dirty       = 0;
   fs->dcache[v].dir_inodeid = dirid;
   fs->dcache[v].block_no    = blkno;
   fs->dcache[v].lru         = ++fs->lru_clock;
   memcpy(page, fs->dcache[v].entries, BLOCK_SIZE);
}

static void dc_write(fs_t* fs, inodeid_t dirid, unsigned blkno, fs_dentry_t* page)
{
   dc_flush_expired(fs);
   for (int i = 0; i < DIR_CACHE_SIZE; i++) {
      if (fs->dcache[i].valid && fs->dcache[i].dir_inodeid == dirid &&
          fs->dcache[i].block_no == blkno) {
         memcpy(fs->dcache[i].entries, page, BLOCK_SIZE);
         if (!fs->dcache[i].dirty) { fs->dcache[i].dirty = 1; fs->dcache[i].dirty_time = time(NULL); }
         fs->dcache[i].lru = ++fs->lru_clock;
         return;
      }
   }
   int v = dc_victim(fs); dc_flush(fs, v);
   memcpy(fs->dcache[v].entries, page, BLOCK_SIZE);
   fs->dcache[v].valid       = 1;
   fs->dcache[v].dirty       = 1;
   fs->dcache[v].dirty_time  = time(NULL);
   fs->dcache[v].dir_inodeid = dirid;
   fs->dcache[v].block_no    = blkno;
   fs->dcache[v].lru         = ++fs->lru_clock;
}

/* =====================================================================
 * FS metadata  (load/store bitmaps + inode table)
 * ===================================================================== */

static void fsi_load_fsdata(fs_t* fs)
{
   blocks_t* bks = fs->blocks;
   block_read(bks, 0, fs->blk_bmap);
   block_read(bks, 1, fs->inode_bmap);
   for (int i = 0; i < ITAB_NUM_BLKS; i++)
      block_read(bks, i+2, &((char*)fs->inode_tab)[i*BLOCK_SIZE]);
#define NOT_FS_INITIALIZER 1
}

static void fsi_store_fsdata(fs_t* fs)
{
   /* flush caches so inode_tab is up to date before writing */
   ic_flush_all(fs);
   dc_flush_all(fs);
   bc_flush_all(fs);

   blocks_t* bks = fs->blocks;
   block_write(bks, 0, fs->blk_bmap);
   block_write(bks, 1, fs->inode_bmap);
   for (int i = 0; i < ITAB_NUM_BLKS; i++)
      block_write(bks, i+2, &((char*)fs->inode_tab)[i*BLOCK_SIZE]);
}

/* =====================================================================
 * Other internal helpers  (unchanged logic from original)
 * ===================================================================== */

static void fsi_inode_init(fs_inode_t* inode, fs_itype_t type)
{
   inode->type = type;
   inode->size = 0;
   for (int i = 0; i < INODE_NUM_BLKS; i++) inode->blocks[i] = 0;
   for (int i = 0; i < 4; i++) inode->reserved[i] = 0;
}

/* search directory 'dir' for entry 'file' – now uses dir cache */
static int fsi_dir_search(fs_t* fs, inodeid_t dir, char* file, inodeid_t* fileid)
{
   fs_dentry_t page[DIR_PAGE_ENTRIES];
   fs_inode_t* idir = ic_get(fs, dir);
   int num = idir->size / sizeof(fs_dentry_t);
   int iblock = 0;

   while (num > 0) {
      dc_read(fs, dir, idir->blocks[iblock++], page);
      for (int i = 0; i < DIR_PAGE_ENTRIES && num > 0; i++, num--) {
         if (strcmp(page[i].name, file) == 0) { *fileid = page[i].inodeid; return 0; }
      }
   }
   return -1;
}

/* =====================================================================
 * Public API  (logic identical to original; I/O via cache)
 * ===================================================================== */

void io_delay_on(int disk_delay);

fs_t* fs_new(unsigned num_blocks, int disk_delay)
{
   io_delay_on(disk_delay);
   fs_t* fs = (fs_t*) malloc(sizeof(fs_t));
   memset(fs, 0, sizeof(fs_t));
   fs->blocks = block_new(num_blocks, BLOCK_SIZE);
   fsi_load_fsdata(fs);
   return fs;
}

int fs_format(fs_t* fs)
{
   if (fs == NULL) { printf("[fs] argument is null.\n"); return -1; }

   char null_block[BLOCK_SIZE];
   memset(null_block, 0, sizeof(null_block));
   for (int i = 0; i < (int)block_num_blocks(fs->blocks); i++)
      block_write(fs->blocks, i, null_block);

   BMAP_SET(fs->blk_bmap, 0);
   BMAP_SET(fs->blk_bmap, 1);
   for (int i = 0; i < ITAB_NUM_BLKS; i++) BMAP_SET(fs->blk_bmap, i+2);

   BMAP_SET(fs->inode_bmap, 0);
   BMAP_SET(fs->inode_bmap, 1);
   fsi_inode_init(&fs->inode_tab[1], FS_DIR);

   fsi_store_fsdata(fs);
   return 0;
}

int fs_get_attrs(fs_t* fs, inodeid_t file, fs_file_attrs_t* attrs)
{
   if (fs == NULL || file >= ITAB_SIZE || attrs == NULL) {
      dprintf("[fs_get_attrs] malformed arguments.\n"); return -1;
   }
   if (!BMAP_ISSET(fs->inode_bmap, file)) {
      dprintf("[fs_get_attrs] inode is not being used.\n"); return -1;
   }
   fs_inode_t* inode = ic_get(fs, file);
   attrs->inodeid = file;
   attrs->type    = inode->type;
   attrs->size    = inode->size;
   switch (inode->type) {
      case FS_DIR:  attrs->num_entries = inode->size / sizeof(fs_dentry_t); break;
      case FS_FILE: attrs->num_entries = -1; break;
      default: dprintf("[fs_get_attrs] fatal error - invalid inode.\n"); exit(-1);
   }
   return 0;
}

int fs_lookup(fs_t* fs, char* file, inodeid_t* fileid)
{
   char *token;
   char line[MAX_PATH_NAME_SIZE];
   char *search = "/";
   int i = 0, dir = 0;

   if (fs == NULL || file == NULL) { dprintf("[fs_lookup] malformed arguments.\n"); return -1; }
   if (file[0] != '/') { dprintf("[fs_lookup] malformed pathname.\n"); return -1; }

   strcpy(line, file);
   token = strtok(line, search);
   while (token != NULL) {
      i++;
      if (i == 1) dir = 1;
      if (!BMAP_ISSET(fs->inode_bmap, dir)) { dprintf("[fs_lookup] inode is not being used.\n"); return -1; }
      fs_inode_t* idir = ic_get(fs, dir);
      if (idir->type != FS_DIR) { dprintf("[fs_lookup] inode is not a directory.\n"); return -1; }
      inodeid_t fid;
      if (fsi_dir_search(fs, dir, token, &fid) < 0) { dprintf("[fs_lookup] file does not exist.\n"); return 0; }
      *fileid = fid;
      dir = fid;
      token = strtok(NULL, search);
   }
   return 1;
}

int fs_read(fs_t* fs, inodeid_t file, unsigned offset, unsigned count,
   char* buffer, int* nread)
{
   if (fs == NULL || file >= ITAB_SIZE || buffer == NULL || nread == NULL) {
      dprintf("[fs_read] malformed arguments.\n"); return -1;
   }
   if (!BMAP_ISSET(fs->inode_bmap, file)) { dprintf("[fs_read] inode is not being used.\n"); return -1; }

   fs_inode_t* ifile = ic_get(fs, file);
   if (ifile->type != FS_FILE) { dprintf("[fs_read] inode is not a file.\n"); return -1; }
   if (offset >= ifile->size) { *nread = 0; return 0; }

   int pos = 0;
   int iblock = offset / BLOCK_SIZE;
   int blks_used = OFFSET_TO_BLOCKS(ifile->size);
   int max = MIN(count, ifile->size - offset);
   int tbl_pos;
   unsigned int *blk;
   char block[BLOCK_SIZE];

   while (pos < max && iblock < blks_used) {
      if (iblock < INODE_NUM_BLKS) { blk = ifile->blocks; tbl_pos = iblock; }
      bc_read(fs, blk[tbl_pos], block);   /* <-- cached */
      int start = ((pos == 0) ? (offset % BLOCK_SIZE) : 0);
      int num = MIN(BLOCK_SIZE - start, max - pos);
      memcpy(&buffer[pos], &block[start], num);
      pos += num;
      iblock++;
   }
   *nread = pos;
   return 0;
}

int fs_write(fs_t* fs, inodeid_t file, unsigned offset, unsigned count,
   char* buffer)
{
   if (fs == NULL || file >= ITAB_SIZE || buffer == NULL) {
      dprintf("[fs_write] malformed arguments.\n"); return -1;
   }
   if (!BMAP_ISSET(fs->inode_bmap, file)) { dprintf("[fs_write] inode is not being used.\n"); return -1; }

   fs_inode_t* ifile = ic_get(fs, file);
   if (ifile->type != FS_FILE) { dprintf("[fs_write] inode is not a file.\n"); return -1; }
   if (offset > ifile->size) offset = ifile->size;

   unsigned *blk;
   int blks_used = OFFSET_TO_BLOCKS(ifile->size);
   int blks_req  = MAX(OFFSET_TO_BLOCKS(offset + count), blks_used) - blks_used;

   dprintf("[fs_write] count=%d, offset=%d, fsize=%d, bused=%d, breq=%d\n",
      count, offset, ifile->size, blks_used, blks_req);

   if (blks_req > 0) {
      if (blks_req > INODE_NUM_BLKS - blks_used) {
         dprintf("[fs_write] no free block entries in inode.\n"); return -1;
      }
      dprintf("[fs_write] required %d blocks, used %d\n", blks_req, blks_used);
      for (int i = blks_used; i < blks_used + blks_req; i++) {
         if (i < INODE_NUM_BLKS) blk = &ifile->blocks[i];
         if (!fsi_bmap_find_free(fs->blk_bmap, block_num_blocks(fs->blocks), blk)) {
            dprintf("[fs_write] there are no free blocks.\n"); return -1;
         }
         BMAP_SET(fs->blk_bmap, *blk);
         dprintf("[fs_write] block %d allocated.\n", *blk);
      }
   }

   char block[BLOCK_SIZE];
   int num = 0, pos;
   int iblock = offset / BLOCK_SIZE;

   /* write within existing blocks */
   while (num < (int)count && iblock < blks_used) {
      if (iblock < INODE_NUM_BLKS) { blk = ifile->blocks; pos = iblock; }
      bc_read(fs, blk[pos], block);            /* <-- cached read */
      int start = ((num == 0) ? (offset % BLOCK_SIZE) : 0);
      for (int i = start; i < BLOCK_SIZE && num < (int)count; i++, num++) block[i] = buffer[num];
      bc_write(fs, blk[pos], block);           /* <-- cached write */
      iblock++;
   }

   dprintf("[fs_write] written %d bytes within.\n", num);

   /* write into newly allocated blocks */
   while (num < (int)count && iblock < blks_used + blks_req) {
      if (iblock < INODE_NUM_BLKS) { blk = ifile->blocks; pos = iblock; }
      for (int i = 0; i < BLOCK_SIZE && num < (int)count; i++, num++) block[i] = buffer[num];
      bc_write(fs, blk[pos], block);           /* <-- cached write */
      iblock++;
   }

   if (num != (int)count) { printf("[fs_write] severe error: num=%d != count=%d!\n", num, count); exit(-1); }

   ifile->size = MAX(offset + count, ifile->size);
   ic_dirty(fs, file);   /* mark inode dirty */

   fsi_store_fsdata(fs);
   dprintf("[fs_write] written %d bytes, file size %d.\n", count, ifile->size);
   return 0;
}

int fs_create(fs_t* fs, inodeid_t dir, char* file, inodeid_t* fileid)
{
   if (fs == NULL || dir >= ITAB_SIZE || file == NULL || fileid == NULL) {
      printf("[fs_create] malformed arguments.\n"); return -1;
   }
   if (strlen(file) == 0 || strlen(file)+1 > FS_MAX_FNAME_SZ) {
      dprintf("[fs_create] file name size error.\n"); return -1;
   }
   if (!BMAP_ISSET(fs->inode_bmap, dir)) { dprintf("[fs_create] inode is not being used.\n"); return -1; }

   fs_inode_t* idir = ic_get(fs, dir);
   if (idir->type != FS_DIR) { dprintf("[fs_create] inode is not a directory.\n"); return -1; }
   if (fsi_dir_search(fs, dir, file, fileid) == 0) { dprintf("[fs_create] file already exists.\n"); return -1; }

   unsigned finode;
   if (!fsi_bmap_find_free(fs->inode_bmap, ITAB_SIZE, &finode)) {
      dprintf("[fs_create] there are no free inodes.\n"); return -1;
   }

   if (idir->size % BLOCK_SIZE == 0) {
      unsigned fblock;
      if (!fsi_bmap_find_free(fs->blk_bmap, block_num_blocks(fs->blocks), &fblock)) {
         dprintf("[fs_create] no free blocks to augment directory.\n"); return -1;
      }
      BMAP_SET(fs->blk_bmap, fblock);
      idir->blocks[idir->size / BLOCK_SIZE] = fblock;
      ic_dirty(fs, dir);
      idir = ic_get(fs, dir); /* re-fetch after dirty */
   }

   fs_dentry_t page[DIR_PAGE_ENTRIES];
   unsigned blkno = idir->blocks[idir->size / BLOCK_SIZE];
   dc_read(fs, dir, blkno, page);
   fs_dentry_t* entry = &page[idir->size % BLOCK_SIZE / sizeof(fs_dentry_t)];
   strcpy(entry->name, file);
   entry->inodeid = finode;
   dc_write(fs, dir, blkno, page);

   idir->size += sizeof(fs_dentry_t);
   ic_dirty(fs, dir);

   BMAP_SET(fs->inode_bmap, finode);
   fsi_inode_init(&fs->inode_tab[finode], FS_FILE);
   ic_invalidate(fs, finode);

   fsi_store_fsdata(fs);
   *fileid = finode;
   return 0;
}

int fs_mkdir(fs_t* fs, inodeid_t dir, char* newdir, inodeid_t* newdirid)
{
   if (fs == NULL || dir >= ITAB_SIZE || newdir == NULL || newdirid == NULL) {
      printf("[fs_mkdir] malformed arguments.\n"); return -1;
   }
   if (strlen(newdir) == 0 || strlen(newdir)+1 > FS_MAX_FNAME_SZ) {
      dprintf("[fs_mkdir] directory size error.\n"); return -1;
   }
   if (!BMAP_ISSET(fs->inode_bmap, dir)) { dprintf("[fs_mkdir] inode is not being used.\n"); return -1; }

   fs_inode_t* idir = ic_get(fs, dir);
   if (idir->type != FS_DIR) { dprintf("[fs_mkdir] inode is not a directory.\n"); return -1; }
   if (fsi_dir_search(fs, dir, newdir, newdirid) == 0) { dprintf("[fs_mkdir] directory already exists.\n"); return -1; }

   unsigned finode;
   if (!fsi_bmap_find_free(fs->inode_bmap, ITAB_SIZE, &finode)) {
      dprintf("[fs_mkdir] there are no free inodes.\n"); return -1;
   }

   if (idir->size % BLOCK_SIZE == 0) {
      unsigned fblock;
      if (!fsi_bmap_find_free(fs->blk_bmap, block_num_blocks(fs->blocks), &fblock)) {
         dprintf("[fs_mkdir] no free blocks to augment directory.\n"); return -1;
      }
      BMAP_SET(fs->blk_bmap, fblock);
      idir->blocks[idir->size / BLOCK_SIZE] = fblock;
      ic_dirty(fs, dir);
      idir = ic_get(fs, dir);
   }

   fs_dentry_t page[DIR_PAGE_ENTRIES];
   unsigned blkno = idir->blocks[idir->size / BLOCK_SIZE];
   dc_read(fs, dir, blkno, page);
   fs_dentry_t* entry = &page[idir->size % BLOCK_SIZE / sizeof(fs_dentry_t)];
   strcpy(entry->name, newdir);
   entry->inodeid = finode;
   dc_write(fs, dir, blkno, page);

   idir->size += sizeof(fs_dentry_t);
   ic_dirty(fs, dir);

   BMAP_SET(fs->inode_bmap, finode);
   fsi_inode_init(&fs->inode_tab[finode], FS_DIR);
   ic_invalidate(fs, finode);

   fsi_store_fsdata(fs);
   *newdirid = finode;
   return 0;
}

int fs_readdir(fs_t* fs, inodeid_t dir, fs_file_name_t* entries, int maxentries,
   int* numentries)
{
   if (fs == NULL || dir >= ITAB_SIZE || entries == NULL ||
      numentries == NULL || maxentries < 0) {
      dprintf("[fs_readdir] malformed arguments.\n"); return -1;
   }
   if (!BMAP_ISSET(fs->inode_bmap, dir)) { dprintf("[fs_readdir] inode is not being used.\n"); return -1; }

   fs_inode_t* idir = ic_get(fs, dir);
   if (idir->type != FS_DIR) { dprintf("[fs_readdir] inode is not a directory.\n"); return -1; }

   fs_dentry_t page[DIR_PAGE_ENTRIES];
   int num = MIN(idir->size / sizeof(fs_dentry_t), maxentries);
   int iblock = 0, ientry = 0;

   while (num > 0) {
      dc_read(fs, dir, idir->blocks[iblock++], page);
      for (int i = 0; i < DIR_PAGE_ENTRIES && num > 0; i++, num--) {
         strcpy(entries[ientry].name, page[i].name);
         entries[ientry].type = ic_get(fs, page[i].inodeid)->type;
         ientry++;
      }
   }
   *numentries = ientry;
   return 0;
}

int fs_copy(fs_t* fs, char* srcpath, char* tgtpath)
{
   /* copy-on-write: TO DO */
   return -1;
}

void fs_dump(fs_t* fs)
{
   printf("Free block bitmap:\n");
   fsi_dump_bmap(fs->blk_bmap, BLOCK_SIZE);
   printf("\n");
   printf("Free inode table bitmap:\n");
   fsi_dump_bmap(fs->inode_bmap, BLOCK_SIZE);
   printf("\n");
}
