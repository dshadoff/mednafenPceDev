/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "pce.h"
#include <trio/trio.h>
#include <iconv.h>

#include "huc6280.h"
#include "dis6280.h"
#include "debug.h"
#include "vce.h"
#include "huc.h"
#include <mednafen/hw_video/huc6270/vdc.h>
#include "pcecd.h"
#include <mednafen/hw_sound/pce_psg/pce_psg.h>
#include <mednafen/cdrom/scsicd.h>
#include <mednafen/hw_misc/arcade_card/arcade_card.h>


#define PCELOG_STDOUT   0
#define PCEFONT_STRINGSEARCH   0

#define CD_BOOT         0xE000
#define CD_RESET        0xE003
#define CD_BASE         0xE006
#define CD_READ         0xE009
#define CD_SEEK         0xE00C
#define CD_EXEC         0xE00F
#define CD_PLAY         0xE012
#define CD_SEARCH       0xE015
#define CD_PAUSE        0xE018
#define CD_STAT         0xE01B
#define CD_SUBQ         0xE01E
#define CD_DINFO        0xE021
#define CD_CONTNTS      0xE024
#define CD_SUBRD        0xE027
#define CD_PCMRD        0xE02A
#define CD_FADE         0xE02D
#define AD_RESET        0xE030
#define AD_TRANS        0xE033
#define AD_READ         0xE036
#define AD_WRITE        0xE039
#define AD_PLAY         0xE03C
#define AD_CPLAY        0xE03F
#define AD_STOP         0xE042
#define AD_STAT         0xE045
#define EX_GETVER       0xE05A
#define EX_SETVEC       0xE05D
#define EX_GETFNT       0xE060
#define EX_VSYNC        0xE07B

extern bool IsHSYNCBreakPoint();
extern bool IsVSYNCBreakPoint();


namespace MDFN_IEN_PCE
{

static HuC6280 ShadowCPU;

extern VCE *vce;
extern ArcadeCard *arcade_card;
static uint32 vram_addr_mask;

static PCE_PSG *psg = NULL;

static bool IsSGX;
static bool IsNAV;
static bool IsSGXorNAV;

static void RedoDH(void);
static void SyscardFuncLog(uint32 PC);

extern uint8 HuCNaviLatch[];

//
//
//
#define NUMBT 64
struct BTEntry
{
 uint32 from;
 uint32 to;
 uint32 vector;
 uint32 branch_count;
 bool valid;
};

static int BTIndex;
static BTEntry BTEntries[NUMBT];
static bool BTEnabled;
//
//
//


typedef struct __PCE_BPOINT 
{
        unsigned int A[2];
        int type;
        bool logical;
} PCE_BPOINT;

static std::vector<PCE_BPOINT> BreakPointsRead, BreakPointsWrite, BreakPointsAux0Read, BreakPointsAux0Write;

static uint8 BreakPointsPC[65536 / 8];
static bool BreakPointsPCUsed;

static uint8 BreakPointsOp[256];
static bool BreakPointsOpUsed;


static bool NeedExecSimu;	// Cache variable, recalculated in RedoDH().

static void (*CPUCB)(uint32 PC, bool bpoint) = NULL;
static bool CPUCBContinuous = false;
static bool FoundBPoint = false;
static void (*LogFunc)(const char *, const char *);
bool PCE_LoggingOn = false;
static uint16 LastPC = 0xBEEF;

static void AddBranchTrace(uint32 from, uint32 to, uint32 vector)
{
 BTEntry *prevbt = &BTEntries[(BTIndex + NUMBT - 1) % NUMBT];

 from &= 0xFFFF;
 to &= 0xFFFF;

 //if(BTEntries[(BTIndex - 1) & 0xF] == PC) return;

 if(prevbt->from == from && prevbt->to == to && prevbt->vector == vector && prevbt->branch_count < 0xFFFFFFFF && prevbt->valid)
  prevbt->branch_count++;
 else
 {
  BTEntries[BTIndex].from = from;
  BTEntries[BTIndex].to = to;
  BTEntries[BTIndex].vector = vector;
  BTEntries[BTIndex].branch_count = 1;
  BTEntries[BTIndex].valid = true;

  BTIndex = (BTIndex + 1) % NUMBT;
 }
}

static void EnableBranchTrace(bool enable)
{
 BTEnabled = enable;
 if(!enable)
 {
  BTIndex = 0;
  memset(BTEntries, 0, sizeof(BTEntries));
 }
 RedoDH();
}

static std::vector<BranchTraceResult> GetBranchTrace(void)
{
 BranchTraceResult tmp;
 std::vector<BranchTraceResult> ret;

 for(int x = 0; x < NUMBT; x++)
 {
  const BTEntry *bt = &BTEntries[(x + BTIndex) % NUMBT];

  if(!bt->valid)
   continue;

  tmp.count = bt->branch_count;
  trio_snprintf(tmp.from, sizeof(tmp.from), "%04X", bt->from);
  trio_snprintf(tmp.to, sizeof(tmp.to), "%04X", bt->to);

  tmp.code[1] = 0;
  switch(bt->vector)
  {
   default: tmp.code[0] = 0;
	    break;

   case 0xFFFE:
	tmp.code[0] = 'R';	// RESET
        break;

   case 0xFFFA:
	tmp.code[0] = 'T';	// TIMER
        break;

   case 0xFFF8:
	tmp.code[0] = '1';	// IRQ1
        break;

   case 0xFFF6:
	tmp.code[0] = '2';	// IRQ2
        break;
  }

  ret.push_back(tmp);
 }
 return(ret);
}

static INLINE bool TestOpBP(uint8 opcode)
{
 return(BreakPointsOp[opcode]);
}

static INLINE bool TestPCBP(uint16 PC)
{
 return((BreakPointsPC[PC >> 3] >> (PC & 0x7)) & 1);
}

void PCEDBG_CheckBP(int type, uint32 address, unsigned int len)
{
 std::vector<PCE_BPOINT>::iterator bpit, bpit_end;

 if(type == BPOINT_READ)
 {
  bpit = BreakPointsRead.begin();
  bpit_end = BreakPointsRead.end();
 }
 else if(type == BPOINT_WRITE)
 {
  bpit = BreakPointsWrite.begin();
  bpit_end = BreakPointsWrite.end();
 }
 else if(type == BPOINT_AUX_READ)
 {
  bpit = BreakPointsAux0Read.begin();
  bpit_end = BreakPointsAux0Read.end();
 }
 else if(type == BPOINT_AUX_WRITE)
 {
  bpit = BreakPointsAux0Write.begin();
  bpit_end = BreakPointsAux0Write.end();
 }
 else
  return;

 for(; bpit != bpit_end; bpit++)
 {
  uint32 tmp_address = address;
  uint32 tmp_len = len;

  while(tmp_len--)
  {
   if(tmp_address >= bpit->A[0] && tmp_address <= bpit->A[1])
   {
    FoundBPoint = true;
    break;
   }
   tmp_address++;
  }
 }
}


uint32 PCEDBG_GetStackPtr(void)
{
 return(HuCPU.GetRegister(HuC6280::GSREG_SP));
}

uint32 PCEDBG_MemPeek(uint32 A, unsigned int bsize, bool hl, bool logical)
{
 uint32 ret = 0;

 PCE_InDebug++;

 for(unsigned int i = 0; i < bsize; i++)
 {
  if(logical)
  {
   A &= 0xFFFF;

   ret |= HuCPU.PeekLogical(A) << (i * 8);
  }
  else
  {
   A &= (1 << 21) - 1;

   ret |= HuCPU.PeekPhysical(A) << (i * 8);
  }

  A++;
 }

 PCE_InDebug--;

 return(ret);
}

void PCEDBG_IRQ(int level)
{
 if(level == 0) // NMI
 {

 }
 else if(level == 1)
  HuCPU.IRQBegin(HuC6280::IQIRQ1);
 else if(level == 2)
  HuCPU.IRQBegin(HuC6280::IQIRQ2);
 else if(level == 3)
  HuCPU.IRQBegin(HuC6280::IQTIMER);
}

class DisPCE : public Dis6280
{
	public:
	DisPCE(void)
	{

	}

	uint8 GetX(void)
	{
	 return(HuCPU.GetRegister(HuC6280::GSREG_X));
	}

	uint8 GetY(void)
	{
	 return(HuCPU.GetRegister(HuC6280::GSREG_Y));
	}

	uint8 Read(uint16 A)
	{
	 uint8 ret;

	 PCE_InDebug++;

	 ret = HuCPU.PeekLogical(A);

	 PCE_InDebug--;

	 return(ret);
	}
};

static DisPCE DisObj;

void PCEDBG_Disassemble(uint32 &a, uint32 SpecialA, char *TextBuf)
{
	uint16 tmpa = a;

	PCE_InDebug++;
	uint8 bank = HuCPU.GetMPR(tmpa);
	PCE_InDebug--;

	DisObj.Disassemble(tmpa, SpecialA, TextBuf, bank, PCE_IsCD);

	a = tmpa;
}

static NO_INLINE void TestRWBP(void)
{
 ShadowCPU.LoadShadow(HuCPU);

 vce->ResetSimulateVDC();

 ShadowCPU.Run(true);

 //printf("%d, %02x\n",ShadowCPU.IRQlow);
 //assert(!ShadowCPU.IRQlow);
}

static bool MachineStateChanged = false;
void PCEDBG_MachineStateChanged(void)
{
 if(PCE_InDebug)
  MachineStateChanged = true;
}

static bool CPUHandler(uint32 PC)
{
 // MachineStateChanged can be set to true under CPUCB().

 PCE_InDebug++;

 FoundBPoint = TestPCBP(PC) | TestOpBP(HuCPU.PeekLogical(PC)) | IsVSYNCBreakPoint() | IsHSYNCBreakPoint();

 if(NeedExecSimu)
  TestRWBP();

 CPUCBContinuous |= FoundBPoint;
 if(CPUCBContinuous && CPUCB)
 {
  vce->Update(HuCPU.Timestamp());
  CPUCB(PC, FoundBPoint);
 }

 if((PC >= CD_BOOT && PC <= EX_VSYNC) && PCE_LoggingOn && PCE_IsCD)
   SyscardFuncLog(PC);

 LastPC = PC;
 PCE_InDebug--;
 assert(!PCE_InDebug);

 bool ret = MachineStateChanged;
 MachineStateChanged = 0;
 return(ret);
}

#if PCEFONT_STRINGSEARCH
static int32 FindInMem(uint16 StartAddr, uint16 EndAddr, uint8 *buf, uint16 LenBytes)
 {
  uint16 ptr;
  uint16 cntr;
  int32 ret = -1;   /* success = addr; fail = -1 */
  int found = 0;

  for (ptr = StartAddr; ptr < EndAddr; ptr++)
  {
   if (HuCPU.PeekLogical(ptr) == *buf)
   {
    found = 1;
    for (cntr = 1; cntr < LenBytes; cntr++)
    {
     if (HuCPU.PeekLogical(ptr+cntr) != *(buf+cntr))
     {
      found = 0;
      break;
     }
    }
   }
   if (found == 1)
    break;
  }

  if (found == 1)
   ret = ptr;

  return(ret);
 }
#endif

 extern uint64 PCE_TimestampBase;
 static void SyscardFuncLog(uint32 PC)
 {
  uint8 AL, AH, BL, BH, CL, CH, DL, DH;
  uint8 mpr[8];
  uint16 temp1, temp2;
  char buf1[128], buf2[128], buf3[128];

  uint64 currtimestamp;

  static uint64 cdstat_lasttimestamp = 0;
  static uint16 cdstat_freq = 0;
  uint64 cdstat_threshold = PCE_MASTER_CLOCK / 90;

  static uint64 adstat_lasttimestamp = 0;
  static uint16 adstat_freq = 0;
  uint64 adstat_threshold = PCE_MASTER_CLOCK / 90;

  static uint64 getfnt_lasttimestamp = 0;
  static uint16 getfnt_index = 0;
  uint64 getfnt_threshold = PCE_MASTER_CLOCK / 90;

  AL = HuCPU.PeekLogical(0x20F8);
  AH = HuCPU.PeekLogical(0x20F9);
  BL = HuCPU.PeekLogical(0x20FA);
  BH = HuCPU.PeekLogical(0x20FB);
  CL = HuCPU.PeekLogical(0x20FC);
  CH = HuCPU.PeekLogical(0x20FD);
  DL = HuCPU.PeekLogical(0x20FE);
  DH = HuCPU.PeekLogical(0x20FF);

  mpr[0] = HuCPU.GetRegister(HuC6280::GSREG_MPR0);
  mpr[1] = HuCPU.GetRegister(HuC6280::GSREG_MPR1);
  mpr[2] = HuCPU.GetRegister(HuC6280::GSREG_MPR2);
  mpr[3] = HuCPU.GetRegister(HuC6280::GSREG_MPR3);
  mpr[4] = HuCPU.GetRegister(HuC6280::GSREG_MPR4);
  mpr[5] = HuCPU.GetRegister(HuC6280::GSREG_MPR5);
  mpr[6] = HuCPU.GetRegister(HuC6280::GSREG_MPR6);
  mpr[7] = HuCPU.GetRegister(HuC6280::GSREG_MPR7);

  currtimestamp = PCE_TimestampBase + HuCPU.GetRegister(HuC6280::GSREG_STAMP);

  /* if int32, rollover occurs at ~199 seconds */
  if (currtimestamp < cdstat_lasttimestamp)
   cdstat_lasttimestamp = currtimestamp;

  if (currtimestamp < adstat_lasttimestamp)
   adstat_lasttimestamp = currtimestamp;

  if (currtimestamp < getfnt_lasttimestamp)
   getfnt_lasttimestamp = currtimestamp;

  /* if beyond threshold, reset counter */
  if ((currtimestamp - cdstat_lasttimestamp) > cdstat_threshold)
   cdstat_freq = 0;

  if ((currtimestamp - adstat_lasttimestamp) > adstat_threshold)
   adstat_freq = 0;

  if ((currtimestamp - getfnt_lasttimestamp) > getfnt_threshold)
   getfnt_index = 0;


  if(PC == CD_READ)
  {
   temp1 = (BH << 8) | BL;
   temp2 = (AH << 8) | AL;

   if (DH == 1)
    PCEDBG_DoLog("BIOS", "Call CD_READ from $%04X, SRC=%02X%02X%02X, DEST=LOC $%04X (Bank $%02X), LEN=$%02X sectors", LastPC, CL, CH, DL, temp1, mpr[temp1 >>13], AL);

   else if (DH == 0xFE)
    PCEDBG_DoLog("BIOS", "Call CD_READ from $%04X, SRC=%02X%02X%02X, DEST=VRAM $%04X, LEN=$%04X bytes", LastPC, CL, CH, DL, temp1, temp2);

   else if (DH == 0xFF)
    PCEDBG_DoLog("BIOS", "Call CD_READ from $%04X, SRC=%02X%02X%02X, DEST=VRAM $%04X, LEN=$%02X sectors", LastPC, CL, CH, DL, temp1, AL);

   else if ((DH > 1) && (DH < 7))
    PCEDBG_DoLog("BIOS", "Call CD_READ from $%04X, SRC=%02X%02X%02X, DEST=BANK $%02X, LEN=$%02X sectors", LastPC, CL, CH, DL, BL, AL);

   else
    PCEDBG_DoLog("BIOS", "Call CD_READ from $%04X, BAD parms AL/AH, BL/BH, CL/CH, DL/DH = %02X/%02X, %02X/%02X, %02X/%02X, %02x/%02X", LastPC, AL, AH, BL, BH, CL, CH, DL, DH);
  }

  if(PC == CD_SEEK)
  {
   PCEDBG_DoLog("BIOS", "Call CD_SEEK from $%04X, DEST=%02X%02X%02X", LastPC, CL, CH, DL);
  }

  if(PC == CD_EXEC)
  {
   temp1 = (BH << 8) | BL;

   if (DH == 1)
    PCEDBG_DoLog("BIOS", "Call CD_EXEC from $%04X, SRC=%02X%02X%02X, DEST=LOC $%04X (Bank $%02X), LEN=$%02X sectors", LastPC, CL, CH, DL, temp1, mpr[temp1>>13], AL);

   else if ((DH > 1) && (DH < 7))
    PCEDBG_DoLog("BIOS", "Call CD_EXEC from $%04X, SRC=%02X%02X%02X, DEST=BANK $%02X, LEN=$%02X sectors", LastPC, CL, CH, DL, BL, AL);

   else
    PCEDBG_DoLog("BIOS", "Call CD_EXEC from $%04X, BAD parms AL/AH, BL/BH, CL/CH, DL/DH = %02X/%02X, %02X/%02X, %02X/%02X, %02x/%02X", LastPC, AL, AH, BL, BH, CL, CH, DL, DH);
  }

  if(PC == CD_PLAY)
  {
   if ((BH & 0xC0) == 0)    /* LBA */
    trio_snprintf(buf1, 128, "Start LBA %02X%02X%02X", AL, AH, BL);

   else if ((BH & 0xC0) == 0x40)    /* MSF */
    trio_snprintf(buf1, 128, "Start MSF %02X:%02X.%02X", AL, AH, BL);

   else if ((BH & 0xC0) == 0x80)    /* track */
    trio_snprintf(buf1, 128, "Start Track %02X", AL);

   else if ((BH & 0xC0) == 0xC0)    /* current */
    trio_snprintf(buf1, 128, "Start Current Loc.");


   if ((DH & 0xC0) == 0)    /* LBA */
    trio_snprintf(buf2, 128, "End LBA %02X%02X%02X", CL, CH, DL);

   else if ((DH & 0xC0) == 0x40)    /* MSF */
    trio_snprintf(buf2, 128, "End MSF %02X:%02X.%02X", CL, CH, DL);

   else if ((DH & 0xC0) == 0x80)    /* track */
    trio_snprintf(buf2, 128, "End Track %02X", CL);

   else if ((DH & 0xC0) == 0xC0)    /* current */
    trio_snprintf(buf2, 128, "End Leadout");


   if ((DH & 0xBF) == 0)    /* Mute */
    trio_snprintf(buf3, 128, "Mode=Mute");

   else if ((DH & 0xBF) == 1)    /* Infinite Repeat */
    trio_snprintf(buf3, 128, "Mode=Infin. Repeat");

   else if ((DH & 0xBF) == 2)    /* Normal, Drive Busy */
    trio_snprintf(buf3, 128, "Mode=Normal:Drive Busy");

   else if ((DH & 0xBF) == 3)    /* Normal, Return Immed. */
    trio_snprintf(buf3, 128, "Mode=Normal:Return Immed.");

   else if ((DH & 0xBF) == 4)    /* Unchanged */
    trio_snprintf(buf3, 128, "Mode=Unchanged");
   else    /* Everything else */
    trio_snprintf(buf3, 128, "Mode= $%02X", DH);


   PCEDBG_DoLog("BIOS", "Call CD_PLAY from $%04X %s %s %s", LastPC, buf1, buf2, buf3);
  }

  if(PC == CD_SEARCH)
  {
   if ((BH & 0x1) == 0)    /* Return When Done */
    trio_snprintf(buf1, 128, "Return When Done");
   else                    /* Return Immed. */
    trio_snprintf(buf1, 128, "Return Immed.");

   if ((BH & 0x2) == 0)    /* Pause */
    trio_snprintf(buf2, 128, "Pause");
   else                    /* Play */
    trio_snprintf(buf2, 128, "Play");

   if ((BH & 0xFC) == 0)            /* LBA */
    PCEDBG_DoLog("BIOS", "Call CD_SEARCH from $%04X, Start LBA %02X%02X%02X, %s, %s", LastPC, AL, AH, BL, buf2, buf1);

   else if ((BH & 0xFC) == 0x40)    /* MSF */
    PCEDBG_DoLog("BIOS", "Call CD_SEARCH from $%04X, Start MSF %02X:%02X.%02X, %s, %s", LastPC, AL, AH, BL, buf2, buf1);

   else if ((BH & 0xFC) == 0x80)    /* track */
    PCEDBG_DoLog("BIOS", "Call CD_SEARCH from $%04X, Start Track %02X, %s, %s", LastPC, AL, buf2, buf1);

   else if ((BH & 0xC0) == 0xC0)    /* current */
    PCEDBG_DoLog("BIOS", "Call CD_SEARCH from $%04X, Bad Parms AL/AH, BL/BH, CL/CH, DL/DH = %02X/%02X, %02X/%02X, %02X/%02X, %02X/%02X", LastPC, AL, AH, BL, BH, CL, CH, DL, DH);
  }

  if(PC == CD_PAUSE)
  {
   PCEDBG_DoLog("BIOS", "Call CD_PAUSE from $%04X", LastPC);
  }

  if(PC == CD_STAT)
  {
   temp1 = HuCPU.GetRegister(HuC6280::GSREG_A);

   trio_snprintf(buf1, 128, " ");

 /*printf("prev = %08X, curr = %08X, diff = %08X, cdstat_freq = %02X\n", cdstat_lasttimestamp, currtimestamp, (currtimestamp - cdstat_lasttimestamp), cdstat_freq);*/

   /* if less than 1/30 sec between calls, mark once then suppress */
   if ((currtimestamp - cdstat_lasttimestamp) < cdstat_threshold)
   {
    if (cdstat_freq > 0)
     trio_snprintf(buf1, 128, ", Frequent (suppressed)");  /* show message on the second log */

    if (cdstat_freq < 10)
     cdstat_freq++;
   }

   if (cdstat_freq <= 2)
   {
    if (temp1 == 0)
     PCEDBG_DoLog("BIOS", "Call CD_STAT from $%04X, Drive busy check%s", LastPC, buf1);

    else
     PCEDBG_DoLog("BIOS", "Call CD_STAT from $%04X, Drive ready check%s", LastPC, buf1);
   }

   cdstat_lasttimestamp = currtimestamp;
  }

  if(PC == CD_SUBQ)
  {
   temp1 = (BH << 8) | BL;
   PCEDBG_DoLog("BIOS", "Call CD_SUBQ from $%04X, Buffer addr = $%04X (Bank $%02X)", LastPC, temp1, mpr[temp1>>13]);
  }

  if(PC == CD_DINFO)
  {
   temp1 = (BH << 8) | BL;

   if (AL == 0)
    trio_snprintf(buf1, 128, "Min/Max Track");

   else if (AL == 1)
    trio_snprintf(buf1, 128, "Leadout MSF");

   else if (AL == 2)
    trio_snprintf(buf1, 128, "Track MSF/SUBQ (Track %02X)", AH);

   else if (AL == 3)
    trio_snprintf(buf1, 128, "Track LBA/SUBQ (Track %02X)", AH);

   else
    trio_snprintf(buf1, 128, "Invalid Request ($%02X)", AL);


   PCEDBG_DoLog("BIOS", "Call CD_DINFO from $%04X, Buffer=$%04X (Bank $%02X), %s", LastPC, temp1, mpr[temp1>>13], buf1);
  }

  if(PC == CD_CONTNTS)
  {
   PCEDBG_DoLog("BIOS", "Call CD_CONTNTS from $%04X", LastPC);
  }

  if(PC == CD_SUBRD)
  {
   PCEDBG_DoLog("BIOS", "Call CD_SUBRD from $%04X", LastPC);
  }

  if(PC == CD_PCMRD)
  {
   temp1 = HuCPU.GetRegister(HuC6280::GSREG_A);

   if (temp1 == 0)
    PCEDBG_DoLog("BIOS", "Call CD_PCMRD from $%04X, Right channel", LastPC);
   else
    PCEDBG_DoLog("BIOS", "Call CD_PCMRD from $%04X, Left channel", LastPC);
  }

  if(PC == CD_FADE)
  {
   temp1 = HuCPU.GetRegister(HuC6280::GSREG_A);

   if (temp1 == 0)
    trio_snprintf(buf1, 128, "Cancel Fade");

   else if (temp1 == 8)
    trio_snprintf(buf1, 128, "PCM Fadeout 6.0 sec");

   else if (temp1 == 0x0A)
    trio_snprintf(buf1, 128, "ADPCM Fadeout 6.0 sec");

   else if (temp1 == 0x0C)
    trio_snprintf(buf1, 128, "PCM Fadeout 2.5 sec");

   else if (temp1 == 0x0E)
    trio_snprintf(buf1, 128, "ADPCM Fadeout 2.5 sec");

   else
   trio_snprintf(buf1, 128, "Error Value $%02X", temp1);


   PCEDBG_DoLog("BIOS", "Call CD_FADE from $%04X, %s", LastPC, buf1);
  }

  if(PC == AD_RESET)
  {
   PCEDBG_DoLog("BIOS", "Call AD_RESET from $%04X", LastPC);
  }

  if(PC == AD_TRANS)
  {
   temp1 = (BH << 8) | BL;

   if (DH == 0)
    PCEDBG_DoLog("BIOS", "Call AD_TRANS from $%04X, SRC=%02X%02X%02X, DEST=ADPCM $%04X, LEN=$%02X sectors", LastPC, CL, CH, DL, temp1, AL);

   else
    PCEDBG_DoLog("BIOS", "Call AD_TRANS from $%04X, SRC=%02X%02X%02X, DEST=ADPCM (Current), LEN=$%02X sectors", LastPC, CL, CH, DL, AL);
  }

  if(PC == AD_READ)
  {
   temp1 = (BH << 8) | BL;
   temp2 = (CH << 8) | CL;

   if (DH == 0)
    PCEDBG_DoLog("BIOS", "Call AD_READ from $%04X, SRC=ADPCM $%04X, DEST=LOC $%04X (Bank $%02X)", LastPC, temp2, temp1, mpr[temp1>>13]);

   else if (DH == 0xff)
    PCEDBG_DoLog("BIOS", "Call AD_READ from $%04X, SRC=ADPCM $%04X, DEST=VRAM $%04X", LastPC, temp2, temp1);

   else if ((DH >=2) && (DH <= 6))
    PCEDBG_DoLog("BIOS", "Call AD_READ from $%04X, SRC=ADPCM $%04X, DEST=Bank $%02X", LastPC, temp2, BL);

   else
    PCEDBG_DoLog("BIOS", "Call AD_READ from $%04X, bad registers AL/AH = %02X/%02X, BL/BH = %02X/%02X, CL/CH = %02X/%02X, DL/DH = %02X/%02X", LastPC, AL, AH, BL, BH, CL, CH, DL, DH);
  }

  if(PC == AD_WRITE)
  {
   temp1 = (BH << 8) | BL;
   temp2 = (CH << 8) | CL;

   if (DH == 0)
    PCEDBG_DoLog("BIOS", "Call AD_WRITE from $%04X, SRC=LOC $%04X (Bank $%02X), DEST=ADPCM $%04X, NUMBYTES=$%04X", LastPC, temp1, mpr[temp1>>13], temp2, ((AH<<8)|AL));

   else if (DH == 0xff)
    PCEDBG_DoLog("BIOS", "Call AD_WRITE from $%04X, SRC=VRAM $%04X, DEST=ADPCM $%04X, NUMBYTES=$%04X", LastPC, temp1, temp2, ((AH<<8)|AL));

   else if ((DH >=2) && (DH <= 6))
    PCEDBG_DoLog("BIOS", "Call AD_WRITE from $%04X, SRC=Bank $%02X, DEST=ADPCM $%04X, NUMBYTES=$%04X", LastPC, BL, temp2, ((AH<<8)|AL));

   else
    PCEDBG_DoLog("BIOS", "Call AD_WRITE from $%04X, bad registers AL/AH = %02X/%02X, BL/BH = %02X/%02X, CL/CH = %02X/%02X, DL/DH = %02X/%02X", LastPC, AL, AH, BL, BH, CL, CH, DL, DH);
  }

  if(PC == AD_PLAY)
  {
   temp1 = (BH << 8) | BL;
   temp2 = (AH << 8) | AL;

   if ((DL & 0x80) == 0)
    trio_snprintf(buf1, 128, "Play Mode: Auto-stop");
   else
    trio_snprintf(buf1, 128, "Play Mode: Repeat");

   if ((DL & 1) == 0)
    trio_snprintf(buf2, 128, "Cntr Mode: Set ADR,Len,Rate");
   else
    trio_snprintf(buf2, 128, "Cntr Mode: Set PREV ADR,Len,Rate");

   PCEDBG_DoLog("BIOS", "Call AD_PLAY from $%04X, SRC=ADPCM $%04X, LEN=$%04X bytes, FREQ=%dKHz, %s, %s", LastPC, temp1, temp2, 32/(16-(DH & 0x0f)), buf1, buf2);
  }

  if(PC == AD_CPLAY)
  {
   PCEDBG_DoLog("BIOS", "Call AD_CPLAY from $%04X, SRC LBA=%02X%02X%02X, NumRec=%02X%02X%02X, FREQ=%d KHz", LastPC, CL, CH, DL, BL, AH, AL, 32/(16-(DH & 0x0f)) );
  }

  if(PC == AD_STOP)
  {
   PCEDBG_DoLog("BIOS", "Call AD_STOP from $%04X", LastPC);
  }

  if(PC == AD_STAT)
  {
   trio_snprintf(buf1, 128, " ");

   /* if less than 1/30 sec between calls, mark once then suppress */
   if ((currtimestamp - adstat_lasttimestamp) < adstat_threshold)
   {
    if (adstat_freq > 0)
     trio_snprintf(buf1, 128, ", Frequent (suppressed)");  /* show message on the second log */

    if (adstat_freq < 10)
     adstat_freq++;
   }

   if (adstat_freq <= 2)
   {
    PCEDBG_DoLog("BIOS", "Call AD_STAT from $%04X%s", LastPC, buf1);
   }

   adstat_lasttimestamp = currtimestamp;
  }

  if(PC == EX_GETVER)
  {
   PCEDBG_DoLog("BIOS", "Call EX_GETVER from $%04X", LastPC);
  }

  if(PC == EX_SETVEC)
  {
   temp1 = HuCPU.GetRegister(HuC6280::GSREG_A);
   temp2 = (HuCPU.GetRegister(HuC6280::GSREG_Y) << 8) | HuCPU.GetRegister(HuC6280::GSREG_X);

   if (temp1 == 0)
    trio_snprintf(buf1, 128, "IRQ2");

   else if (temp1 == 1)
    trio_snprintf(buf1, 128, "IRQ1");

   else if (temp1 == 2)
    trio_snprintf(buf1, 128, "TIMER");

   else if (temp1 == 3)
    trio_snprintf(buf1, 128, "NMI");

   else if (temp1 == 4)
    trio_snprintf(buf1, 128, "SYNC");

   else if (temp1 == 5)
    trio_snprintf(buf1, 128, "RCR");

   else if (temp1 == 6)
    trio_snprintf(buf1, 128, "SOFT RESET");

   else
    trio_snprintf(buf1, 128, "INVALID ($%02X)", temp1);

   PCEDBG_DoLog("BIOS", "Call EX_SETVEC from $%04X, Set %s to $%04X (Bank $%02X)", LastPC, buf1, temp2, mpr[temp2>>13]);
  }

  if(PC == EX_GETFNT)
  {
   uint16 sjis_glyph;
#if PCEFONT_STRINGSEARCH
   static int32 found_loc1 = -1;
   static int32 found_loc2 = -1;
   static int32 ptr_loc = -1;
   static bool found = FALSE;
   static uint8 getfnt_buf[2000];
   uint8 getfnt_ptr[10];
#endif

   temp1 = (BH << 8) | BL;
   sjis_glyph = HuCPU.PeekLogical(0x20F8) | (HuCPU.PeekLogical(0x20F9) << 8);

   if (DH == 0)
    trio_snprintf(buf1, 128, "16x16");

   else if (DH == 1)
    trio_snprintf(buf1, 128, "12x12");

   else
    trio_snprintf(buf1, 128, "     ");

#if PCEFONT_STRINGSEARCH
   /* if less than 1/30 sec between calls, mark once then suppress */
   if ((currtimestamp - getfnt_lasttimestamp) > getfnt_threshold)
   {
    getfnt_index = 0;
    getfnt_buf[0] = AH;
    getfnt_buf[1] = AL;
    found = FALSE;
   }
   else
   {
    getfnt_index++;
    getfnt_buf[(getfnt_index * 2)] = AH;
    getfnt_buf[(getfnt_index * 2) + 1] = AL;

    if ((getfnt_index >= 3) && !found)
    {

     found_loc1 = FindInMem(0x0000, 0xFFFF, &getfnt_buf[0], (getfnt_index + 1) * 2);

     if (found_loc1 > -1)
     {
      found_loc2 = FindInMem(found_loc1 + 1, 0xFFFF, &getfnt_buf[0], (getfnt_index + 1) * 2);

      if (found_loc2 == -1)  /* found exactly one instance */
      {
       found = TRUE;
       getfnt_ptr[0] = found_loc1 & 0xff;
       getfnt_ptr[1] = found_loc1 >> 8;

       ptr_loc = FindInMem(0x0000, 0xFFFF, &getfnt_ptr[0], 2);
       if (ptr_loc > -1)  /* definitve string; also found ptr */
       {
        trio_snprintf(buf2, 128, "Found string @ $%04X (Bank $%02X), ptr @ $%04X (Bank $%02X)", found_loc1, mpr[found_loc1>>13], ptr_loc, mpr[ptr_loc>>13]);
       }
       else  /* definitve string; ptr not found*/
       {
        trio_snprintf(buf2, 128, "Found string @ $%04X (Bank $%02X), ptr not located", found_loc1, mpr[found_loc1>>13]);
       }
      }
      else /* found more than one instance of string */
      {
        trio_snprintf(buf2, 128, "Found string candidates @ $%04X (Bank $%02X), @ $%04X (Bank $%02X)", found_loc1, mpr[found_loc1>13], found_loc2, mpr[found_loc2>>13]);
      }
     }
     else /* did not find string in mem */
     {
        trio_snprintf(buf2, 128, " ");
     }
    }
    else
    {
     found_loc1 = -1;  /* turn off the second line, since the string was already located */
    }
   }
#endif // PCEFONT_STRINGSEARCH

   PCEDBG_DoLog("BIOS", "Call EX_GETFNT from $%04X, Buf=$%04X (Bank $%02X), SJIS=0x%04X = %s, FontNum=%02X (%s)", LastPC, temp1, mpr[temp1>>13], sjis_glyph, PCEDBG_ShiftJIS_to_UTF8(sjis_glyph), DH, buf1);

#if PCEFONT_STRINGSEARCH   
   if (found_loc1 > -1)
    PCEDBG_DoLog("BIOS", "     EX_GETFNT String Search:  %s", buf2);
#endif // PCEFONT_STRINGSEARCH

   getfnt_lasttimestamp = currtimestamp;
  }
 }

static DECLFR(ReadHandler)
{
 std::vector<PCE_BPOINT>::iterator bpit;

 if((A & 0x1FFFFF) >= (0xFF * 8192) && (A & 0x1FFFFF) <= (0xFF * 8192 + 0x3FF))
 {
  VDC_SimulateResult result;

  int which_vdc = vce->SimulateReadVDC(A & 0x80000003, &result);

  if(result.ReadCount)
   PCEDBG_CheckBP(BPOINT_AUX_READ, (which_vdc << 16) | result.ReadStart, result.ReadCount);

  if(result.WriteCount)
   PCEDBG_CheckBP(BPOINT_AUX_WRITE, (which_vdc << 16) | result.WriteStart, result.WriteCount);

  if(result.RegReadDone)
   PCEDBG_CheckBP(BPOINT_AUX_READ, 0x20000 | (which_vdc << 16) | result.RegRWIndex, 1);
 }

 for(bpit = BreakPointsRead.begin(); bpit != BreakPointsRead.end(); bpit++)
 {
  unsigned int testA = bpit->logical ? ShadowCPU.GetLastLogicalReadAddr() : A;

  if(testA >= bpit->A[0] && testA <= bpit->A[1])
  {
   FoundBPoint = 1;
   break;
  }
 }

 return(HuCPU.PeekPhysical(A));
}

static DECLFW(WriteHandler)
{
 std::vector<PCE_BPOINT>::iterator bpit;

 if((A & 0x1FFFFF) >= (0xFF * 8192) && (A & 0x1FFFFF) <= (0xFF * 8192 + 0x3FF))
 {
  VDC_SimulateResult result;

  int which_vdc = vce->SimulateWriteVDC(A & 0x80000003, V, &result);

  if(result.ReadCount)
   PCEDBG_CheckBP(BPOINT_AUX_READ, (which_vdc << 16) | result.ReadStart, result.ReadCount);

  if(result.WriteCount)
   PCEDBG_CheckBP(BPOINT_AUX_WRITE, (which_vdc << 16) | result.WriteStart, result.WriteCount);

  if(result.RegWriteDone)
   PCEDBG_CheckBP(BPOINT_AUX_WRITE, 0x20000 | (which_vdc << 16) | result.RegRWIndex, 1);
 }


 for(bpit = BreakPointsWrite.begin(); bpit != BreakPointsWrite.end(); bpit++)
 {
  unsigned int testA;

  if(!bpit->logical)
   testA = A;
  else
  {
   if(A & 0x80000000) continue;		// Ignore ST0/ST1/ST2 writes, which always use hardcoded physical addresses.

   testA = ShadowCPU.GetLastLogicalWriteAddr();
  }

  if(testA >= bpit->A[0] && testA <= bpit->A[1])
  {
   FoundBPoint = 1;
   break;
  }
 }
}

static void RedoDH(void)
{
 bool BPointsUsed;

 NeedExecSimu = BreakPointsRead.size() || BreakPointsWrite.size() || BreakPointsAux0Read.size() || BreakPointsAux0Write.size();

 BPointsUsed = BreakPointsPCUsed || BreakPointsOpUsed || BreakPointsRead.size() || BreakPointsWrite.size() || 
		BreakPointsAux0Read.size() || BreakPointsAux0Write.size();

 if(BPointsUsed || CPUCB || PCE_LoggingOn)
  HuCPU.SetCPUHook(CPUHandler, BTEnabled ? AddBranchTrace : NULL);
 else
  HuCPU.SetCPUHook(NULL, BTEnabled ? AddBranchTrace : NULL);
}

void PCEDBG_AddBreakPoint(int type, unsigned int A1, unsigned int A2, bool logical)
{
 PCE_BPOINT tmp;

 tmp.A[0] = A1;
 tmp.A[1] = A2;
 tmp.type =type;
 tmp.logical = logical;

 if(type == BPOINT_READ)
  BreakPointsRead.push_back(tmp);
 else if(type == BPOINT_WRITE)
  BreakPointsWrite.push_back(tmp);
 else if(type == BPOINT_PC)
 {
  for(unsigned int i = A1; i <= A2; i++)
  {
   if((unsigned int)i < 65536)
   {
    BreakPointsPCUsed = true;
    BreakPointsPC[i >> 3] |= 1 << (i & 0x7);
   }
  }
 }
 else if(type == BPOINT_AUX_READ)
  BreakPointsAux0Read.push_back(tmp);
 else if(type == BPOINT_AUX_WRITE)
  BreakPointsAux0Write.push_back(tmp);
 else if(type == BPOINT_OP)
 {
  for(unsigned int i = A1; i <= A2; i++)
  {
   if((unsigned int)i < 256)
   {
    BreakPointsOpUsed = true;
    BreakPointsOp[i] = 1;
   }
  }   
 }

 RedoDH();
}

void PCEDBG_FlushBreakPoints(int type)
{
 std::vector<PCE_BPOINT>::iterator bpit;

 if(type == BPOINT_READ)
  BreakPointsRead.clear();
 else if(type == BPOINT_WRITE)
  BreakPointsWrite.clear();
 else if(type == BPOINT_PC)
 {
  memset(BreakPointsPC, 0, sizeof(BreakPointsPC));
  BreakPointsPCUsed = false;
 }
 else if(type == BPOINT_AUX_READ)
  BreakPointsAux0Read.clear();
 else if(type == BPOINT_AUX_WRITE)
  BreakPointsAux0Write.clear();
 else if(type == BPOINT_OP)
 {
  memset(BreakPointsOp, 0, sizeof(BreakPointsOp));
  BreakPointsOpUsed = false;
 }

 RedoDH();
}

static void SetCPUCallback(void (*callb)(uint32 PC, bool bpoint), bool continuous)
{
 CPUCB = callb;
 CPUCBContinuous = continuous;
 RedoDH();
}

void PCEDBG_DoLog(const char *type, const char *format, ...)
{
 if(LogFunc)
 {
  char *temp;

#if PCELOG_STDOUT
   uint64 currtimestamp;
   currtimestamp = PCE_TimestampBase + HuCPU.GetRegister(HuC6280::GSREG_STAMP);
#endif

  va_list ap;
  va_start(ap, format);

  temp = trio_vaprintf(format, ap);
#if PCELOG_STDOUT
   fprintf(stdout, "%012lu:%s:%s\n", currtimestamp, type, temp);
   fflush(stdout);
#endif

  LogFunc(type, temp);
  free(temp);

  va_end(ap);
 }
}

static iconv_t sjis_ict = (iconv_t)-1;

void PCEDBG_SetLogFunc(void (*func)(const char *, const char *))
{
 LogFunc = func;

 PCE_LoggingOn = func ? true : false;
 SCSICD_SetLog(func ? PCEDBG_DoLog : NULL);

 if(PCE_LoggingOn)
 {
  if(sjis_ict == (iconv_t)-1)
   sjis_ict = iconv_open("UTF-8", "shift_jis");
 }
 else
 {
  if(sjis_ict != (iconv_t)-1)
  {
   iconv_close(sjis_ict);
   sjis_ict = (iconv_t)-1;
  }
 }
 RedoDH();
}

char *PCEDBG_ShiftJIS_to_UTF8(const uint16 sjc)
{
 static char ret[16];
 char inbuf[3];
 char *in_ptr, *out_ptr;
 size_t ibl, obl;

 ret[0] = 0;

 if(sjc < 256)
 {
  inbuf[0] = sjc;
  inbuf[1] = 0;
  ibl = 1;
 }
 else
 {
  inbuf[0] = sjc >> 8;
  inbuf[1] = sjc >> 0;
  inbuf[2] = 0;
  ibl = 2;
 }

 in_ptr = inbuf;
 out_ptr = ret;
 obl = 16;

 iconv(sjis_ict, (ICONV_CONST char **)&in_ptr, &ibl, &out_ptr, &obl);

 return(ret);
}

static uint32 GetRegister_HuCNavi(const unsigned int id, char *special, const uint32 special_len)
{
 if(id == HuC6280::GSREG_NAVI0)
 {
  return(HuCNaviLatch[0]);
 }
 else if(id == HuC6280::GSREG_NAVI1)
 {
  return(HuCNaviLatch[1]);
 }
 else if(id == HuC6280::GSREG_NAVI2)
 {
  return(HuCNaviLatch[2]);
 }
}

static void SetRegister_HuCNavi(const unsigned int id, uint32 value)
{
 if(id == HuC6280::GSREG_NAVI0)
 {
  HuCNaviLatch[0] = value;
 }
 else if(id == HuC6280::GSREG_NAVI1)
 {
  HuCNaviLatch[1] = value;
 }
 else if(id == HuC6280::GSREG_NAVI2)
 {
  HuCNaviLatch[2] = value;
 }
}


extern uint64 PCE_TimestampBase;
static uint32 GetRegister_HuC6280(const unsigned int id, char *special, const uint32 special_len)
{
 if(id == HuC6280::GSREG_STAMP)
 {
  return(PCE_TimestampBase + HuCPU.GetRegister(id, special, special_len));
 }
 else if(id == HuC6280::GSREG_SECONDS)
 {
  return((uint32)(PCE_TimestampBase/PCE_MASTER_CLOCK));
 }
 return(HuCPU.GetRegister(id, special, special_len));
}

static void SetRegister_HuC6280(const unsigned int id, uint32 value)
{
 HuCPU.SetRegister(id, value);
}

static uint32 GetRegister_PSG(const unsigned int id, char *special, const uint32 special_len)
{
 return(psg->GetRegister(id, special, special_len));
}

static void SetRegister_PSG(const unsigned int id, uint32 value)
{
 psg->SetRegister(id, value);
}

static uint32 GetRegister_VDC(const unsigned int id, char *special, const uint32 special_len)
{
 if(id & 0x8000)
  return(vce->GetRegister(id &~ 0x8000, special, special_len));

 return(vce->GetRegisterVDC(0, id, special, special_len));
}

static void SetRegister_VDC(const unsigned int id, uint32 value)
{
 if(id & 0x8000)
 {
  vce->SetRegister(id &~ 0x8000, value);
  return;
 }

 vce->SetRegisterVDC(0, id, value);
}

static uint32 GetRegister_SGXVDC(const unsigned int id, char *special, const uint32 special_len)
{
 if(id & 0x8000)
  return(vce->GetRegister(id &~ 0x8000, special, special_len));

 return(vce->GetRegisterVDC(1, id, special, special_len));
}

static void SetRegister_SGXVDC(const unsigned int id, uint32 value)
{
 if(id & 0x8000)
 {
  vce->SetRegister(id &~ 0x8000, value);
  return;
 }

 vce->SetRegisterVDC(1, id, value);
}

static uint32 GetRegister_CD(const unsigned int id, char *special, const uint32 special_len)
{
 return(PCECD_GetRegister(id, special, special_len));
}

static void SetRegister_CD(const unsigned int id, uint32 value)
{
 PCECD_SetRegister(id, value);
}


static const RegType Regs_HuCNavi[] =
{
	{ 0, 0, "---NAVI----", "", 0xFFFF },
	{ HuC6280::GSREG_NAVI0, 1, "Bnk20-3F", "MetaBank 20-3F", 1 },
	{ HuC6280::GSREG_NAVI1, 1, "Bnk40-5F", "MetaBank 40-5F", 1 },
	{ HuC6280::GSREG_NAVI2, 1, "Bnk60-7F", "MetaBank 60-7F", 1 },
	{ 0, 0, "-----------", "", 0xFFFF },

	{ 0, 0, "", "", 0 },
};

static const RegGroupType RegsGroup_HuCNavi =
{
	"HuC6280",
        Regs_HuCNavi,
        GetRegister_HuCNavi,
        SetRegister_HuCNavi
};

static const RegType Regs_HuC6280[] =
{
	{ 0, 0, "----CPU----", "", 0xFFFF },

	{ HuC6280::GSREG_PC,    5, "PC",   "Program Counter",   2 },
	{ HuC6280::GSREG_A,     8, "A",    "Accumulator",       1 },
	{ HuC6280::GSREG_X,     8, "X",    "X Index Register",  1 },
	{ HuC6280::GSREG_Y,     8, "Y",    "Y Index Register",  1 },
	{ HuC6280::GSREG_SP,    7, "SP",   "Stack Pointer",     1 },
	{ HuC6280::GSREG_P,     8, "P",    "Status",            1 },

	{ 0, 0, "   NVTBDIZC", "", 0xFFFF },
	{ 0, 0, "----MPR----", "", 0xFFFF },

	{ HuC6280::GSREG_MPR0,  5, "MPR0", "MPR0",              1 },
	{ HuC6280::GSREG_MPR1,  5, "MPR1", "MPR1",              1 },
	{ HuC6280::GSREG_MPR2,  5, "MPR2", "MPR2",              1 },
	{ HuC6280::GSREG_MPR3,  5, "MPR3", "MPR3",              1 },
	{ HuC6280::GSREG_MPR4,  5, "MPR4", "MPR4",              1 },
	{ HuC6280::GSREG_MPR5,  5, "MPR5", "MPR5",              1 },
	{ HuC6280::GSREG_MPR6,  5, "MPR6", "MPR6",              1 },
	{ HuC6280::GSREG_MPR7,  5, "MPR7", "MPR7",              1 },

	{ 0, 0, "---HWREG---", "", 0xFFFF },

	{ HuC6280::GSREG_SPD,   6, "SPD",  "CPU Speed",         1 },
	{ HuC6280::GSREG_IRQM,  5, "IRQM", "IRQ Mask",          1 },

	{ 0, 0, "---TIMER---", "", 0xFFFF },

	{ HuC6280::GSREG_TIMS,  5, "TIMS", "Timer Status",      1 },
	{ HuC6280::GSREG_TIMV,  5, "TIMV", "Timer Value",       1 },
	{ HuC6280::GSREG_TIML,  5, "TIML", "Timer Load",        1 },
	{ HuC6280::GSREG_TIMD,  3, "TIMD", "Timer Div Counter", 2 },

	{ 0, 0, "---CLOCK---", "", 0xFFFF },

	{ HuC6280::GSREG_SECONDS, 4, "Sec",  "Seconds",         2 },
	{ HuC6280::GSREG_STAMP,   1, "TS",   "Timestamp",       4 },

	{ 0, 0, "-----------", "", 0xFFFF },

	{ 0, 0, "", "", 0 },
};

static const RegGroupType RegsGroup_HuC6280 =
{
	"HuC6280",
        Regs_HuC6280,
        GetRegister_HuC6280,
        SetRegister_HuC6280
};


#define CHPDMOO(n)      \
	{ 0, 0, "----CH"#n"----", "", 0xFFFF },	\
	{ PSG_GSREG_CH0_FREQ    | (n << 8), 3, "Freq",       "PSG Ch"#n" Frequency(Period)", 2 }, \
	{ PSG_GSREG_CH0_CTRL    | (n << 8), 5, "Ctrl",       "PSG Ch"#n" Control",           1 }, \
	{ PSG_GSREG_CH0_BALANCE | (n << 8), 2, "Balance",    "PSG Ch"#n" Balance",           1 }, \
	{ PSG_GSREG_CH0_WINDEX  | (n << 8), 3, "WIndex",     "PSG Ch"#n" Waveform Index",    1 }, \
	{ PSG_GSREG_CH0_SCACHE  | (n << 8), 3, "SCache",     "PSG Ch"#n" Sample Cache",      1 }

static const RegType Regs_PSG[] =
{
	{ 0, 0, "----PSG----", "", 0xFFFF },

	{ PSG_GSREG_SELECT,   3, "Select",    "PSG Channel Select",    1 },
	{ PSG_GSREG_GBALANCE, 1, "GBalance",  "PSG Global Balance",    1 },
	{ PSG_GSREG_LFOFREQ,  2, "LFOFreq",   "PSG LFO Freq",          1 },
	{ PSG_GSREG_LFOCTRL,  2, "LFOCtrl",   "PSG LFO Control",       1 },

	CHPDMOO(0),
	CHPDMOO(1),
	CHPDMOO(2),
	CHPDMOO(3),
	CHPDMOO(4),
	{ PSG_GSREG_CH4_NCTRL, 4, "Noise",    "PSG Ch4 Noise Control", 1 },
/*	{ PSG_GSREG_CH4_LFSR,  1, "LFSR",     "PSG Ch4 Noise LFSR",    3 }, */
	CHPDMOO(5),
	{ PSG_GSREG_CH5_NCTRL, 4, "Noise",    "PSG Ch5 Noise Control", 1 },
/*	{ PSG_GSREG_CH5_LFSR,  1, "LFSR",     "PSG Ch5 Noise LFSR",    3 }, */

	{ 0, 0, "-----------", "", 0xFFFF },

	{ 0, 0, "", "", 0 },
};

static const RegGroupType RegsGroup_PSG =
{
	"PSG",
	Regs_PSG,
	GetRegister_PSG,
	SetRegister_PSG
};

static const RegType Regs_VDC[] =
{
	{ 0, 0, "---VDC-A---", "", 0xFFFF },

	{ 0x0000 | VDC::GSREG_SELECT,   3, "Select", "Register Select",         1 },
	{ 0x0000 | VDC::GSREG_STATUS,   3, "Status", "Status",                  1 },

	{ 0x0000 | VDC::GSREG_MAWR,     3, "MAWR",   "Memory Write Address",    2 },
	{ 0x0000 | VDC::GSREG_MARR,     3, "MARR",   "Memory Read Address",     2 },
	{ 0x0000 | VDC::GSREG_CR,       5, "CR",     "Control",                 2 },
	{ 0x0000 | VDC::GSREG_RCR,      4, "RCR",    "Raster Counter",          2 },
	{ 0x0000 | VDC::GSREG_BXR,      4, "BXR",    "X Scroll",                2 },
	{ 0x0000 | VDC::GSREG_BYR,      4, "BYR",    "Y Scroll",                2 },
	{ 0x0000 | VDC::GSREG_MWR,      4, "MWR",    "Memory Width",            2 },

	{ 0x0000 | VDC::GSREG_HSR,      4, "HSR",    "HSR",                     2 },
	{ 0x0000 | VDC::GSREG_HDR,      4, "HDR",    "HDR",                     2 },
	{ 0x0000 | VDC::GSREG_VSR,      4, "VSR",    "VSR",                     2 },
	{ 0x0000 | VDC::GSREG_VDR,      4, "VDR",    "VDR",                     2 },

	{ 0x0000 | VDC::GSREG_VCR,      4, "VCR",    "VCR",                     2 },
	{ 0x0000 | VDC::GSREG_DCR,      4, "DCR",    "DMA Control",             2 },
	{ 0x0000 | VDC::GSREG_SOUR,     3, "SOUR",   "VRAM DMA Source Address", 2 },
	{ 0x0000 | VDC::GSREG_DESR,     3, "DESR",   "VRAM DMA Dest Address",   2 },
	{ 0x0000 | VDC::GSREG_LENR,     3, "LENR",   "VRAM DMA Length",         2 },
	{ 0x0000 | VDC::GSREG_DVSSR,    2, "DVSSR",  "DVSSR Update Address",    2 },

	{ 0, 0, "----VCE----", "", 0xFFFF },

	{ 0x8000 | VCE::GSREG_CR,       4, "VCECR",  "VCE Control Register",            1 },
	{ 0x8000 | VCE::GSREG_CTA,      1, "VCECTA", "VCE Color/Palette Table Address", 2 },

	{ 0, 0, "---VIDEO---", "", 0xFFFF },

	{ 0x8000 | VCE::GSREG_FRAMENUM, 1, "FR",     "Current Frame",                4 },
	{ 0x8000 | VCE::GSREG_SCANLINE, 3, "Line",   "Current Scanline",                2 },

	{ 0, 0, "-----------", "", 0xFFFF },

	{ 0, 0, "", "", 0 },
};

static const RegGroupType RegsGroup_VDC =
{
	"VDC",
	Regs_VDC,
	GetRegister_VDC,
	SetRegister_VDC
};


static const RegType Regs_SGXVDC[] =
{
	{ 0, 0, "---VDC-B---", "", 0xFFFF },

	{ 0x0000 | VDC::GSREG_SELECT, 3, "Select", "Register Select, VDC-B",         1 },
	{ 0x0000 | VDC::GSREG_STATUS, 3, "Status", "Status, VDC-B",                  1 },

	{ 0x0000 | VDC::GSREG_MAWR,   3, "MAWR",   "Memory Write Address, VDC-B",    2 },
	{ 0x0000 | VDC::GSREG_MARR,   3, "MARR",   "Memory Read Address, VDC-B",     2 },
	{ 0x0000 | VDC::GSREG_CR,     5, "CR",     "Control, VDC-B",                 2 },
	{ 0x0000 | VDC::GSREG_RCR,    4, "RCR",    "Raster Counter, VDC-B",          2 },
	{ 0x0000 | VDC::GSREG_BXR,    4, "BXR",    "X Scroll, VDC-B",                2 },
	{ 0x0000 | VDC::GSREG_BYR,    4, "BYR",    "Y Scroll, VDC-B",                2 },
	{ 0x0000 | VDC::GSREG_MWR,    4, "MWR",    "Memory Width, VDC-B",            2 },

	{ 0x0000 | VDC::GSREG_HSR,    4, "HSR",    "HSR, VDC-B",                     2 },
	{ 0x0000 | VDC::GSREG_HDR,    4, "HDR",    "HDR, VDC-B",                     2 },
	{ 0x0000 | VDC::GSREG_VSR,    4, "VSR",    "VSR, VDC-B",                     2 },
	{ 0x0000 | VDC::GSREG_VDR,    4, "VDR",    "VDR, VDC-B",                     2 },

	{ 0x0000 | VDC::GSREG_VCR,    4, "VCR",    "VCR, VDC-B",                     2 },
	{ 0x0000 | VDC::GSREG_DCR,    4, "DCR",    "DMA Control, VDC-B",             2 },
	{ 0x0000 | VDC::GSREG_SOUR,   3, "SOUR",   "VRAM DMA Source Address, VDC-B", 2 },
	{ 0x0000 | VDC::GSREG_DESR,   3, "DESR",   "VRAM DMA Dest Address, VDC-B",   2 },
	{ 0x0000 | VDC::GSREG_LENR,   3, "LENR",   "VRAM DMA Length, VDC-B",         2 },
	{ 0x0000 | VDC::GSREG_DVSSR,  2, "DVSSR",  "DVSSR Update Address, VDC-B",    2 },

	{ 0, 0, "-VDC-MIXER-", "", 0xFFFF },

	{ 0x8000 | VCE::GSREG_ST_MODE,        3, "STMode", "ST(ST0/ST1/ST2) Mode/Target", 1 },
	{ 0x8000 | VCE::GSREG_PRIORITY_0,     4, "PRIO0",  "VPC Priority Register 0",     1 },
	{ 0x8000 | VCE::GSREG_PRIORITY_1,     4, "PRIO1",  "VPC Priority Register 1",     1 },
	{ 0x8000 | VCE::GSREG_WINDOW_WIDTH_0, 2, "WIND0",  "VPC Window Width Register 0", 2 },
	{ 0x8000 | VCE::GSREG_WINDOW_WIDTH_1, 2, "WIND1",  "VPC Window Width Register 1", 2 },

	{ 0, 0, "-----------", "", 0xFFFF },

	{ 0, 0, "", "", 0 },
};

static const RegGroupType RegsGroup_SGXVDC =
{
	"VDC-B",
	Regs_SGXVDC,
	GetRegister_SGXVDC,
	SetRegister_SGXVDC
};

static const RegType Regs_CD[] =
{
	{ 0, 0, "---CDROM---", "", 0xFFFF },

	{ CD_GSREG_BSY,               6, "BSY",      "SCSI BSY",                      1 },
	{ CD_GSREG_REQ,               6, "REQ",      "SCSI REQ",                      1 },
	{ CD_GSREG_MSG,               6, "MSG",      "SCSI MSG",                      1 },
	{ CD_GSREG_IO,                7, "IO",       "SCSI IO",                       1 },
	{ CD_GSREG_CD,                7, "CD",       "SCSI CD",                       1 },
	{ CD_GSREG_SEL,               6, "SEL",      "SCSI SEL",                      1 },

	{ 0, 0, "---ADPCM---", "", 0xFFFF },

	{ CD_GSREG_ADPCM_CONTROL,     2, "Control",  "ADPCM Control",                 1 },
	{ CD_GSREG_ADPCM_FREQ,        5, "Freq",     "ADPCM Frequency",               1 },
	{ CD_GSREG_ADPCM_CUR,         4, "CUR",      "ADPCM Current 12-bit Value",    2 },
	{ CD_GSREG_ADPCM_WRADDR,      1, "WrAddr",   "ADPCM Write Address",           2 },
	{ CD_GSREG_ADPCM_RDADDR,      1, "RdAddr",   "ADPCM Read Address",            2 },
	{ CD_GSREG_ADPCM_LENGTH,      1, "Length",   "ADPCM Length",                  2 },
	{ CD_GSREG_ADPCM_PLAYNIBBLE,  1, "PlNibble", "ADPCM Play Nibble Select",      1 },

	{ CD_GSREG_ADPCM_PLAYING,     2, "Playing",  "ADPCM Playing Flag",            1 },
	{ CD_GSREG_ADPCM_HALFREACHED, 5, "Half",     "ADPCM Half-point Reached Flag", 1 },
	{ CD_GSREG_ADPCM_ENDREACHED,  6, "End",      "ADPCM End Reached Flag",        1 },

	{ 0, 0, "-----------", "", 0xFFFF },

	{ 0, 0, "", "", 0 },
};

static const RegGroupType RegsGroup_CD =
{
	"CD",
	Regs_CD,
	GetRegister_CD,
	SetRegister_CD
};

static void Do16BitGet(const char *name, uint32 Address, uint32 Length, uint8 *Buffer)
{
 int wc = 0;

 if(!strcmp(name, "vram0"))
  wc = 0;
 else if(!strcmp(name, "vram1"))
  wc = 1;
 else if(!strcmp(name, "sat0"))
  wc = 2;
 else if(!strcmp(name, "sat1"))
  wc = 3;
 else if(!strcmp(name, "pram"))
  wc = 4;

 while(Length)
 {
  uint16 data;

  if(wc == 4)
   data = vce->PeekPRAM((Address >> 1) & 0x1FF);
  else if(wc & 2) 
   data = vce->PeekVDCSAT(wc & 1, (Address >> 1) & 0xFF);
  else
   data = vce->PeekVDCVRAM(wc & 1, (Address >> 1) & vram_addr_mask);

  if((Address & 1) || Length == 1)
  {
   *Buffer = data >> ((Address & 1) << 3);
   Buffer++;
   Address++;
   Length--;
  }
  else
  {
   Buffer[0] = data & 0xFF;
   Buffer[1] = data >> 8;

   Buffer += 2;
   Address += 2;
   Length -= 2;
  }
 }
}

static void Do16BitPut(const char *name, uint32 Address, uint32 Length, uint32 Granularity, bool hl, const uint8 *Buffer)
{
 int wc = 0;

 if(!strcmp(name, "vram0"))
  wc = 0;
 else if(!strcmp(name, "vram1"))
  wc = 1;
 else if(!strcmp(name, "sat0"))
  wc = 2;
 else if(!strcmp(name, "sat1"))
  wc = 3;
 else if(!strcmp(name, "pram"))
  wc = 4;

 while(Length)
 {
  uint16 data;
  int inc_amount;

  if((Address & 1) || Length == 1)
  {
   if(wc == 4)
    data = vce->PeekPRAM((Address >> 1) & 0x1FF);
   else if(wc & 2)
    data = vce->PeekVDCSAT(wc & 1, (Address >> 1) & 0xFF);
   else
    data = vce->PeekVDCVRAM(wc & 1, (Address >> 1) & vram_addr_mask);

   data &= ~(0xFF << ((Address & 1) << 3));
   data |= *Buffer << ((Address & 1) << 3);

   inc_amount = 1;
  }
  else
  {
   data = Buffer[0] | (Buffer[1] << 8);
   inc_amount = 2;
  }

  if(wc == 4)
   vce->PokePRAM((Address >> 1) & 0x1FF, data);
  else if(wc & 2)
   vce->PokeVDCSAT(wc & 1, (Address >> 1) & 0xFF, data);
  else
   vce->PokeVDCVRAM(wc & 1, (Address >> 1) & vram_addr_mask, data);

  Buffer += inc_amount;
  Address += inc_amount;
  Length -= inc_amount;
 }
}


static void GetAddressSpaceBytes(const char *name, uint32 Address, uint32 Length, uint8 *Buffer)
{
 PCE_InDebug++;

 if(!strcmp(name, "cpu"))
 {
  while(Length--)
  {
   Address &= 0xFFFF;

   *Buffer = HuCPU.PeekLogical(Address);

   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "physical"))
 {
  while(Length--)
  {
   Address &= 0x1FFFFF;

   *Buffer = HuCPU.PeekPhysical(Address);

   Address++;
   Buffer++;
  }

 }
 else if(!strcmp(name, "ram"))
 {
  while(Length--)
  {
   Address &= (IsSGXorNAV ? 32768 : 8192) - 1;
   *Buffer = PCE_PeekMainRAM(Address);
   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "navirom"))
 {
  while(Length--)
  {
   Address &= 0x7FFFFF;
   *Buffer = PCE_PeekNaviROM(Address);
   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "adpcm"))
  ADPCM_PeekRAM(Address, Length, Buffer);
 else if(!strcmp(name, "acram"))
  arcade_card->PeekRAM(Address, Length, Buffer);
 else if(!strcmp(name, "bram"))
 {
  while(Length--)
  {
   Address &= 0x7FF;
   *Buffer = HuC_PeekBRAM(Address);
   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "mb128"))
 {
  while(Length--)
  {
   Address &= 0x1FFFF;
   *Buffer = HuC_PeekMB128(Address);
   Address++;
   Buffer++;
  }
 }
 else if(!strncmp(name, "psgram", 6))
  psg->PeekWave(name[6] - '0', Address, Length, Buffer);

 PCE_InDebug--;
}

static void PutAddressSpaceBytes(const char *name, uint32 Address, uint32 Length, uint32 Granularity, bool hl, const uint8 *Buffer)
{
 PCE_InDebug++;

 if(!strcmp(name, "cpu"))
 {
  while(Length--)
  {
   Address &= 0xFFFF;

   HuCPU.PokeLogical(Address, *Buffer, hl);

   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "physical"))
 {
  while(Length--)
  {
   Address &= 0x1FFFFF;

   HuCPU.PokePhysical(Address, *Buffer, hl);

   Address++;
   Buffer++;
  }

 }
 else if(!strcmp(name, "ram"))
 {
  while(Length--)
  {
   Address &= (IsSGXorNAV ? 32768 : 8192) - 1;
   PCE_PokeMainRAM(Address, *Buffer);
   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "navirom"))
 {
  while(Length--)
  {
   Address &= 0x7FFFFF;
   PCE_PokeNaviROM(Address, *Buffer);
   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "adpcm"))
  ADPCM_PokeRAM(Address, Length, Buffer);
 else if(!strcmp(name, "acram"))
  arcade_card->PokeRAM(Address, Length, Buffer);
 else if(!strcmp(name, "bram"))
 {
  while(Length--)
  {
   Address &= 0x7FF;
   HuC_PokeBRAM(Address, *Buffer);
   Address++;
   Buffer++;
  }
 }
 else if(!strcmp(name, "mb128"))
 {
  while(Length--)
  {
   Address &= 0x1FFFF;
   HuC_PokeMB128(Address, *Buffer);
   Address++;
   Buffer++;
  }
 }
 else if(!strncmp(name, "psgram", 6))
  psg->PokeWave(name[6] - '0', Address, Length, Buffer);

 PCE_InDebug--;
}


static void SetGraphicsDecode(MDFN_Surface *surface, int line, int which, int xscroll, int yscroll, int pbn)
{
 vce->SetGraphicsDecode(surface, line, which, xscroll, yscroll, pbn);
}

DebuggerInfoStruct PCEDBGInfo =
{
 false,
 "shift_jis",
 7,
 1,             // Instruction alignment(bytes)
 16,
 21,
 0x2000,
 0x2000, // ZP
 0x2100, // SP
 PCEDBG_GetStackPtr,

 PCEDBG_MemPeek,
 PCEDBG_Disassemble,
 NULL,
 PCEDBG_IRQ,
 NULL, //NESDBG_GetVector,
 PCEDBG_FlushBreakPoints,
 PCEDBG_AddBreakPoint,
 SetCPUCallback,
 EnableBranchTrace,
 GetBranchTrace,
 SetGraphicsDecode,
 PCEDBG_SetLogFunc,
};

static void Cleanup(void)
{

}

void PCEDBG_Kill(void)
{
 Cleanup();
}

void PCEDBG_Init(bool sgx, bool nav, PCE_PSG *new_psg, const uint32 vram_size)
{
 try
 {
  BTIndex = 0;
  memset(BTEntries, 0, sizeof(BTEntries));
  BTEnabled = false;

  BreakPointsPCUsed = false;
  BreakPointsOpUsed = false;

  memset(BreakPointsOp, 0, sizeof(BreakPointsOp));
  memset(BreakPointsPC, 0, sizeof(BreakPointsPC));

  ShadowCPU.Init(false);

  for(int x = 0; x < 0x100; x++)
  {
   ShadowCPU.SetFastRead(x, NULL);

   ShadowCPU.SetReadHandler(x, ReadHandler);
   ShadowCPU.SetWriteHandler(x, WriteHandler);
  }

  ShadowCPU.Power();

  psg = new_psg;

  IsSGX = sgx;
  IsNAV = nav;
  IsSGXorNAV = IsSGX | IsNAV;

  if(IsNAV)
   MDFNDBG_AddRegGroup(&RegsGroup_HuCNavi);

  MDFNDBG_AddRegGroup(&RegsGroup_HuC6280);

  MDFNDBG_AddRegGroup(&RegsGroup_VDC);

  if(IsSGX)
   MDFNDBG_AddRegGroup(&RegsGroup_SGXVDC);

  ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "cpu", "CPU Logical", 16, 0, true);
  ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "physical", "CPU Physical", 21, 0, true);
  ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "ram", "RAM", IsSGXorNAV ? 15 : 13, 0, true);

  if(IsNAV)
  {
   ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "navirom", "Navigator ROM", 23, 0, false);
  }

  ASpace_AddPalette(Do16BitGet, Do16BitPut, "pram", "VCE Palette RAM", 10, 0, false, 2, ENDIAN_LITTLE, PALETTE_PCE);

  vram_addr_mask = (vram_size - 1);
  if(IsSGX)
  {
   ASpace_Add16(Do16BitGet, Do16BitPut, "vram0", "VDC-A VRAM", MDFN_log2(vram_size) + 1, 0, true, ENDIAN_LITTLE);
   ASpace_Add16(Do16BitGet, Do16BitPut, "sat0", "VDC-A SAT", 8 + 1, 0, true, ENDIAN_LITTLE);
   ASpace_Add16(Do16BitGet, Do16BitPut, "vram1", "VDC-B VRAM", MDFN_log2(vram_size) + 1, 0, true, ENDIAN_LITTLE);
   ASpace_Add16(Do16BitGet, Do16BitPut, "sat1", "VDC-B SAT", 8 + 1, 0, true, ENDIAN_LITTLE);
  }
  else
  {
   ASpace_Add16(Do16BitGet, Do16BitPut, "vram0", "VDC VRAM", MDFN_log2(vram_size) + 1, 0, true, ENDIAN_LITTLE);
   ASpace_Add16(Do16BitGet, Do16BitPut, "sat0", "VDC SAT", 8 + 1, 0, true, ENDIAN_LITTLE);
  }

  if(PCE_IsCD)
  {
   ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "adpcm", "ADPCM RAM", 16, 0, false);
   MDFNDBG_AddRegGroup(&RegsGroup_CD);
  }

  if(arcade_card)
  {
   ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "acram", "Arcade Card RAM", 21, 0, false);
  }

  if(HuC_IsBRAMAvailable())
  {
   ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "bram", "BRAM", 11, 0, false);
  }

  if(HuC_IsMB128Available())
  {
   ASpace_Add(GetAddressSpaceBytes, PutAddressSpaceBytes, "mb128", "Memory Base 128", 17, 0, false);
  }

  for(int x = 0; x < 6; x++)
  {
     AddressSpaceType newt;
     char tmpname[128], tmpinfo[128];

     trio_snprintf(tmpname, 128, "psgram%d", x);
     trio_snprintf(tmpinfo, 128, "PSG Ch %d RAM", x);

     newt.GetAddressSpaceBytes = GetAddressSpaceBytes;
     newt.PutAddressSpaceBytes = PutAddressSpaceBytes;

     newt.name = std::string(tmpname);
     newt.long_name = std::string(tmpinfo);
     newt.TotalBits = 5;
     newt.NP2Size = 0;
     newt.PossibleSATB = false;

     newt.Wordbytes  = 1;
     newt.Endianness = ENDIAN_LITTLE;
     newt.MaxDigit   = 1;
     newt.IsPalette  = false;

     newt.IsWave = true;
     newt.WaveFormat = ASPACE_WFMT_UNSIGNED;
     newt.WaveBits = 5;

     ASpace_Add(newt); //PSG_GetAddressSpaceBytes, PSG_PutAddressSpaceBytes, tmpname, tmpinfo, 5);
  }
  MDFNDBG_AddRegGroup(&RegsGroup_PSG);
 }
 catch(...)
 {
  Cleanup();
  throw;
 }
}


};
