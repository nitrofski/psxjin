/***************************************************************************
                       record.c  -  description
                             -------------------
    begin                : Fri Nov 09 2001
    copyright            : (C) 2001 by Darko Matesic
    email                : thedarkma@ptt.yu
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version. See also the license.txt file for *
 *   additional informations.                                              *
 *                                                                         *
 ***************************************************************************/

//*************************************************************************//
// History of changes:
//
// 2003/04/17 - Avery Lee
// - repaired AVISetStreamFormat call
//
// 2001/12/18 - Darko Matesic
// - two types of compression (16bit & 24bit)
// - FPSE 24bit MDEC support
//
// 2001/11/09 - Darko Matesic
// - first revision
//
//*************************************************************************//

#include "stdafx.h"
#include <stdio.h>
#include <direct.h>
#include <vfw.h>
#include <math.h>
#include "externals.h"
#include "gpu_record.h"
#include "gpu.h"
#include "PsxCommon.h"

BOOL			RECORD_RECORDING = FALSE;
BOOL			RUN_ONCE = FALSE;
BITMAPINFOHEADER	RECORD_BI = {40,0,0,1,16,0,0,2048,2048,0,0};
unsigned char		RECORD_BUFFER[1600*1200*3];
unsigned long		RECORD_INDEX;
unsigned long		RECORD_RECORDING_MODE;
unsigned long		RECORD_VIDEO_SIZE;
unsigned long		RECORD_RECORDING_WIDTH;
unsigned long		RECORD_RECORDING_HEIGHT;
unsigned long		RECORD_FRAME_RATE_SCALE;
unsigned long		RECORD_TOTAL_BYTES;
unsigned long		RECORD_COMPRESSION_MODE;
COMPVARS			RECORD_COMPRESSION2;
unsigned char		RECORD_COMPRESSION_STATE2[1048576];


PCOMPVARS			pCompression = NULL;
AVISTREAMINFO		strhdr;
PAVIFILE			pfile = NULL;
PAVISTREAM			ps = NULL;
PAVISTREAM			psCompressed = NULL;
AVICOMPRESSOPTIONS	opts;

unsigned long		frame;
unsigned long		skip;

//--------------------------------------------------------------------

BOOL RECORD_Start()
{
	char filename[256];
	static FILE *data;
	RECORD_TOTAL_BYTES = 0;
	if (RECORD_RECORDING_MODE==0)
	{
		sprintf(&filename[0],"%s%s%03d_%s.avi",Movie.AviDrive,Movie.AviDirectory,Movie.AviCount,Movie.AviFnameShort);
		RECORD_BI.biWidth = Config.CurWinX;
		RECORD_BI.biHeight = Config.CurWinY;
	}
	else
	{
		Config.SplitAVI = true;
		sprintf(&filename[0],"%s%s%03d_%s-%dx%d.avi",Movie.AviDrive,Movie.AviDirectory,Movie.AviCount,Movie.AviFnameShort,Config.CurWinX, Config.CurWinY);
		RECORD_BI.biWidth = Config.CurWinX;
		RECORD_BI.biHeight = Config.CurWinY;
	}
	if (HIWORD(VideoForWindowsVersion())<0x010a)
	{
		MessageBox(NULL,"Video for Windows version is too old !\nAbording Recording.","Error", MB_OK|MB_ICONSTOP);
		return FALSE;
	}
	if ((data=fopen(filename,"wb"))==NULL) goto error;
	RECORD_BI.biBitCount = 24;
	RECORD_BI.biSizeImage = RECORD_BI.biWidth*RECORD_BI.biHeight*3;
	pCompression = &RECORD_COMPRESSION2;
	AVIFileInit();
	if (AVIFileOpen(&pfile,filename,OF_WRITE | OF_CREATE,NULL)!=AVIERR_OK) goto error;
	memset(&strhdr,0,sizeof(strhdr));
	strhdr.fccType                = streamtypeVIDEO;
	strhdr.fccHandler             = pCompression->fccHandler;
	strhdr.dwScale                = RECORD_FRAME_RATE_SCALE + 1;
	strhdr.dwRate                 = (PSXDisplay.PAL)?50:60;// FPS
	strhdr.dwSuggestedBufferSize  = RECORD_BI.biSizeImage;
	SetRect(&strhdr.rcFrame,0,0,RECORD_BI.biWidth,RECORD_BI.biHeight);
	if (AVIFileCreateStream(pfile,&ps,&strhdr)!=AVIERR_OK) goto error;

	opts.fccType			= pCompression->fccType;
	opts.fccHandler			= pCompression->fccHandler;
	opts.dwKeyFrameEvery	= pCompression->lKey;
	opts.dwQuality			= pCompression->lQ;
	opts.dwBytesPerSecond	= pCompression->lDataRate;
	opts.dwFlags			= AVICOMPRESSF_DATARATE|AVICOMPRESSF_KEYFRAMES|AVICOMPRESSF_VALID;
	opts.lpFormat			= &RECORD_BI;
	opts.cbFormat			= sizeof(RECORD_BI);
	opts.lpParms			= pCompression->lpState;
	opts.cbParms			= pCompression->cbState;
	opts.dwInterleaveEvery	= 0;

	if (AVIMakeCompressedStream(&psCompressed,ps,&opts,NULL)!=AVIERR_OK) goto error;

//if(AVIStreamSetFormat(psCompressed,0,&RECORD_BI,RECORD_BI.biSizeImage)!=AVIERR_OK) goto error;
// fixed:
	if (AVIStreamSetFormat(psCompressed,0,&RECORD_BI,sizeof(RECORD_BI))!=AVIERR_OK) goto error;

	frame = 0;
	skip = RECORD_FRAME_RATE_SCALE;
	return TRUE;	
error:
	RECORD_Stop();
	RECORD_RECORDING = FALSE;
	return FALSE;
	}

//--------------------------------------------------------------------

void RECORD_Stop()
{
	if (ps) AVIStreamClose(ps);
	if (psCompressed) AVIStreamClose(psCompressed);
	if (pfile) AVIFileClose(pfile);	
	AVIFileExit();	
}

//--------------------------------------------------------------------

BOOL RECORD_WriteFrame()
{
	long ByteBuffer = 0;
	if (skip)
	{
		skip--;
		return TRUE;
	}
	skip=RECORD_FRAME_RATE_SCALE;
	if (!RECORD_GetFrame()) return FALSE;
	if (FAILED(AVIStreamWrite(psCompressed,frame,1,RECORD_BUFFER,RECORD_BI.biSizeImage,AVIIF_KEYFRAME,NULL,&ByteBuffer)))
	{
		RECORD_Stop();
		return FALSE;
	}
	frame++;
	RECORD_TOTAL_BYTES += ByteBuffer;
	if (RECORD_TOTAL_BYTES > 2007152000) 
	{
		RECORD_Stop();
		Movie.AviCount++;
		RECORD_Start();
	}


	return TRUE;
}

//--------------------------------------------------------------------

BOOL RECORD_GetFrame()
{
	static unsigned short *srcs,*src,*dests,cs;
	static unsigned char *srcc,*destc;
	static long x,y,cx,cy,ax,ay;
	static unsigned long cl;

	if (PSXDisplay.Disabled)
	{
		memset(RECORD_BUFFER,0,RECORD_BI.biSizeImage);
		return TRUE;
	}

	srcs = (unsigned short*)&psxVuw[PSXDisplay.DisplayPosition.x+(PSXDisplay.DisplayPosition.y<<10)];
	dests = (unsigned short*)RECORD_BUFFER;
	destc = (unsigned char*)RECORD_BUFFER;
	ax = (PSXDisplay.DisplayMode.x*(s64)65536)/RECORD_BI.biWidth;
	ay = (PSXDisplay.DisplayMode.y*(s64)65536)/RECORD_BI.biHeight;
	cy = (PSXDisplay.DisplayMode.y-1)<<16;
	if (RECORD_BI.biBitCount==16)
	{
		if (PSXDisplay.RGB24)
		{
			if (iFPSEInterface)
			{
				for (y=0;y<RECORD_BI.biHeight;y++)
				{
					srcc = (unsigned char*)&srcs[(cy&0xffff0000)>>6];
					cx = 0;
					for (x=0;x<RECORD_BI.biWidth;x++)
					{
						cl = *((unsigned long*)&srcc[(cx>>16)*3]);
						*(dests++) = (unsigned short)(((cl&0xf80000)>>9)|((cl&0xf800)>>6)|((cl&0xf8)>>3));
						cx += ax;
					}
					cy -= ay;
					if (cy<0) cy=0;
				}
			}
			else
			{
				for (y=0;y<RECORD_BI.biHeight;y++)
				{
					srcc = (unsigned char*)&srcs[(cy&0xffff0000)>>6];
					cx = 0;
					for (x=0;x<RECORD_BI.biWidth;x++)
					{
						cl = *((unsigned long*)&srcc[(cx>>16)*3]);
						*(dests++) = (unsigned short)(((cl&0xf8)<<7)|((cl&0xf800)>>6)|((cl&0xf80000)>>19));
						cx += ax;
					}
					cy -= ay;
					if (cy<0) cy=0;
				}
			}
		}
		else
		{
			for (y=0;y<RECORD_BI.biHeight;y++)
			{
				src = &srcs[(cy&0xffff0000)>>6];
				cx = 0;
				for (x=0;x<RECORD_BI.biWidth;x++)
				{
					cs = src[cx>>16];
					*(dests++) = ((cs&0x7c00)>>10)|(cs&0x03e0)|((cs&0x001f)<<10);
					cx += ax;
				}
				cy -= ay;
				if (cy<0) cy=0;
			}
		}
	}
	else if (RECORD_BI.biBitCount==24)
	{
		if (PSXDisplay.RGB24)
		{
			if (iFPSEInterface)
			{
				for (y=0;y<RECORD_BI.biHeight;y++)
				{
					srcc = (unsigned char*)&srcs[(cy&0xffff0000)>>6];
					cx = 0;
					for (x=0;x<RECORD_BI.biWidth;x++)
					{
						cl = *((unsigned long*)&srcc[(cx>>16)*3]);
						*(destc++) = (unsigned char)(cl&0xff);
						*(destc++) = (unsigned char)((cl&0xff00)>>8);
						*(destc++) = (unsigned char)((cl&0xff0000)>>16);
						cx += ax;
					}
					cy -= ay;
					if (cy<0) cy=0;
				}
			}
			else
			{
				for (y=0;y<RECORD_BI.biHeight;y++)
				{
					srcc = (unsigned char*)&srcs[(cy&0xffff0000)>>6];
					cx = 0;
					for (x=0;x<RECORD_BI.biWidth;x++)
					{
						cl = *((unsigned long*)&srcc[(cx>>16)*3]);
						*(destc++) = (unsigned char)((cl&0xff0000)>>16);
						*(destc++) = (unsigned char)((cl&0xff00)>>8);
						*(destc++) = (unsigned char)(cl&0xff);
						cx += ax;
					}
					cy -= ay;
					if (cy<0) cy=0;
				}
			}
		}
		else
		{
			for (y=0;y<RECORD_BI.biHeight;y++)
			{
				src = &srcs[(cy&0xffff0000)>>6];
				cx = 0;
				for (x=0;x<RECORD_BI.biWidth;x++)
				{
					cs = src[cx>>16];
					*(destc++) = (unsigned char)((cs&0x7c00)>>7);
					*(destc++) = (unsigned char)((cs&0x03e0)>>2);
					*(destc++) = (unsigned char)((cs&0x001f)<<3);
					cx += ax;
				}
				cy -= ay;
				if (cy<0) cy=0;
			}
		}
	}
	else
		memset(RECORD_BUFFER,0,RECORD_BI.biSizeImage);

	return TRUE;
}
