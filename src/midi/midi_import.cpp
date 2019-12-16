#include "midi.h"
#include <fstream>
#include <math.h>

#include "../views/settings/configEditor.hpp"
#include "../libs/simpleini/SimpleIni.h"

extern ConfigEditor* config;
extern CSimpleIniA ini_gmlist;

/* Tracker channel status */

typedef struct trackerChannelProperties{
	int noteOn;
	int midiChannelMappings;
	int pedalCanRelease;
	int firstNotePos;
	int vol;
	int pan;
	bool isInitialVolSet;
	bool isInitialPanSet;
	int lastNoteVol;
	int midiTrackMappings;
	int channelPBend;
	int oldVol;
	int oldPan;
	int age;
	int stolenUsed;
};

static trackerChannelProperties trackerCh[FM_ch];


/* MIDI channel status */

typedef struct midiChannelProperties{
	int vol;
	int pan;
	int currentInstr;
	int expression;
	int localKeyboard;
	int channelPoly;
	int drumKit;
	int pedal;
	int firstNote;
	int currentTempo;
	int legato;
	int pitchBendRange;
};

static midiChannelProperties midiCh[16];



static int patternSize, currentTempo;
static int lastPos, isXG;
static double realRow;
static short midiFormat, tracks;
static int maxOrder, currentTrack, lastNotePos, loopStart;
static int order, row, tempoDivisor, totalLength;
typedef struct oldChannel{
	int channel, age, priority;
};
static oldChannel oldestChannels[FM_ch]; // forward


static int rpnSelect1, rpnSelect2;

static int sortChannels(void const *a, void const *b)
{
	oldChannel *pa = (oldChannel*)a;
	oldChannel *pb = (oldChannel*)b;
	return (pb->priority) - (pa->priority);
}

unsigned long ReadVarLen(ifstream* f)
{
	register unsigned long value;
	register char c;

	if ((value = f->get()) & 0x80)
	{
		value &= 0x7F;
		do
		{
			value = (value << 7) + ((c = f->get()) & 0x7F);
		} while (c & 0x80);
	}

	return(value);
}
//////////////////

typedef struct instrument{
	unsigned char id, type;
}instrument;


instrument* instrumentList;

int instrumentExists(unsigned char id, unsigned char type)
{

	for (unsigned i = 0; i < fm->instrumentCount; i++)
	{
		if (instrumentList[i].id == id && instrumentList[i].type == type)
			return i;
	}
	return -1;
}

int addInstrument(int id, unsigned char type)
{
	// out of range values (stupid midis!)
	if (type == 1 && (id<24 || id > 87))
	{
		id = 23;
	}
	else if (type == 0 && id > 127)
	{
		id = 0;
	}
	int i;
	if ((i = instrumentExists(id, type)) < 0)
	{
		string instrumentFile, instrumentName;
		if (type == 0)
		{
			instrumentFile = ini_gmlist.GetValue("melodic", int2str[id].c_str(), "0");
			instrumentName = midiProgramNames[id];
		}
		else
		{
			if (isXG && id < 35)
			{
				instrumentFile = ini_gmlist.GetValue("percussion", string(int2str[id] + "XG").c_str(), "0");
				instrumentName = midiXGPerc[id - 23];
			}
			else
			{
				instrumentFile = ini_gmlist.GetValue("percussion", int2str[id].c_str(), "0");
				instrumentName = midiPercussionNames[id - 23];
			}
		}
		if (fm_loadInstrument(fm, string(string(appdir + "/instruments/") + instrumentFile + string(".fmci")).c_str(), fm->instrumentCount) < 0)
		{
			fm_resizeInstrumentList(fm, fm->instrumentCount+1);
		}

		sprintf(&fm->instrument[fm->instrumentCount - 1].name[0], "%s", instrumentName.c_str());

		instrumentList = (instrument*)realloc(instrumentList, sizeof(instrument)*fm->instrumentCount);

		instrumentList[fm->instrumentCount - 1].id = id; // if unknown percussion, still store its original ID to avoid adding it multiple times
		instrumentList[fm->instrumentCount - 1].type = type;

		return fm->instrumentCount - 1;
	}
	else
	{
		return i;
	}

}


int isGlobalEffect(unsigned char fx)
{
	return (fx == 'T' || fx == 'B' || fx == 'C');
}


int midi_findoldestChannelBackward()
{

	for (unsigned i = 0; i < FM_ch; i++)
	{

		int pos = order*patternSize + row;
		while (pos>0 && fm->pattern[pos / patternSize][pos % patternSize][i].note == 255)
		{
			pos--;
		}

		// if we found a note instead of a note off, this channel is still playing !
		if (fm->pattern[pos / patternSize][pos % patternSize][i].note <= 127 && trackerCh[i].midiChannelMappings != 9)
		{ // perc channel doesn't always have note off
			oldestChannels[i].priority = 0;
		}
		else
		{
			oldestChannels[i].priority = order*patternSize + row - pos;
		}

		oldestChannels[i].channel = i;

	}

	qsort(oldestChannels, FM_ch, sizeof(oldChannel), sortChannels);

	return oldestChannels[0].priority > 0;
}

int midi_findoldestChannelForward()
{

	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].stolenUsed)
		{
			oldestChannels[i].priority = 0;
			oldestChannels[i].channel = i;
			continue;
		}
		/* Looking forward for the next note on/off command */

		int pos = order*patternSize + row;
		if (fm->pattern[pos / patternSize][pos % patternSize][i].note == 128)
			pos++;

		while (pos < fm->patternCount * patternSize - 1 && fm->pattern[pos / patternSize][pos % patternSize][i].note == 255)
		{
			pos++;
		}


		oldestChannels[i].age = pos - (order*patternSize + row);

		/* Discard channels finishing with a note off (means that a note was playing */

		if (fm->pattern[pos / patternSize][pos % patternSize][i].note == 128)
		{
			oldestChannels[i].age = 0;
		}

		oldestChannels[i].priority = oldestChannels[i].age;

		if (oldestChannels[i].priority > 0)
		{
			/* Start walking backwards */
			pos = order*patternSize + row;
			while (pos > 0 && fm->pattern[pos / patternSize][pos % patternSize][i].note == 255)
			{
				pos--;
			}

			/* Note off found or drum : the channel is free */
			if (fm->pattern[pos / patternSize][pos % patternSize][i].note == 128
				|| fm->pattern[pos / patternSize][pos % patternSize][i].note < 128 && instrumentList[fm->pattern[pos / patternSize][pos % patternSize][i].instr].type == 1 && pos - (order*patternSize + row) < -3)
			{
				oldestChannels[i].priority += 1000;
			}



		}

		oldestChannels[i].channel = i;

	}

	qsort(oldestChannels, FM_ch, sizeof(oldChannel), sortChannels);

	if (oldestChannels[0].priority > 0)
	{

		/* Store last channel vol/pan to be able to restore it afterwards */

		int pos = min(fm->patternCount * patternSize, order * patternSize + row + oldestChannels[0].age);
		int volFound = 0;
		int panFound = 0;
		while (pos > 0 && (!volFound || !panFound))
		{
			if (fm->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx == 'M')
			{
				trackerCh[oldestChannels[0].channel].oldVol = fm->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx;
				volFound = 1;
			}
			else if (fm->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx == 'X')
			{
				trackerCh[oldestChannels[0].channel].oldPan = fm->pattern[pos / patternSize][pos % patternSize][oldestChannels[0].channel].fx;
				panFound = 1;
			}
			pos--;
		}
		if (pos == 0)
		{
			if (!volFound)
			{
				trackerCh[oldestChannels[0].channel].oldVol = fm->ch[oldestChannels[0].channel].initial_vol;
			}
			if (!panFound)
			{
				trackerCh[oldestChannels[0].channel].oldPan = fm->ch[oldestChannels[0].channel].initial_pan;
			}
		}

		/* Cleanup the stolen channel */

		for (int i = order * patternSize + row; i < min(fm->patternCount * patternSize, order * patternSize + row + oldestChannels[0].age); i++)
		{

			fm->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].vol = 255;
			if (!isGlobalEffect(fm->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].fx))
			{
				fm->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].fx = 255;
				fm->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].vol = 255;
				fm->pattern[i / patternSize][i % patternSize][oldestChannels[0].channel].fxdata = 255;
			}
		}
		return 1;
	}
	return 0;
}

void midi_writefx(int realChannel, int fx, int fxdata, int rowOffset = 0)
{



	int pos = order*patternSize + row + rowOffset;

	if (fx == 'M')
	{
		/* Don't write the command if the channel volume is already the same */
		if (trackerCh[realChannel].vol == fxdata)
			return;

		trackerCh[realChannel].vol = fxdata;

		/* The first channel volume command must be stored as initial_vol, not effect */

		pos--;

		while (pos > 0 && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 'M')
			pos--;

		if (pos <= 0 && !trackerCh[realChannel].isInitialVolSet)
		{
			trackerCh[realChannel].isInitialVolSet = 1;
			fm->ch[realChannel].initial_vol = fxdata;
			return;
		}
	}
	else if (fx == 'X')
	{
		/* Don't write the command if the channel panning is already the same */
		if (trackerCh[realChannel].pan == fxdata)
			return;

		trackerCh[realChannel].pan = fxdata;

		/* The first channel panning command must be stored as initial_pan, not effect */

		pos--;

		while (pos > 0 && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 'X')
			pos--;

		if (pos <= 0 && !trackerCh[realChannel].isInitialPanSet)
		{
			trackerCh[realChannel].isInitialPanSet = 1;
			fm->ch[realChannel].initial_pan = fxdata;
			return;
		}
	}



	// move already existing global event to another channel if needed
	if (isGlobalEffect(fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx) && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
	{

		for (unsigned ch = 0; ch < FM_ch; ch++)
		{
			if (fm->pattern[pos / patternSize][pos%patternSize][ch].fx == 255)
			{
				fm->pattern[pos / patternSize][pos%patternSize][ch].fx = fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx;
				fm->pattern[pos / patternSize][pos%patternSize][ch].fxdata = fm->pattern[pos / patternSize][pos%patternSize][realChannel].fxdata;
				fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx = 255;
				break;
			}
			if (ch == FM_ch - 1)
			{ // no free channel found : keep the global event, don't write the new effect
				return;
			}
		}
	}

	pos = order*patternSize + row + rowOffset;


	if (fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
	{
		/* write global effects to other patterns if another effect is already there */
		if (isGlobalEffect(fx))
		{
			for (unsigned ch = 0; ch < FM_ch; ch++)
			{
				if (fm->pattern[pos / patternSize][pos%patternSize][ch].fx == 255)
				{
					realChannel = ch;
					break;
				}
			}
		}
		/* Other effects, try to put them before/after if effect already there, except Delays that are useless if on another row */
		else if (fx != 'D' && fx != 'I')
		{

			/* Try before */
			if (pos > 0 && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 255 && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
			{
				pos--;
				/* Try after */
				if (pos < fm->patternCount*patternSize - 2 && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 255 && fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != fx)
				{
					pos += 2;
					/* Reset at initial position */
					if (fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx != 255)
					{
						pos--;
						/* Keep important channel volume 'M'/'I' effects, discard others */
						if (fx != 'M' && fx != 'I')
							return;
					}
				}
			}
		}
		else
		{
			if (fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'M' && pos > 0)
			{
				fm->pattern[(pos - 1) / patternSize][(pos - 1) % patternSize][realChannel].fx = fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx;
				fm->pattern[(pos - 1) / patternSize][(pos - 1) % patternSize][realChannel].fxdata = fm->pattern[pos / patternSize][pos%patternSize][realChannel].fxdata;
			}
		}
	}
	if (fx == 'D')
	{
		if (fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'M' || fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'X'
			|| fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'I')
		{
			return;
		}
	}
	else if (fx == 'I')
	{
		if (fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'M' || fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx == 'X')
		{
			return;
		}
	}

	fm->pattern[pos / patternSize][pos%patternSize][realChannel].fx = fx;
	fm->pattern[pos / patternSize][pos%patternSize][realChannel].fxdata = fxdata;
}

void midi_effect(int midiChannel, unsigned char fx, unsigned char fxdata)
{

	if (fx == 'M') { midiCh[midiChannel].vol = fxdata; }

	if (fx == 'X') { midiCh[midiChannel].pan = fxdata; }

	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack)
		{
			midi_writefx(i, fx, fxdata);
		}
	}

}

void updateChannelParams(int realChannel, int midiChannel)
{

	midi_writefx(realChannel, 'X', midiCh[midiChannel].pan);
	midi_writefx(realChannel, 'M', midiCh[midiChannel].vol);

}



int reserveChannel(int note, int midiChannel)
{

	int channel = -1;

	for (unsigned i = 0; i < FM_ch; i++)
	{
		// same note already playing OR mono mode
		if (trackerCh[i].midiChannelMappings == midiChannel && (trackerCh[i].noteOn == note + 1 || midiCh[midiChannel].channelPoly == 0) && trackerCh[i].midiTrackMappings == currentTrack)
		{
			channel = i;
			break;
		}
	}

	if (channel == -1)
	{
		for (unsigned i = 0; i < FM_ch; i++)
		{
			// channel previously used by the same instrument
			if (trackerCh[i].midiChannelMappings == midiChannel && (trackerCh[i].noteOn == 0
				|| midiChannel == 9 && (note == 42 || note == 44 || note == 46) && (trackerCh[i].noteOn == 43 || trackerCh[i].noteOn == 45 || trackerCh[i].noteOn == 47)) // group high hat on the same channel
				&& trackerCh[i].midiTrackMappings == currentTrack)
			{
				channel = i;
				break;
			}
		}
	}

	if (channel == -1)
	{
		for (unsigned i = 0; i < FM_ch; i++)
		{
			// unused channel
			if (trackerCh[i].midiChannelMappings == -1)
			{
				channel = i;
				// copy midi channel vol/pan to the new allocated channel
				trackerCh[i].firstNotePos = order*patternSize + row;
				updateChannelParams(i, midiChannel);

				break;
			}
		}
	}

	if (channel == -1)
	{

		/* MIDI format 0 */
		if (tracks == 1)
		{
			if (midi_findoldestChannelBackward())
			{
				channel = oldestChannels[0].channel;
				updateChannelParams(channel, midiChannel);
			}
		}
		/* MIDI format 1 is far more complicated to handle */
		else
		{

			if (midi_findoldestChannelForward() /*(16*(8.0/fm->diviseur)*(currentTempo/120.0))*/)
			{ // only if long-time inactive channel

				channel = oldestChannels[0].channel;
				trackerCh[channel].stolenUsed = 1;
				trackerCh[channel].age = oldestChannels[0].age;
				updateChannelParams(channel, midiChannel);
			}
		}
	}


	if (channel >= 0)
	{
		trackerCh[channel].midiChannelMappings = midiChannel;
		trackerCh[channel].midiTrackMappings = currentTrack;
		trackerCh[channel].noteOn = note + 1;
	}
	return channel;
}

int freeChannel(int note, int midiChannel)
{

	for (unsigned i = 0; i < FM_ch; i++)
	{

		if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].noteOn - 1 == note && trackerCh[i].midiTrackMappings == currentTrack)
		{
			if (!midiCh[midiChannel].pedal)
			{
				trackerCh[i].noteOn = 0;
			}
			return i;
		}
	}
	return -1;
}

int midi_writeDelay(int channel)
{

	float delay = abs(realRow - (int)realRow);

	if (delay >= 0.125 - 0.5*0.125)
	{
		if ((int)round(8 * delay) == 8)
		{
			return 1;
		}
		else
		{
			midi_writefx(channel, 'D', (int)round(8 * delay));
		}
	}

	return 0;
}

void midi_noteOff(int note, int midiChannel)
{
	if (midiChannel == 9)
		return;
	int channel;
	if ((channel = freeChannel(note, midiChannel)) >= 0)
	{

		if (trackerCh[channel].stolenUsed)
			trackerCh[channel].stolenUsed = 0;

		if (midiCh[midiChannel].pedal)
		{
			trackerCh[channel].pedalCanRelease = note + 1;
			return;
		}
		trackerCh[channel].pedalCanRelease = 0;

		/* fast note on/off : tracker quantification would put them on the same row... */
		if (fm->pattern[order][row][channel].note == note)
		{
			int pos = order*patternSize + row + 1;
			if (pos / patternSize < fm->patternCount && fm->pattern[pos / patternSize][pos % patternSize][channel].note == 255)
			{
				fm->pattern[pos / patternSize][pos%patternSize][channel].note = 128;
			}
		}
		/* free slot, just write the note off */
		else if (fm->pattern[order][row][channel].note == 255)
		{
			fm->pattern[order][row][channel].note = 128;
		}
		/* note with no instr : fake pitch bend, stop */
		else if (fm->pattern[order][row][channel].instr == 255)
		{
			fm->pattern[order][row][channel].note = 128;
		}
		if (config->subquantize.checked && fm->pattern[order][row][channel].note == 128)
		{
			midi_writeDelay(channel);
		}
	}
}

void midi_noteOn(int note, int volume, int midiChannel, int instrument)
{

	if (midiCh[midiChannel].localKeyboard == 0)
		return;
	int channel;
	if (volume == 0)
	{
		midi_noteOff(note, midiChannel);
	}
	else
	{
		int addedPercussion = -1;
		if (midiChannel == 9)
		{
			// handle timpani from orchestra drum kit
			if (midiCh[midiChannel].drumKit == 48 && note > 40 && note < 54)
			{
				instrument = addInstrument(47, 0);
			}
			else
			{
				addedPercussion = addInstrument(note, 1);
			}
		}

		if ((channel = reserveChannel(note, midiChannel % 16)) >= 0)
		{
			// same row (happen in case of very fast note < quantization)
			if (fm->pattern[order][row][channel].note < 128)
			{
				trackerCh[channel].noteOn = fm->pattern[order][row][channel].note + 1;
				if ((channel = reserveChannel(note, midiChannel % 16)) < 0)
					return;
			}

			trackerCh[channel].pedalCanRelease = 0;
			int pos = order*patternSize + row + 1;
			if (pos / patternSize == fm->patternCount)
			{
				fm_insertPattern(fm, patternSize, fm->patternCount);
			}
			if (fm->pattern[pos / patternSize][pos%patternSize][channel].note == 128)
			{ // remove a note off that was added by a fast note on the same row (happen in case of very fast note < quantization)
				fm->pattern[pos / patternSize][pos%patternSize][channel].note = 255;
			}
			trackerCh[channel].lastNoteVol = volume;

			if (config->subquantize.checked && midi_writeDelay(channel))
			{

				fm->pattern[pos / patternSize][pos%patternSize][channel].note = addedPercussion >= 0 ? 60 : note;
				fm->pattern[pos / patternSize][pos%patternSize][channel].vol = (volume / 1.282828)*midiCh[midiChannel].expression / 99.0;

				if (!midiCh[midiChannel].legato)
					fm->pattern[pos / patternSize][pos%patternSize][channel].instr = addedPercussion >= 0 ? addedPercussion : instrument;

			}
			else
			{
				fm->pattern[order][row][channel].note = addedPercussion >= 0 ? 60 : note;
				fm->pattern[order][row][channel].vol = (volume / 1.282828)*midiCh[midiChannel].expression / 99.0;

				if (!midiCh[midiChannel].legato)
					fm->pattern[order][row][channel].instr = addedPercussion >= 0 ? addedPercussion : instrument;

			}

			trackerCh[channel].channelPBend = note;
		}
	}
}

// global effects (tempo, loops...)
void midi_globalEffect(unsigned char fx, unsigned char fxdata)
{

	int emptyChannel = 0;
	int pos = patternSize*order + row;

	if (fx == 'B' || fx == 'C')
		pos = max(0, pos - 1);

	while (fm->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx != 255 && emptyChannel < FM_ch - 1 && fm->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx != fx)
	{
		if (fx == fm->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx)
			return;
		emptyChannel++;
	}
	fm->pattern[pos / patternSize][pos%patternSize][emptyChannel].fx = fx;
	fm->pattern[pos / patternSize][pos%patternSize][emptyChannel].fxdata = fxdata;
}




void midi_expression(int midiChannel, int vol)
{
	if (midiCh[midiChannel].expression == (int)((midiCh[midiChannel].vol*0.0101010101010101)*vol / 1.282828))
		return;

	midiCh[midiChannel].expression = (midiCh[midiChannel].vol*0.0101010101010101)*vol / 1.282828; // 0-127 to 0-1 range
	for (unsigned i = 0; i < FM_ch; i++)
	{
		if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack)
		{
			fm->pattern[order][row][i].vol = midiCh[midiChannel].expression*trackerCh[i].lastNoteVol / 127.0;
		}
	}
}
ifstream midifile;
void midi_handleEvents(int type, int midiChannel, unsigned char data)
{

	/* Check if some stolen channels have expired */
	for (unsigned i = 0; i < FM_ch; i++)
	{


		if (trackerCh[i].age > 0)
		{

			trackerCh[i].age -= order*patternSize + row - lastPos;

			if (trackerCh[i].age <= 0)
			{

				trackerCh[i].stolenUsed = 0;

				if (trackerCh[i].oldPan != trackerCh[i].pan)
					midi_writefx(i, 'X', trackerCh[i].oldPan, trackerCh[i].age);
				if (trackerCh[i].oldVol != trackerCh[i].vol)
					midi_writefx(i, 'M', trackerCh[i].oldVol, trackerCh[i].age);

				/* This channel is now available again, 99 (or any other fake value) to ensure those channels stays in 'stealing' mode */
				trackerCh[i].midiChannelMappings = 99;
				trackerCh[i].midiTrackMappings = 99;
				trackerCh[i].noteOn = 0;
			}
		}
	}
	lastPos = order*patternSize + row;
	unsigned char data2 = 0;
	if (type != 0xC && type != 0xD)
		data2 = midifile.get();

	switch (type)
	{
		case 8: // note off
			midi_noteOff(data, midiChannel);
			break;
		case 9: // note on

			// wtf a midi without any program change ? hello Masami ?!
			if (midiCh[midiChannel].currentInstr == -1 && midiChannel != 9)
			{
				addInstrument(0, 0);
				midiCh[midiChannel].currentInstr = 0;
			}
			midi_noteOn(data, data2, midiChannel, midiCh[midiChannel].currentInstr);

			break;
		case 0xA: // Polyphonic Key Pressure
			break;
		case 0xB: // Controller Change

			switch (data)
			{
				case 0x01: // modulation (handled as vibrato)
					midi_effect(midiChannel, 'H', 96 + data2 / 16);
					break;
				case 0x06: // RPN param (1st part)
					if (rpnSelect1 == 0 && rpnSelect2 == 0)
					{ // pitch bend sensitivity
						midiCh[midiChannel].pitchBendRange = data2;
					}
					/*if (rpnSelect1 == 0 && rpnSelect2 == 1){ // master fine tuning

					}*/
					if (rpnSelect1 == 0 && rpnSelect2 == 2)
					{ // master coarse tuning
						fm->transpose = data2 - 64;
					}
					break;
				case 0x26: // (38) RPN param (2nd part)
					if (rpnSelect1 == 0 && rpnSelect2 == 0)
					{ // pitch bend sensitivity

					}
					break;
				case 0x07: // channel volume

					midi_effect(midiChannel, 'M', data2 / 1.282828);
					break;
				case 0x08: // channel balance
				case 0x0A: // (10) channel panning
					midi_effect(midiChannel, 'X', data2 * 2);
					break;
				case 0x0B: // (11) expression controller -- handled as volume tracker commands

					midi_expression(midiChannel, data2);

					break;
				case 0x40: // (64) sustain pedal
					midiCh[midiChannel].pedal = data2 > 63;

					/* Releasing the pedal should stop the notes playing on this channel */
					if (!midiCh[midiChannel].pedal)
					{
						for (unsigned i = 0; i < FM_ch; i++)
						{
							if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack && trackerCh[i].pedalCanRelease == trackerCh[i].noteOn)
							{

								/* Free cell */
								if (fm->pattern[order][row][i].note == 255)
								{
									fm->pattern[order][row][i].note = 128;

								}
								/* Occupied cell : write into next row */
								else
								{
									int pos = order*patternSize + row + 1;
									if (pos / patternSize < fm->patternCount && fm->pattern[pos / patternSize][pos % patternSize][i].note == 255)
									{
										fm->pattern[pos / patternSize][pos%patternSize][i].note = 128;
									}
								}
								trackerCh[i].noteOn = 0;
							}
						}
					}
					break;
				case 0x44: // (68) legato pedal
					midiCh[midiChannel].legato = data2 > 63;
					break;
				case 0x62: /* (98) NRPN select1 -- not handled atm */
					rpnSelect1 = 127;
					break;
				case 0x63: /* (99) NRPN select2 -- not handled atm */
					rpnSelect2 = 127;
					break;
				case 0x64: /* (100) RPN select1 */
					rpnSelect1 = data2;
					break;
				case 0x65: /* (101) RPN select2 */
					rpnSelect2 = data2;
					break;
				case 0x74: /* (116) loop start */
				case 0x76: /* 118 */
				case 111: /* rpg maker loop points */
					loopStart = order*patternSize + row;
					break;
				case 0x75: /* (117) loop end */
				case 0x77: /* 119 */
					midi_globalEffect('B', loopStart / patternSize);
					midi_globalEffect('C', loopStart%patternSize);

					loopStart = -1;
					break;
				case 0x78: // (120) all sound off */
				case 0x7B: // (123) all notes off */
					for (unsigned i = 0; i < FM_ch; i++)
					{
						if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack)
						{
							fm->pattern[order][row][i].note = 128;
							trackerCh[i].noteOn = 0;
						}
					};
					break;
				case 0x79: /* (121) controller reset */
					midiCh[midiChannel].channelPoly = 1;
					midiCh[midiChannel].pedal = 0;
					break;
				case 0x7A: /* (122) local keyboard */
					midiCh[midiChannel].localKeyboard = data2 / 64;
					break;
				case 0x7E: /* (126) mono channel */
					midiCh[midiChannel].channelPoly = 0;
					break;
				case 0x7F: /* (127) polyphonic channel */
					midiCh[midiChannel].channelPoly = 1;
					break;
			}
			break;
		case 0xC: /* Program Change */
			if (midiChannel % 16 == 9)
			{
				midiCh[midiChannel].drumKit = data;
			}
			else
			{
				midiCh[midiChannel].currentInstr = addInstrument(data, 0);
			}
			break;
		case 13: /* currentChannelNumber Key Pressure */
			break;
		case 14: /* Pitch Bend */
			if (midiCh[midiChannel].pitchBendRange <= 2)
				midi_effect(midiChannel, 'I', 2 * data2 + (data > 63)); /* use 1 bit from lsb for more precision (0-127 to 0-255 range) */
			else
			{
				for (unsigned i = 0; i < FM_ch; i++)
				{
					if (trackerCh[i].midiChannelMappings == midiChannel && trackerCh[i].midiTrackMappings == currentTrack && trackerCh[i].noteOn)
					{
						float ratio = (float)midiCh[midiChannel].pitchBendRange / 128;
						if (fm->pattern[order][row][i].instr == 255 && fm->pattern[order][row][i].note == 255 ||
							fm->pattern[order][row][i].instr == 255 && fm->pattern[order][row][i].note <128)
						{
							int pitchBendNote = trackerCh[i].noteOn - 1 + (2 * data2 + (data>63) - 128) * ratio + 0.5;
							pitchBendNote = clamp(pitchBendNote, 0, 127);
							if (trackerCh[i].channelPBend != pitchBendNote)
							{

								fm->pattern[order][row][i].note = pitchBendNote;
								trackerCh[i].channelPBend = pitchBendNote;
								if (config->subquantize.checked)
								{
									midi_writeDelay(i);
								}
							}
						}
					}
				}
			}
			break;
	}
}


int parseMidiRows(unsigned short delta_time_ticks)
{
	unsigned char eventType, eventData;
	realRow = 0;
	row = 0, order = -1;
	int lastStatus = 0, temp = 0;
	long long deltaAcc = 0;

	rpnSelect1 = rpnSelect2 = 127; /* rpn default is null */
	patternSize = config->patternSize.value;

	for (unsigned i = 0; i < 16; i++)
	{
		midiCh[i].currentInstr = -1;
		midiCh[i].vol = 99;
		midiCh[i].drumKit = 1;
		midiCh[i].pan = 127;
		midiCh[i].channelPoly = 1;
		midiCh[i].expression = 99;
		midiCh[i].pitchBendRange = 2; /* default is 2 semitones */
		midiCh[i].localKeyboard = 1;
		midiCh[i].pedal = midiCh[i].firstNote = midiCh[i].legato = 0;
	}

	double roundRow = config->subquantize.checked ? 0 : 0.5;

	while (order >= -1 && !midifile.eof())
	{ /* order set to -1 when end of track is found */

		deltaAcc += ReadVarLen(&midifile) / tempoDivisor;
		realRow = deltaAcc / (delta_time_ticks / (double)fm->diviseur) + roundRow;

		while (realRow >= patternSize*(order + 1))
		{
			order++;
			if (order > maxOrder)
			{
				if (order < 255)
				{
					fm_insertPattern(fm, patternSize, fm->patternCount);
					maxOrder = order;
				}
				else
				{
					return 0;
				}
			}
		}
		row = (int)(realRow) % patternSize;
		eventType = midifile.get();

		// Running status !
		if (eventType / 16 < 8)
		{

			midi_handleEvents(lastStatus / 16, lastStatus % 16, eventType);
		}
		else
		{ // New status
			if (eventType / 16 < 15)
			{
				eventData = midifile.get();
				midi_handleEvents(eventType / 16, eventType % 16, eventData);
			}
			else
			{ // Meta event
				if (eventType % 16 < 8)
				{ //sysex
					temp = ReadVarLen(&midifile);
					char *sysex = (char*)malloc(temp);
					midifile.read(sysex, temp);
					if (temp >= 7 && !memcmp(sysex, "\x43\x10\x4C\x00\x00\x7E\x00", 7))
					{
						isXG = 1;
					}
				}
				else
				{// meta 
					eventType = midifile.get();
					switch (eventType)
					{
						case 0x00: // seq number
							midifile.ignore(3);
							break;
						case 0x01: // text
						case 0x02: // copyright
						case 0x03: // seq name
						case 0x04: // instr name
						case 0x05: // lyrics
						case 0x06:// marker
						case 0x07: // cue point
						case 0x7F:{ // sequencer specific data
									  temp = ReadVarLen(&midifile);
									  char* d = (char*)malloc(temp);
									  midifile.read(d, temp);

									  if (eventType == 0x03) // sequence name
										  strncpy(fm->songName, d, min(63, temp));
									  else if (eventType == 0x02) // copyright
										  strncpy(fm->author, d, min(63, temp));
									  else if (eventType == 0x01)
									  { // text
										  strncpy(fm->comments, d, min(255, temp));
									  }
									  else if (eventType == 0x06)
									  {
										  if (strncmp(d, "loopStart", temp) == 0)
										  { // FF7's loop points
											  loopStart = order*patternSize + row;
										  }
										  else if (strncmp(d, "loopEnd", temp) == 0)
										  {
											  midi_globalEffect('B', loopStart / patternSize);
											  midi_globalEffect('C', loopStart%patternSize);
										  }
									  }
									  free(d);
						}break;

						case 0x20: // MIDI currentChannelNumber Prefix
						case 0x21: // prefix port
							midifile.ignore(2);
							break;
						case 0x2F: // end of track
							if (order*patternSize + row > totalLength)
							{
								totalLength = order*patternSize + row;
							}

							midifile.ignore(1);
							order = -2;
							break;
						case 0x51:{ // (81) tempo
									  midifile.ignore(1);
									  unsigned char tempoRaw[3];
									  midifile.read((char*)&tempoRaw, 3);

									  int tempo = 60000000 / ((tempoRaw[0] << 16) | (tempoRaw[1] << 8) | tempoRaw[2]);

									  // tempo > 255 : scale import speed to handle it
									  tempoDivisor = 1;
									  while (tempo > 255)
									  {
										  tempoDivisor *= 2;
										  tempo *= 0.5;
									  }

									  if (order == 0 && row == 0)
									  {
										  fm->initial_tempo = tempo;
									  }
									  else if (tempo != currentTempo)
									  {
										  midi_globalEffect('T', tempo);
									  }
									  currentTempo = tempo;
						}break;
						case 0x54: // smpte offset (dunno whats this shit)
							midifile.ignore(6);
							break;
						case 0x58: // time
							midifile.ignore(5);
							break;

						case 0x59: // key
							midifile.ignore(3);
							break;

					}
				}
			}
			lastStatus = eventType;
		}
	}

	return 1;
}


int midiImport(const char* filename)
{

	midifile.open(filename, ios::binary);
	if (!midifile.is_open())
		return FM_ERR_FILEIO;

	int currentVol = fm->_globalVolume;
	fm_clearSong(fm);
	fm_resizeInstrumentList(fm, 0);

	fm->diviseur = config->diviseur.value;
	fm_setVolume(fm, currentVol);
	fm->initial_tempo = 120;
	loopStart = -1;

	totalLength = 0;
	tempoDivisor = 1;
	for (unsigned i = 0; i < FM_ch; i++)
	{
		fm->ch[i].initial_reverb = 20;
		trackerCh[i].firstNotePos = -1;
		trackerCh[i].midiChannelMappings = -1;
		trackerCh[i].midiTrackMappings = -1;
		trackerCh[i].noteOn = 0;
		trackerCh[i].age = -1;
		trackerCh[i].pan = -1;
		trackerCh[i].vol = -1;
		trackerCh[i].channelPBend = -1;
		trackerCh[i].isInitialPanSet = 0;
		trackerCh[i].isInitialVolSet = 0;
		trackerCh[i].stolenUsed = 0;
	}

	isXG = 0;
	unsigned short delta_time_ticks;

	char temp[4];
	midifile.read((char*)&temp, 4);
	if (strncmp(temp, "RIFF", 4) == 0)
	{
		midifile.ignore(20);
	}

	midifile.ignore(4); // MThd + chuckSize
	midifile.read((char*)&midiFormat, 2);
	midiFormat = (midiFormat >> 8) | (midiFormat << 8);
	midifile.read((char*)&tracks, 2); // nb of tracks
	tracks = (tracks >> 8) | (tracks << 8);
	midifile.read((char*)&delta_time_ticks, 2);
	delta_time_ticks = (delta_time_ticks >> 8) | (delta_time_ticks << 8);

	if (instrumentList)
	{
		free(instrumentList);
		instrumentList = 0;
	}

	maxOrder = -1;

	for (currentTrack = 0; currentTrack < tracks; currentTrack++)
	{
		midifile.ignore(8); // expecting MTrk + chunk size, we dont need them

		if (!parseMidiRows(delta_time_ticks))
			break;
	}
	/* rpg maker loop point , */
	if (loopStart >= 0)
	{
		row = totalLength%patternSize;
		order = totalLength / patternSize;
		midi_globalEffect('B', loopStart / patternSize);
		midi_globalEffect('C', loopStart%patternSize);
		loopStart = -1;
	}


	/*if (fm->pattern[order][row].note[i]==255 && trackerCh[i].noteOn>0)
		fm->pattern[order][row].note[i]=128;*/



	// no instrument (unlikely?) : avoid crash
	if (fm->instrumentCount == 0)
	{
		if (fm_loadInstrument(fm, "instruments/keyboards/piano.fmci", 0) < 0)
		{
			fm_resizeInstrumentList(fm, 1);
		}
	}
	instrList->select(0);
	midifile.close();

	for (int i = 0; i < fm->instrumentCount; i++)
	{
		if (!fm_isInstrumentUsed(fm, i))
		{
			fm_removeInstrument(fm, i, 1);
			/* Because instruments after the removed one are re-numbered, we need to check again the same i (it's the next instrument) */
			i--;
		}
	}


	fm_buildStateTable(fm, 0, fm->patternCount, 0, FM_ch);

	return 0;
}
