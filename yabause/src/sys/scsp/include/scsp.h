/*  Copyright 2004 Stephane Dallongeville
    Copyright 2004-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef SCSP_H
#define SCSP_H

#include "core.h"
#include "sh2core.h"
#include <stddef.h> // size_t, for ScspGetRegisterName

#ifdef __cplusplus
extern "C" {
#endif

#define SNDCORE_DEFAULT -1
#define SNDCORE_DUMMY   0
#define SNDCORE_WAV     10 // should really be 1, but I'll probably break people's stuff

#define SCSP_MUTE_SYSTEM    1
#define SCSP_MUTE_USER      2

typedef struct
{
   int id;
   const char *Name;
   int (*Init)(void);
   void (*DeInit)(void);
   int (*Reset)(void);
   int (*ChangeVideoFormat)(int vertfreq);
   void (*UpdateAudio)(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
   u32 (*GetAudioSpace)(void);
   void (*MuteAudio)(void);
   void (*UnMuteAudio)(void);
   void (*SetVolume)(int volume);
#ifdef USE_SCSPMIDI
	int (*MidiChangePorts)(int inport, int outport);
	u8 (*MidiIn)(int *isdata);
	int (*MidiOut)(u8 data);
#endif
} SoundInterface_struct;

typedef struct
{
   u32 D[8];
   u32 A[8];
   u32 SR;
   u32 PC;
} m68kregs_struct;

typedef struct
{
  u32 addr;
} m68kcodebreakpoint_struct;

#define MAX_BREAKPOINTS 10

typedef struct
{
  u32 scsptiming1;
  u32 scsptiming2;  // 16.16 fixed point
  m68kcodebreakpoint_struct codebreakpoint[MAX_BREAKPOINTS];
  int numcodebreakpoints;
  void (*BreakpointCallBack)(u32);
  int inbreakpoint;
} ScspInternal;

extern SoundInterface_struct SNDDummy;
extern SoundInterface_struct SNDWave;
extern u8 *SoundRam;

u8 FASTCALL SoundRamReadByte(SH2_struct *context, u8* mem,u32 addr);
u16 FASTCALL SoundRamReadWord(SH2_struct *context, u8* mem,u32 addr);
u32 FASTCALL SoundRamReadLong(SH2_struct *context, u8* mem, u32 addr);
void FASTCALL SoundRamWriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val);
void FASTCALL SoundRamWriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val);
void FASTCALL SoundRamWriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val);

int ScspInit(int coreid);
int ScspChangeSoundCore(int coreid);
void ScspDeInit(void);
void M68KStart(void);
void M68KStop(void);
void ScspReset(void);
void ScspHalt(void);
int ScspChangeVideoFormat(int type);
void ScspAddCycles(u64 cycles);
void ScspConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len);
void ScspReceiveCDDA(const u8 *sector);
int SoundSaveState(void ** stream);
int SoundLoadState(const void * stream, int version, int size);
void ScspSlotDebugStats(u8 slotnum, char *outstring);
void ScspCommonControlRegisterDebugStats(char *outstring);
int ScspSlotDebugSaveRegisters(u8 slotnum, const char *filename);
u32 ScspSlotDebugAudio (u32 *workbuf, s16 *buf, u32 len);
void ScspSlotResetDebug(u8 slotnum);
int ScspSlotDebugAudioSaveWav(u8 slotnum, const char *filename);
// Export complet (registres communs + 32 slots + DSP) dans un seul fichier
// texte, et dump binaire brut de la RAM son (0x80000 octets). Voir
// ScspDspFullDebugStats (scspdsp.h) pour la partie DSP du rapport.
int ScspSaveFullDebugReport(const char *filename);
int ScspSaveSoundRam(const char *filename);

//////////////////////////////////////////////////////////////////////////////
// Generic SCSP register watch.
//
// UIDebugSCSP referenced this API but it did not exist anywhere in the core,
// so the Qt port did not link. Implemented in scsp.c, hooked into the three
// register write entry points (scsp_w_b / scsp_w_w / scsp_w_d), which is the
// funnel every writer goes through -- SH2 (scsp_w_*), sound CPU (c68k_*_write)
// and SCU DMA all end up there -- so a watch catches the write whoever issued
// it.
//
// Addresses are SCSP register offsets, not absolute bus addresses: 0x000-0x3FF
// are the 32 slot register banks (0x20 bytes each, slot n at n*0x20) and
// 0x400-0x43F are the common control registers. For instance 0x216 is slot
// 16's EFSDL/EFPAN (the CD-DA left channel -- see Technical Bulletin #29) and
// 0x400 is MEM4MB/DAC18B/VER/MVOL. The DSP areas (COEF at 0x700, MADRS at
// 0x780, MPRO at 0x800) are deliberately not watchable here: they are dumped
// as a whole by the SCSP DSP debugger instead.
//
// The log is a fixed-size ring buffer: it never allocates while the emulation
// runs and silently drops the oldest entries once full.
#define SCSP_MAX_REGISTER_WATCHES     8
#define SCSP_REGISTER_WATCH_MAX_ADDR  0x43F
#define SCSP_REGISTER_WATCH_LOG_SIZE  4096

// Returns 0 on success, -1 if the address is out of range, already watched, or
// if SCSP_MAX_REGISTER_WATCHES is already reached.
int ScspAddRegisterWatch(u32 addr);
int ScspDelRegisterWatch(u32 addr);
void ScspClearRegisterWatches(void);
int ScspGetRegisterWatchCount(void);
// Returns the watched address at "index" (0 .. ScspGetRegisterWatchCount()-1),
// or 0xFFFFFFFF if the index is out of range.
u32 ScspGetRegisterWatchAddr(int index);
// Human readable name of a register offset ("Slot 16 EFSDL/EFPAN", "MVOL",
// ...). Always NUL-terminated, never writes past maxlen.
void ScspGetRegisterName(u32 addr, char *outstring, size_t maxlen);
// Number of entries currently held in the ring buffer (<= SCSP_REGISTER_WATCH_LOG_SIZE).
int ScspGetRegisterWatchLogCount(void);
int ScspSaveRegisterWatchLog(const char *filename);
void ScspClearRegisterWatchLog(void);
void ScspMuteAudio(int flags);
void ScspUnMuteAudio(int flags);
void ScspSetVolume(int volume);
void ScspAsynMain(void * p);
void ScspExecAsync();
void FASTCALL scsp_w_b(SH2_struct *context, u8*, u32, u8);
void FASTCALL scsp_w_w(SH2_struct *context, u8*, u32, u16);
void FASTCALL scsp_w_d(SH2_struct *context, u8*, u32, u32);
u8 FASTCALL scsp_r_b(SH2_struct *context, u8*, u32);
u16 FASTCALL scsp_r_w(SH2_struct *context, u8*, u32);
u32 FASTCALL scsp_r_d(SH2_struct *context, u8*, u32);

void scsp_init(u8 *scsp_ram, void (*sint_hand)(u32), void (*mint_hand)(void));
void scsp_shutdown(void);
void scsp_reset(void);

void scsp_midi_in_send(u8 data);
void scsp_midi_out_send(u8 data);
u8 scsp_midi_in_read(void);
u8 scsp_midi_out_read(void);
void scsp_update(s32 *bufL, s32 *bufR, u32 len);
void scsp_update_monitor(void);
void scsp_update_timer(u32 len);

u32 FASTCALL c68k_word_read(const u32 adr);

void M68KWriteNotify(u32 address, u32 size);
void M68KGetRegisters(m68kregs_struct *regs);
void M68KSetRegisters(m68kregs_struct *regs);
void M68KSetBreakpointCallBack(void (*func)(u32));
int M68KAddCodeBreakpoint(u32 addr);
void M68KSortCodeBreakpoints(void);
int M68KDelCodeBreakpoint(u32 addr);
m68kcodebreakpoint_struct *M68KGetBreakpointList(void);
void M68KClearCodeBreakpoints(void);

void scsp_debug_instrument_get_data(int i, u32 * sa, int * is_muted);
void scsp_debug_instrument_set_mute(u32 sa, int mute);
void scsp_debug_instrument_clear();
void scsp_debug_get_envelope(int chan, int * env, int * state);
void scsp_debug_set_mode(int mode);
void new_scsp_exec(s32 cycles);

void M68KStep(void);

extern void ScspLockThread();
extern void ScspUnLockThread();

#ifdef __cplusplus
}
#endif

#endif
