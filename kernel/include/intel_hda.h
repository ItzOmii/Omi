/*
 * MeepOS - Intel HDA audio output
 */

#ifndef _MEEPOS_INTEL_HDA_H
#define _MEEPOS_INTEL_HDA_H

#include "types.h"

/* Registers */
#define HDA_GCAP 0x00
#define HDA_VMIN 0x02
#define HDA_VMAJ 0x03
#define HDA_OUTPAY 0x04
#define HDA_GCTL 0x08
#define HDA_STATESTS 0x0E
#define HDA_GSTS 0x10
#define HDA_IC 0x60   /* immediate command */
#define HDA_IR 0x64   /* immediate response */
#define HDA_ICS 0x68  /* immediate command status */

#define HDA_GCTL_CRST 0x00000001

#define HDA_ICS_BUSY 0x1
#define HDA_ICS_VALID 0x2

/* Stream descriptor registers (relative to stream base) */
#define HDA_SD_CTL 0x00
#define HDA_SD_STS 0x03
#define HDA_SD_LPIB 0x04
#define HDA_SD_CBL 0x08
#define HDA_SD_LVI 0x0C
#define HDA_SD_FIFOW 0x0E
#define HDA_SD_FIFOS 0x10
#define HDA_SD_FMT 0x12
#define HDA_SD_BDLPL 0x18
#define HDA_SD_BDLPU 0x1C

#define HDA_SD_CTL_SRST 0x01
#define HDA_SD_CTL_RUN 0x02

/* Verbs */
#define HDA_VERB_GET_PARAM 0xF00
#define HDA_VERB_SET_FORMAT 0x2
#define HDA_VERB_SET_AMP 0x3
#define HDA_VERB_SET_POWER 0x705
#define HDA_VERB_SET_STREAM_CH 0x706
#define HDA_VERB_SET_PIN_CTL 0x707
#define HDA_VERB_SET_EAPD 0x70C
#define HDA_VERB_SET_CONN_SELECT 0x701

/* Parameters */
#define HDA_PARAM_VENDOR 0x00
#define HDA_PARAM_SUB_NODES 0x04
#define HDA_PARAM_FG_TYPE 0x05
#define HDA_PARAM_WIDGET_CAPS 0x09
#define HDA_PARAM_PCM_RATES 0x0A
#define HDA_PARAM_PIN_CAPS 0x0C
#define HDA_PARAM_CONN_LIST_LEN 0x0E
#define HDA_PARAM_CONN_LIST 0x0F

/* Widget types (bits 23:20 of widget caps) */
#define HDA_WIDGET_DAC 0x0
#define HDA_WIDGET_PIN 0x4

/* Pin device (bits 23:20 of pin caps) */
#define HDA_PIN_LINE_OUT 0x0
#define HDA_PIN_SPEAKER 0x1
#define HDA_PIN_HP_OUT 0x2

typedef struct {
  uint8_t bus, device, function;
  uint32_t bar0;
} hda_pci_t;

/* Returns 0 on success. */
int hda_init(hda_pci_t *dev, uint32_t sample_rate, uint8_t channels);

/* Free bytes the DMA ring can accept right now. */
uint32_t hda_space(void);
/* Copies interleaved int16 samples into the ring; returns bytes written. */
uint32_t hda_write(const int16_t *pcm, uint32_t bytes);
void hda_start(void);
void hda_stop(void);
int hda_is_playing(void);
uint32_t hda_rate(void);
uint8_t hda_channels(void);

/* Boot-time check: finds any PCI audio controller and brings it up. */
int hda_probe_and_init(void);

#endif /* _MEEPOS_INTEL_HDA_H */