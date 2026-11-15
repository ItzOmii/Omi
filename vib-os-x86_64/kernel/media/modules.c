/*
 * MeepOS - boot module lookup (files carried on the boot medium).
 */

#include "../include/limine_modules.h"
#include "../include/string.h"

void *meep_media_mp3 = 0;
uint64_t meep_media_mp3_len = 0;
void *meep_media_mp4 = 0;
uint64_t meep_media_mp4_len = 0;

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val);

static struct meep_module_response *mod_resp = 0;

static int ends_with(const char *s, const char *suffix) {
  if (!s || !suffix) return 0;
  size_t ls = strlen(s), lf = strlen(suffix);
  if (lf > ls) return 0;
  return strcmp(s + (ls - lf), suffix) == 0;
}

int meep_module_find(const char *suffix, void **ptr, uint64_t *len) {
  if (!mod_resp) return 0;
  for (uint64_t i = 0; i < mod_resp->module_count; i++) {
    struct meep_limine_file *f = mod_resp->modules[i];
    if (!f) continue;
    if (ends_with(f->path, suffix)) {
      if (ptr) *ptr = f->address;
      if (len) *len = f->size;
      return 1;
    }
  }
  return 0;
}

int meep_modules_probe(void *response_ptr) {
  mod_resp = (struct meep_module_response *)response_ptr;
  if (!mod_resp) {
    serial_puts("[mod] no module response\n");
    return 0;
  }
  serial_puts("[mod] count=");
  serial_puthex(mod_resp->module_count);
  serial_puts("\n");

  for (uint64_t i = 0; i < mod_resp->module_count; i++) {
    struct meep_limine_file *f = mod_resp->modules[i];
    if (!f) continue;
    serial_puts("[mod] ");
    serial_puts(f->path ? f->path : "(null)");
    serial_puts(" size=");
    serial_puthex(f->size);
    serial_puts("\n");
  }

  meep_module_find("sample.mp3", &meep_media_mp3, &meep_media_mp3_len);
  meep_module_find("sample.mp4", &meep_media_mp4, &meep_media_mp4_len);
  return (int)mod_resp->module_count;
}

extern void serial_puthex(uint64_t val);