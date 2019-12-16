#include "configEditor.hpp"
#include <math.h>


#ifdef _WIN32
#include "pa_win_wasapi.h"
#include "pa_win_ds.h"
#endif


extern PaStream *stream;
extern PaStreamParameters out;

int nearestPow2( int aSize ){
  return round(pow( 2, round( log( aSize ) / log( 2 ) ) )); 
}

int ConfigEditor::selectSoundDevice(int soundDeviceId, int _samplerate, int _latency, bool force)
{

	soundDeviceId = min(Pa_GetDeviceCount()-1,soundDeviceId);
	currentSoundDeviceId = soundDeviceId;
	for (int i = 0; i < soundDevicesList.text.size(); i++)
	{
		if (soundDeviceId == soundDeviceIds[i])
		{
			soundDevicesList.select(i);
		}
	}
	int playing = fm->playing;

	PaStreamParameters nout;
	nout.device = soundDeviceId;
	nout.channelCount = 2;
	nout.sampleFormat = paInt16;
	nout.suggestedLatency = 0.001*_latency;

	const PaDeviceInfo* p = Pa_GetDeviceInfo(soundDeviceId);
	const PaHostApiInfo *phost = Pa_GetHostApiInfo(p->hostApi);
#ifdef _WIN32
	PaWasapiStreamInfo wasapi_p;
	PaWinDirectSoundStreamInfo dsound_p;


	wasapiExclusive.visible = (phost->type == paWASAPI);

	/* Optional exclusive mode for WASAPI (allows very low latency) */
	if (phost->type == paWASAPI && wasapiExclusive.checked)
	{
		wasapi_p.flags= paWinWasapiExclusive;
		wasapi_p.version=1;
		wasapi_p.hostApiType = paWASAPI;
		wasapi_p.size = sizeof(PaWasapiStreamInfo);

		nout.hostApiSpecificStreamInfo = &wasapi_p;
	}
	/* DirectSound wants power of two latencies, and doesn't like it < 32 */
	else if (phost->type == paDirectSound) {

		nout.hostApiSpecificStreamInfo = 0;
		nout.suggestedLatency = 0.001*max(32,nearestPow2(_latency));
	}
	else
#endif
	{
		nout.hostApiSpecificStreamInfo = 0;
	}




	PaError err = Pa_IsFormatSupported(NULL, &nout, _samplerate);

	if (err != paFormatIsSupported || phost->type == paWASAPI && _samplerate != (int)round(p->defaultSampleRate))
	{
		sampleRateError.setString("Unsupported !");
		if (approvedSampleRate == -1)
		{
			_samplerate = p->defaultSampleRate;
		}
		else
		{
			_samplerate = approvedSampleRate;
		}

		return -1;
	}
	else
	{

		if (force || soundDeviceId != approvedDeviceId || _samplerate != approvedSampleRate || _latency != currentLatency || sampleRateError.getString() != "")
		{

			sampleRateError.setString("");

			Pa_AbortStream(stream);
			Pa_CloseStream(stream);

			PaError err;

			out=nout;

			if ((err = Pa_OpenStream(&stream, NULL, &out, _samplerate, 0, 0, (PaStreamCallback*)callbackFunc, fm)) != paNoError)
			{
				sampleRateError.setString("Unable to open");
				error(Pa_GetErrorText(err));
				return -1;
			}


			if (_samplerate != approvedSampleRate)
			{
				fm_setSampleRate(fm, _samplerate);
			}


			Pa_StartStream(stream);
			approvedDeviceId = soundDeviceId;
			approvedSampleRate = _samplerate;
			currentLatency = _latency;
		}
	}


	//approvedDeviceId = soundDeviceId;




	return _samplerate;
}


void ConfigEditor::selectBestSoundDevice()
{
	if (!audioInitialized)
		return;

	int soundDeviceId = atoi(ini_config.GetValue("config", "soundDeviceId", "-1"));
	if (equalsIgnoreCase(lastRunVersion.c_str(), "1.4"))
	{
		
		soundDeviceId+=directXdevicesCount;
	}
	int sampleRate = atoi(ini_config.GetValue("config", "sampleRate", "-1"));
	int latency = atoi(ini_config.GetValue("config", "desiredLatency", "-1"));



	if (soundDeviceId == -1)
	{
		if((soundDeviceId = Pa_GetDefaultOutputDevice())==-1)
			return;
	}


	const PaDeviceInfo* p = Pa_GetDeviceInfo(soundDeviceId);

	if (sampleRate == -1)
	{
		sampleRate = p->defaultSampleRate;
	}

	if (latency == -1)
	{
		latency = 1000 * p->defaultHighOutputLatency;
	}

	/* Open the sound device.. fallback to default one if there is a problem */
	if (selectSoundDevice(soundDeviceId, sampleRate, latency) < 0)
	{
		soundDeviceId = Pa_GetDefaultOutputDevice();

		/* No default sound device ? Give up instead of crashing */
		if (soundDeviceId <0)
			return;

		const PaDeviceInfo* p = Pa_GetDeviceInfo(soundDeviceId);
		sampleRate = p->defaultSampleRate;
		latency = 1000 * p->defaultHighOutputLatency;
		selectSoundDevice(soundDeviceId, sampleRate, latency);

	}
}
