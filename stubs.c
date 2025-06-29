#include <windows.h>
#include <stdio.h>
#include <ctype.h>
#include "player.h"
#include "globals.h"

#define dprintf(...) if (enableLogging && fh) { fprintf(fh, __VA_ARGS__); fflush(NULL); }

static float midiVol = 1.0;
static float waveVol = 1.0;
static int waveBits = -1;

static HINSTANCE realWinmmDLL = NULL;

void stub_midivol(int vol) { midiVol = vol < 0 || vol > 99  ? 1.0 : vol / 100.0; }
void stub_wavevol(int vol) { waveVol = vol < 0 || vol > 99  ? 1.0 : vol / 100.0; }

void unloadRealDLL()
{
	if (realWinmmDLL) {
		FreeLibrary(realWinmmDLL);
		realWinmmDLL = NULL;
	}
}

/* if winmm.dll is already loaded, return its handle */
/* otherwise, load it */
HINSTANCE loadRealDLL()
{
	if (!realWinmmDLL) {
		char winmm_path[MAX_PATH];

		GetSystemDirectory(winmm_path, MAX_PATH);
		strcat(winmm_path, "\\winmm.dll");

		realWinmmDLL = LoadLibrary(winmm_path);
	}
	return realWinmmDLL;
}

/* on Windows Vista and later, midiOutSetVolume is always tied to master volume */
MMRESULT WINAPI fake_midiStreamOut(HMIDISTRM a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamOut");

#ifdef MIDI_VELOCITY_SCALING
	if (midiVol != 1.0 && a1 && a1->lpData) {
		for (int i = 0, j = a1->dwBytesRecorded; i < j; i += sizeof(DWORD)*3) {
			MIDIEVENT *pe = (MIDIEVENT *)(a1->lpData + i);
			if (pe->dwEvent & MEVT_F_LONG) {
				/* DWORD padding is required */
				i += (MEVT_EVENTPARM(pe->dwEvent) + 3) & 0xFFFFFC;
			} else if (MEVT_EVENTTYPE(pe->dwEvent) == MEVT_SHORTMSG) {
				char *pv = (char *)&pe->dwEvent; 
				if (!(pe->dwEvent&0x80)) {
					/* running status */
					pv[1] *= midiVol;
				} else if ((pe->dwEvent&0xF0) < 0xB0) {
					/* new voice status */
					pv[2] *= midiVol;
				}
			}
		}
	}
#endif

	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutOpen(LPHWAVEOUT a0, UINT a1, LPCWAVEFORMATEX a2, DWORD a3, DWORD a4, DWORD a5)
{
	static MMRESULT(WINAPI *funcp)(LPHWAVEOUT a0, UINT a1, LPCWAVEFORMATEX a2, DWORD a3, DWORD a4, DWORD a5) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutOpen");

	/* FIXME: waveOutOpen can be called multiple times with different sample bits for sound mixing */
	/* However, it is almost always to be 16-bits; extremely rarely to be 8-bits or of a mixed-up */
	if (a2) waveBits = a2->wBitsPerSample;
	return (*funcp)(a0, a1, a2, a3, a4, a5);
}

/* on Windows Vista and later, waveOutSetVolume is always tied to master volume */
MMRESULT WINAPI fake_waveOutWrite(HWAVEOUT a0, LPWAVEHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, LPWAVEHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutWrite");

	/* let owr own OGG wave pass through */
	if ((waveVol != 1.0 || midiVol != 1.0 ) && a1 && a1->lpData && a1->dwUser != 0xCDDA7777) {
		/* Windows is f**ked up. MIDI synth driver converts MIDI to WAVE and then calls winmm.waveOutWrite!!! */
		void *addr = __builtin_return_address(0);
		char caller[MAX_PATH];
		MEMORY_BASIC_INFORMATION mbi;
		VirtualQuery(addr, &mbi, sizeof(MEMORY_BASIC_INFORMATION));
		GetModuleFileName(mbi.AllocationBase, caller, MAX_PATH);

		float vol = 1.0;
		char *pos = strrchr(caller, '\\');
		/* Mixer: msacm32.drv */
		if (strstr(pos, "wdmaud.drv")) vol = midiVol;
		else if (!strstr(pos, ".drv")) vol = waveVol;

		if (vol != 1.0) {
			short *wave16;
			char *wave8;
			switch (waveBits) {
				case 16:
					wave16 = (short *)a1->lpData;
					for (int i = 0, j = a1->dwBufferLength/2; i < j; i++) {
						wave16[i] *= vol;
					}
					break;
				case 8:
					wave8 = (char *)a1->lpData;
					for (int i = 0, j = a1->dwBufferLength; i < j; i++) {
						wave8[i] *= vol;
					}
					break;
				default:
					; /* WTF? */
			}
		}
	}

	return (*funcp)(a0, a1, a2);
}

MCIERROR WINAPI relay_mciSendCommandA(MCIDEVICEID a0, UINT a1, DWORD a2, DWORD a3)
{
	static MCIERROR(WINAPI *funcp)(MCIDEVICEID a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciSendCommandA");
	return (*funcp)(a0, a1, a2, a3);
}

MCIERROR WINAPI relay_mciSendStringA(LPCSTR a0, LPSTR a1, UINT a2, HWND a3)
{
	static MCIERROR(WINAPI *funcp)(LPCSTR a0, LPSTR a1, UINT a2, HWND a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciSendStringA");
	return (*funcp)(a0, a1, a2, a3);
}

/**/
/* stubs for functions to call from the real winmm.dll */
/**/

LRESULT WINAPI fake_CloseDriver(HDRVR a0, LONG a1, LONG a2)
{
	static LRESULT(WINAPI *funcp)(HDRVR a0, LONG a1, LONG a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "CloseDriver");
	return (*funcp)(a0, a1, a2);
}

HDRVR WINAPI fake_OpenDriver(LPCWSTR a0, LPCWSTR a1, LONG a2)
{
	static HDRVR(WINAPI *funcp)(LPCWSTR a0, LPCWSTR a1, LONG a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "OpenDriver");
	return (*funcp)(a0, a1, a2);
}

LRESULT WINAPI fake_SendDriverMessage(HDRVR a0, UINT a1, LONG a2, LONG a3)
{
	static LRESULT(WINAPI *funcp)(HDRVR a0, UINT a1, LONG a2, LONG a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "SendDriverMessage");
	return (*funcp)(a0, a1, a2, a3);
}

HMODULE WINAPI fake_DrvGetModuleHandle(HDRVR a0)
{
	static HMODULE(WINAPI *funcp)(HDRVR a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "DrvGetModuleHandle");
	return (*funcp)(a0);
}

HMODULE WINAPI fake_GetDriverModuleHandle(HDRVR a0)
{
	static HMODULE(WINAPI *funcp)(HDRVR a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "GetDriverModuleHandle");
	return (*funcp)(a0);
}

LRESULT WINAPI fake_DefDriverProc(DWORD a0, HDRVR a1, UINT a2, LPARAM a3, LPARAM a4)
{
	static LRESULT(WINAPI *funcp)(DWORD a0, HDRVR a1, UINT a2, LPARAM a3, LPARAM a4) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "DefDriverProc");
	return (*funcp)(a0, a1, a2, a3, a4);
}

UINT WINAPI fake_mmsystemGetVersion()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmsystemGetVersion");
	return (*funcp)();
}

BOOL WINAPI fake_sndPlaySoundA(LPCSTR a0, UINT a1)
{
	static BOOL(WINAPI *funcp)(LPCSTR a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "sndPlaySoundA");
	return (*funcp)(a0, a1);
}

BOOL WINAPI fake_sndPlaySoundW(LPCWSTR a0, UINT a1)
{
	static BOOL(WINAPI *funcp)(LPCWSTR a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "sndPlaySoundW");
	return (*funcp)(a0, a1);
}

BOOL WINAPI fake_PlaySound(LPCSTR a0, HMODULE a1, DWORD a2)
{
	static BOOL(WINAPI *funcp)(LPCSTR a0, HMODULE a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "PlaySound");
	return (*funcp)(a0, a1, a2);
}

BOOL WINAPI fake_PlaySoundA(LPCSTR a0, HMODULE a1, DWORD a2)
{
	static BOOL(WINAPI *funcp)(LPCSTR a0, HMODULE a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "PlaySoundA");
	return (*funcp)(a0, a1, a2);
}

BOOL WINAPI fake_PlaySoundW(LPCWSTR a0, HMODULE a1, DWORD a2)
{
	static BOOL(WINAPI *funcp)(LPCWSTR a0, HMODULE a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "PlaySoundW");
	return (*funcp)(a0, a1, a2);
}

UINT WINAPI fake_waveOutGetNumDevs()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetNumDevs");
	return (*funcp)();
}

MMRESULT WINAPI fake_waveOutGetDevCapsA(UINT a0, LPWAVEOUTCAPSA a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPWAVEOUTCAPSA a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetDevCapsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutGetDevCapsW(UINT a0, LPWAVEOUTCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPWAVEOUTCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutGetVolume(HWAVEOUT a0, PDWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, PDWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetVolume");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutSetVolume(HWAVEOUT a0, DWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, DWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutSetVolume");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutGetErrorTextA(MMRESULT a0, LPSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetErrorTextA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutGetErrorTextW(MMRESULT a0, LPWSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPWSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetErrorTextW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutClose(HWAVEOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutClose");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveOutPrepareHeader(HWAVEOUT a0, LPWAVEHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, LPWAVEHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutPrepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutUnprepareHeader(HWAVEOUT a0, LPWAVEHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, LPWAVEHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutUnprepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutPause(HWAVEOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutPause");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveOutRestart(HWAVEOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutRestart");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveOutReset(HWAVEOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutReset");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveOutBreakLoop(HWAVEOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutBreakLoop");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveOutGetPosition(HWAVEOUT a0, LPMMTIME a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, LPMMTIME a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetPosition");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveOutGetPitch(HWAVEOUT a0, PDWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, PDWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetPitch");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutSetPitch(HWAVEOUT a0, DWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, DWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutSetPitch");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutGetPlaybackRate(HWAVEOUT a0, PDWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, PDWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetPlaybackRate");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutSetPlaybackRate(HWAVEOUT a0, DWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, DWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutSetPlaybackRate");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutGetID(HWAVEOUT a0, LPUINT a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, LPUINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutGetID");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveOutMessage(HWAVEOUT a0, UINT a1, DWORD a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(HWAVEOUT a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveOutMessage");
	return (*funcp)(a0, a1, a2, a3);
}

UINT WINAPI fake_waveInGetNumDevs()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetNumDevs");
	return (*funcp)();
}

MMRESULT WINAPI fake_waveInGetDevCapsA(UINT a0, LPWAVEINCAPSA a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPWAVEINCAPSA a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetDevCapsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInGetDevCapsW(UINT a0, LPWAVEINCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPWAVEINCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInGetErrorTextA(MMRESULT a0, LPSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetErrorTextA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInGetErrorTextW(MMRESULT a0, LPWSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPWSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetErrorTextW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInOpen(LPHWAVEIN a0, UINT a1, LPCWAVEFORMATEX a2, DWORD a3, DWORD a4, DWORD a5)
{
	static MMRESULT(WINAPI *funcp)(LPHWAVEIN a0, UINT a1, LPCWAVEFORMATEX a2, DWORD a3, DWORD a4, DWORD a5) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInOpen");
	return (*funcp)(a0, a1, a2, a3, a4, a5);
}

MMRESULT WINAPI fake_waveInClose(HWAVEIN a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInClose");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveInPrepareHeader(HWAVEIN a0, LPWAVEHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0, LPWAVEHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInPrepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInUnprepareHeader(HWAVEIN a0, LPWAVEHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0, LPWAVEHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInUnprepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInAddBuffer(HWAVEIN a0, LPWAVEHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0, LPWAVEHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInAddBuffer");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInStart(HWAVEIN a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInStart");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveInStop(HWAVEIN a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInStop");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveInReset(HWAVEIN a0)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInReset");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_waveInGetPosition(HWAVEIN a0, LPMMTIME a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0, LPMMTIME a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetPosition");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_waveInGetID(HWAVEIN a0, LPUINT a1)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0, LPUINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInGetID");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_waveInMessage(HWAVEIN a0, UINT a1, DWORD a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(HWAVEIN a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "waveInMessage");
	return (*funcp)(a0, a1, a2, a3);
}

UINT WINAPI fake_midiOutGetNumDevs()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetNumDevs");
	return (*funcp)();
}

MMRESULT WINAPI fake_midiStreamOpen(LPHMIDISTRM a0, LPUINT a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5)
{
	static MMRESULT(WINAPI *funcp)(LPHMIDISTRM a0, LPUINT a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamOpen");
	return (*funcp)(a0, a1, a2, a3, a4, a5);
}

MMRESULT WINAPI fake_midiStreamClose(HMIDISTRM a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamClose");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiStreamProperty(HMIDISTRM a0, LPBYTE a1, DWORD a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0, LPBYTE a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamProperty");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiStreamPosition(HMIDISTRM a0, LPMMTIME a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0, LPMMTIME a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamPosition");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiStreamPause(HMIDISTRM a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamPause");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiStreamRestart(HMIDISTRM a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamRestart");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiStreamStop(HMIDISTRM a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDISTRM a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiStreamStop");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiConnect(HMIDI a0, HMIDIOUT a1, PVOID a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDI a0, HMIDIOUT a1, PVOID a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiConnect");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiDisconnect(HMIDI a0, HMIDIOUT a1, PVOID a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDI a0, HMIDIOUT a1, PVOID a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiDisconnect");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutGetDevCapsA(UINT a0, LPMIDIOUTCAPSA a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPMIDIOUTCAPSA a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetDevCapsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutGetDevCapsW(UINT a0, LPMIDIOUTCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPMIDIOUTCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutGetVolume(HMIDIOUT a0, PDWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, PDWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetVolume");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_midiOutSetVolume(HMIDIOUT a0, DWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, DWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutSetVolume");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_midiOutGetErrorTextA(MMRESULT a0, LPSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetErrorTextA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutGetErrorTextW(MMRESULT a0, LPWSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPWSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetErrorTextW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutOpen(LPHMIDIOUT a0, UINT a1, DWORD a2, DWORD a3, DWORD a4)
{
	static MMRESULT(WINAPI *funcp)(LPHMIDIOUT a0, UINT a1, DWORD a2, DWORD a3, DWORD a4) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutOpen");
	return (*funcp)(a0, a1, a2, a3, a4);
}

MMRESULT WINAPI fake_midiOutClose(HMIDIOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutClose");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiOutPrepareHeader(HMIDIOUT a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutPrepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutUnprepareHeader(HMIDIOUT a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutUnprepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutShortMsg(HMIDIOUT a0, DWORD a1)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, DWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutShortMsg");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_midiOutLongMsg(HMIDIOUT a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutLongMsg");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiOutReset(HMIDIOUT a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutReset");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiOutCachePatches(HMIDIOUT a0, UINT a1, LPWORD a2, UINT a3)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, UINT a1, LPWORD a2, UINT a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutCachePatches");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_midiOutCacheDrumPatches(HMIDIOUT a0, UINT a1, LPWORD a2, UINT a3)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, UINT a1, LPWORD a2, UINT a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutCacheDrumPatches");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_midiOutGetID(HMIDIOUT a0, LPUINT a1)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, LPUINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutGetID");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_midiOutMessage(HMIDIOUT a0, UINT a1, DWORD a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(HMIDIOUT a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiOutMessage");
	return (*funcp)(a0, a1, a2, a3);
}

UINT WINAPI fake_midiInGetNumDevs()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInGetNumDevs");
	return (*funcp)();
}

MMRESULT WINAPI fake_midiInGetDevCapsA(UINT a0, LPMIDIINCAPSA a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPMIDIINCAPSA a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInGetDevCapsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInGetDevCapsW(UINT a0, LPMIDIINCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPMIDIINCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInGetErrorTextA(MMRESULT a0, LPSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInGetErrorTextA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInGetErrorTextW(MMRESULT a0, LPWSTR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(MMRESULT a0, LPWSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInGetErrorTextW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInOpen(LPHMIDIIN a0, UINT a1, DWORD a2, DWORD a3, DWORD a4)
{
	static MMRESULT(WINAPI *funcp)(LPHMIDIIN a0, UINT a1, DWORD a2, DWORD a3, DWORD a4) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInOpen");
	return (*funcp)(a0, a1, a2, a3, a4);
}

MMRESULT WINAPI fake_midiInClose(HMIDIIN a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInClose");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiInPrepareHeader(HMIDIIN a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInPrepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInUnprepareHeader(HMIDIIN a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInUnprepareHeader");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInAddBuffer(HMIDIIN a0, LPMIDIHDR a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0, LPMIDIHDR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInAddBuffer");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_midiInStart(HMIDIIN a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInStart");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiInStop(HMIDIIN a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInStop");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiInReset(HMIDIIN a0)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInReset");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_midiInGetID(HMIDIIN a0, LPUINT a1)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0, LPUINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInGetID");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_midiInMessage(HMIDIIN a0, UINT a1, DWORD a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(HMIDIIN a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "midiInMessage");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_auxGetDevCapsW(UINT a0, LPAUXCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPAUXCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "auxGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_auxOutMessage(UINT a0, UINT a1, DWORD a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "auxOutMessage");
	return (*funcp)(a0, a1, a2, a3);
}

UINT WINAPI fake_mixerGetNumDevs()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetNumDevs");
	return (*funcp)();
}

MMRESULT WINAPI fake_mixerGetDevCapsA(UINT a0, LPMIXERCAPSA a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPMIXERCAPSA a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetDevCapsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerGetDevCapsW(UINT a0, LPMIXERCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPMIXERCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerOpen(LPHMIXER a0, UINT a1, DWORD a2, DWORD a3, DWORD a4)
{
	static MMRESULT(WINAPI *funcp)(LPHMIXER a0, UINT a1, DWORD a2, DWORD a3, DWORD a4) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerOpen");
	return (*funcp)(a0, a1, a2, a3, a4);
}

MMRESULT WINAPI fake_mixerClose(HMIXER a0)
{
	static MMRESULT(WINAPI *funcp)(HMIXER a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerClose");
	return (*funcp)(a0);
}

DWORD WINAPI fake_mixerMessage(HMIXER a0, UINT a1, DWORD a2, DWORD a3)
{
	static DWORD(WINAPI *funcp)(HMIXER a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerMessage");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_mixerGetLineInfoA(HMIXEROBJ a0, LPMIXERLINEA a1, DWORD a2)
{
	static MMRESULT(WINAPI *funcp)(HMIXEROBJ a0, LPMIXERLINEA a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetLineInfoA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerGetLineInfoW(HMIXEROBJ a0, LPMIXERLINEW a1, DWORD a2)
{
	static MMRESULT(WINAPI *funcp)(HMIXEROBJ a0, LPMIXERLINEW a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetLineInfoW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerGetID(HMIXEROBJ a0, PUINT a1, DWORD a2)
{
	static MMRESULT(WINAPI *funcp)(HMIXEROBJ a0, PUINT a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetID");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerGetLineControlsA(HMIXEROBJ a0, LPMIXERLINECONTROLSA a1, DWORD a2)
{
	static MMRESULT(WINAPI *funcp)(HMIXEROBJ a0, LPMIXERLINECONTROLSA a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetLineControlsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerGetLineControlsW(HMIXEROBJ a0, LPMIXERLINECONTROLSW a1, DWORD a2)
{
	static MMRESULT(WINAPI *funcp)(HMIXEROBJ a0, LPMIXERLINECONTROLSW a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mixerGetLineControlsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mixerGetControlDetailsA(HMIXEROBJ a0, LPMIXERCONTROLDETAILS a1, DWORD a2)
{
	LPMIXERCONTROLDETAILS_UNSIGNED pUnsigned = a1->paDetails;
	pUnsigned->dwValue = auxVol & 0xFFFF;
	dprintf("fake_mixerGetControlDetailsA, auxVol=%08X\n", auxVol)
	return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fake_mixerGetControlDetailsW(HMIXEROBJ a0, LPMIXERCONTROLDETAILS a1, DWORD a2)
{
	LPMIXERCONTROLDETAILS_UNSIGNED pUnsigned = a1->paDetails;
	pUnsigned->dwValue = auxVol & 0xFFFF;
	dprintf("fake_mixerGetControlDetailsW, auxVol=%08X\n", auxVol)
	return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fake_mixerSetControlDetails(HMIXEROBJ a0, LPMIXERCONTROLDETAILS a1, DWORD a2)
{
	LPMIXERCONTROLDETAILS_UNSIGNED pUnsigned = a1->paDetails;
	DWORD rawVolume = pUnsigned->dwValue;
	dprintf("fake_mixerSetControlDetails, rawVolume=%08X\n", rawVolume)
	auxVol = (rawVolume << 16) | rawVolume;
	plr_volume((auxVol & 0xFFFF) * cddaVol / 65535, (auxVol >> 16) * cddaVol / 65535);

	return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fake_timeGetSystemTime(LPMMTIME a0, UINT a1)
{
	static MMRESULT(WINAPI *funcp)(LPMMTIME a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeGetSystemTime");
	return (*funcp)(a0, a1);
}

DWORD WINAPI fake_timeGetTime()
{
	static DWORD(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeGetTime");
	return (*funcp)();
}

MMRESULT WINAPI fake_timeSetEvent(UINT a0, UINT a1, LPTIMECALLBACK a2, DWORD a3, UINT a4)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, UINT a1, LPTIMECALLBACK a2, DWORD a3, UINT a4) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeSetEvent");
	return (*funcp)(a0, a1, a2, a3, a4);
}

MMRESULT WINAPI fake_timeKillEvent(UINT a0)
{
	static MMRESULT(WINAPI *funcp)(UINT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeKillEvent");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_timeGetDevCaps(LPTIMECAPS a0, UINT a1)
{
	static MMRESULT(WINAPI *funcp)(LPTIMECAPS a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeGetDevCaps");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_timeBeginPeriod(UINT a0)
{
	static MMRESULT(WINAPI *funcp)(UINT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeBeginPeriod");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_timeEndPeriod(UINT a0)
{
	static MMRESULT(WINAPI *funcp)(UINT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "timeEndPeriod");
	return (*funcp)(a0);
}

UINT WINAPI fake_joyGetNumDevs()
{
	static UINT(WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyGetNumDevs");
	return (*funcp)();
}

MMRESULT WINAPI fake_joyGetDevCapsA(UINT a0, LPJOYCAPSA a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPJOYCAPSA a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyGetDevCapsA");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_joyGetDevCapsW(UINT a0, LPJOYCAPSW a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPJOYCAPSW a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyGetDevCapsW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_joyGetPos(UINT a0, LPJOYINFO a1)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPJOYINFO a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyGetPos");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_joyGetPosEx(UINT a0, LPJOYINFOEX a1)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPJOYINFOEX a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyGetPosEx");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_joyGetThreshold(UINT a0, LPUINT a1)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, LPUINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyGetThreshold");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_joyReleaseCapture(UINT a0)
{
	static MMRESULT(WINAPI *funcp)(UINT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyReleaseCapture");
	return (*funcp)(a0);
}

MMRESULT WINAPI fake_joySetCapture(HWND a0, UINT a1, UINT a2, BOOL a3)
{
	static MMRESULT(WINAPI *funcp)(HWND a0, UINT a1, UINT a2, BOOL a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joySetCapture");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_joySetThreshold(UINT a0, UINT a1)
{
	static MMRESULT(WINAPI *funcp)(UINT a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joySetThreshold");
	return (*funcp)(a0, a1);
}

FOURCC WINAPI fake_mmioStringToFOURCCA(LPCSTR a0, UINT a1)
{
	static FOURCC(WINAPI *funcp)(LPCSTR a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioStringToFOURCCA");
	return (*funcp)(a0, a1);
}

FOURCC WINAPI fake_mmioStringToFOURCCW(LPCWSTR a0, UINT a1)
{
	static FOURCC(WINAPI *funcp)(LPCWSTR a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioStringToFOURCCW");
	return (*funcp)(a0, a1);
}

LPMMIOPROC WINAPI fake_mmioInstallIOProcA(FOURCC a0, LPMMIOPROC a1, DWORD a2)
{
	static LPMMIOPROC(WINAPI *funcp)(FOURCC a0, LPMMIOPROC a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioInstallIOProcA");
	return (*funcp)(a0, a1, a2);
}

LPMMIOPROC WINAPI fake_mmioInstallIOProcW(FOURCC a0, LPMMIOPROC a1, DWORD a2)
{
	static LPMMIOPROC(WINAPI *funcp)(FOURCC a0, LPMMIOPROC a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioInstallIOProcW");
	return (*funcp)(a0, a1, a2);
}

HMMIO WINAPI fake_mmioOpenA(LPSTR a0, LPMMIOINFO a1, DWORD a2)
{
	static HMMIO(WINAPI *funcp)(LPSTR a0, LPMMIOINFO a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioOpenA");
	return (*funcp)(a0, a1, a2);
}

HMMIO WINAPI fake_mmioOpenW(LPWSTR a0, LPMMIOINFO a1, DWORD a2)
{
	static HMMIO(WINAPI *funcp)(LPWSTR a0, LPMMIOINFO a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioOpenW");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mmioRenameA(LPCSTR a0, LPCSTR a1, LPCMMIOINFO a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(LPCSTR a0, LPCSTR a1, LPCMMIOINFO a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioRenameA");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_mmioRenameW(LPCWSTR a0, LPCWSTR a1, LPCMMIOINFO a2, DWORD a3)
{
	static MMRESULT(WINAPI *funcp)(LPCWSTR a0, LPCWSTR a1, LPCMMIOINFO a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioRenameW");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_mmioClose(HMMIO a0, UINT a1)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioClose");
	return (*funcp)(a0, a1);
}

LONG WINAPI fake_mmioRead(HMMIO a0, HPSTR a1, LONG a2)
{
	static LONG(WINAPI *funcp)(HMMIO a0, HPSTR a1, LONG a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioRead");
	return (*funcp)(a0, a1, a2);
}

LONG WINAPI fake_mmioWrite(HMMIO a0, LPCSTR a1, LONG a2)
{
	static LONG(WINAPI *funcp)(HMMIO a0, LPCSTR a1, LONG a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioWrite");
	return (*funcp)(a0, a1, a2);
}

LONG WINAPI fake_mmioSeek(HMMIO a0, LONG a1, int a2)
{
	static LONG(WINAPI *funcp)(HMMIO a0, LONG a1, int a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioSeek");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mmioGetInfo(HMMIO a0, LPMMIOINFO a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPMMIOINFO a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioGetInfo");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mmioSetInfo(HMMIO a0, LPCMMIOINFO a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPCMMIOINFO a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioSetInfo");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mmioSetBuffer(HMMIO a0, LPSTR a1, LONG a2, UINT a3)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPSTR a1, LONG a2, UINT a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioSetBuffer");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_mmioFlush(HMMIO a0, UINT a1)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, UINT a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioFlush");
	return (*funcp)(a0, a1);
}

MMRESULT WINAPI fake_mmioAdvance(HMMIO a0, LPMMIOINFO a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPMMIOINFO a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioAdvance");
	return (*funcp)(a0, a1, a2);
}

LRESULT WINAPI fake_mmioSendMessage(HMMIO a0, UINT a1, LPARAM a2, LPARAM a3)
{
	static LRESULT(WINAPI *funcp)(HMMIO a0, UINT a1, LPARAM a2, LPARAM a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioSendMessage");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_mmioDescend(HMMIO a0, LPMMCKINFO a1, const MMCKINFO* a2, UINT a3)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPMMCKINFO a1, const MMCKINFO* a2, UINT a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioDescend");
	return (*funcp)(a0, a1, a2, a3);
}

MMRESULT WINAPI fake_mmioAscend(HMMIO a0, LPMMCKINFO a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPMMCKINFO a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioAscend");
	return (*funcp)(a0, a1, a2);
}

MMRESULT WINAPI fake_mmioCreateChunk(HMMIO a0, LPMMCKINFO a1, UINT a2)
{
	static MMRESULT(WINAPI *funcp)(HMMIO a0, LPMMCKINFO a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmioCreateChunk");
	return (*funcp)(a0, a1, a2);
}

MCIERROR WINAPI fake_mciSendCommandW(MCIDEVICEID a0, UINT a1, DWORD a2, DWORD a3)
{
	static MCIERROR(WINAPI *funcp)(MCIDEVICEID a0, UINT a1, DWORD a2, DWORD a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciSendCommandW");
	return (*funcp)(a0, a1, a2, a3);
}

MCIERROR WINAPI fake_mciSendStringW(LPCWSTR a0, LPWSTR a1, UINT a2, HWND a3)
{
	static MCIERROR(WINAPI *funcp)(LPCWSTR a0, LPWSTR a1, UINT a2, HWND a3) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciSendStringW");
	return (*funcp)(a0, a1, a2, a3);
}

MCIDEVICEID WINAPI fake_mciGetDeviceIDA(LPCSTR a0)
{
	static MCIDEVICEID(WINAPI *funcp)(LPCSTR a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetDeviceIDA");
	return (*funcp)(a0);
}

MCIDEVICEID WINAPI fake_mciGetDeviceIDW(LPCWSTR a0)
{
	static MCIDEVICEID(WINAPI *funcp)(LPCWSTR a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetDeviceIDW");
	return (*funcp)(a0);
}

MCIDEVICEID WINAPI fake_mciGetDeviceIDFromElementIDA(DWORD a0, LPCSTR a1)
{
	static MCIDEVICEID(WINAPI *funcp)(DWORD a0, LPCSTR a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetDeviceIDFromElementIDA");
	return (*funcp)(a0, a1);
}

MCIDEVICEID WINAPI fake_mciGetDeviceIDFromElementIDW(DWORD a0, LPCWSTR a1)
{
	static MCIDEVICEID(WINAPI *funcp)(DWORD a0, LPCWSTR a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetDeviceIDFromElementIDW");
	return (*funcp)(a0, a1);
}

BOOL WINAPI fake_mciGetErrorStringA(MCIERROR a0, LPSTR a1, UINT a2)
{
	static BOOL(WINAPI *funcp)(MCIERROR a0, LPSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetErrorStringA");
	return (*funcp)(a0, a1, a2);
}

BOOL WINAPI fake_mciGetErrorStringW(MCIERROR a0, LPWSTR a1, UINT a2)
{
	static BOOL(WINAPI *funcp)(MCIERROR a0, LPWSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetErrorStringW");
	return (*funcp)(a0, a1, a2);
}

BOOL WINAPI fake_mciSetYieldProc(MCIDEVICEID a0, YIELDPROC a1, DWORD a2)
{
	static BOOL(WINAPI *funcp)(MCIDEVICEID a0, YIELDPROC a1, DWORD a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciSetYieldProc");
	return (*funcp)(a0, a1, a2);
}

HTASK WINAPI fake_mciGetCreatorTask(MCIDEVICEID a0)
{
	static HTASK(WINAPI *funcp)(MCIDEVICEID a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetCreatorTask");
	return (*funcp)(a0);
}

YIELDPROC WINAPI fake_mciGetYieldProc(MCIDEVICEID a0, PDWORD a1)
{
	static YIELDPROC(WINAPI *funcp)(MCIDEVICEID a0, PDWORD a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetYieldProc");
	return (*funcp)(a0, a1);
}

BOOL WINAPI fake_mciExecute(LPCSTR a0)
{
	static BOOL(WINAPI *funcp)(LPCSTR a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciExecute");
	return (*funcp)(a0);
}

BOOL WINAPI fake_DriverCallback(DWORD a0, DWORD a1, HDRVR a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6)
{
	static BOOL(WINAPI *funcp)(DWORD a0, DWORD a1, HDRVR a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "DriverCallback");
	return (*funcp)(a0, a1, a2, a3, a4, a5, a6);
}

BOOL WINAPI fake_NotifyCallbackData(DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4)
{
	static BOOL(WINAPI *funcp)(DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "NotifyCallbackData");
	return (*funcp)(a0, a1, a2, a3, a4);
}

MMRESULT WINAPI fake_joyConfigChanged(DWORD a0)
{
	static MMRESULT (WINAPI *funcp)(DWORD a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "joyConfigChanged");
	return (*funcp)(a0);
}

BOOL WINAPI fake_mciFreeCommandResource(UINT a0)
{
	static BOOL (WINAPI *funcp)(UINT a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciFreeCommandResource");
	return (*funcp)(a0);
}

UINT WINAPI fake_mciLoadCommandResource(HANDLE a0, LPCWSTR a1, UINT a2)
{
	static UINT (WINAPI *funcp)(HANDLE a0, LPCWSTR a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciLoadCommandResource");
	return (*funcp)(a0, a1, a2);
}

DWORD WINAPI fake_mmGetCurrentTask()
{
	static DWORD (WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmGetCurrentTask");
	return (*funcp)();
}

void WINAPI fake_mmTaskBlock(DWORD a0)
{
	static void (WINAPI *funcp)(DWORD a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmTaskBlock");
	return (*funcp)(a0);
}

typedef VOID (*LPTASKCALLBACK)(DWORD dwInst);

UINT WINAPI fake_mmTaskCreate(LPTASKCALLBACK a0, HANDLE* a1, DWORD_PTR a2)
{
	static UINT (WINAPI *funcp)(LPTASKCALLBACK a0, HANDLE* a1, DWORD_PTR a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmTaskCreate");
	return (*funcp)(a0, a1, a2);
}

BOOL WINAPI fake_mmTaskSignal(DWORD a0)
{
	static BOOL (WINAPI *funcp)(DWORD a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmTaskSignal");
	return (*funcp)(a0);
}

void WINAPI fake_mmTaskYield()
{
	static void (WINAPI *funcp)() = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mmTaskYield");
	return (*funcp)();
}

BOOL WINAPI fake_mciDriverNotify(HWND a0, MCIDEVICEID a1, UINT a2)
{
	static BOOL (WINAPI *funcp)(HWND a0, MCIDEVICEID a1, UINT a2) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciDriverNotify");
	return (*funcp)(a0, a1, a2);
}

UINT WINAPI fake_mciDriverYield(MCIDEVICEID a0)
{
	static UINT (WINAPI *funcp)(MCIDEVICEID a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciDriverYield");
	return (*funcp)(a0);
}

DWORD_PTR WINAPI fake_mciGetDriverData(MCIDEVICEID a0)
{
	static DWORD_PTR (WINAPI *funcp)(MCIDEVICEID a0) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciGetDriverData");
	return (*funcp)(a0);
}

BOOL WINAPI fake_mciSetDriverData(MCIDEVICEID a0, DWORD_PTR a1)
{
	static BOOL (WINAPI *funcp)(MCIDEVICEID a0, DWORD_PTR a1) = NULL;
	if (funcp == NULL)
		funcp = (void*)GetProcAddress(loadRealDLL(), "mciSetDriverData");
	return (*funcp)(a0, a1);
}

/* FIXME: unhandled exports */
/* mmDrvInstall */
/* aux32Message */
/* mci32Message */
/* mid32Message */
/* mod32Message */
/* mxd32Message */
/* tid32Message */
/* wid32Message */
/* wod32Message */
