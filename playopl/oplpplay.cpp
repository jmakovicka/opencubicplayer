/* OpenCP Module Player
 * copyright (c) 2005-'22 Stian Skjelstad <stian.skjelstad@gmail.com>
 *
 * OPLPlay interface routines
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "types.h"
extern "C"
{
#include "boot/plinkman.h"
#include "boot/psetting.h"
#include "cpiface/cpiface.h"
#include "dev/player.h"
#include "filesel/dirdb.h"
#include "filesel/filesystem.h"
#include "filesel/pfilesel.h"
#include "filesel/mdb.h"
#include "stuff/compat.h"
#include "stuff/err.h"
#include "stuff/poutput.h"
#include "stuff/sets.h"
#include "stuff/timer.h"
}
#include "oplplay.h"

static time_t starttime, pausetime;
static uint32_t pausefadestart;
static uint8_t pausefaderelspeed;
static int8_t pausefadedirect;
static char utf8_8_dot_3  [12*4+1];  /* UTF-8 ready */
static char utf8_16_dot_3 [20*4+1]; /* UTF-8 ready */
static struct moduleinfostruct mdbdata;

static oplTuneInfo globinfo;
static oplChanInfo ci;

static void startpausefade(void)
{
	if (plPause)
		starttime=starttime+dos_clock()-pausetime;

	if (pausefadedirect)
	{
		if (pausefadedirect<0)
			plPause=1;
		pausefadestart=2*dos_clock()-DOS_CLK_TCK-pausefadestart;
	} else
		pausefadestart=dos_clock();

	if (plPause)
	{
		plChanChanged=1;
		oplPause(plPause=0);
		pausefadedirect=1;
	} else
		pausefadedirect=-1;
}

static void dopausefade(void)
{
	int16_t i;
	if (pausefadedirect>0)
	{
		i=((int32_t)dos_clock()-pausefadestart)*64/DOS_CLK_TCK;
		if (i<0)
			i=0;
		if (i>=64)
		{
			i=64;
			pausefadedirect=0;
		}
	} else {
		i=64-((int32_t)dos_clock()-pausefadestart)*64/DOS_CLK_TCK;
		if (i>=64)
			i=64;
		if (i<=0)
		{
			i=0;
			pausefadedirect=0;
			pausetime=dos_clock();
			oplPause(plPause=1);
			plChanChanged=1;
			mcpSetFadePars(64);
			return;
		}
	}
	pausefaderelspeed=i;
	mcpSetFadePars(i);
}

static char convnote(long freq)
{
	if (!freq) return 0xff;

	float frfac=(float)freq/220.0;

	float nte=12*(log(frfac)/log(2))+48;

	if (nte<0 || nte>127) nte=0xff;
	return (char)nte;
}

static void logvolbar(int &l, int &r)
{
	if (l>32)
		l=32+((l-32)>>1);
	if (l>48)
		l=48+((l-48)>>1);
	if (l>56)
		l=56+((l-56)>>1);
	if (l>64)
		l=64;
	if (r>32)
		r=32+((r-32)>>1);
	if (r>48)
		r=48+((r-48)>>1);
	if (r>56)
		r=56+((r-56)>>1);
	if (r>64)
		r=64;
}

static void drawvolbar(uint16_t *buf, int, unsigned char st)
{
	int l,r;
	l=ci.vol;
	r=ci.vol;
	logvolbar(l, r);

	l=(l+4)>>3;
	r=(r+4)>>3;
	if (plPause)
		l=r=0;
	if (st)
	{
		writestring(buf, 8-l, 0x08, "\376\376\376\376\376\376\376\376", l);
		writestring(buf, 9, 0x08, "\376\376\376\376\376\376\376\376", r);
	} else {
		uint16_t left[] =  {0x0ffe, 0x0bfe, 0x0bfe, 0x09fe, 0x09fe, 0x01fe, 0x01fe, 0x01fe};
		uint16_t right[] = {0x01fe, 0x01fe, 0x01fe, 0x09fe, 0x09fe, 0x0bfe, 0x0bfe, 0x0ffe};
		writestringattr(buf, 8-l, left+8-l, l);
		writestringattr(buf, 9, right, r);
	}
}

static void drawlongvolbar(uint16_t *buf, int, unsigned char st)
{
	int l,r;
	l=ci.vol;
	r=ci.vol;
	logvolbar(l, r);
	l=(l+2)>>2;
	r=(r+2)>>2;
	if (plPause)
		l=r=0;
	if (st)
	{
		writestring(buf, 16-l, 0x08, "\376\376\376\376\376\376\376\376\376\376\376\376\376\376\376\376", l);
		writestring(buf, 17,   0x08, "\376\376\376\376\376\376\376\376\376\376\376\376\376\376\376\376", r);
	} else {
		uint16_t left[] =  {0x0ffe, 0x0ffe, 0x0bfe, 0x0bfe, 0x0bfe, 0x0bfe, 0x09fe, 0x09fe, 0x09fe, 0x09fe, 0x01fe, 0x01fe, 0x01fe, 0x01fe, 0x01fe, 0x01fe};
		uint16_t right[] = {0x01fe, 0x01fe, 0x01fe, 0x01fe, 0x01fe, 0x01fe, 0x09fe, 0x09fe, 0x09fe, 0x09fe, 0x0bfe, 0x0bfe, 0x0bfe, 0x0bfe, 0x0ffe, 0x0ffe};
		writestringattr(buf, 16-l, left+16-l, l);
		writestringattr(buf, 17,   right, r);
	}
}


static void oplDrawGStrings ()
{
	mcpDrawGStrings ();

	oplpGetGlobInfo(globinfo);

	mcpDrawGStringsSongXofY
	(
		utf8_8_dot_3,
		utf8_16_dot_3,
		globinfo.currentSong,
		globinfo.songs,
		plPause,
		plPause?((pausetime-starttime)/DOS_CLK_TCK):((dos_clock()-starttime)/DOS_CLK_TCK),
		&mdbdata
	);
}

static void drawchannel(uint16_t *buf, int len, int i) /* TODO */
{
        unsigned char st=plMuteCh[i];

        unsigned char tcol=st?0x08:0x0F;
        unsigned char tcold=st?0x08:0x07;

	const char *waves4 []= {"sine", "half", "2x  ", "saw "};
	const char *waves16[]= {"sine curves     ", "half sine curves", "positiv sines   ", "sawtooth        "};

	switch (len)
	{
		case 36:
			writestring(buf, 0, tcold, " ---- --- -- - -- \372\372\372\372\372\372\372\372 \372\372\372\372\372\372\372\372 ", 36);
			break;
		case 62:
			writestring(buf, 0, tcold, " ---------------- ---- --- --- --- -------  \372\372\372\372\372\372\372\372 \372\372\372\372\372\372\372\372 ", 62);
			break;
		case 128:
			writestring(buf, 0, tcold, "                   \263        \263       \263       \263                \263               \263   \372\372\372\372\372\372\372\372\372\372\372\372\372\372\372\372 \372\372\372\372\372\372\372\372\372\372\372\372\372\372\372\372", 128);
			break;
		case 76:
			writestring(buf, 0, tcold, "                  \263      \263     \263     \263     \263             \263 \372\372\372\372\372\372\372\372 \372\372\372\372\372\372\372\372", 76);
			break;
		case 44:
			writestring(buf, 0, tcold, " ---- ---- --- -- --- --  \372\372\372\372\372\372\372\372 \372\372\372\372\372\372\372\372 ", 44);

		break;
	}

	oplpGetChanInfo(i,ci);

	if (!ci.vol)
		return;
	uint8_t nte=convnote(ci.freq);
	char nchar[4];

	if (nte<0xFF)
	{
		nchar[0]="CCDDEFFGGAAB"[nte%12];
		nchar[1]="-#-#--#-#-#-"[nte%12];
		nchar[2]="0123456789ABCDEFGHIJKLMN"[nte/12];
		nchar[3]=0;
	} else
		strcpy(nchar,"   ");

	switch(len)
	{
		case 36:
			writestring(buf+1, 0, tcol, waves4[ci.wave], 4);
			writestring(buf+6, 0, tcol, nchar, 3);
/*
			writenum(buf+10, 0, tcol, ci.pulse>>4, 16, 2, 0);
			if (ci.filtenabled && ftype)
				writenum(buf+13, 0, tcol, ftype, 16, 1, 0);
			if (efx)
				writestring(buf+15, 0, tcol, fx2[efx], 2);*/
			drawvolbar(buf+18, i, st);
			break;
		case 44:
			writestring(buf+1, 0, tcol, waves4[ci.wave], 4);
/*
			writenum(buf+6, 0, tcol, ci.ad, 16, 2, 0);
			writenum(buf+8, 0, tcol, ci.sr, 16, 2, 0);*/
			writestring(buf+11, 0, tcol, nchar, 3);
/*
			writenum(buf+15, 0, tcol, ci.pulse>>4, 16, 2, 0);
			if (ci.filtenabled && ftype)
				writestring(buf+18, 0, tcol, filters3[ftype], 3);
			if (efx)
				writestring(buf+22, 0, tcol, fx2[efx], 2);*/
			drawvolbar(buf+26, i, st);
			break;
		case 62:
			writestring(buf+1, 0, tcol, waves16[ci.wave], 16);
/*
			writenum(buf+18, 0, tcol, ci.ad, 16, 2, 0);
			writenum(buf+20, 0, tcol, ci.sr, 16, 2, 0);*/
			writestring(buf+23, 0, tcol, nchar, 3);
/*
			writenum(buf+27, 0, tcol, ci.pulse, 16, 3, 0);
			if (ci.filtenabled && ftype)
				writestring(buf+31, 0, tcol, filters3[ftype], 3);
			if (efx)
				writestring(buf+35, 0, tcol, fx7[efx], 7);*/
			drawvolbar(buf+44, i, st);
			break;
		case 76:
			writestring(buf+1, 0, tcol, waves16[ci.wave], 16);
/*
			writenum(buf+20, 0, tcol, ci.ad, 16, 2, 0);
			writenum(buf+22, 0, tcol, ci.sr, 16, 2, 0);*/
			writestring(buf+27, 0, tcol, nchar, 3);
/*
			writenum(buf+33, 0, tcol, ci.pulse, 16, 3, 0);
			if (ci.filtenabled && ftype)
				writestring(buf+39, 0, tcol, filters3[ftype], 3);
			writestring(buf+45, 0, tcol, fx11[efx], 11);*/
			drawvolbar(buf+59, i, st);
			break;
		case 128:
			writestring(buf+1, 0, tcol, waves16[ci.wave], 16);
/*
			writenum(buf+22, 0, tcol, ci.ad, 16, 2, 0);
			writenum(buf+24, 0, tcol, ci.sr, 16, 2, 0);*/
			writestring(buf+31, 0, tcol, nchar, 3);
/*
			writenum(buf+39, 0, tcol, ci.pulse, 16, 3, 0);
			if (ci.filtenabled && ftype)
				writestring(buf+47, 0, tcol, filters12[ftype], 12);
			writestring(buf+64, 0, tcol, fx11[efx], 11);*/
			drawlongvolbar(buf+81, i, st);
			break;
	}
}



static int oplProcessKey(uint16_t key)
{
	struct oplTuneInfo ti;

	switch (key)
	{
		case KEY_ALT_K:
			cpiKeyHelp('p', "Start/stop pause with fade");
			cpiKeyHelp('P', "Start/stop pause with fade");

			cpiKeyHelp(KEY_CTRL_HOME, "Restart Song");
			cpiKeyHelp('<', "Previous Song");
			cpiKeyHelp('>', "Next song");

			cpiKeyHelp(KEY_CTRL_P, "Start/stop pause");
			mcpSetProcessKey (key);
			return 0;
		case 'p': case 'P':
			startpausefade();
			break;
		case KEY_CTRL_P:
			pausefadedirect=0;
			if (plPause)
				starttime=starttime+dos_clock()-pausetime;
			else
				pausetime=dos_clock();
			plPause=!plPause;
			oplPause(plPause);
			break;
		case KEY_CTRL_HOME:
			oplpGetGlobInfo (ti);
			oplSetSong (ti.currentSong);
			break;
		case '<':
			oplpGetGlobInfo (ti);
			oplSetSong (ti.currentSong - 1);
			break;
		case '>':
			oplpGetGlobInfo (ti);
			oplSetSong (ti.currentSong + 1);
			break;
#if 0
		case KEY_CTRL_UP:
		/* case 0x8D00: //ctrl-up */
			oplSetPos(oplGetPos()-oplrate);
			break;
		case KEY_CTRL_DOWN:
		/* case 0x9100: //ctrl-down */
			oplSetPos(oplGetPos()+oplrate);
			break;*/
		case '<':
		case KEY_CTRL_LEFT:
		/* case 0x7300: //ctrl-left */
			{
				int skip=opllen>>5;
				if (skip<128*1024)
					skip=128*1024;
				oplSetPos(oplGetPos()-skip);
			}
			break;
		case '>':
		case KEY_CTRL_RIGHT:
		/* case 0x7400: //ctrl-right */
			{
				int skip=opllen>>5;
				if (skip<128*1024)
					skip=128*1024;
				oplSetPos(oplGetPos()+skip);
			}
			break;
		case 0x7700: //ctrl-home TODO keys
			oplSetPos(0);
			break;
	#endif
		default:
			return mcpSetProcessKey (key);
	}
	return 1;
}


static int oplLooped(void)
{
	if (pausefadedirect)
		dopausefade();
	oplSetLoop(fsLoopMods);
	oplIdle();
	if (plrIdle)
		plrIdle();
	return !fsLoopMods&&oplIsLooped();
}

static void oplCloseFile(void)
{
	oplClosePlayer();
}

static int oplOpenFile(struct moduleinfostruct *info, struct ocpfilehandle_t *file, const char *ldlink, const char *loader) /* no loader needed/used by this plugin */
{
	const char *filename;
	size_t buffersize = 16*1024;
	uint8_t *buffer = (uint8_t *)malloc (buffersize);
	size_t bufferfill = 0;

	mdbdata = *info;
	dirdbGetName_internalstr (file->dirdb_ref, &filename);
	{
		int res;
		while (!file->eof(file))
		{
			if (buffersize == bufferfill)
			{
				if (buffersize >= 16*1024*1024)
				{
					fprintf (stderr, "oplOpenFile: %s is bigger than 16 Mb - further loading blocked\n", filename);
					free (buffer);
					return -1;
				}
				buffersize += 16*1024;
				buffer = (uint8_t *)realloc (buffer, buffersize);
			}
			res = file->read (file, buffer + bufferfill, buffersize - bufferfill);
			if (res<=0)
				break;
			bufferfill += res;
		}
	}
	fprintf(stderr, "OPL/AdPlug: loading %s\n", filename);
	utf8_XdotY_name ( 8, 3, utf8_8_dot_3 , filename);
	utf8_XdotY_name (16, 3, utf8_16_dot_3, filename);

	plIsEnd=oplLooped;
	plProcessKey=oplProcessKey;
	plDrawGStrings=oplDrawGStrings;
	plGetMasterSample=plrGetMasterSample;
	plGetRealMasterVolume=plrGetRealMasterVolume;

	if (!oplOpenPlayer(filename, buffer, bufferfill, file))
	{
		free (buffer);
		return -1;
	}
	free (buffer);

	starttime=dos_clock();
	plPause=0;
	pausefadedirect=0;

	plNLChan=plNPChan=18;
	plUseChannels(drawchannel);
	plSetMute=oplMute;

	oplpGetGlobInfo(globinfo);

	return errOk;
}

extern "C"
{
	cpifaceplayerstruct oplPlayer = {"[AdPlug OPL plugin]", oplOpenFile, oplCloseFile};
	struct linkinfostruct dllextinfo =
	{
		"playopl" /* name */,
		"OpenCP AdPlug (OPL) Player (c) 2005-'22 Stian Skjelstad" /* desc */,
		DLLVERSION /* ver */
	};
}
