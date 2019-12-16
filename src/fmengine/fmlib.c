#include "fmlib.h"
#include "lz4/lz4hc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
#include <float.h>
#include <math.h>


/* Current version of instrument/song formats */
#define FMCI_version 1
#define FMCS_version 1


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define _99TO1 1/99
#define _24TO1 1/24
#define _2400TO1 1/2400
#define SEMITONE_RATIO 0.059463 * 0.01 /* 0.059463 = ratio between two semitones https://en.wikipedia.org/wiki/Twelfth_root_of_two */

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define clamp(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

/* Sine wave lookup table size */

#define LUTsize 2048
#define LUTratio (LUTsize / 1024)

/* Reverb delays, in number of samples at 48000Hz. Automatically scaled for other samples rates. */

#define REVERB_DELAY_L1 1.6*4096 // 85ms
#define REVERB_DELAY_L2 1.5*2485 // 72 
#define REVERB_DELAY_R1 1.6*3801 // 79
#define REVERB_DELAY_R2 1.5*2333 // 69
#define REVERB_ALLPASS2 1.5*1170 // 5.5
#define REVERB_ALLPASS1 1.5*2508 // 7.7ms


static float wavetable[8][LUTsize];


/* Exponential tables for envelopes and volumes scales */

static float expEnv[100], expVol[100], expVolOp[100];

// waveforms
static const unsigned int lfoMasks[28] = {
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xffc00 * LUTratio, // sine
	0xf0000 * LUTratio,  // less reso
	0xefc00 * LUTratio,  // less reso
	0xdfc00 * LUTratio,  // less
	0xbfc00 * LUTratio,  // squarelike
	0x88000 * LUTratio, // high freq
	0x40000 * LUTratio, // square
	0x60000 * LUTratio, // pulse/near square
	0x7fc00 * LUTratio, // abs sine
	0x78000 * LUTratio, // abs sine less reso
	0x70000 * LUTratio, // less reso
	0x3fc00 * LUTratio, // saw
	0xa0000 * LUTratio, // ? 10
	0xfffc00 * LUTratio, // sine
	0x2ffc00 * LUTratio, // sine
};

static const unsigned int lfoWaveforms[28] = {
	0,
	1,
	2,
	3,
	4,
	5,
	6,
	7,
	0,  // less reso
	0,  // less reso
	0,  // less
	0,  // squarelike
	0, // high freq
	0, // square
	0, // pulse/near square
	0, // abs sine
	0, // abs sine less reso
	0, // less reso
	0, // saw
	0, // ? 10
	0, // sine
	0, // sine
};


/* Band-limited triangle, square and sawtooth generators */

float trg(float x, float theta) { return 1 - 2 * acos((1 - theta) * sin(2 * M_PI*x)) / M_PI; }

float sqr(float x, float theta) { return 2 * atan(sin(2 * M_PI* x) / theta) / M_PI; }

float swt(float x, float theta) { return (1 + trg((2 * x - 1) / 4, theta) * sqr(x / 2, theta)) / 2; }

void fm_setDefaults(fmsynth* f)
{

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		f->ch[ch].note = 255;
		f->ch[ch].instrNumber = 255;
		f->ch[ch].vol = expVol[99];
		f->ch[ch].initial_vol = 99;
		f->ch[ch].reverbSend = 0;
		f->ch[ch].destPan = f->ch[ch].pan = f->ch[ch].initial_pan = 127;
		f->ch[ch].noteVol = 99;
	}

	fm_setVolume(f, 60);
	f->initial_tempo = 120;
	f->diviseur = 4;
	f->initialReverbLength = f->reverbLength = 0.875;
	f->initialReverbRoomSize = 0.55;
	f->looping = -1;
	f->channelStatesDone = 0;
	f->playbackVolume = 1;
}



static unsigned int g_seed = 0;
int fast_rand(void)
{
	g_seed = (214013 * g_seed + 2531011);
	return (g_seed >> 16) & 0x7FFF;
}

void fm_destroy(fmsynth* f)
{
	free(f->revBuf);
	free(f->instrument);
	for (unsigned i = 0; i < f->patternCount; i++)
	{
		free(f->pattern[i]);
		free(f->channelStates[i]);
	}
	free(f->patternSize);
	free(f->pattern);
	free(f->channelStates);
	free(f);
}

fmsynth* fm_create(int _sampleRate)
{
	fmsynth *f = calloc(1, sizeof(fmsynth));

	if (f)
	{

		/* avoid slow float denormals + round floats down (needed if program compiled with QIfirst option (FISTP) fast int/float conversion) */
		#ifdef _WIN32
		_control87(_RC_DOWN, _MCW_RC);
		#endif
		_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);

		/* Build waveform tables */

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[0][i] = sin(i * 2 * M_PI / LUTsize);					// 0 sine

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[1][i] = (swt((float)(i + LUTsize / 2) / LUTsize, 0.2) - 0.5)*2.5*(1 / 0.464670);	// 3 soft saw

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[2][i] = (swt((float)(i + LUTsize / 2) / LUTsize, 0.05) - 0.5) * 2 * (1 / 0.649969);	// 4 saw

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[3][i] = trg((float)i / LUTsize, 0.01)*(1 / 0.909893);			// 1 triangle

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[4][i] = sqr((float)i / LUTsize, 0.1)*0.7*(1 / 0.655584);			// 2 square

		for (unsigned i = 0; i < LUTsize / 2; i++)
			wavetable[5][i] = sin(i * 2 * M_PI / (LUTsize / 2));				// 5 double sine

		for (unsigned i = 0; i < LUTsize / 2; i++)
			wavetable[6][i] = sin(i * 2 * M_PI / LUTsize);					// 6 half period sine

		for (unsigned i = 0; i < LUTsize; i++)
			wavetable[7][i] = fast_rand() / 16383.5 - 0.5;

		/*for (int i = 0; i < 7; i++){
			float max=0;
			for (int j = 0; j< LUTsize; j++){
			if (wavetable[i][j]>max){
			max = wavetable[i][j];

			}
			}
			printf("max %d is %f\n", i, max);
			}*/



		/* Build exponential tables for volume/envelopes */

		float ini = 0.00001;
		for (unsigned i = 1; i < 99; i++)
		{
			expVol[i] = pow(10, (log(100.0 / (i + 1)) * (-10)) / 20.0);
			expEnv[i] = ini;
			ini *= 1.1;
			expVolOp[i] = expVol[i] * (i*0.01);
		}

		expEnv[96] = 0.1;
		expEnv[97] = 0.2;
		expEnv[98] = 0.5;
		expEnv[99] = expVol[99] = expVolOp[99] = 1;

		const char tuningRatios[4][12]= {
			{0,0,0,0,0,0,0,0,0,0,0,0},
			{12,2,4,6,2,10,0,8,4,0,8,4},
			{10,-14,3,20,-3,14,-10,7,-17,0,17,-7}

		};
		for (int i = 0; i < 3; i++)
		{
			for (int j = 0; j < 12; j++)
			{
				f->tuningRatios[i][j]=tuningRatios[i][j];
			}
		}


		 

		fm_setDefaults(f);

		if (!fm_setSampleRate(f, _sampleRate))
		{
			free(f);
			return 0;
		}
	}

	return f;
}

int fm_initReverb(fmsynth *f, float roomSize)
{
	/* Initialize reverb parameters and buffers */

	f->reverbPhaseL = f->reverbPhaseL2 = f->reverbPhaseR = f->reverbPhaseR2 = f->allpassPhaseL = f->allpassPhaseR = f->allpassPhaseL2 = f->allpassPhaseR2 = 0;

	unsigned mod1 = roomSize*REVERB_DELAY_L1 / f->sampleRateRatio; // 85ms
	unsigned mod2 = roomSize* REVERB_DELAY_L2 / f->sampleRateRatio; // 72 
	unsigned mod3 = roomSize*REVERB_DELAY_R1 / f->sampleRateRatio; // 79
	unsigned mod4 = roomSize*REVERB_DELAY_R2 / f->sampleRateRatio; // 69
	unsigned mod5 = (roomSize*REVERB_ALLPASS1) / f->sampleRateRatio; // 5.5
	unsigned mod6 = (roomSize*REVERB_ALLPASS2) / f->sampleRateRatio; // 7.7ms

	unsigned revBufSize = mod1 + mod2 + mod3 + mod4 + 2 * (mod5 + mod6);

	float* newR = realloc(f->revBuf, sizeof(float)*revBufSize);

	if (!newR)
	{
		return 0;
	}

	f->reverbRoomSize = roomSize;
	f->revBufSize = revBufSize;
	f->revBuf = newR;

	memset(f->revBuf, 0, sizeof(float)*revBufSize);



	f->reverbMod1 = mod1;
	f->reverbMod2 = mod2;
	f->reverbMod3 = mod3;
	f->reverbMod4 = mod4;
	f->allpassMod = mod5;
	f->allpassMod2 = mod6;

	f->revOffset2 = f->reverbMod1 + f->reverbMod2;
	f->revOffset3 = f->revOffset2 + f->reverbMod3;
	f->revOffset4 = f->revOffset3 + f->reverbMod4;
	f->revOffset5 = f->revOffset4 + f->allpassMod;
	f->revOffset6 = f->revOffset5 + f->allpassMod;
	f->revOffset7 = f->revOffset6 + f->allpassMod2;
	return 1;
}

int fm_setSampleRate(fmsynth* f, int sampleRate)
{

	f->sampleRate = sampleRate;
	f->sampleRateRatio = 48000.0 / sampleRate;
	f->transitionSpeed = 20 * (1 / f->sampleRateRatio);

	/* initialize the MIDI note frequencies table (converted into phase accumulator increments) */

	for (unsigned x = 0; x < 128; ++x)
		f->noteIncr[x] = pow(2, (x - 9.0) / 12.0) / sampleRate * 32840 * 440 * LUTratio;

	/* Reset instruments so their values can be regenerated */
	for (unsigned ch = 0; ch < FM_ch; ++ch)
		f->ch[ch].cInstr = 0;

	return fm_initReverb(f, f->initialReverbRoomSize);
}

/* Calculates the volume of each operator */
void fm_calcOpVol(fm_operator *o, int note, int volume)
{

	float noteScaling = 1 + (note - o->kbdCenterNote)*o->volScaling;
	float opVol = (expVol[volume] * o->velSensitivity + (1 - o->velSensitivity))*expVolOp[o->baseVol];
	o->vol = clamp(opVol * noteScaling, 0, 1) * 5000 * LUTratio;

}

/* Calculates the pitch of each operator */
void fm_calcPitch(fmsynth* f, int ch, int note)
{
	int noteOrig = note;
	note = clamp(note + f->ch[ch].transpose + f->transpose * ((f->ch[ch].instr->flags & FM_INSTR_TRANSPOSABLE) >> 2), 0, 127);

	f->ch[ch].note = note;

	float frequency = f->noteIncr[note] + f->noteIncr[note] * SEMITONE_RATIO* f->ch[ch].instr->temperament[noteOrig % 12];

	for (unsigned op = 0; op < FM_op; ++op)
	{
		fm_operator* o = &f->ch[ch].op[op];

		if (o->fixedFreq == 0)
		{
			o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1);
		}
		/* Fixed frequency */
		else
			o->incr = (o->mult * o->mult + (float)o->mult *(float)o->finetune*_24TO1) * LUTratio*f->sampleRateRatio;

		o->incr += o->incr*f->ch[ch].tuning;
	}
}


void fm_initChannels(fmsynth* f)
{
	if (f->order >= f->patternCount || f->row >= f->patternSize[f->order])
		return;

	f->tempo = f->channelStates[f->order][f->row].tempo;
	f->globalVolume = expVol[f->_globalVolume] * 4096 / LUTsize;
	f->reverbLength = f->initialReverbLength;
	if (f->initialReverbRoomSize != f->reverbRoomSize)
	{
		fm_initReverb(f, f->initialReverbRoomSize);
	}

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		f->ch[ch].cInstr = 0;
		f->ch[ch].pan = f->ch[ch].destPan = f->channelStates[f->order][f->row].pan[ch];
		f->ch[ch].vol = expVol[f->channelStates[f->order][f->row].vol[ch]];
		f->ch[ch].reverbSend = expVol[f->ch[ch].initial_reverb];
		f->ch[ch].pitchBend = 1;
		f->ch[ch].fadeFrom=0;
		f->ch[ch].fadeFrom2=0;
		f->ch[ch].currentEnvLevel=0;
	}
}




void _fm_render(fmsynth* f, float* buffer, unsigned length)
{

	unsigned b = 0;
	while (b < length)
	{
		// player
		if (f->playing)
		{
			/* Song frame tick */
			if (f->frameTimer == 0)
			{

				for (unsigned ch = 0; ch < FM_ch; ++ch)
				{
					Cell* row = &f->pattern[f->order][f->row][ch];
					f->ch[ch].fxActive = 0;

					/* Note stop ? */
					if (row->note == 128 && row->fx != 'D')
					{
						fm_stopNote(f, ch);
					}
					/* Note play ? */
					else if (row->note != 255)
					{
						// portamento or note delay : don't play the note now !
						if (row->fx != 'D' && (row->fx != 'G' || row->fx == 'G' && f->ch[ch].note == 255))
						{
							fm_playNote(f, row->instr, row->note, ch, row->vol);
							f->ch[ch].baseArpeggioNote = row->note;
						}
					}
					// only volume change
					else if (row->vol != 255 && f->ch[ch].instr)
					{
						f->ch[ch].noteVol = row->vol;
						for (unsigned op = 0; op < FM_op; ++op)
						{
							fm_calcOpVol(&f->ch[ch].op[op], f->ch[ch].note, row->vol);
						}
					}

					/* Handle effects (after note actions) */

					f->ch[ch].fxData = row->fxdata;
					switch (row->fx)
					{

						case 'B': // jump pattern
							f->tempOrder = f->ch[ch].fxData;
							break;
						case 'C': // jump row
							f->tempRow = f->ch[ch].fxData;
							break;
						case 'L': // temperament
							for (int i = 0; i < f->instrumentCount; i++)
							{
								for (int j = 0; j < 12; j++)
								{
									f->instrument[i].temperament[j] = f->tuningRatios[f->ch[ch].fxData/16][(j+24-f->ch[ch].fxData%16-3)%12];
								}
							}
							break;
						case 'G': // portamento
							// update portamento dest frequency if note set
							if (row->note != 255)
							{
								for (unsigned op = 0; op < FM_op; ++op)
								{
									float pitchScaling = 1 + ((int)row->note - f->ch[ch].instr->op[op].kbdCenterNote)*f->ch[ch].instr->op[op].kbdPitchScaling*0.001;

									if (f->ch[ch].instr->op[op].fixedFreq == 0)
									{
										f->ch[ch].op[op].portaDestIncr = f->noteIncr[clamp(row->note+f->transpose,0,127)] * pitchScaling*(f->ch[ch].op[op].mult + (double)f->ch[ch].op[op].finetune*0.041666666667 + (double)f->ch[ch].op[op].detune*0.00041666666667);

									}
									else // fixed frequency
										f->ch[ch].op[op].portaDestIncr = (f->ch[ch].op[op].mult * (f->ch[ch].op[op].mult) + (double)f->ch[ch].op[op].mult*(double)f->ch[ch].op[op].finetune*0.041666666667) * LUTratio;
								}
							}
							// repeated effects
						case 'A': // arpeggio
						case 'D': // delay
						case 'E': // portamento up
						case 'F': // portamento down
						case 'W': // global volume slide
						case 'P': // panning slide

							f->ch[ch].arpTimer = 0;
							f->ch[ch].arpIter = 0;
							f->ch[ch].fxActive = row->fx;
							break;
						case 'Q': // retrigger note
							if (f->ch[ch].fxData > 0)
							{
								f->ch[ch].arpTimer = 24 / f->ch[ch].fxData;
								f->ch[ch].arpIter = 0;
								f->ch[ch].fxActive = row->fx;
							}
							break;
						case 'H': // vibrato
							f->ch[ch].lfoEnv = 1;
							f->ch[ch].lfoIncr = (f->ch[ch].fxData / 16 * 128)*LUTratio;
							for (unsigned op = 0; op < FM_op; ++op)
							{
								f->ch[ch].op[op].lfoFM = (f->ch[ch].fxData % 16)*0.003;
							}
							break;
						case 'I': // pitch bend
							f->ch[ch].fxActive = 'I';
							f->ch[ch].pitchBend = 1 - (float)(128 - f->ch[ch].fxData) * 0.00092852373168154813872606848242328;
							break;
						case 'J': // tremolo
							f->ch[ch].lfoEnv = 1;
							f->ch[ch].lfoIncr = (f->ch[ch].fxData / 16 * 128)*LUTratio;
							for (unsigned op = 0; op < FM_op; ++op)
							{
								f->ch[ch].op[op].lfoAM = (f->ch[ch].fxData % 16)*(1.0 / 16);
							}
							break;
						case 'K':
							if (!f->ch[ch].instr)
								break;
							/* Global instrument edit*/
							if (f->ch[ch].instr->kfx / 32 == 0)
							{
								switch (f->ch[ch].instr->kfx)
								{
									case 0:
										f->ch[ch].instrVol = expVol[min(99, f->ch[ch].fxData)];
										break;
									case 1:
										f->ch[ch].transpose = clamp(f->ch[ch].fxData, -12, 12);
										fm_calcPitch(f, ch, f->ch[ch].untransposedNote);
										break;
									case 2:
										f->ch[ch].tuning = 0.0006*clamp(f->ch[ch].fxData, -100, 100);
										fm_calcPitch(f, ch, f->ch[ch].untransposedNote);
										break;
									case 3:
										f->ch[ch].lfoIncr = 1 + expVol[clamp(f->ch[ch].fxData, 0, 99)] * expVol[clamp(f->ch[ch].fxData, 0, 99)] * 5000 * f->sampleRateRatio*LUTratio;
										break;
									case 4:
										f->ch[ch].lfoDelayCptMax = expVol[clamp(f->ch[ch].fxData, 0, 99)] * expVol[clamp(f->ch[ch].fxData, 0, 99)] * 200000 * f->sampleRateRatio;
										break;
									case 5:
										f->ch[ch].lfoA = expEnv[clamp(f->ch[ch].fxData, 0, 99)] * f->sampleRateRatio;
										break;
									case 6:
										f->ch[ch].lfoMask = lfoMasks[clamp(f->ch[ch].fxData, 0, 19)];
										f->ch[ch].lfoWaveform = lfoWaveforms[clamp(f->ch[ch].fxData, 0, 19)];
										break;
									case 7:
										f->ch[ch].lfoOffset = clamp(f->ch[ch].fxData, 0, 31) * LUTsize / 32;
										break;

								}
							}
							/* Operator edit */
							else
							{
								fm_operator *o = &f->ch[ch].op[f->ch[ch].instr->kfx / 32 - 1];

								switch (f->ch[ch].instr->kfx % 32)
								{
									case 0:
										o->baseVol = min(99, f->ch[ch].fxData);
										fm_calcOpVol(o, f->ch[ch].note, f->ch[ch].noteVol);
										break;
									case 1:
										o->baseVol = o->vol * !f->ch[ch].instr->op[f->ch[ch].instr->kfx / 32 - 1].muted;
										fm_calcOpVol(o, f->ch[ch].note, f->ch[ch].noteVol);
										break;
									case 2:
										o->waveform = wavetable[clamp(f->ch[ch].fxData, 0, 7)];
										break;
									case 3:{
											   o->mult = clamp(f->ch[ch].fxData, 0, 40);
											   float frequency = f->noteIncr[f->ch[ch].note] + f->noteIncr[f->ch[ch].note] * SEMITONE_RATIO * f->ch[ch].instr->temperament[f->ch[ch].note % 12];
											   o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1) * (1 + f->ch[ch].tuning);
											   break;
									}
									case 4:
										o->mult = clamp(f->ch[ch].fxData, 0, 255);
										o->incr = (o->mult * o->mult + (float)o->mult *(float)o->finetune*_2400TO1) * LUTratio*f->sampleRateRatio * (1 + f->ch[ch].tuning);
										break;
									case 5:{
											   o->finetune = clamp(f->ch[ch].fxData, 0, 24);
											   float frequency = f->noteIncr[f->ch[ch].note] + f->noteIncr[f->ch[ch].note] * SEMITONE_RATIO * f->ch[ch].instr->temperament[f->ch[ch].note % 12];
											   o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1) * (1 + f->ch[ch].tuning);
											   break;
									}
									case 6:{
											   o->detune = clamp(f->ch[ch].fxData, -100, 100);
											   float frequency = f->noteIncr[f->ch[ch].note] + f->noteIncr[f->ch[ch].note] * SEMITONE_RATIO * f->ch[ch].instr->temperament[f->ch[ch].note % 12];
											   o->incr = frequency *(o->mult + (float)o->finetune*_24TO1 + (float)o->detune*_2400TO1) * (1 + f->ch[ch].tuning);
											   break;
									}
									case 7:
										o->delay = expEnv[f->ch[ch].fxData] * 3000000 / f->sampleRateRatio;
										break;
									case 8:
										o->i = expVol[f->ch[ch].fxData];
										break;
									case 9:
										o->baseA = clamp(f->ch[ch].fxData, 0, 99);
										break;
									case 10:
										o->h = expEnv[clamp(f->ch[ch].fxData, 0, 80)] * 700000 / f->sampleRateRatio;
										break;
									case 11:
										o->baseD = clamp(f->ch[ch].fxData, 0, 99);
										break;
									case 12:
										o->s = expVol[clamp(f->ch[ch].fxData, 0, 99)];
										break;
									case 13:{
												char value = clamp(f->ch[ch].fxData, -99, 99);
												o->r = (value >= 0) ? exp(-(expEnv[value])*f->sampleRateRatio) : 2 - exp(-(expEnv[abs(value)])*f->sampleRateRatio);
												break;
									}
									case 14:
										o->envLoop = clamp(f->ch[ch].fxData, 0, 1);
										break;
									case 15:
										o->lfoFM = expVol[clamp(f->ch[ch].fxData, 0, 99)] * expVol[clamp(f->ch[ch].fxData, 0, 99)];
										break;

									case 16:
										o->lfoAM = expVol[clamp(f->ch[ch].fxData, 0, 99)];
										break;




								}
							}


							break;
						case 'M': // channel volume
							f->ch[ch].vol = expVol[f->ch[ch].fxData];
							break;
						case 'R': // reverb send
							f->ch[ch].reverbSend = expVol[f->ch[ch].fxData];
							break;
						case 'S': // global reverb params
							if (f->ch[ch].fxData <= 40)
							{
								f->reverbLength = 0.5 + f->ch[ch].fxData*0.0125;
							}
							else
							{
								fm_initReverb(f, clamp(f->ch[ch].fxData - 40, 1, 40)*0.025);
							}
							break;
						case 'T': // tempo
							f->tempo = max(1, f->ch[ch].fxData);
							break;


						case 'X': // panning
							f->ch[ch].destPan = f->ch[ch].fxData;
							break;
					}

				}
			}
			f->frameTimer += 8;
			if (f->frameTimer >= (60.0 / f->diviseur) * f->sampleRate / f->tempo)
			{

				f->frameTimer = 0;

				if (++f->row >= f->patternSize[f->order])
				{ // jump to next pattern
					f->row = 0;
					f->order++;
				}

				if (f->tempOrder != -1 || f->tempRow != -1)
				{
					f->loopCount++;

					if (f->tempOrder != -1)
						f->order = min(f->tempOrder, f->patternCount - 1);

					if (f->tempRow != -1)
						f->row = min(f->tempRow, f->patternSize[f->order] - 1);

					f->tempOrder = f->tempRow = -1;
				}

				if (f->order >= f->patternCount)
				{
					f->loopCount++;
					f->order = 0;
				}

				if (f->looping != -1 && f->loopCount > f->looping)
				{
					f->playing = 0;
				}

			}

			if (f->frameTimerFx >= 0.005*(60.0 / f->diviseur) * f->sampleRate / f->tempo)
			{

				for (unsigned ch = 0; ch < FM_ch; ++ch)
				{
					switch (f->ch[ch].fxActive)
					{
						case 'A': // arpeggio
						{
									  f->ch[ch].arpTimer++;
									  if (f->ch[ch].arpTimer >= 8)
									  {
										  f->ch[ch].arpTimer -= 8;
										  f->ch[ch].arpIter = (f->ch[ch].arpIter + 1) % 3;
										  fm_playNote(f, 255, f->ch[ch].arpIter == 0 ? f->ch[ch].baseArpeggioNote : f->ch[ch].arpIter == 1 ? (f->ch[ch].baseArpeggioNote + f->ch[ch].fxData % 16) : (f->ch[ch].baseArpeggioNote + f->ch[ch].fxData / 16), ch, 255);
									  }

						}
							break;
						case 'Q': // retrigger note
						{
									  f->ch[ch].arpTimer++;
									  if (f->ch[ch].arpTimer >= 24 / f->ch[ch].fxData && f->ch[ch].arpIter < f->ch[ch].fxData)
									  {
										  f->ch[ch].arpTimer -= 24 / f->ch[ch].fxData;
										  fm_playNote(f, f->ch[ch].instrNumber, f->ch[ch].untransposedNote, ch, 255);
										  f->ch[ch].arpIter++;
									  }
						}
							break;
						case 'D': // delay
						{
									  int delay = f->frameTimer / ((60.0 / f->diviseur) * f->sampleRate / f->tempo / 8);
									  if (delay >= f->ch[ch].fxData)
									  {

										  if (f->pattern[f->order][f->row][ch].note < 127)
											  fm_playNote(f, f->pattern[f->order][f->row][ch].instr, f->pattern[f->order][f->row][ch].note, ch, f->pattern[f->order][f->row][ch].vol);
										  else if (f->pattern[f->order][f->row][ch].note == 128)
											  fm_stopNote(f, ch);

										  f->ch[ch].fxActive = 0;
									  }
						}
							break;

						case 'E': // portamento up
							for (unsigned op = 0; op < FM_op; ++op)
							{
								f->ch[ch].op[op].incr += f->ch[ch].fxData*f->ch[ch].op[op].incr*0.0001;
							}
							break;
						case 'F': // portamento down
							for (unsigned op = 0; op < FM_op; ++op)
							{
								f->ch[ch].op[op].incr += -f->ch[ch].fxData*f->ch[ch].op[op].incr*0.0001;
							}
							break;
						case 'G': // portamento
							for (unsigned op = 0; op < FM_op; ++op)
							{
								f->ch[ch].op[op].incr += (f->ch[ch].op[op].portaDestIncr - f->ch[ch].op[op].incr)*f->ch[ch].fxData*0.001;
							}
							break;
						case 'I':{ // pitch bend

									 //int pos = f->order*f->patternSize[f->order]+f->row+1;

									 /*if (f->pattern[pos / f->patternSize[f->order]][pos%f->patternSize[f->order]].fx == 'I'){
										 float nextPB = 1-(float)(64-f->pattern[pos / f->patternSize[f->order]][pos%f->patternSize[f->order]].fxdata[ch]) / 538.1489198433845617116833784366;
										 f->pitchBend[ch]=(f->pitchBend[ch]*10+nextPB)/11;
										 }*/
									 break;
						}
						case 'N': // channel volume slide
							f->ch[ch].vol = clamp(f->ch[ch].vol + ((int)f->ch[ch].fxData - 127)*0.0001, 0, 1);
							break;
						case 'P': // panning slide
							f->ch[ch].pan = clamp(f->ch[ch].pan + (127 - (int)f->ch[ch].fxData)*-0.05, 0, 255);
							break;
						case 'W': // global volume slide
							f->globalVolume = clamp(f->globalVolume + ((int)f->ch[ch].fxData - 127)*0.0001, 0, 1);
							break;

					}
				}
				f->frameTimerFx -= 0.005*(60.0 / f->diviseur) * f->sampleRate / f->tempo;
			}
			f->frameTimerFx++;
		}


		for (unsigned ch = 0; ch < FM_ch; ++ch)
		{
			if (!f->ch[ch].active)
				continue;


			f->ch[ch].pan = (f->ch[ch].pan*(f->transitionSpeed - 1) + f->ch[ch].destPan) / f->transitionSpeed;
			//f->ch[ch].vol = (f->ch[ch].vol*(speed-1)+f->ch[ch].destVol)/speed;


			// Update lfo
			if (f->ch[ch].lfoDelayCpt++ >= f->ch[ch].lfoDelayCptMax)
			{
				f->ch[ch].lfoPhase += f->ch[ch].lfoIncr;
				f->ch[ch].lfoEnv += (1.f - f->ch[ch].lfoEnv)*f->ch[ch].lfoA;
				f->ch[ch].lfo = wavetable[f->ch[ch].lfoWaveform][((f->ch[ch].lfoPhase & f->ch[ch].lfoMask) >> 10) % LUTsize] * f->ch[ch].lfoEnv;
			}
			int opOutUsed = 0;
			f->ch[ch].currentEnvLevel = 0;
			for (unsigned op = 0; op < FM_op; ++op)
			{
				fm_operator* o = &f->ch[ch].op[op];
				if (o->connectOut != &f->noConnect)
				{
					opOutUsed += f->ch[ch].op[o->id].state;
					f->ch[ch].currentEnvLevel += f->ch[ch].op[o->id].env;
				}

				/* Handle envelope */

				switch (o->state)
				{
					/* Delay */
					case 1:
						if (o->envCount++ >= o->delay)
						{

							if (f->ch[ch].instr->op[op].pitchInitialRatio > 0)
								o->pitchMod = 1 + expVol[f->ch[ch].instr->op[op].pitchInitialRatio] * expVol[f->ch[ch].instr->op[op].pitchInitialRatio] * 12;
							else if (f->ch[ch].instr->op[op].pitchInitialRatio < 0)
								o->pitchMod = 1 + (float)f->ch[ch].instr->op[op].pitchInitialRatio*_99TO1;
							else
								o->pitchMod = 1;

							o->pitchTime = expEnv[f->ch[ch].instr->op[op].pitchDecay] * f->sampleRateRatio;
							o->pitchDestRatio = 1;

							if (f->ch[ch].instr->phaseReset || o->env < 0.1)
							{
								o->phase = o->offset;
							}

							if (o->envCount >= 99999999)
							{
								o->env = o->s;
							}
							else if (f->ch[ch].instr->envReset)
								o->env = o->i;



							o->env += (1.4f - o->env) * o->a;
							if (o->env >= 1.f)
							{
								o->env = 1.f;
								o->state = o->h > 0 ? 3 : 4;
							}
							else
								o->state = 2;
						}
						break;
						/* Attack */
					case 2:
						o->env += (1.4f - o->env) * o->a;
						if (o->env >= 1.f)
						{
							o->env = 1.f;
							o->state = o->h > 0 ? 3 : 4;
						}
						break;
						/* Hold */
					case 3:
						if (o->envCount++ >= o->h)
							o->state++;
						break;
						/* Decay - Sustain */
					case 4:
						o->env -= (o->env - o->s) * o->d;
						if (o->env - o->s < 0.001f)
						{
							o->env = o->s;


							if (o->s < 0.001f)
							{
								if (o->envLoop)
								{
									o->envCount = 99999999;
									o->state = 1;
								}
								else
								{
									o->state = o->env = o->amp = 0;
								}
							}
							else
							{
								o->envCount = 99999999;
								if (o->envLoop)
								{

									o->state = 1;
								}
								else
								{
									o->state = 5;
								}
							}
						}

						break;
						/* Release */
					case 6:
						o->env *= o->r;

						if (o->r <= 1)
						{
							if (o->env < 0.001f)
								o->state = o->env = o->amp = 0;
						}
						else
						{
							if (o->env >= 1.f)
							{
								o->env = 1.f;
								o->state = 5;
							}
						}
						break;
				}

				o->pitchMod -= (o->pitchMod - o->pitchDestRatio)*o->pitchTime;
				o->ampDelta = (o->env * o->vol *(1.f - f->ch[ch].lfo * o->lfoAM) - o->amp) / 8;
				//o->amp = o->env * o->vol *(1.f - f->ch[ch].lfo * o->lfoAM );
				o->pitch = o->incr * o->pitchMod * f->ch[ch].pitchBend *(1 + f->ch[ch].lfo * o->lfoFM);

			}
			f->ch[ch].active = opOutUsed;
		}

		/* Previous stuff didnt need to be updated for every sample, we do 8 rendering steps for 1 update step to save CPU */

		for (unsigned iter = 0; iter < 8; iter++)
		{
			float rendu = 0, renduL = 0, renduR = 0, fxL = 0, fxR = 0;

			for (unsigned ch = 0; ch < FM_ch; ++ch)
			{
				if (!f->ch[ch].active || f->ch[ch].muted)
					continue;

				/* FM calculations, unrolled to be sure the compiler doesn't generate a loop */

				f->ch[ch].op[0].phase += f->ch[ch].op[0].pitch;
				f->ch[ch].op[0].amp += f->ch[ch].op[0].ampDelta;
				f->ch[ch].op[0].out = f->ch[ch].op[0].waveform[((f->ch[ch].op[0].phase >> 10) + (unsigned)*f->ch[ch].op[0].connect + (unsigned)*f->ch[ch].op[0].connect2 + (unsigned)(*f->ch[ch].feedbackSource*f->ch[ch].feedbackLevel)) % LUTsize] * f->ch[ch].op[0].amp;


				f->ch[ch].op[1].phase += f->ch[ch].op[1].pitch;
				f->ch[ch].op[1].amp += f->ch[ch].op[1].ampDelta;
				f->ch[ch].op[1].out = f->ch[ch].op[1].waveform[((f->ch[ch].op[1].phase >> 10) + (unsigned)*f->ch[ch].op[1].connect + (unsigned)*f->ch[ch].op[1].connect2) % LUTsize] * f->ch[ch].op[1].amp;


				f->ch[ch].op[2].phase += f->ch[ch].op[2].pitch;
				f->ch[ch].op[2].amp += f->ch[ch].op[2].ampDelta;
				f->ch[ch].op[2].out = f->ch[ch].op[2].waveform[((f->ch[ch].op[2].phase >> 10) + (unsigned)*f->ch[ch].op[2].connect + (unsigned)*f->ch[ch].op[2].connect2) % LUTsize] * f->ch[ch].op[2].amp;


				f->ch[ch].op[3].phase += f->ch[ch].op[3].pitch;
				f->ch[ch].op[3].amp += f->ch[ch].op[3].ampDelta;
				f->ch[ch].op[3].out = f->ch[ch].op[3].waveform[((f->ch[ch].op[3].phase >> 10) + (unsigned)*f->ch[ch].op[3].connect + (unsigned)*f->ch[ch].op[3].connect2) % LUTsize] * f->ch[ch].op[3].amp;


				f->ch[ch].op[4].phase += f->ch[ch].op[4].pitch;
				f->ch[ch].op[4].amp += f->ch[ch].op[4].ampDelta;
				f->ch[ch].op[4].out = f->ch[ch].op[4].waveform[((f->ch[ch].op[4].phase >> 10) + (unsigned)*f->ch[ch].op[4].connect + (unsigned)*f->ch[ch].op[4].connect2) % LUTsize] * f->ch[ch].op[4].amp;


				f->ch[ch].op[5].phase += f->ch[ch].op[5].pitch;
				f->ch[ch].op[5].amp += f->ch[ch].op[5].ampDelta;
				f->ch[ch].op[5].out = f->ch[ch].op[5].waveform[((f->ch[ch].op[5].phase >> 10) + (unsigned)*f->ch[ch].op[5].connect + (unsigned)*f->ch[ch].op[5].connect2) % LUTsize] * f->ch[ch].op[5].amp;


				f->ch[ch].mixer = *f->ch[ch].op[0].toMix + *f->ch[ch].op[1].toMix + *f->ch[ch].op[2].toMix + *f->ch[ch].op[3].toMix;

				rendu = (*f->ch[ch].op[0].connectOut + *f->ch[ch].op[1].connectOut + *f->ch[ch].op[2].connectOut + *f->ch[ch].op[3].connectOut + *f->ch[ch].op[4].connectOut + *f->ch[ch].op[5].connectOut)*f->ch[ch].vol*f->ch[ch].instrVol;

				f->ch[ch].lastRender2 = f->ch[ch].lastRender;
				f->ch[ch].lastRender = rendu;

				/* Is a smooth transition needed between two notes ? */

				if (f->ch[ch].fade > 0.00001)
				{
					rendu = rendu*(1 - f->ch[ch].fade) + f->ch[ch].fadeFrom*f->ch[ch].fade;
					f->ch[ch].fadeFrom += f->ch[ch].delta*f->ch[ch].fade;
					f->ch[ch].fade *= f->ch[ch].fadeIncr;
				}

				float trenduL = rendu*wavetable[0][LUTsize / 4 + (unsigned)f->ch[ch].pan*LUTratio];
				float trenduR = rendu*wavetable[0][(unsigned)f->ch[ch].pan*LUTratio];


				renduL += trenduL;
				renduR += trenduR;
				fxL += trenduL*f->ch[ch].reverbSend;
				fxR += trenduR*f->ch[ch].reverbSend;
			}

			/* Reverb phases */

			unsigned prevPhaseL = f->reverbPhaseL;
			f->reverbPhaseL = (f->reverbPhaseL + 1) % f->reverbMod1;
			unsigned prevPhaseL2 = f->reverbPhaseL2;
			f->reverbPhaseL2 = (f->reverbPhaseL2 + 1) % f->reverbMod2;
			unsigned prevPhaseR = f->reverbPhaseR;
			f->reverbPhaseR = (f->reverbPhaseR + 1) % f->reverbMod3;
			unsigned prevPhaseR2 = f->reverbPhaseR2;
			f->reverbPhaseR2 = (f->reverbPhaseR2 + 1) % f->reverbMod4;

			/* Two comb filters, left */

			f->outL = ((f->revBuf[f->reverbPhaseL] + f->revBuf[f->reverbMod1 + f->reverbPhaseL2]))*0.5;
			f->revBuf[f->reverbPhaseL] =  fxR + (f->revBuf[f->reverbPhaseL] + f->revBuf[prevPhaseL])*0.5*f->reverbLength;
			f->revBuf[f->reverbMod1 + f->reverbPhaseL2] =fxL + (f->revBuf[f->reverbMod1 + f->reverbPhaseL2] + f->revBuf[f->reverbMod1 + prevPhaseL2])*0.5*f->reverbLength;

			/* Two comb filters, right */

			f->outR = ((f->revBuf[f->revOffset2 + f->reverbPhaseR] + f->revBuf[f->revOffset3 + f->reverbPhaseR2]))*0.5;
			f->revBuf[f->revOffset2 + f->reverbPhaseR] = fxL + (f->revBuf[f->revOffset2 + f->reverbPhaseR] + f->revBuf[f->revOffset2 + prevPhaseR])*0.5*f->reverbLength;
			f->revBuf[f->revOffset3 + f->reverbPhaseR2] =  fxR + (f->revBuf[f->revOffset3 + f->reverbPhaseR2] + f->revBuf[f->revOffset3 + prevPhaseR2])*0.5*f->reverbLength;

			/* First allpass */

			float outL2 = 0.5*f->outL + f->revBuf[f->revOffset4 + f->allpassPhaseL];
			f->revBuf[f->revOffset4 + f->allpassPhaseL] = f->outL - 0.5 * outL2;
			f->allpassPhaseL = (f->allpassPhaseL + 1) % f->allpassMod;

			float outR2 = 0.5*f->outR + f->revBuf[f->revOffset5 + f->allpassPhaseR];
			f->revBuf[f->revOffset5 + f->allpassPhaseR] = f->outR - 0.5 * outR2;
			f->allpassPhaseR = (f->allpassPhaseR + 1) % f->allpassMod;

			/* Second allpass */

			float outL22 = 0.5*outL2 + f->revBuf[f->revOffset6 + f->allpassPhaseL2];
			f->revBuf[f->revOffset6 + f->allpassPhaseL2] = outL2 - 0.5 * outL22;
			f->allpassPhaseL2 = (f->allpassPhaseL2 + 1) % f->allpassMod2;

			float outR22 = 0.5*outR2 + f->revBuf[f->revOffset7 + f->allpassPhaseR2];
			f->revBuf[f->revOffset7 + f->allpassPhaseR2] = outR2 - 0.5 * outR22;
			f->allpassPhaseR2 = (f->allpassPhaseR2 + 1) % f->allpassMod2;


			/* Final mix */
			buffer[b] = (renduL + outL22) * f->globalVolume * f->playbackVolume;
			buffer[b + 1] =(renduR + outR22) * f->globalVolume * f->playbackVolume;
			b += 2;
			if (b>=length)
				return;
		}
	}
}

void fm_playNote(fmsynth* f, unsigned _instrument, unsigned note, unsigned ch, unsigned volume)
{
	if (ch >= FM_ch || _instrument == 255 && !f->ch[ch].instr || _instrument != 255 && _instrument >= f->instrumentCount)
		return;

	/* Instrument changed, update parameters */
	if (_instrument != 255 && _instrument < f->instrumentCount && f->ch[ch].cInstr != &f->instrument[_instrument])
	{

		f->ch[ch].cInstr = &f->instrument[_instrument];
		f->ch[ch].instrNumber = _instrument;

		f->ch[ch].instr = &f->instrument[_instrument];
		f->ch[ch].instrVol = expVol[f->ch[ch].instr->volume];
		f->ch[ch].lfoMask = lfoMasks[f->ch[ch].instr->lfoWaveform];
		f->ch[ch].lfoWaveform = lfoWaveforms[f->ch[ch].instr->lfoWaveform];
		f->ch[ch].feedbackLevel = expVol[f->ch[ch].instr->feedback];
		f->ch[ch].lfoA = expEnv[f->ch[ch].instr->lfoA] * f->sampleRateRatio;
		f->ch[ch].lfoIncr = 1 + expVol[f->ch[ch].instr->lfoSpeed] * expVol[f->ch[ch].instr->lfoSpeed] * 5000 * f->sampleRateRatio*LUTratio;
		f->ch[ch].lfoDelayCptMax = expVol[f->ch[ch].instr->lfoDelay] * expVol[f->ch[ch].instr->lfoDelay] * 200000 * f->sampleRateRatio;
		f->ch[ch].lfoEnv = f->ch[ch].lfoDelayCpt = f->ch[ch].lfo = f->ch[ch].lfoPhase = 0;
		f->ch[ch].pitchBend = 1;
		f->ch[ch].transpose = f->ch[ch].instr->transpose;
		f->ch[ch].tuning = 0.0006 * f->ch[ch].instr->tuning;
		f->ch[ch].lfoOffset = f->ch[ch].instr->lfoOffset * LUTsize / 32;
		for (unsigned op = 0; op < FM_op; ++op)
		{
			fm_operator* o = &f->ch[ch].op[op];
			o->env = 0;
			o->connectOut = (f->ch[ch].instr->op[op].connectOut >= 0) ? &f->ch[ch].op[f->ch[ch].instr->op[op].connectOut].out : &f->noConnect;
			o->id = f->ch[ch].instr->op[op].connectOut;
			o->connect = (f->ch[ch].instr->op[op].connect >= 0) ? &f->ch[ch].op[f->ch[ch].instr->op[op].connect].out : &f->noConnect;
			o->connect2 = (f->ch[ch].instr->op[op].connect2>5) ? &f->ch[ch].mixer :
				(f->ch[ch].instr->op[op].connect2 >= 0 ? &f->ch[ch].op[f->ch[ch].instr->op[op].connect2].out : &f->noConnect);

			o->waveform = wavetable[f->ch[ch].instr->op[op].waveform];
			o->lfoFM = expVol[f->ch[ch].instr->op[op].lfoFM] * expVol[f->ch[ch].instr->op[op].lfoFM];
			o->lfoAM = expVol[f->ch[ch].instr->op[op].lfoAM];

			o->delay = expEnv[f->ch[ch].instr->op[op].delay] * 3000000 / f->sampleRateRatio;

			o->i = expVol[f->ch[ch].instr->op[op].i];
			o->h = expEnv[f->ch[ch].instr->op[op].h] * 700000 / f->sampleRateRatio;
			o->s = expVol[f->ch[ch].instr->op[op].s];
			o->r = (f->ch[ch].instr->op[op].r >= 0) ? exp(-(expEnv[f->ch[ch].instr->op[op].r])*f->sampleRateRatio) : 2 - exp(-(expEnv[abs(f->ch[ch].instr->op[op].r)])*f->sampleRateRatio);

			o->finetune = f->ch[ch].instr->op[op].finetune;
			o->detune = f->ch[ch].instr->op[op].detune;
			o->mult = f->ch[ch].instr->op[op].mult;
			o->baseVol = f->ch[ch].instr->op[op].vol * !f->ch[ch].instr->op[op].muted;
			o->baseA = f->ch[ch].instr->op[op].a;
			o->baseD = f->ch[ch].instr->op[op].d;
			o->fixedFreq = f->ch[ch].instr->op[op].fixedFreq;
			o->offset = ((unsigned int)f->ch[ch].instr->op[op].offset)* LUTsize * 32;
			o->envLoop = f->ch[ch].instr->op[op].envLoop;
			o->pitchFinalRatio = f->ch[ch].instr->op[op].pitchFinalRatio;
			o->velSensitivity = (float)f->ch[ch].instr->op[op].velSensitivity*_99TO1;
			o->volScaling = f->ch[ch].instr->op[op].kbdVolScaling*0.001;
			o->kbdCenterNote = f->ch[ch].instr->op[op].kbdCenterNote;
		}
		for (unsigned op = 0; op < FM_op - 2; ++op)
		{
			f->ch[ch].op[op].toMix = (f->ch[ch].instr->toMix[op] >= 0) ? &f->ch[ch].op[f->ch[ch].instr->toMix[op]].out : &f->noConnect;
		}
		f->ch[ch].feedbackSource = &f->ch[ch].op[f->ch[ch].instr->feedbackSource].out;

	}

	/* Note changed */
	if (note < 128 && f->ch[ch].instr)
	{
		f->ch[ch].untransposedNote = note;


		fm_calcPitch(f, ch, note);

		if (volume < 100)
			f->ch[ch].noteVol = volume;

		if (f->ch[ch].instr->flags & FM_INSTR_LFORESET)
		{
			f->ch[ch].lfoEnv = f->ch[ch].lfoDelayCpt = f->ch[ch].lfo = 0;
			f->ch[ch].lfoPhase = f->ch[ch].lfoOffset * LUTsize / 2;
		}


		/* Trigger note transition smoothing algorithm to avoid clicks/pops */
		if (f->ch[ch].instr->flags & FM_INSTR_SMOOTH && f->ch[ch].currentEnvLevel > 0.1 && (f->ch[ch].instr->envReset || f->ch[ch].instr->phaseReset))
		{

			f->ch[ch].fade = 1;
			f->ch[ch].fadeFrom = f->ch[ch].lastRender;
			f->ch[ch].delta = clamp((f->ch[ch].lastRender - f->ch[ch].lastRender2), -2000, 2000)*f->sampleRateRatio;

			f->ch[ch].fadeIncr = 0.95 - f->ch[ch].note*0.001;
		}

		for (unsigned op = 0; op < FM_op; ++op)
		{
			fm_operator* o = &f->ch[ch].op[op];

			fm_calcOpVol(o, f->ch[ch].note, volume == 255 ? f->ch[ch].noteVol : volume);
			o->amp = 0;
			o->a = expEnv[(int)max(0, min(99, (o->baseA + f->ch[ch].instr->op[op].kbdAScaling*((int)f->ch[ch].note - f->ch[ch].instr->op[op].kbdCenterNote)*0.07f)))] * f->sampleRateRatio;
			o->d = 1 - exp(-expEnv[(int)max(0, min(99, (o->baseD + f->ch[ch].instr->op[op].kbdDScaling*((int)f->ch[ch].note - f->ch[ch].instr->op[op].kbdCenterNote)*0.07f)))] * f->sampleRateRatio);

			if (_instrument != 255)
			{
				if (f->ch[ch].instr->envReset)
				{
					o->env = 0;
					o->out = 0;
				}

				f->ch[ch].op0 = o->envCount = o->pitchTime = 0;
				o->pitchMod = o->pitchDestRatio = 1;
				o->state = 1;
			}
		}



	}



	f->ch[ch].active = 1;
}

/* Creates a table containing all current pannings/volumes/tempo/time info for each row, for fast seeking */

void fm_buildStateTable(fmsynth* f, unsigned orderStart, unsigned orderEnd, unsigned channelStart, unsigned channelEnd)
{

	orderStart = clamp(orderStart, 0, f->patternCount);
	orderEnd = clamp(orderEnd, 0, f->patternCount);
	channelStart = clamp(channelStart, 0, FM_ch);
	channelEnd = clamp(channelEnd, 0, FM_ch);


	for (int order = orderStart; order < orderEnd; order++)
	{

		if (order == 0)
		{
			for (unsigned ch = 0; ch < FM_ch; ch++)
			{
				f->channelStates[order][0].pan[ch] = f->ch[ch].initial_pan;
				f->channelStates[order][0].vol[ch] = f->ch[ch].initial_vol;
			}
			f->channelStates[order][0].tempo = f->initial_tempo;
			f->channelStates[order][0].time = 0;
		}
		for (int j = 0; j < f->patternSize[order]; j++)
		{

			/* Replicate previous row data (tempo/time) */
			if (j>0)
			{
				f->channelStates[order][j].tempo = f->channelStates[order][j - 1].tempo;
				f->channelStates[order][j].time = f->channelStates[order][j - 1].time + 60.f / (f->channelStates[order][j].tempo*f->diviseur);
			}
			else if (order > 0)
			{
				f->channelStates[order][j].tempo = f->channelStates[order - 1][f->patternSize[order - 1] - 1].tempo;
				f->channelStates[order][j].time = f->channelStates[order - 1][f->patternSize[order - 1] - 1].time + 60.f / (f->channelStates[order][j].tempo*f->diviseur);
			}
			for (unsigned ch = channelStart; ch< channelEnd; ch++)
			{
				/* Replicate previous row data (pan/vol for each channel) */
				if (j>0)
				{
					f->channelStates[order][j].vol[ch] = f->channelStates[order][j - 1].vol[ch];
					f->channelStates[order][j].pan[ch] = f->channelStates[order][j - 1].pan[ch];
				}
				else if (order > 0)
				{
					f->channelStates[order][j].vol[ch] = f->channelStates[order - 1][f->patternSize[order - 1] - 1].vol[ch];
					f->channelStates[order][j].pan[ch] = f->channelStates[order - 1][f->patternSize[order - 1] - 1].pan[ch];
				}

				switch (f->pattern[order][j][ch].fx)
				{
					case 'T':
						f->channelStates[order][j].tempo = f->pattern[order][j][ch].fxdata == 0 ? 1 : f->pattern[order][j][ch].fxdata;
						break;
					case 'X':
						f->channelStates[order][j].pan[ch] = f->pattern[order][j][ch].fxdata;
						break;
					case 'M':
						f->channelStates[order][j].vol[ch] = f->pattern[order][j][ch].fxdata;
						break;
				}
			}
		}
	}
	f->channelStatesDone = 1;
}



void fm_render(fmsynth* f, void* buffer, unsigned length, unsigned type)
{

	float *rendered = malloc(4*length); // float = 4bytes

	if (!rendered)
		return;

	_fm_render(f, (float*)rendered, length);

	switch (type%64)
	{
		case FM_RENDER_FLOAT:
		{
			float *buf_f = buffer;
			for (unsigned i = 0; i < length; i++)
			{
				buf_f[i] = clamp(rendered[i]/32768,-1.0,1.0);
			}
			break;
		}
		case FM_RENDER_8:
		{
			if(type & FM_RENDER_PAD32)
			{
				int *buf_32 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_32[i] = (clamp((signed char)(rendered[i]/256), -128,127));
				}
			}
			else
			{
				unsigned char *buf_8 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_8[i] = clamp(128+rendered[i]/256, 0,255);
				}
			}
			
			break;
		}
		case FM_RENDER_16:{
			if(type & FM_RENDER_PAD32)
			{
				int *buf_32 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_32[i] = clamp(rendered[i], -32768,32767);
				}
			}
			else
			{
				signed short *buf_16 = buffer;
				for (unsigned i = 0; i < length; i++)
				{
					buf_16[i] = clamp(rendered[i], -32768,32767);
				}
			}
			
			break;
		}
		case FM_RENDER_24:
		{
			unsigned char *buf_24 = buffer;

			if(type & FM_RENDER_PAD32)
			{
				for (unsigned i = 0; i < length; i++)
				{
					int val = clamp(rendered[i]*256, -8388608,8388607);

					// negative 24bit values should stay negative 32 bit values ! (negative has the top byte to 255)
					buf_24[i*4+3] = (val  < 0) ? 255 : 0;
					
					buf_24[i*4+2] = (unsigned char)((val&0x00ff0000) >> 16);
					buf_24[i*4+1] = (unsigned char)((val&0x00ff00)>>8);
					buf_24[i*4] = (unsigned char)(val & 0xff);
					
					
				}
			}
			else
			{
				for (unsigned i = 0; i < length; i++)
				{
					int val = clamp(rendered[i]*256, -8388608,8388607);

					buf_24[i*3+2] = (unsigned char)((val&0x00ff0000) >> 16);
					buf_24[i*3+1] = (unsigned char)((val&0x00ff00)>>8);
					buf_24[i*3] = (unsigned char)(val & 0xff);

				}
			}
			break;
		}
		case FM_RENDER_32:
		{
			int *buf_32 = buffer;
			for (unsigned i = 0; i < length; i++)
			{
				buf_32[i] = (signed int)clamp(((double)rendered[i]*256*256),-2147483648.f,2147483647.f);
			}
			break;
		}
	}

	free(rendered);
}

void fm_stopNote(fmsynth* f, unsigned ch)
{
	if (ch >= FM_ch || !f->ch[ch].active || f->ch[ch].note > 127)
		return;


	for (unsigned op = 0; op < FM_op; ++op)
	{
		fm_operator *o = &f->ch[ch].op[op];

		o->state = 6;
		o->pitchTime = expEnv[f->ch[ch].instr->op[op].pitchRelease] * f->sampleRateRatio;

		if (o->pitchFinalRatio>0)
			o->pitchDestRatio = 1 + expVol[o->pitchFinalRatio] * expVol[o->pitchFinalRatio] * 12;
		else if (f->ch[ch].instr->op[op].pitchFinalRatio < 0)
			o->pitchDestRatio = 1 + (float)o->pitchFinalRatio*_99TO1;
		else
			o->pitchDestRatio = 1;
	}
	f->ch[ch].note = 255;

}

void fm_stopSound(fmsynth* f)
{
	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		f->ch[ch].active = 0;
		f->ch[ch].lastRender = f->ch[ch].lastRender2 = 0;
		f->ch[ch].note = 255;
		f->ch[ch].cInstr = 0;
		f->ch[ch].instrNumber = 255;
		f->ch[ch].currentEnvLevel = 0;
		for (unsigned op = 0; op < FM_op; ++op)
		{
			f->ch[ch].op[op].state = f->ch[ch].op[op].env = f->ch[ch].op[op].amp = 0;
		}
	}
	memset(f->revBuf, 0, f->revBufSize*sizeof(float));
}

void fm_play(fmsynth* f)
{
	if (f->playing)
	{
		fm_stop(f,1);
		fm_setPosition(f,0,0,2);
	}
	if (!f->channelStatesDone)
		fm_buildStateTable(f, 0, f->patternCount, 0, FM_ch);
	f->playing = f->patternCount > 0;
	f->frameTimer = f->frameTimerFx = 0;
	f->tempRow = f->tempOrder = -1;
	f->looping = -1;
	f->loopCount = 0;
	fm_initChannels(f);
}


void fm_stop(fmsynth* f, int cut)
{
	if (cut)
	{
		fm_stopSound(f);
	}
	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		fm_stopNote(f, ch);
		f->ch[ch].cInstr = 0;
	}
	f->playing = 0;
}

void fm_setPosition(fmsynth* f, int order, int row, int cutNotes)
{
	if (cutNotes == 1)
	for (unsigned ch = 0; ch < FM_ch; ++ch) fm_stopNote(f, ch);
	else if (cutNotes == 2)
		fm_stopSound(f);

	f->order = clamp(order, 0, f->patternCount - 1);
	f->row = clamp(row, 0, (int)f->patternSize[order] - 1);
	f->frameTimer = f->frameTimerFx = 0;
	if (f->playing)
		fm_initChannels(f);
}

/* Writes a LZ4 compressed block */

int write_compressed(char* data, unsigned short size, FILE *fp)
{
	char* compressedData = malloc(size * (257 / 256) + 1);
	if (!compressedData) return 0;

	unsigned short compressedDataSize = LZ4_compress_HC(data, compressedData, size, LZ4_compressBound(size), 9);
	if (compressedDataSize <= 0) return 0;

	fwrite(&size, 2, 1, fp);
	fwrite(&compressedDataSize, 2, 1, fp);
	fwrite(compressedData, compressedDataSize, 1, fp);
	free(compressedData);
	return 1;
}



#include <stdint.h>

static uint32_t adler32(const void *buf, size_t buflength)
{
	const uint8_t *buffer = (const uint8_t*)buf;

	uint32_t s1 = 1;
	uint32_t s2 = 0;

	for (size_t n = 0; n < buflength; n++)
	{
		s1 = (s1 + buffer[n]) % 65521;
		s2 = (s2 + s1) % 65521;
	}
	return (s2 << 16) | s1;
}

int fm_saveSong(fmsynth* f, const char* filename)
{
	FILE *fp = fopen(filename, "wb+");
	if (!fp)
	{
		return 0;
	}
	fputc('F', fp);
	fputc('M', fp);
	fputc('C', fp);
	fputc('S', fp);
	fputc(0x00, fp); // unused byte
	fputc(FMCS_version, fp); // version
	unsigned char temp = strlen(&f->songName[0]);
	fwrite(&temp, sizeof(temp), 1, fp);
	fwrite(&f->songName[0], temp, 1, fp);

	temp = strlen(&f->author[0]);
	fwrite(&temp, sizeof(temp), 1, fp);
	fwrite(&f->author[0], temp, 1, fp);

	temp = strlen(&f->comments[0]);
	fwrite(&temp, sizeof(temp), 1, fp);
	fwrite(&f->comments[0], temp, 1, fp);

	fwrite(&f->initial_tempo, sizeof(f->initial_tempo), 1, fp); // tempo
	fwrite(&f->diviseur, sizeof(f->diviseur), 1, fp); // quarter note
	fwrite(&f->_globalVolume, sizeof(f->_globalVolume), 1, fp);
	fwrite(&f->transpose, sizeof(f->transpose), 1, fp);

	temp = round(f->initialReverbLength * 160);
	fwrite(&temp, sizeof(temp), 1, fp);

	temp = round(f->initialReverbRoomSize * 160);
	fwrite(&temp, sizeof(temp), 1, fp);

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		fwrite(&f->ch[ch].initial_pan, sizeof(f->ch[ch].initial_pan), 1, fp); // ch panning
		fwrite(&f->ch[ch].initial_vol, sizeof(f->ch[ch].initial_vol), 1, fp); // ch volume
		fwrite(&f->ch[ch].initial_reverb, sizeof(f->ch[ch].initial_reverb), 1, fp); // ch volume
	}

	temp = f->patternCount;

	fwrite(&temp, sizeof(temp), 1, fp);
	for (unsigned i = 0; i < f->patternCount; i++)
	{
		temp = f->patternSize[i];
		fwrite(&temp, sizeof(temp), 1, fp);
		if (!write_compressed((char*)&f->pattern[i][0], sizeof(Cell)*f->patternSize[i] * FM_ch, fp))
			return 0;
	}
	fwrite(&f->instrumentCount, 1, 1, fp);


	for (int slot = 0; slot < f->instrumentCount; slot++)
	{
		f->instrument[slot].version = FMCI_version;
	}

	if (!write_compressed((char*)&f->instrument[0], sizeof(fm_instrument)*f->instrumentCount, fp))
		return 0;


	int totalSize = ftell(fp);
	char *all = malloc(totalSize);
	fseek(fp, 0, SEEK_SET);
	fread(all, totalSize, 1, fp);

	unsigned checksum = adler32(all, totalSize);

	fseek(fp, 0, SEEK_END);
	fwrite((char*)&checksum, 4, 1, fp);

	fclose(fp);

	return 1;
}

void fm_patternClear(fmsynth* f)
{
	if (f->pattern && f->patternCount > 0)
	{

		f->row = f->order = 0;

		for (unsigned i = 0; i < f->patternCount; i++)
		{
			free(f->pattern[i]);
			free(f->channelStates[i]);
		}

		f->patternCount = 0;
	}
}

void fm_instrumentRecovery(fm_instrument * i)
{
	i->magic[0] = 'F';
	i->magic[1] = 'M';
	i->magic[2] = 'C';
	i->magic[3] = 'I';

	i->lfoWaveform = clamp(i->lfoWaveform, 0, 19);
	i->volume = clamp(i->volume, 0, 99);
	i->feedbackSource = clamp(i->feedbackSource, 0, 5);
	i->transpose = clamp(i->transpose, -12, 12);
	i->tuning = clamp(i->tuning, -100, 100);

	int nbOuts = 0;

	for (int j = 0; j < 4; j++)
		i->toMix[j] = clamp(i->toMix[j], -1, 5);

	for (int op = 0; op < 6; op++)
	{
		i->op[op].vol = clamp(i->op[op].vol, 0, 99);
		i->op[op].delay = clamp(i->op[op].delay, 0, 70);
		i->op[op].a = clamp(i->op[op].a, 0, 99);
		i->op[op].h = clamp(i->op[op].h, 0, 80);
		i->op[op].d = clamp(i->op[op].d, 0, 99);
		i->op[op].s = clamp(i->op[op].s, 0, 99);
		i->op[op].r = clamp(i->op[op].r, -99, 99);
		if (i->op[op].fixedFreq)
			i->op[op].mult = clamp(i->op[op].mult, 0, 255);
		else
			i->op[op].mult = clamp(i->op[op].mult, 0, 40);
		i->op[op].finetune = clamp(i->op[op].finetune, 0, 24);
		i->op[op].detune = clamp(i->op[op].detune, -100, 100);
		i->op[op].waveform = clamp(i->op[op].waveform, 0, 7);
		i->op[op].offset = clamp(i->op[op].offset, 0, 31);
		i->op[op].pitchDecay = clamp(i->op[op].pitchDecay, 0, 99);
		i->op[op].pitchRelease = clamp(i->op[op].pitchRelease, 0, 99);
		i->op[op].pitchInitialRatio = clamp(i->op[op].pitchInitialRatio, -99, 99);
		i->op[op].pitchFinalRatio = clamp(i->op[op].pitchFinalRatio, -99, 99);


		i->op[op].connect = clamp(i->op[op].connect, -1, 5);
		i->op[op].connect2 = clamp(i->op[op].connect2, -1, 6);
		i->op[op].connectOut = clamp(i->op[op].connectOut, -1, 5);

		if (i->op[op].connectOut >= 0)
		{
			nbOuts++;
		}

		if (i->op[op].connect == op)
			i->op[op].connect = -1;

		if (i->op[op].connect2 == op)
			i->op[op].connect2 = -1;

		for (int op2 = 0; op2 < 6; op2++)
		{
			if (op != op2)
			{
				if (i->op[op].connect == op2 && i->op[op2].connect == op)
				{
					i->op[op2].connect = -1;
				}
				if (i->op[op].connect2 == op2 && i->op[op2].connect2 == op)
				{
					i->op[op2].connect2 = -1;
				}
			}
		}
	}
	if (nbOuts == 0)
	{
		i->op[0].connectOut = 0;
		i->op[1].connectOut = 1;
		i->op[2].connectOut = 2;
		i->op[3].connectOut = 3;
		i->op[4].connectOut = 4;
		i->op[5].connectOut = 5;
	}
}




static int readFromMemory(fmsynth *f, char *dst, int len, char *from)
{
	if (f->readSeek >= f->totalFileSize)
		return 0;

	memcpy(dst, from + f->readSeek, len);
	f->readSeek += len;
	return 1;
}

static char* readFromMemoryPtr(fmsynth *f, int len, char *from)
{
	if (f->readSeek >= f->totalFileSize)
		return 0;

	f->readSeek += len;
	return &from[f->readSeek - len];
}

/* Reads a LZ4 compressed block */

static int read_compressed_mem(fmsynth *f, char* dest, char* data)
{
	unsigned short originalDataSize, compressedDataSize;
	readFromMemory(f, (char*)&originalDataSize, 2, data);
	readFromMemory(f, (char*)&compressedDataSize, 2, data);

	if (originalDataSize == 0 || compressedDataSize == 0)
		return 0;

	char* compressedData = readFromMemoryPtr(f, compressedDataSize, data);
	if (!compressedData) return 0;

	if (LZ4_decompress_safe(compressedData, dest, compressedDataSize, originalDataSize) < 0)
	{
		return 0;
	}

	return 1;
}

int fm_loadSongFromMemory(fmsynth* f, char* data, unsigned len)
{
	f->totalFileSize = len;
	unsigned char nbOrd, nbRow, temp;
	unsigned int error = 0;

	if (f->totalFileSize < 3 * FM_ch + 6)
	{
		return 2;
	}


	f->readSeek = 5;
	readFromMemory(f, &temp, 1, data);

	if (temp > FMCS_version)
	{
		return FM_ERR_FILEVERSION;
	}


	f->order = f->row = 0;
	fm_patternClear(f);

	readFromMemory(f, &temp, 1, data);

	readFromMemory(f, &f->songName[0], temp, data);
	f->songName[temp] = 0;

	readFromMemory(f, &temp, 1, data);
	readFromMemory(f, &f->author[0], temp, data);
	f->author[temp] = 0;

	readFromMemory(f, &temp, 1, data);
	readFromMemory(f, &f->comments[0], temp, data);
	f->comments[temp] = 0;




	readFromMemory(f, &f->initial_tempo, sizeof(f->initial_tempo), data);
	f->initial_tempo = max(1, f->initial_tempo);

	readFromMemory(f, &f->diviseur, sizeof(f->diviseur), data);
	f->diviseur = clamp(f->diviseur, 1, 32);

	readFromMemory(f, &f->_globalVolume, sizeof(f->_globalVolume), data);

	fm_setVolume(f, f->_globalVolume);

	readFromMemory(f, &f->transpose, sizeof(f->transpose), data);

	readFromMemory(f, &temp, sizeof(temp), data);
	f->initialReverbLength = (float)temp / 160;

	readFromMemory(f, &temp, sizeof(temp), data);
	f->initialReverbRoomSize = (float)temp / 160;

	fm_initReverb(f, f->initialReverbRoomSize);

	for (unsigned ch = 0; ch < FM_ch; ++ch)
	{
		f->ch[ch].cInstr = 0;
		readFromMemory(f, &f->ch[ch].initial_pan, sizeof(f->ch[ch].initial_pan), data); // ch panning

		readFromMemory(f, &f->ch[ch].initial_vol, sizeof(f->ch[ch].initial_vol), data); // ch volume
		f->ch[ch].initial_vol = min(f->ch[ch].initial_vol, 99);

		readFromMemory(f, &f->ch[ch].initial_reverb, sizeof(f->ch[ch].initial_reverb), data); // ch volume
		f->ch[ch].initial_reverb = min(f->ch[ch].initial_reverb, 99);
	}

	readFromMemory(f, &nbOrd, sizeof(nbOrd), data);
	fm_resizePatterns(f, nbOrd);

	for (unsigned i = 0; i < nbOrd; i++)
	{ 
		readFromMemory(f, &nbRow, sizeof(nbRow), data);
		
		fm_resizePattern(f, i, nbRow==0 ? 256 : nbRow, 0);


		if (!read_compressed_mem(f, (char*)&f->pattern[i][0], data))
		{
			error++;
			continue;
		}
	}


	readFromMemory(f, &f->instrumentCount, 1, data);
	fm_resizeInstrumentList(f, f->instrumentCount);

	if (f->instrumentCount <= 0 || f->instrumentCount > 255)
	{
		fm_resizeInstrumentList(f, 1);
	}
	if (f->patternCount == 0)
	{
		fm_resizePatterns(f, 1);
	}

	if (!read_compressed_mem(f, (char*)&f->instrument[0], data))
		error++;

	unsigned checksum;

	if (!readFromMemory(f, (char*)&checksum, 4, data))
	{
		error++;
	}

	if (checksum != adler32(data, max(0, (int)f->totalFileSize - 4)))
	{
		error++;
	}


	if (error)
	{
		for (int i = 0; i < f->instrumentCount; i++)
		{
			fm_instrumentRecovery(&f->instrument[i]);
		}
		return FM_ERR_FILECORRUPTED;
	}

	fm_buildStateTable(f, 0, f->patternCount, 0, FM_ch);

	return 0;
}

char* fm_fileToMemory(fmsynth *f, const char* filename)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp)
	{
		return 0;
	}

	fseek(fp, 0, SEEK_END);
	f->totalFileSize = ftell(fp);
	char *all = malloc(f->totalFileSize);
	if (!all)
	{
		return 0;
	}

	fseek(fp, 0, SEEK_SET);
	fread(all, f->totalFileSize, 1, fp);
	fclose(fp);
	f->readSeek = 0;
	return all;
}

int fm_loadSong(fmsynth* f, const char* filename)
{
	char *data = fm_fileToMemory(f, filename);

	if (!data)
		return FM_ERR_FILEIO;

	int result = fm_loadSongFromMemory(f, data, f->totalFileSize);
	free(data);


	return result;
}

void fm_clearSong(fmsynth* f)
{
	fm_resizePatterns(f, 0);
	f->order = f->row = f->transpose = 0;
	fm_setDefaults(f);
	memset(f->songName, 0, 64);
	memset(f->author, 0, 64);
	memset(f->comments, 0, 256);
}

void fm_createDefaultInstrument(fmsynth* f, unsigned slot)
{
	strncpy((char*)&f->instrument[slot].name[0], "Default", 7);
	strncpy((char*)&f->instrument[slot].magic[0], "FMCI", 4);
	f->instrument[slot].dummy = 0;
	f->instrument[slot].version = FMCI_version;
	for (unsigned op = 0; op < FM_op; ++op)
	{
		f->instrument[slot].op[op].connectOut = op;
		f->instrument[slot].op[op].connect = -1;
		f->instrument[slot].op[op].connect2 = -1;
	}
	f->instrument[slot].volume = 99;
	f->instrument[slot].op[0].a = 99;
	f->instrument[slot].op[0].mult = 1;
	f->instrument[slot].op[0].vol = 99;
	f->instrument[slot].op[0].r = 99;
}

int fm_resizeInstrumentList(fmsynth* f, unsigned size)
{
	if (size > 255)
	{
		return 0;
	}
	if (f->instrumentCount > 0 && size == 0)
	{
		f->instrumentCount = 0;
		return 1;
	}

	fm_instrument* newI = realloc(f->instrument, sizeof(fm_instrument)*size);

	if (!newI)
	{
		return 0;
	}

	f->instrument = newI;

	if (size > f->instrumentCount)
	{
		memset((char*)&f->instrument[f->instrumentCount], 0, (size - f->instrumentCount)*sizeof(fm_instrument));
		for (unsigned i = f->instrumentCount; i < size; i++)
		{
			fm_createDefaultInstrument(f,i);
		}
	}
	f->instrumentCount = size;
	return 1;
}

int fm_resizePatterns(fmsynth* f, unsigned count)
{
	if (count > 256)
		return 0;

	if (f->patternCount > 0 && count == 0)
	{
		f->patternCount = 0;
		return 1;
	}

	if (count < f->patternCount && f->pattern)
	{
		for (unsigned i = count; i < f->patternCount; i++)
		{
			free(f->pattern[i]);
			free(f->channelStates[i]);
		}
		if (f->order >= count)
			f->order = max(0, count - 1);
	}

	unsigned int* newPs = realloc(f->patternSize, sizeof(unsigned)*count);
	Cell(** newPa)[24] = realloc(f->pattern, sizeof(Cell*)*count);
	ChannelState** newC = realloc(f->channelStates, sizeof(ChannelState*)*count);

	if (!newPs || !newPa || !newC)
	{
		return 0;
	}

	f->patternSize = newPs;
	f->pattern = newPa;
	f->channelStates = newC;

	unsigned oldPatternCount = f->patternCount;
	f->patternCount = count;

	if (count > oldPatternCount)
	{
		for (unsigned i = oldPatternCount; i < count; i++)
		{
			f->pattern[i] = 0;
			f->channelStates[i] = 0;
			f->patternSize[i] = 0;
			if (!fm_resizePattern(f, i, 1, 0))
			{
				return 0;
			}
		}
	}


	f->channelStatesDone = 0;
	return 1;
}


int fm_clearPattern(fmsynth* f, unsigned pattern, unsigned rowStart, unsigned count)
{
	if (pattern >= f->patternCount || rowStart > 255 || count > 256)
		return 0;
	memset(&f->pattern[pattern][rowStart], 255, count*sizeof(Cell)*FM_ch);
	memset(&f->channelStates[pattern][rowStart], 255, count*sizeof(ChannelState));
	f->channelStatesDone = 0;
	return 1;
}


int fm_insertPattern(fmsynth* f, unsigned rows, unsigned pos)
{
	{
		if (pos > f->patternCount || !fm_resizePatterns(f, f->patternCount + 1))
		return 0;
	}
	Cell(*newPtr)[24] = f->pattern[f->patternCount - 1];
	ChannelState* newPtri = f->channelStates[f->patternCount - 1];

	for (unsigned i = f->patternCount - 1; i > pos; i--)
	{
		f->pattern[i] = f->pattern[i - 1];
		f->channelStates[i] = f->channelStates[i - 1];
		f->patternSize[i] = f->patternSize[i - 1];
	}
	f->pattern[pos] = newPtr;
	f->channelStates[pos] = newPtri;

	if (!fm_resizePattern(f, pos, rows, 0))
		return 0;
	fm_clearPattern(f, pos, 0, rows);
	f->channelStatesDone = 0;
	return 1;
}

int fm_removePattern(fmsynth* f, unsigned order)
{
	if (order >= f->patternCount)
		return 0;

	if (f->patternCount == 1)
	{
		fm_clearPattern(f, f->patternCount - 1, 0, f->patternSize[0]);
	}
	else
	{
		free(f->pattern[order]);
		free(f->channelStates[order]);
		for (unsigned i = order; i < f->patternCount - 1; i++)
		{
			f->pattern[i] = f->pattern[i + 1];
			f->channelStates[i] = f->channelStates[i + 1];
			f->patternSize[i] = f->patternSize[i + 1];
		}
		f->patternCount--;

	}
	f->order = min(f->order, f->patternCount - 1);
	f->row = min(f->row, f->patternSize[f->order] - 1);
	f->channelStatesDone = 0;
	return 1;
}

int fm_resizePattern(fmsynth* f, unsigned order, unsigned size, unsigned scaleContent)
{
	if (order >= f->patternCount || size == 0)
	{
		return 0;
	}

	int oldPatternSize = f->patternSize[order];

	size = clamp(size, 1, 256);

	float scaleRatio = 0;
	if (scaleContent)
	{
		scaleRatio = (float)size / oldPatternSize;
	}

	/* Shrink content */
	if (scaleContent && scaleRatio < 1)
	{
		for (int i = 0; i < f->patternSize[order]; i++)
		{
			for (int ch = 0; ch < FM_ch; ch++)
				f->pattern[order][(unsigned)round(i*0.5)][ch] = f->pattern[order][i][ch];
		}
	}

	Cell(*newP)[24] = realloc(f->pattern[order], sizeof(Cell)*size*FM_ch);
	ChannelState* newC = realloc(f->channelStates[order], sizeof(ChannelState)*size);

	if (newP && newC)
	{
		f->pattern[order] = newP;
		f->channelStates[order] = newC;
	}
	else
	{
		return 0;
	}

	if (size > f->patternSize[order])
		fm_clearPattern(f, order, f->patternSize[order], size - f->patternSize[order]);


	f->patternSize[order] = size;
	f->row = min(f->row, f->patternSize[f->order] - 1);


	/* Expand content */
	if (scaleContent && scaleRatio > 1)
	{
		for (int i = oldPatternSize - 1; i >= 0; i--)
		{
			for (int ch = 0; ch < FM_ch; ch++)
				f->pattern[order][(unsigned)(i*scaleRatio)][ch] = f->pattern[order][i][ch];

			memset(&f->pattern[order][(unsigned)round(i*scaleRatio) + 1], 255, sizeof(Cell)*FM_ch);
		}
	}
	f->channelStatesDone = 0;
	return 1;
}

float fm_getTime(fmsynth* f)
{
	if (f->order >= f->patternCount || f->row > f->patternSize[f->order])
		return 0;

	return f->channelStates[f->order][f->row].time;
}

int fm_saveInstrument(fmsynth* f, const char* filename, unsigned slot)
{
	if (slot < f->instrumentCount)
	{
		FILE *fp = fopen(filename, "wb");
		if (!fp)
			return 0;
		fputc('F', fp);
		fputc('M', fp);
		fputc('C', fp);
		fputc('I', fp);
		fputc(0x00, fp); // unused byte
		fputc(FMCI_version, fp); // version

	//	f->instrument[slot].flags |= FM_INSTR_TRANSPOSABLE;
		if (!write_compressed((char*)&f->instrument[slot].name[0], sizeof(fm_instrument)-6, fp))
			return 0;
		fclose(fp);
		return 1;
	}
	return 0;
}

int fm_loadInstrumentFromMemory(fmsynth* f, char *data, unsigned slot)
{
	if (slot >= f->instrumentCount)
	{
		fm_resizeInstrumentList(f, slot + 1);
	}

	readFromMemory(f, (char*)&f->instrument[slot].magic, 4, data);
	readFromMemory(f, (char*)&f->instrument[slot].dummy, 1, data);
	readFromMemory(f, (char*)&f->instrument[slot].version, 1, data);

	if (f->instrument[slot].version > FMCI_version)
	{
		return FM_ERR_FILEVERSION;
	}


	if (!read_compressed_mem(f, (char*)&f->instrument[slot].name[0], data))
		return FM_ERR_FILECORRUPTED;

	return 0;
}


int fm_loadInstrument(fmsynth* f, const char* filename, unsigned slot)
{

	char *data = fm_fileToMemory(f, filename);

	if (!data)
		return FM_ERR_FILEIO;

	int result = fm_loadInstrumentFromMemory(f, data, slot);
	free(data);

	return result;
}



void fm_removeInstrument(fmsynth* f, unsigned slot, int removeOccurences)
{
	if (slot >= f->instrumentCount)
		return;

	if (removeOccurences)
	{
		for (unsigned i = 0; i < f->patternCount; i++)
		{
			for (unsigned j = 0; j < f->patternSize[i]; j++)
			{
				for (unsigned ch = 0; ch < FM_ch; ch++)
				{
					if (f->pattern[i][j][ch].instr == slot)
					{
						f->pattern[i][j][ch].instr = f->pattern[i][j][ch].vol = f->pattern[i][j][ch].note = f->pattern[i][j][ch].fx = f->pattern[i][j][ch].fxdata = 255;
					}
					else if (f->pattern[i][j][ch].instr < 255 && f->pattern[i][j][ch].instr > slot)
					{
						f->pattern[i][j][ch].instr--;
					}
				}
			}
		}
	}

	if (f->instrumentCount == 1)
		return;

	for (unsigned i = slot; i < f->instrumentCount - 1; i++)
	{
		f->instrument[i] = f->instrument[i + 1];
	}
	fm_resizeInstrumentList(f, f->instrumentCount - 1);
}

void fm_setVolume(fmsynth *f, int volume)
{
	volume = clamp(volume, 0, 99);
	f->_globalVolume = volume;
	f->globalVolume = expVol[volume] * 4096 / LUTsize;
}


void fm_setPlaybackVolume(fmsynth *f, int volume)
{
	f->playbackVolume = expVol[volume] * 4096 / LUTsize;
}

void fm_getPosition(fmsynth* f, int *order, int *row)
{
	*order = f->order;
	*row = f->row;
}

void fm_setTime(fmsynth* f, int time, int cutNotes)
{
	for (int i = 0; i < f->patternCount; i++)
	{
		for (int j = 0; j < f->patternSize[i]; j++)
		{
			if (f->channelStates[i][j].time >= time)
			{
				fm_setPosition(f, i, j, cutNotes);
				return;
			}
		}
	}
	fm_setPosition(f, f->patternCount - 1, f->patternSize[f->patternCount - 1] - 1, cutNotes);
}

void fm_movePattern(fmsynth* f, int from, int to)
{
	if (from < 0 || from >= f->patternCount || to<0 || to >= f->patternCount)
		return;

	/* Move patterns by pairs until the elements are in the right position */
	for (int j = from; j >to; j--)
	{

		Cell(*ptr)[24] = f->pattern[j];
		f->pattern[j] = f->pattern[j - 1];
		f->pattern[j - 1] = ptr;

		ChannelState* ptr2 = f->channelStates[j];
		f->channelStates[j] = f->channelStates[j - 1];
		f->channelStates[j - 1] = ptr2;

		unsigned int size = f->patternSize[j];
		f->patternSize[j] = f->patternSize[j - 1];
		f->patternSize[j - 1] = size;
	}

	for (int j = from; j < to; j++)
	{

		Cell(*ptr)[24] = f->pattern[j];
		f->pattern[j] = f->pattern[j + 1];
		f->pattern[j + 1] = ptr;

		ChannelState* ptr2 = f->channelStates[j];
		f->channelStates[j] = f->channelStates[j + 1];
		f->channelStates[j + 1] = ptr2;

		unsigned int size = f->patternSize[j];
		f->patternSize[j] = f->patternSize[j + 1];
		f->patternSize[j + 1] = size;
	}
	f->channelStatesDone = 0;
}

void fm_moveChannels(fmsynth* f, int from, int to)
{
	if (from < 0 || from >= FM_ch || to < 0 || to >= FM_ch)
		return;

	/* Move pattern contents */
	for (int i = 0; i < f->patternCount; i++)
	{
		for (int j = 0; j < f->patternSize[i]; j++)
		{

			for (int ch = from; ch >to; ch--)
			{

				Cell ptr = f->pattern[i][j][ch];
				f->pattern[i][j][ch] = f->pattern[i][j][ch - 1];
				f->pattern[i][j][ch - 1] = ptr;

				unsigned char ptr2 = f->channelStates[i][j].pan[ch];
				f->channelStates[i][j].pan[ch] = f->channelStates[i][j].pan[ch - 1];
				f->channelStates[i][j].pan[ch - 1] = ptr2;

				unsigned char ptr3 = f->channelStates[i][j].vol[ch];
				f->channelStates[i][j].vol[ch] = f->channelStates[i][j].vol[ch - 1];
				f->channelStates[i][j].vol[ch - 1] = ptr3;
			}

			for (int ch = from; ch < to; ch++)
			{

				Cell ptr = f->pattern[i][j][ch];
				f->pattern[i][j][ch] = f->pattern[i][j][ch + 1];
				f->pattern[i][j][ch + 1] = ptr;

				unsigned char ptr2 = f->channelStates[i][j].pan[ch];
				f->channelStates[i][j].pan[ch] = f->channelStates[i][j].pan[ch + 1];
				f->channelStates[i][j].pan[ch + 1] = ptr2;

				unsigned char ptr3 = f->channelStates[i][j].vol[ch];
				f->channelStates[i][j].vol[ch] = f->channelStates[i][j].vol[ch + 1];
				f->channelStates[i][j].vol[ch + 1] = ptr3;

			}

		}
	}

	fm_stopSound(f);

	/* Move channels */

	for (int ch = from; ch > to; ch--)
	{

		fm_channel channel = f->ch[ch];
		f->ch[ch] = f->ch[ch - 1];
		f->ch[ch - 1] = channel;
	}

	for (int ch = from; ch < to; ch++)
	{

		fm_channel channel = f->ch[ch];
		f->ch[ch] = f->ch[ch + 1];
		f->ch[ch + 1] = channel;
	}


	f->channelStatesDone = 0;
}

void fm_setChannelVolume(fmsynth *f, int channel, int volume)
{
	volume = clamp(volume, 0, 99);
	f->ch[channel].initial_vol = volume;
	f->ch[channel].vol = expVol[volume];
	f->channelStatesDone = 0;
}
void fm_setChannelPanning(fmsynth *f, int channel, int panning)
{
	panning = clamp(panning, 0, 255);
	f->ch[channel].initial_pan = panning;
	f->ch[channel].destPan = panning;
	f->channelStatesDone = 0;
}

void fm_setChannelReverb(fmsynth *f, int channel, int reverb)
{
	reverb = clamp(reverb, 0, 99);
	f->ch[channel].initial_reverb = reverb;
	f->ch[channel].reverbSend = expVol[reverb];
}

void fm_setTempo(fmsynth* f, int tempo)
{
	tempo = clamp(tempo, 1, 255);
	f->tempo = f->initial_tempo = tempo;
	f->channelStatesDone = 0;
}

float fm_getSongLength(fmsynth* f)
{
	if (f->patternCount == 0)
		return 0;

	if (!f->channelStatesDone)
		fm_buildStateTable(f, 0, f->patternCount, 0, FM_ch);

	return f->channelStates[f->patternCount - 1][f->patternSize[f->patternCount - 1] - 1].time + 1.0 / f->channelStates[f->patternCount - 1][f->patternSize[f->patternCount - 1] - 1].tempo*(60.0 / f->diviseur);
}

float fm_volumeToExp(int volume)
{
	return expVol[volume];
}

int fm_write(fmsynth *f, unsigned pattern, unsigned row, unsigned channel, Cell data)
{
	if (pattern >= f->patternCount || row >= f->patternSize[pattern] || channel >= FM_ch)
		return 0;

	struct Cell *current = &f->pattern[pattern][row][channel];

	if (data.note != 255)
		current->note = data.note;

	if (data.instr != 255)
		current->instr = data.instr;

	if (data.vol != 255)
		current->vol = data.vol;

	if (data.fx != 255)
	{
		current->fx = data.fx;
		f->channelStatesDone = 0;
	}

	if (data.fxdata != 255)
	{
		current->fxdata = data.fxdata;
		f->channelStatesDone = 0;
	}

	return 1;
}

int fm_getPatternSize(fmsynth *f, int pattern)
{
	if (pattern >= f->patternCount)
		return 0;
	return f->patternSize[pattern];
}

int fm_insertRows(fmsynth *f, unsigned pattern, unsigned row, unsigned count)
{
	if (pattern >= f->patternCount || row >= f->patternSize[pattern] || f->patternSize[pattern] + count > 256)
		return 0;

	if (!fm_resizePattern(f, pattern, f->patternSize[pattern] + count, 0))
		return 0;

	f->channelStatesDone = 0;

	for (int i = f->patternSize[pattern] - 1; i >= row; i--)
	{
		for (unsigned ch = 0; ch < FM_ch; ch++)
		{
			f->pattern[f->order][i][ch] = f->pattern[f->order][i - count][ch];
		}
	}

	if (!fm_clearPattern(f, pattern, row, count))
		return 0;
	return 1;
}

int fm_removeRows(fmsynth *f, unsigned pattern, unsigned row, unsigned count)
{
	if (pattern >= f->patternCount || row + count > f->patternSize[pattern])
		return 0;

	for (int i = row; i < f->patternSize[pattern] - count; i++)
	{
		for (unsigned ch = 0; ch < FM_ch; ch++)
		{
			f->pattern[pattern][i][ch] = f->pattern[pattern][i + count][ch];
		}
	}

	f->channelStatesDone = 0;

	if (!fm_resizePattern(f, pattern, f->patternSize[pattern] - count, 0))
		return 0;

	return 1;
}

int fm_isInstrumentUsed(fmsynth *f, unsigned id)
{
	unsigned instrCount=0;

	for (int j = 0; j < f->patternCount; j++)
	{
		for (int k = 0; k < f->patternSize[j]; k++)
		{
			for (int l = 0; l < FM_ch; l++)
			{
				if (f->pattern[j][k][l].instr == id)
					instrCount++;
			}
		}
	}
	return instrCount >0;
}
