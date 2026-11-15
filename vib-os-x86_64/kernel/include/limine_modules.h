/*
 * MeepOS - Limine module (file) protocol support.
 *
 * Lets the Media player see real files that live on the boot medium
 * (sample.mp3 / sample.mp4) without needing a disk driver or mount.
 */

#ifndef _MEEPOS_LIMINE_MODULES_H
#define _MEEPOS_LIMINE_MODULES_H

#include "types.h"

#define LIMINE_MODULE_REQUEST_ID0 0x3e7e279702be32afULL
#define LIMINE_MODULE_REQUEST_ID1 0xca1c4f3bd1280ceeULL

struct meep_uuid {
  uint8_t bytes[16];
};

struct meep_limine_file {
  uint64_t revision;
  void *address;
  uint64_t size;
  char *path;
  char *cmdline;
  uint32_t media_type;
  uint32_t unused;
  uint32_t tftp_ip;
  uint32_t tftp_port;
  uint32_t partition_index;
  uint32_t mbr_disk_id;
  struct meep_uuid gpt_disk_uuid;
  struct meep_uuid gpt_part_uuid;
  struct meep_uuid part_uuid;
};

struct meep_module_response {
  uint64_t revision;
  uint64_t module_count;
  struct meep_limine_file **modules;
};

struct meep_module_request {
  uint64_t id[4];
  uint64_t revision;
  struct meep_module_response *response;
  uint64_t internal_module_count;
  void *internal_modules;
};

/* Filled in by limine_boot.c once bootinfo is parsed. */
extern void *meep_media_mp3;
extern uint64_t meep_media_mp3_len;
extern void *meep_media_mp4;
extern uint64_t meep_media_mp4_len;

/* Returns module count discovered (0 if none). */
int meep_modules_probe(void *response_ptr);

/*
 * Finds a boot module whose path ends with `suffix` (e.g. "sample.mp3").
 * Returns 1 and fills out ptr/len on success.
 */
int meep_module_find(const char *suffix, void **ptr, uint64_t *len);

#endif /* _MEEPOS_LIMINE_MODULES_H */