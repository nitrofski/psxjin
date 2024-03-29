/*  PSXjin - Pc Psx Emulator
 *  Copyright (C) 1999-2003  PSXjin Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
//THIS ALL IS FOR THE CDROM REGISTERS HANDLING
#include "PsxCommon.h"
#include "CdRom.h"

// global variables
cdrStruct cdr;

#define CdlSync         0
#define CdlNop	        1
#define CdlSetloc		2
#define CdlPlay	        3
#define CdlForward		4
#define CdlBackward		5
#define CdlReadN		6
#define CdlStandby		7
#define CdlStop	        8
#define CdlPause        9
#define CdlInit 		10
#define CdlMute	        11
#define CdlDemute		12
#define CdlSetfilter	13
#define CdlSetmode		14
#define CdlGetmode      15
#define CdlGetlocL		16
#define CdlGetlocP		17
#define Cdl18     		18
#define CdlGetTN		19
#define CdlGetTD		20
#define CdlSeekL		21
#define CdlSeekP		22
#define CdlTest 		25
#define CdlID   		26
#define CdlReadS		27
#define CdlReset		28
#define CdlReadToc      30

#define AUTOPAUSE		249
#define READ_ACK		250
#define READ			251
#define REPPLAY_ACK		252
#define REPPLAY			253
#define ASYNC			254
/* don't set 255, it's reserved */

char *CmdName[0x100]= {
	"CdlSync",    "CdlNop",       "CdlSetloc",  "CdlPlay",
	"CdlForward", "CdlBackward",  "CdlReadN",   "CdlStandby",
	"CdlStop",    "CdlPause",     "CdlInit",    "CdlMute",
	"CdlDemute",  "CdlSetfilter", "CdlSetmode", "CdlGetmode",
	"CdlGetlocL", "CdlGetlocP",   "Cdl18",      "CdlGetTN",
	"CdlGetTD",   "CdlSeekL",     "CdlSeekP",   NULL,
	NULL,         "CdlTest",      "CdlID",      "CdlReadS",
	"CdlReset",   NULL,           "CDlReadToc", NULL
};

unsigned char Test04[] = { 0 };
unsigned char Test05[] = { 0 };
unsigned char Test20[] = { 0x98, 0x06, 0x10, 0xC3 };
unsigned char Test22[] = { 0x66, 0x6F, 0x72, 0x20, 0x45, 0x75, 0x72, 0x6F };
unsigned char Test23[] = { 0x43, 0x58, 0x44, 0x32, 0x39 ,0x34, 0x30, 0x51 };

// 1x = 75 sectors per second
// PSXCLK = 1 sec in the ps
// so (PSXCLK / 75) / BIAS = cdr read time (linuzappz)
#define cdReadTime ((PSXCLK / 75) / BIAS)

#define btoi(b)		((b)/16*10 + (b)%16)		/* BCD to u_char */
#define itob(i)		((i)/10*16 + (i)%10)		/* u_char to BCD */

static struct CdrStat stat;
static struct SubQ *subq;

unsigned long CDR_fakeStatus();

#define CDRINT(eCycle) { \
	psxRegs.interrupt|= 0x4; \
	psxRegs.intCycle[2+1] = eCycle; \
	psxRegs.intCycle[2] = psxRegs.cycle; }

#define CDREAD_INT(eCycle) { \
	psxRegs.interrupt|= 0x40000; \
	psxRegs.intCycle[2+16+1] = eCycle; \
	psxRegs.intCycle[2+16] = psxRegs.cycle; }

#define StartReading(type) { \
   	cdr.Reading = type; \
  	cdr.FirstSector = 1; \
  	cdr.Readed = 0xff; \
	AddIrqQueue(READ_ACK, 0x800); \
}	

#define StopReading() { \
	if (cdr.Reading) { \
		cdr.Reading = 0; \
		psxRegs.interrupt&=~0x40000; \
	} \
}

#define StopCdda() { \
	if (cdr.Play) { \
		if (!Config.Cdda) CDRstop(); \
		cdr.StatP&=~0x80; \
		cdr.Play = 0; \
	} \
}

#define SetResultSize(size) { \
    cdr.ResultP = 0; \
	cdr.ResultC = size; \
	cdr.ResultReady = 1; \
}

void ReadTrack() {

	cdr.Prev[0] = itob(cdr.SetSector[0]);
	cdr.Prev[1] = itob(cdr.SetSector[1]);
	cdr.Prev[2] = itob(cdr.SetSector[2]);

#ifdef CDRLOG
	CDRLOG("KEY *** %x:%x:%x\n", cdr.Prev[0], cdr.Prev[1], cdr.Prev[2]);
#endif
	cdr.RErr = CDRreadTrack(cdr.Prev);
}

// cdr.Stat:
#define NoIntr		0
#define DataReady	1
#define Complete	2
#define Acknowledge	3
#define DataEnd		4
#define DiskError	5

void AddIrqQueue(unsigned char irq, unsigned long ecycle) {
	cdr.Irq = irq;
	if (cdr.Stat) {
		cdr.eCycle = ecycle;
	} else {
		CDRINT(ecycle);
	}
}

void cdrInterrupt() {
	int i;
	unsigned char Irq = cdr.Irq;

	if (cdr.Stat) {
		CDRINT(0x800);
		return;
	}

	cdr.Irq = 0xff;
	cdr.Ctrl&=~0x80;

	switch (Irq) {
    	case CdlSync:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge; 
			break;

    	case CdlNop:
			SetResultSize(1);
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			i = stat.Status;
			if (CDRgetStatus(&stat) != -1) {
				stat.Status = CDR_fakeStatus();
				if (stat.Type == 0xff) cdr.Stat = DiskError;
				if (stat.Status & 0x10) {
					cdr.Stat = DiskError;
					cdr.Result[0]|= 0x11;
					cdr.Result[0]&=~0x02;
				}
				else if (i & 0x10) {
					cdr.StatP |= 0x2;
					cdr.Result[0]|= 0x2;
					CheckCdrom();
				}
			}
			break;
			
		case CdlSetloc:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			break;

		case CdlPlay:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP|= 0x2;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Acknowledge;
			cdr.StatP|= 0x80;
//			if ((cdr.Mode & 0x5) == 0x5) AddIrqQueue(REPPLAY, cdReadTime);
			break;

    	case CdlForward:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

    	case CdlBackward:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

    	case CdlStandby:
			cdr.CmdProcess = 0;
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

		case CdlStop:
			cdr.CmdProcess = 0;
			SetResultSize(1);
        	cdr.StatP&=~0x2;
			cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
//        	cdr.Stat = Acknowledge;
			break;

		case CdlPause:
			SetResultSize(1);
			cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			AddIrqQueue(CdlPause + 0x20, 0x800);
			cdr.Ctrl|= 0x80;
			break;

		case CdlPause + 0x20:
			SetResultSize(1);
        	cdr.StatP&=~0x20;
			cdr.StatP|= 0x2;
			cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

    	case CdlInit:
			SetResultSize(1);
        	cdr.StatP = 0x2;
			cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
//			if (!cdr.Init) {
				AddIrqQueue(CdlInit + 0x20, 0x800);
//			}
        	break;

		case CdlInit + 0x20:
			SetResultSize(1);
			cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			cdr.Init = 1;
			break;

    	case CdlMute:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			break;

    	case CdlDemute:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			break;

    	case CdlSetfilter:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge; 
        	break;

		case CdlSetmode:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
        	break;

    	case CdlGetmode:
			SetResultSize(6);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Result[1] = cdr.Mode;
        	cdr.Result[2] = cdr.File;
        	cdr.Result[3] = cdr.Channel;
        	cdr.Result[4] = 0;
        	cdr.Result[5] = 0;
        	cdr.Stat = Acknowledge;
        	break;

    	case CdlGetlocL:
			SetResultSize(8);
//        	for (i=0; i<8; i++) cdr.Result[i] = itob(cdr.Transfer[i]);
        	for (i=0; i<8; i++) cdr.Result[i] = cdr.Transfer[i];
        	cdr.Stat = Acknowledge;
        	break;

    	case CdlGetlocP:
			SetResultSize(8);
			subq = (struct SubQ*) CDRgetBufferSub();
			if (subq != NULL) {
				cdr.Result[0] = subq->TrackNumber;
				cdr.Result[1] = subq->IndexNumber;
		    	memcpy(cdr.Result+2, subq->TrackRelativeAddress, 3);
		    	memcpy(cdr.Result+5, subq->AbsoluteAddress, 3);
			} else {
	        	cdr.Result[0] = 1;
	        	cdr.Result[1] = 1;
	        	cdr.Result[2] = cdr.Prev[0];
	        	cdr.Result[3] = itob((btoi(cdr.Prev[1])) - 2);
	        	cdr.Result[4] = cdr.Prev[2];
		    	memcpy(cdr.Result+5, cdr.Prev, 3);
			}
        	cdr.Stat = Acknowledge;
        	break;

    	case CdlGetTN:
			cdr.CmdProcess = 0;
			SetResultSize(3);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	if (CDRgetTN(cdr.ResultTN) == -1) {
				cdr.Stat = DiskError;
				cdr.Result[0]|= 0x01;
        	} else {
        		cdr.Stat = Acknowledge;
        	    cdr.Result[1] = itob(cdr.ResultTN[0]);
        	    cdr.Result[2] = itob(cdr.ResultTN[1]);
        	}
        	break;

    	case CdlGetTD:
			cdr.CmdProcess = 0;
        	cdr.Track = btoi(cdr.Param[0]);
			SetResultSize(3);
			cdr.StatP|= 0x2;
        	if (CDRgetTD(cdr.Track, cdr.ResultTD) == -1) {
				cdr.Stat = DiskError;
				cdr.Result[0]|= 0x01;
        	} else {
        		cdr.Stat = Acknowledge;
				cdr.Result[0] = cdr.StatP;
	    		cdr.Result[1] = itob(cdr.ResultTD[1]);
        	    cdr.Result[2] = itob(cdr.ResultTD[2]);
	    	}
			break;

    	case CdlSeekL:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
			cdr.StatP|= 0x40;
        	cdr.Stat = Acknowledge;
			cdr.Seeked = 1;
			AddIrqQueue(CdlSeekL + 0x20, 0x800);
			break;

    	case CdlSeekL + 0x20:
			SetResultSize(1);
			cdr.StatP|= 0x2;
			cdr.StatP&=~0x40;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

    	case CdlSeekP:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
			cdr.StatP|= 0x40;
        	cdr.Stat = Acknowledge;
			AddIrqQueue(CdlSeekP + 0x20, 0x800);
			break;

    	case CdlSeekP + 0x20:
			SetResultSize(1);
			cdr.StatP|= 0x2;
			cdr.StatP&=~0x40;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

		case CdlTest:
        	cdr.Stat = Acknowledge;
        	switch (cdr.Param[0]) {
        	    case 0x20: // System Controller ROM Version
					SetResultSize(4);
					memcpy(cdr.Result, Test20, 4);
					break;
				case 0x22:
					SetResultSize(8);
					memcpy(cdr.Result, Test22, 4);
					break;
				case 0x23: case 0x24:
					SetResultSize(8);
					memcpy(cdr.Result, Test23, 4);
					break;
        	}
			break;

    	case CdlID:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			AddIrqQueue(CdlID + 0x20, 0x800);
			break;

		case CdlID + 0x20:
			SetResultSize(8);
        	if (CDRgetStatus(&stat) == -1) {
        		cdr.Result[0] = 0x00; // 0x08 and cdr.Result[1]|0x10 : audio cd, enters cd player
                cdr.Result[1] = 0x00; // 0x80 leads to the menu in the bios, else loads CD
        	}
        	else {
                if (stat.Type == 2) {
                	cdr.Result[0] = 0x08;
                    cdr.Result[1] = 0x10;
	        	}
	        	else {
                    cdr.Result[0] = 0x00;
                    cdr.Result[1] = 0x00;
	        	}
        	}
        	if (!LoadCdBios) cdr.Result[1] |= 0x80;

        	cdr.Result[2] = 0x00;
        	cdr.Result[3] = 0x00;
			strncpy((char *)&cdr.Result[4], "PSXj", 4);
			cdr.Stat = Complete;
			break;

		case CdlReset:
			SetResultSize(1);
        	cdr.StatP = 0x2;
			cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			break;

    	case CdlReadToc:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Acknowledge;
			AddIrqQueue(CdlReadToc + 0x20, 0x800);
			break;

    	case CdlReadToc + 0x20:
			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
        	cdr.Stat = Complete;
			break;

		case AUTOPAUSE:
			cdr.OCUP = 0;
/*			SetResultSize(1);
			StopCdda();
			StopReading();
			cdr.OCUP = 0;
        	cdr.StatP&=~0x20;
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
    		cdr.Stat = DataEnd;
*/			AddIrqQueue(CdlPause, 0x400);
			break;

		case READ_ACK:
			if (!cdr.Reading) return;

			SetResultSize(1);
			cdr.StatP|= 0x2;
        	cdr.Result[0] = cdr.StatP;
			if (cdr.Seeked == 0) {
				cdr.Seeked = 1;
				cdr.StatP|= 0x40;
			}
			cdr.StatP|= 0x20;
        	cdr.Stat = Acknowledge;

			ReadTrack();

//			CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
			CDREAD_INT(0x40000);
			break;

		case REPPLAY_ACK:
			cdr.Stat = Acknowledge;
			cdr.Result[0] = cdr.StatP;
			SetResultSize(1);
			AddIrqQueue(REPPLAY, cdReadTime);
			break;

		case REPPLAY: 
			if ((cdr.Mode & 5) != 5) break;
/*			if (CDRgetStatus(&stat) == -1) {
				cdr.Result[0] = 0;
				cdr.Result[1] = 0;
				cdr.Result[2] = 0;
				cdr.Result[3] = 0;
				cdr.Result[4] = 0;
				cdr.Result[5] = 0;
				cdr.Result[6] = 0;
				cdr.Result[7] = 0;
			} else memcpy(cdr.Result, &stat.Track, 8);
			cdr.Stat = 1;
			SetResultSize(8);
			AddIrqQueue(REPPLAY_ACK, cdReadTime);
*/			break;

		case 0xff:
			return;

		default:
			cdr.Stat = Complete;
			break;
	}

	if (cdr.Stat != NoIntr && cdr.Reg2 != 0x18) psxHu32(0x1070)|= SWAP32((u32)0x4);

#ifdef CDRLOG
	CDRLOG("Cdr Interrupt %x\n", Irq);
#endif
}

void cdrReadInterrupt() {
	u8 *buf;

	if (!cdr.Reading) return;

	if (cdr.Stat) {
		CDREAD_INT(0x800);
		return;
	}

#ifdef CDRLOG
	CDRLOG("KEY END");
#endif

    cdr.OCUP = 1;
	SetResultSize(1);
	cdr.StatP|= 0x22;
	cdr.StatP&=~0x40;
    cdr.Result[0] = cdr.StatP;

	buf = CDRgetBuffer();
	if (buf == NULL) 
		cdr.RErr = -1;

	if (cdr.RErr == -1) {
#ifdef CDRLOG
		fprintf(emuLog, " err\n");
#endif
		memset(cdr.Transfer, 0, 2340);
		cdr.Stat = DiskError;
		cdr.Result[0]|= 0x01;
		ReadTrack();
		CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
		return;
	}

	//printf("[%12d] committing cd transfer of address %d %d %d %d\n",psxRegs.cycle,cdr.SetSector[0],cdr.SetSector[1],cdr.SetSector[2],cdr.SetSector[3]);
	memcpy(cdr.Transfer, buf, 2340);
	cdr.Stat = DataReady;

#ifdef CDRLOG
	fprintf(emuLog, " %x:%x:%x\n", cdr.Transfer[0], cdr.Transfer[1], cdr.Transfer[2]);
#endif

	if ((cdr.Muted == 1) && (cdr.Mode & 0x40) && (!Config.Xa) && (cdr.FirstSector != -1)) { // CD-XA
		if ((cdr.Transfer[4+2] & 0x4) &&
			((cdr.Mode&0x8) ? (cdr.Transfer[4+1] == cdr.Channel) : 1) &&
			(cdr.Transfer[4+0] == cdr.File)) {
			int ret = xa_decode_sector(&cdr.Xa, cdr.Transfer+4, cdr.FirstSector);

			if (!ret) {
				SPUplayADPCMchannel(&cdr.Xa);
				cdr.FirstSector = 0;
			}
			else cdr.FirstSector = -1;
		}
	}

	cdr.SetSector[2]++;
    if (cdr.SetSector[2] == 75) {
        cdr.SetSector[2] = 0;
        cdr.SetSector[1]++;
        if (cdr.SetSector[1] == 60) {
            cdr.SetSector[1] = 0;
            cdr.SetSector[0]++;
        }
    }

    cdr.Readed = 0;

	if ((cdr.Transfer[4+2] & 0x80) && (cdr.Mode & 0x2)) { // EOF
#ifdef CDRLOG
		CDRLOG("AutoPausing Read\n");
#endif
//		AddIrqQueue(AUTOPAUSE, 0x800);
		AddIrqQueue(CdlPause, 0x800);
	}
	else {
		ReadTrack();
		CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
	}
	psxHu32ref(0x1070)|= SWAP32((u32)0x4);
}

/*
cdrRead0:
	bit 0 - 0 REG1 command send / 1 REG1 data read
	bit 1 - 0 data transfer finish / 1 data transfer ready/in progress
	bit 2 - unknown
	bit 3 - unknown
	bit 4 - unknown
	bit 5 - 1 result ready
	bit 6 - 1 dma ready
	bit 7 - 1 command being processed
*/

unsigned char cdrRead0(void) {
	if (cdr.ResultReady) cdr.Ctrl|= 0x20;
	else cdr.Ctrl&=~0x20;

    if (cdr.OCUP) cdr.Ctrl|= 0x40;
//	else cdr.Ctrl&=~0x40;

    // what means the 0x10 and the 0x08 bits? i only saw it used by the bios
    cdr.Ctrl|=0x18;

#ifdef CDRLOG
	CDRLOG("CD0 Read: %x\n", cdr.Ctrl);
#endif
	return psxHu8(0x1800) = cdr.Ctrl;
}

/*
cdrWrite0:
	0 - to send a command / 1 - to get the result
*/

void cdrWrite0(unsigned char rt) {
#ifdef CDRLOG
	CDRLOG("CD0 write: %x\n", rt);
#endif
	cdr.Ctrl = rt | (cdr.Ctrl & ~0x3);

    if (rt == 0) {
		cdr.ParamP = 0;
		cdr.ParamC = 0;
		cdr.ResultReady = 0;
	}
}

unsigned char cdrRead1(void) {
    if (cdr.ResultReady) { // && cdr.Ctrl & 0x1) {
		psxHu8(0x1801) = cdr.Result[cdr.ResultP++];
		if (cdr.ResultP == cdr.ResultC) cdr.ResultReady = 0;
	} else psxHu8(0x1801) = 0;
#ifdef CDRLOG
	CDRLOG("CD1 Read: %x\n", psxHu8(0x1801));
#endif
	return psxHu8(0x1801);
}

void cdrWrite1(unsigned char rt) {
	int i;

#ifdef CDRLOG
	CDRLOG("CD1 write: %x (%s)\n", rt, CmdName[rt]);
#endif
//	psxHu8(0x1801) = rt;
    cdr.Cmd = rt;
	cdr.OCUP = 0;

#ifdef CDRCMD_DEBUG
	SysPrintf("CD1 write: %x (%s)", rt, CmdName[rt]);
	if (cdr.ParamC) {
		SysPrintf(" Param[%d] = {", cdr.ParamC);
		for (i=0;i<cdr.ParamC;i++) SysPrintf(" %x,", cdr.Param[i]);
		SysPrintf("}\n");
	} else SysPrintf("\n");
#endif

	if (cdr.Ctrl & 0x1) return;

    switch(cdr.Cmd) {
    	case CdlSync:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlNop:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlSetloc:
			StopReading();
			cdr.Seeked = 0;
        	for (i=0; i<3; i++) cdr.SetSector[i] = btoi(cdr.Param[i]);
        	cdr.SetSector[3] = 0;
/*        	if ((cdr.SetSector[0] | cdr.SetSector[1] | cdr.SetSector[2]) == 0) {
				*(u32 *)cdr.SetSector = *(u32 *)cdr.SetSectorSeek;
			}*/
			cdr.Ctrl|= 0x80;
        	cdr.Stat = NoIntr;
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlPlay:
        	if (!cdr.SetSector[0] & !cdr.SetSector[1] & !cdr.SetSector[2]) {
            	if (CDRgetTN(cdr.ResultTN) != -1) {
	                if (cdr.CurTrack > cdr.ResultTN[1]) cdr.CurTrack = cdr.ResultTN[1];
                    if (CDRgetTD((unsigned char)(cdr.CurTrack), cdr.ResultTD) != -1) {
		               	int tmp = cdr.ResultTD[2];
                        cdr.ResultTD[2] = cdr.ResultTD[0];
						cdr.ResultTD[0] = tmp;
	                    if (!Config.Cdda) CDRplay(cdr.ResultTD);
					}
                }
			}
    		else if (!Config.Cdda) CDRplay(cdr.SetSector);
    		cdr.Play = 1;
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
    		break;

    	case CdlForward:
        	if (cdr.CurTrack < 0xaa) cdr.CurTrack++;
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlBackward:
        	if (cdr.CurTrack > 1) cdr.CurTrack--;
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlReadN:
			cdr.Irq = 0;
			StopReading();
			cdr.Ctrl|= 0x80;
        	cdr.Stat = NoIntr; 
			StartReading(1);
        	break;

    	case CdlStandby:
			StopCdda();
			StopReading();
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlStop:
			StopCdda();
			StopReading();
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlPause:
			StopCdda();
			StopReading();
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr;
    		AddIrqQueue(cdr.Cmd, 0x40000);
        	break;

		case CdlReset:
    	case CdlInit:
			StopCdda();
			StopReading();
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlMute:
        	cdr.Muted = 0;
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlDemute:
        	cdr.Muted = 1;
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlSetfilter:
        	cdr.File = cdr.Param[0];
        	cdr.Channel = cdr.Param[1];
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlSetmode:
#ifdef CDRLOG
			CDRLOG("Setmode %x\n", cdr.Param[0]);
#endif 
        	cdr.Mode = cdr.Param[0];
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlGetmode:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlGetlocL:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlGetlocP:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
			AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlGetTN:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlGetTD:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlSeekL:
//			((u32 *)cdr.SetSectorSeek)[0] = ((u32 *)cdr.SetSector)[0];
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlSeekP:
//        	((u32 *)cdr.SetSectorSeek)[0] = ((u32 *)cdr.SetSector)[0];
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlTest:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlID:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	case CdlReadS:
			cdr.Irq = 0;
			StopReading();
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
			StartReading(2);
        	break;

    	case CdlReadToc:
			cdr.Ctrl|= 0x80;
    		cdr.Stat = NoIntr; 
    		AddIrqQueue(cdr.Cmd, 0x800);
        	break;

    	default:
#ifdef CDRLOG
			CDRLOG("Unknown Cmd: %x\n", cdr.Cmd);
#endif
			return;
    }
	if (cdr.Stat != NoIntr) psxHu32ref(0x1070)|= SWAP32((u32)0x4);
}

unsigned char cdrRead2(void) {
	unsigned char ret;

	if (cdr.Readed == 0) {
		ret = 0;
	} else {
		ret = *cdr.pTransfer++;
	}

#ifdef CDRLOG
	CDRLOG("CD2 Read: %x\n", ret);
#endif
	return ret;
}

void cdrWrite2(unsigned char rt) {
#ifdef CDRLOG
	CDRLOG("CD2 write: %x\n", rt);
#endif
    if (cdr.Ctrl & 0x1) {
		switch (rt) {
			case 0x07:
	    		cdr.ParamP = 0;
				cdr.ParamC = 0;
				cdr.ResultReady = 1; //0;
				cdr.Ctrl&= ~3; //cdr.Ctrl = 0;
				break;

			default:
				cdr.Reg2 = rt;
				break;
		}
    } else if (!(cdr.Ctrl & 0x1) && cdr.ParamP < 8) {
		cdr.Param[cdr.ParamP++] = rt;
		cdr.ParamC++;
	}
}

unsigned char cdrRead3(void) {
	if (cdr.Stat) {
		if (cdr.Ctrl & 0x1) psxHu8(0x1803) = cdr.Stat | 0xE0;
		else psxHu8(0x1803) = 0xff;
	} else psxHu8(0x1803) = 0;
#ifdef CDRLOG
	CDRLOG("CD3 Read: %x\n", psxHu8(0x1803));
#endif
	return psxHu8(0x1803);
}

void cdrWrite3(unsigned char rt) {
//#ifdef CDRLOG
	//printf("CD3 write: %x\n", rt);
//#endif

    if (rt == 0x07 && (cdr.Ctrl & 0x1)) {
		cdr.Stat = 0;

		if (cdr.Irq == 0xff) { cdr.Irq = 0; return; }
        if (cdr.Irq) CDRINT(cdr.eCycle);
		

		//zero's code
		//rationale: I don't understand what rt == 0x07 && (cdr.Ctrl & 0x1) means but it looks like it is polling for completion.
        //the old code makes no sense. why schedule an interrupt later if the result is not ready?
        //trigger an interrupt NOW if it IS.
		//well, this breaks things unless it is restricted to my XA test case in SoTN, so....
		if ((cdr.Muted == 1) && (cdr.Mode & 0x40)) {
			if (cdr.Reading && cdr.ResultReady) 
			{
				CDREAD_INT(0);
			}
		}
		else 
		//old code
			if (cdr.Reading && !cdr.ResultReady) 
            CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
		
		return;
	}
	if (rt == 0x80 && !(cdr.Ctrl & 0x1) && cdr.Readed == 0) {
		cdr.Readed = 1;
		cdr.pTransfer = cdr.Transfer;

		switch (cdr.Mode&0x30) {
			case 0x10:
			case 0x00: cdr.pTransfer+=12; break;
			default: break;
		}
	}
}

void psxDma3(u32 madr, u32 bcr, u32 chcr) {
	u32 cdsize;
	u8 *ptr;

#ifdef CDRLOG
	CDRLOG("*** DMA 3 *** %lx addr = %lx size = %lx\n", chcr, madr, bcr);
#endif

	switch (chcr) {
		case 0x11000000:
		case 0x11400100:
			if (cdr.Readed == 0) {
#ifdef CDRLOG
				CDRLOG("*** DMA 3 *** NOT READY\n");
#endif
				break;
			}

			cdsize = (bcr & 0xffff) * 4;

			ptr = (u8*)PSXM(madr);
			if (ptr == NULL) {
#ifdef CPU_LOG
				CDRLOG("*** DMA 3 *** NULL Pointer!!!\n");
#endif
				break;
			}

			if (cdsize) {
				memcpy(ptr, cdr.pTransfer, cdsize);
				psxCpu->Clear(madr, cdsize/4);
			} else {
				// Unusual behaviour on 0-sized DMAs. See https://code.google.com/p/psxjin/issues/detail?id=38
				cdsize = (cdr.Mode & 0x20) ? 0x924 : 0x800;
				memcpy(ptr, cdr.pTransfer, cdsize);
				memset(ptr + cdsize, cdr.pTransfer[(cdr.Mode & 0x20) ? 0x920 : 0x7f8], 0x40000 - cdsize);
				psxCpu->Clear(madr, 0x10000);
			}

			cdr.pTransfer+= cdsize;

			break;
		default:
#ifdef CDRLOG
			CDRLOG("Unknown cddma %lx\n", chcr);
#endif
			break;
	}

	HW_DMA3_CHCR &= ~0x01000000;
	DMA_INTERRUPT(3);
}

void cdrReset() {
	memset(&cdr, 0, sizeof(cdr));
	cdr.CurTrack=1;
	cdr.File=1; cdr.Channel=1;
}

int cdrFreeze(EMUFILE *f, int Mode) {
	cdr.pTransfer = (unsigned char *)( (intptr_t)cdr.pTransfer - (intptr_t)cdr.Transfer);
	gzfreeze(&cdr, sizeof(cdr));
	gzfreeze(&stat, sizeof(stat));
	cdr.pTransfer = (unsigned char *)( (intptr_t)cdr.pTransfer + (intptr_t)cdr.Transfer);
	return 0;
}

