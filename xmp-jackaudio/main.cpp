/*
 * Copyright (C) 2016 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include "jack/jack.h"
#include "jack/ringbuffer.h"
#include "xmpout.h"

static int out_ringbuffers = 2;
static int out_channels = 0;
static int single_xmp_out_size = 0;
static int full_xmp_out_size = 0;
static int xmp_out_samples = 0;
static int jack_autoadjust = 1;

static volatile BOOL out_pause = false;
static volatile BOOL out_writing = false;
static volatile BOOL jack_running = false;
static volatile BOOL thread_running = false;
static volatile BOOL unfreezer = false;

static jack_client_t *out_client = NULL;
static jack_port_t *out_port[4] = { NULL };

static jack_default_audio_sample_t *jack_workbuf = NULL;
static jack_ringbuffer_t *out_buf[4] = { NULL };
static char serverName[64] = { '\0' };

static XMPFUNC_MISC *xmpfmisc = NULL;
static XMPFUNC_STATUS *xmpfstatus = NULL;
static XMPFUNC_REGISTRY *xmpfreg = NULL;

static CRITICAL_SECTION section;
static HANDLE playerEvent;
static HWND xmpwin;

static BOOL CALLBACK OUT_Config(HWND h, UINT m, WPARAM w, LPARAM l);
static const char* WINAPI OUT_Name(DWORD output);
static DWORD WINAPI OUT_GetFlags(DWORD output);
static BOOL WINAPI OUT_Open(DWORD output, XMPOUT_FORMAT *form, HANDLE event);
static void WINAPI OUT_Close();
static BOOL WINAPI OUT_Reset();
static BOOL WINAPI OUT_Pause(BOOL resume);
static DWORD WINAPI OUT_CanWrite();
static BOOL WINAPI OUT_Write(const void *buf, DWORD length);
static DWORD WINAPI OUT_GetBuffered();

static const char *client_uuid = "8db5fcd2-5140-11e6-beb8-9e71128cae77";
static const char *plugin_name = "JACK Audio";
static const char *default_server = "default";

//#define OutputDebugString __noop

static XMPOUT out = {
    0,
	plugin_name,
	OUT_Config,
    OUT_Name,
	OUT_GetFlags,
	OUT_Open,
    OUT_Close,
	OUT_Reset,
	OUT_Pause,
	NULL,
	OUT_CanWrite,
	OUT_Write,
	OUT_GetBuffered,
	NULL,
	NULL,
};

#define ITEM(id) GetDlgItem(h,id)
#define MESS(id,m,w,l) SendDlgItemMessage(h,id,m,(WPARAM)(w),(LPARAM)(l))

static void updateApplyButton(HWND h)
{
	char tmptext[64];
	memset(tmptext, 0, 64);
	MESS(10, WM_GETTEXT, 64, tmptext);
	if (MESS(11, TBM_GETPOS, 0, 0) != out_ringbuffers ||
		strcmp(tmptext, serverName) != 0 || !MESS(13, BM_GETCHECK, 0, 0)) {
		EnableWindow(ITEM(1000), TRUE); // enable "Apply" button
	}
	else
		EnableWindow(ITEM(1000), FALSE); // disable "Apply" button
}

// options page handler (dialog in resource 1000)
static BOOL CALLBACK OUT_Config(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m) {
		case WM_COMMAND:
		{
			switch (LOWORD(w)) {
				case 10: // server name switch
				case 11: // buffers switch
				case 13: // auto adjust switch
				{
					updateApplyButton(h);
				} break;
				case 14: // reset settings
				{
					memset(serverName, 0, 64);
					memcpy(serverName, default_server, strlen(default_server));
					MESS(10, WM_SETTEXT, 0, serverName);
					out_ringbuffers = 2;
					MESS(11, TBM_SETPOS, 1, 2);
					char pos[4];
					sprintf_s(pos, 4, "%i", out_ringbuffers);
					MESS(12, WM_SETTEXT, 0, pos);
					jack_autoadjust = TRUE;
					MESS(13, BM_SETCHECK, TRUE, 0);
					updateApplyButton(h);
				} break;
				case 1000: // Apply
				{
					DWORD serverNameSize = MESS(10, WM_GETTEXT, 64, serverName);
					if (serverNameSize == 0) //default setting
					{
						memset(serverName, 0, 64);
						memcpy(serverName, default_server, strlen(default_server));
						MESS(10, WM_SETTEXT, 0, serverName);
					}
					out_ringbuffers = MESS(11, TBM_GETPOS, 0, 0);
					jack_autoadjust = !!MESS(13, BM_GETCHECK, 0, 0);
					updateApplyButton(h);
				} break;
				default:
					break;
			}
		}
		return 1;

		case WM_NOTIFY:
		{
			case 11:
			{ //slider got moved, update text
				char pos[4];
				sprintf_s(pos, 4, "%i", MESS(11, TBM_GETPOS, 0, 0));
				MESS(12, WM_SETTEXT, 0, pos);
				updateApplyButton(h);
			}
			break;
		}
		return 1;

		case WM_SIZE:
		{ // move version number to bottom-right corner
			HWND v = ITEM(65534);
			RECT r;
			GetClientRect(v, &r);
			SetWindowPos(v, 0, LOWORD(l) - r.right - 2, HIWORD(l) - r.bottom, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
			updateApplyButton(h);
		}
		return 1;

		case WM_INITDIALOG:
		{ //prepare options page
			MESS(10, WM_SETTEXT, 0, serverName);
			MESS(11, TBM_SETRANGE, 1, MAKELONG(2, 8));
			MESS(11, TBM_SETPOS, 1, out_ringbuffers);
			char pos[2];
			sprintf_s(pos, 2, "%u", out_ringbuffers);
			MESS(12, WM_SETTEXT, 0, pos);
			MESS(13, BM_SETCHECK, !!jack_autoadjust, 0);
			updateApplyButton(h);
		}
		return 1;
	}
	return 0;
}

static const char* WINAPI OUT_Name(DWORD output)
{
	//we only have one output selectable
	if (output == 0)
		return plugin_name;
	return NULL;
}

static DWORD WINAPI OUT_GetFlags(DWORD output)
{
	//we dont do anything special
    return 0;
}

static int OUT_process(jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *out;
	int i = 0, j = 0;
	//size needed to fill this process
	int read_size = nframes * sizeof(jack_default_audio_sample_t);
	//check the last channel to make sure the previous ones are also filled
	int read_avail = jack_ringbuffer_read_space(out_buf[out_channels-1]);
	if (!jack_running || out_pause || (read_avail < read_size)) //paused or not a full buffer
	{
		//clear the port buffer so it plays silence
		for (i = 0; i < out_channels; i++)
		{
			out = (jack_default_audio_sample_t*)jack_port_get_buffer(out_port[i], nframes);
			memset(out, 0, read_size);
		}
	}
	else
	{
		//read out the data from the ring buffer
		for (i = 0; i < out_channels; i++)
		{
			out = (jack_default_audio_sample_t*)jack_port_get_buffer(out_port[i], nframes);
			jack_ringbuffer_read(out_buf[i], (char*)out, read_size);
		}
	}
	return 0;
}

static BOOL WINAPI OUT_Open(DWORD output, XMPOUT_FORMAT *form, HANDLE event)
{
	EnterCriticalSection(&section);
	//important for pausing to work
	playerEvent = event;
	//in case we have error messages
	xmpwin = xmpfmisc->GetWindow();
	jack_status_t status = (jack_status_t)0;
	out_client = jack_client_open("XMPlay", (jack_options_t)JackOpenOptions, &status, serverName, client_uuid);
	if (out_client == NULL)
	{
		char errorMessage[128];
		sprintf(errorMessage, "JACK Client could not be opened (%i) !", status);
		MessageBox(xmpwin, errorMessage, plugin_name, MB_OK);
		LeaveCriticalSection(&section);
		return false;
	}

	if (jack_autoadjust)
	{
		form->form.rate = jack_get_sample_rate(out_client);
		form->form.res = 4;
	}
	else
	{
		if (jack_get_sample_rate(out_client) != form->form.rate)
		{
			MessageBox(xmpwin, "The selected Samplerate does not match the JACK Samplerate!", plugin_name, MB_OK);
			jack_client_close(out_client);
			LeaveCriticalSection(&section);
			return false;
		}
		//res 4 = 32 bit
		if (form->form.res != 4)
		{
			MessageBox(xmpwin, "The Audio Resolution has to be 32 bit!", plugin_name, MB_OK);
			jack_client_close(out_client);
			LeaveCriticalSection(&section);
			return false;
		}
	}
	out_channels = form->form.chan;
	//a lot of plugins only work with the size you set
	//in the xmplay output options, so use that as base
	xmp_out_samples = form->buffer;
	single_xmp_out_size = xmp_out_samples * sizeof(jack_default_audio_sample_t);
	full_xmp_out_size = single_xmp_out_size * out_channels;
	if (jack_workbuf)
		xmpfmisc->Free(jack_workbuf);
	//use xmplay alloc to get workbuf
	jack_workbuf = (jack_default_audio_sample_t*)xmpfmisc->Alloc(
		sizeof(jack_default_audio_sample_t)*xmp_out_samples);
	if (jack_workbuf == NULL)
	{
		MessageBox(xmpwin, "Failed to allocate required work buffer!", plugin_name, MB_OK);
		jack_client_close(out_client);
		LeaveCriticalSection(&section);
		return false;
	}
	int i;
	//set up requested channels
	for (i = 0; i < out_channels; i++)
	{
		//simple naming pattern
		char outName[10];
		sprintf(outName, "out%i", i+1);
		out_port[i] = jack_port_register(out_client, outName,
			JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		if (out_port[i] == NULL)
		{
			MessageBox(xmpwin, "Failed to register a new Port for this JACK Client!", plugin_name, MB_OK);
			jack_client_close(out_client);
			xmpfmisc->Free(jack_workbuf);
			jack_workbuf = NULL;
			LeaveCriticalSection(&section);
			return false;
		}
	}
	for (i = 0; i < out_channels; i++)
	{
		if (out_buf[i])
			jack_ringbuffer_free(out_buf[i]);
		//use a simple ringbuffer from jack directly
		out_buf[i] = jack_ringbuffer_create(single_xmp_out_size * out_ringbuffers);
		if (out_buf[i] == NULL)
		{
			MessageBox(xmpwin, "Failed to allocate required Ring Buffer!", plugin_name, MB_OK);
			jack_client_close(out_client);
			xmpfmisc->Free(jack_workbuf);
			jack_workbuf = NULL;
			LeaveCriticalSection(&section);
			return false;
		}
		//I dont think we need this in windows but its in the samples so...
		jack_ringbuffer_mlock(out_buf[i]);
	}
	//setup our runtime variables
	out_pause = false;
	jack_running = true;
	out_writing = false;
	unfreezer = false;
	//make sure we have the process callback set to do anything
	jack_set_process_callback(out_client, OUT_process, 0);
	jack_activate(out_client);
	LeaveCriticalSection(&section);
	return true;
}

static void WINAPI OUT_Close()
{
	EnterCriticalSection(&section);
	//make sure threads get done by forcing them
	jack_running = false;
	out_pause = false;
	LeaveCriticalSection(&section);
	//just in case write is still going
	while (out_writing) Sleep(1);
	EnterCriticalSection(&section);
	//stop transfer, stops process thread
	jack_deactivate(out_client);
	int i;
	for (i = 0; i < out_channels; i++)
	{
		//not sure if needed since deactivate already does this
		jack_port_unregister(out_client, out_port[i]);
		out_port[i] = NULL;
		//should be safe to do this
		jack_ringbuffer_free(out_buf[i]);
		out_buf[i] = NULL;
	}
	//close our connection entirely
	jack_client_close(out_client);
	if (jack_workbuf)
		xmpfmisc->Free(jack_workbuf);
	jack_workbuf = NULL;
	LeaveCriticalSection(&section);
}

static BOOL WINAPI OUT_Reset()
{
	EnterCriticalSection(&section);
	int i;
	for (i = 0; i < out_channels; i++) {
		//should clear out remaining audio buffer
		if (out_buf[i])
			jack_ringbuffer_reset(out_buf[i]);
	}
	LeaveCriticalSection(&section);
	return true;
}

static BOOL WINAPI OUT_Pause(BOOL resume)
{
	EnterCriticalSection(&section);
	//I'd rather save pause state ;)
	out_pause = !resume;
	LeaveCriticalSection(&section);
	return true;
}

static DWORD WINAPI OUT_CanWrite()
{
	//return static value, should be good enough
	return full_xmp_out_size;
}

static BOOL WINAPI OUT_Write(const void *buf, DWORD length)
{
	EnterCriticalSection(&section);
	//make sure to set this so other threads know
	out_writing = true;
	int i, j;
	//this is the length in bytes per channel
	const int single_length = length / out_channels;
	//this is the length of actual samples per channel
	const int total_input = single_length / sizeof(jack_default_audio_sample_t);
	for (i = 0; i < out_channels; i++)
	{
		//this little loop splits up the channels in buf
		const jack_default_audio_sample_t *in = (const jack_default_audio_sample_t*)buf;
		for (j = 0; j < total_input; j++)
			jack_workbuf[j] = in[i + (out_channels*j)];
		//after a channel got separated we can write it into the ringbuffer
		jack_ringbuffer_write(out_buf[i], (char*)jack_workbuf, single_length);
	}
	LeaveCriticalSection(&section);
	//we just wait for the last channel, no need to wait for each individual one
	while (jack_running && jack_ringbuffer_write_space(out_buf[out_channels - 1]) < single_xmp_out_size)
		Sleep(1); //one millisecond should be quick enough at all times
	EnterCriticalSection(&section);
	//make sure sure to tell xmplay we are done
	if (unfreezer) {
		unfreezer = false;
		//got in event, hand one out
		SetEvent(playerEvent);
	}
	//done writing, tell others
	out_writing = false;
	LeaveCriticalSection(&section);
	return jack_running;
}

static DWORD WINAPI OUT_GetBuffered()
{
	EnterCriticalSection(&section);
	//jack quit already but still go called
	if (!jack_running) {
		LeaveCriticalSection(&section);
		return 0;
	}
	//returns the total of all channels
	int total = 0, i;
	for (i = 0; i < out_channels; i++)
		total += jack_ringbuffer_read_space(out_buf[i]);
	LeaveCriticalSection(&section);
	return total;
}

// get the plugin's XMPOUT interface
XMPOUT *WINAPI XMPOUT_GetInterface(DWORD face, InterfaceProc faceproc)
{
	if (face != XMPOUT_FACE) return NULL;
	xmpfmisc = (XMPFUNC_MISC*)faceproc(XMPFUNC_MISC_FACE); // import "misc" functions
	xmpfreg = (XMPFUNC_REGISTRY*)faceproc(XMPFUNC_REGISTRY_FACE);

	// get our config from the internal ini file
	xmpfreg->GetInt(plugin_name, "Buffers", &out_ringbuffers);
	out_ringbuffers = max(2, min(8, out_ringbuffers));

	// use config server name if possible ,else use default one
	memset(serverName, 0, 64);
	DWORD serverNameSize = xmpfreg->GetString(plugin_name, "ServerName", NULL, 0);
	if (serverNameSize) {
		xmpfreg->GetString(plugin_name, "ServerName", serverName, serverNameSize+1);
	}
	else {
		memcpy(serverName, default_server, strlen(default_server));
	}

	xmpfreg->GetInt(plugin_name, "AutoAdjust", &jack_autoadjust);
	jack_autoadjust = !!jack_autoadjust;
	return &out;
}

//just to "properly handle" pause events
static DWORD WINAPI EventThread(_In_ LPVOID lpParameter)
{
	while (thread_running) {
		//thread idling for commands
		EnterCriticalSection(&section);
		if (!jack_running || !out_pause) {
			LeaveCriticalSection(&section);
			Sleep(1);
			continue;
		}
		LeaveCriticalSection(&section);
		//we are paused so lets wait for unpause/stop
		DWORD ret = WaitForSingleObject(playerEvent, INFINITE);
		EnterCriticalSection(&section);
		//possibly quit faster than we catched it
		if (!thread_running) {
			LeaveCriticalSection(&section);
			break;
		}
		//false alarm, just unpaused
		if (!out_pause) {
			LeaveCriticalSection(&section);
			Sleep(1);
			continue;
		}
		if (out_writing) {
			//probably stopped after pausing, unfreeze
			unfreezer = true;
			jack_running = false;
			out_pause = false;
		}
		LeaveCriticalSection(&section);
	}
	return 0;
}

static HANDLE eventThreadHandle;

BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hDLL);
		InitializeCriticalSection(&section);
		thread_running = true;
		//keep a thread around to update us
		eventThreadHandle = CreateThread(0, 0, EventThread, 0, 0, NULL);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		if (xmpfreg) { // store config
			out_ringbuffers = max(2,min(8, out_ringbuffers));
			xmpfreg->SetInt(plugin_name, "Buffers", &out_ringbuffers);
			if (serverName[0] != '\0') {
				xmpfreg->SetString(plugin_name, "ServerName", serverName);
			}
			jack_autoadjust = !!jack_autoadjust;
			xmpfreg->SetInt(plugin_name, "AutoAdjust", &jack_autoadjust);
		}
		EnterCriticalSection(&section);
		//make sure the thread is gone
		thread_running = false;
		LeaveCriticalSection(&section);
		//give it a bit to be all finished up
		WaitForSingleObject(eventThreadHandle, 2000);
		//close out the rest
		CloseHandle(eventThreadHandle);
		DeleteCriticalSection(&section);
	}
    return 1;
}
