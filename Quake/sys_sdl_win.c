/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include "quakedef.h"
#include "rt_pkz.h"

#include <sys/types.h>
#include <errno.h>
#include <io.h>
#include <direct.h>
#include <time.h>

// ---- crash log: on an unhandled exception, write exception info + a raw
// stack trace to crash.log next to the executable, then let Windows show
// its own error dialog. Helps debugging hard crashes without a debugger.

static char cwd[1024];

static LONG WINAPI Sys_CrashLogFilter (EXCEPTION_POINTERS *ep)
{
	EXCEPTION_RECORD *er = ep ? ep->ExceptionRecord : NULL;
	char             logpath[MAX_OSPATH];
	FILE            *f;

	q_snprintf (logpath, sizeof (logpath), "%s/crash.log", cwd);

	f = fopen (logpath, "w");
	if (!f)
		return EXCEPTION_CONTINUE_SEARCH;

	fprintf (f, "Quake crash log\n");
	{
		time_t t = time (NULL);
		struct tm tmv;
		if (localtime_s (&tmv, &t) == 0)
			fprintf (f, "time: %04d-%02d-%02d %02d:%02d:%02d\n",
			         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
			         tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	}

	if (er)
	{
		fprintf (f, "exception code: 0x%08X\n", (unsigned)er->ExceptionCode);
		fprintf (f, "exception address: %p\n", er->ExceptionAddress);
		if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
			fprintf (f, "access violation %s address %p\n",
			         er->ExceptionInformation[0] ? "writing" : "reading",
			         (void *)er->ExceptionInformation[1]);
	}
	else
	{
		fprintf (f, "exception info unavailable\n");
	}

	void   *frames[64];
	USHORT  n = CaptureStackBackTrace (0, 64, frames, NULL);
	fprintf (f, "stack (%u frames):\n", (unsigned)n);
	for (USHORT i = 0; i < n; i++)
	{
		HMODULE hmod = NULL;
		if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        (LPCSTR)frames[i], &hmod) &&
		    hmod)
		{
			char modname[MAX_OSPATH];
			if (GetModuleFileNameA (hmod, modname, sizeof (modname)))
			{
				const char *base = strrchr (modname, '\\');
				base = base ? base + 1 : modname;
				fprintf (f, "  %2u: %s+0x%08X\n", i, base,
				         (unsigned)((uintptr_t)frames[i] - (uintptr_t)hmod));
				continue;
			}
		}
		fprintf (f, "  %2u: %p\n", i, frames[i]);
	}

	fclose (f);
	return EXCEPTION_CONTINUE_SEARCH;
}

qboolean isDedicated;

static HANDLE hinput, houtput;

#define MAX_HANDLES 32 /* johnfitz -- was 10 */
static FILE *sys_handles[MAX_HANDLES];

static double counter_freq;

static int findhandle (void)
{
	int i;

	for (i = 1; i < MAX_HANDLES; i++)
	{
		if (!sys_handles[i])
			return i;
	}
	Sys_Error ("out of handles");
	return -1;
}

long Sys_filelength (FILE *f)
{
	long pos, end;

	pos = ftell (f);
	fseek (f, 0, SEEK_END);
	end = ftell (f);
	fseek (f, pos, SEEK_SET);

	return end;
}

int Sys_FileOpenRead (const char *path, int *hndl)
{
	FILE *f;
	int   i, retval;

	i = findhandle ();
	f = fopen (path, "rb");

	if (!f)
	{
		*hndl = -1;
		retval = -1;
	}
	else
	{
		sys_handles[i] = f;
		*hndl = i;
		retval = Sys_filelength (f);
	}

	return retval;
}

int Sys_FileOpenWrite (const char *path)
{
	FILE *f;
	int   i;

	i = findhandle ();
	f = fopen (path, "wb");

	if (!f)
		Sys_Error ("Error opening %s: %s", path, strerror (errno));

	sys_handles[i] = f;
	return i;
}

void Sys_FileClose (int handle)
{
	if (RT_PKZ_IsHandle (handle))
	{
		RT_PKZ_Close (handle);
		return;
	}
	fclose (sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek (int handle, int position)
{
	if (RT_PKZ_IsHandle (handle))
	{
		RT_PKZ_Seek (handle, position);
		return;
	}
	fseek (sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	if (RT_PKZ_IsHandle (handle))
		return RT_PKZ_Read (handle, dest, count);
	return fread (dest, 1, count, sys_handles[handle]);
}

int Sys_FileWrite (int handle, const void *data, int count)
{
	return fwrite (data, 1, count, sys_handles[handle]);
}

int Sys_FileTime (const char *path)
{
	FILE *f;

	f = fopen (path, "rb");

	if (f)
	{
		fclose (f);
		return 1;
	}

	return -1;
}

static void Sys_GetBasedir (char *argv0, char *dst, size_t dstsize)
{
	char  *tmp;
	size_t rc;

	rc = GetCurrentDirectory (dstsize, dst);
	if (rc == 0 || rc > dstsize)
		Sys_Error ("Couldn't determine current directory");

	tmp = dst;
	while (*tmp != 0)
		tmp++;
	while (*tmp == 0 && tmp != dst)
	{
		--tmp;
		if (tmp != dst && (*tmp == '/' || *tmp == '\\'))
			*tmp = 0;
	}
}

typedef enum
{
	dpi_unaware = 0,
	dpi_system_aware = 1,
	dpi_monitor_aware = 2
} dpi_awareness;
typedef BOOL (WINAPI *SetProcessDPIAwareFunc) ();
typedef HRESULT (WINAPI *SetProcessDPIAwarenessFunc) (dpi_awareness value);

static void Sys_SetDPIAware (void)
{
	HMODULE                    hUser32, hShcore;
	SetProcessDPIAwarenessFunc setDPIAwareness;
	SetProcessDPIAwareFunc     setDPIAware;

	/* Neither SDL 1.2 nor SDL 2.0.3 can handle the OS scaling our window.
	  (e.g. https://bugzilla.libsdl.org/show_bug.cgi?id=2713)
	  Call SetProcessDpiAwareness/SetProcessDPIAware to opt out of scaling.
	*/

	hShcore = LoadLibraryA ("Shcore.dll");
	hUser32 = LoadLibraryA ("user32.dll");
	setDPIAwareness = (SetProcessDPIAwarenessFunc)(hShcore ? GetProcAddress (hShcore, "SetProcessDpiAwareness") : NULL);
	setDPIAware = (SetProcessDPIAwareFunc)(hUser32 ? GetProcAddress (hUser32, "SetProcessDPIAware") : NULL);

	if (setDPIAwareness) /* Windows 8.1+ */
		setDPIAwareness (dpi_monitor_aware);
	else if (setDPIAware) /* Windows Vista-8.0 */
		setDPIAware ();

	if (hShcore)
		FreeLibrary (hShcore);
	if (hUser32)
		FreeLibrary (hUser32);
}

static void Sys_SetTimerResolution (void)
{
	/* Set OS timer resolution to 1ms.
	   Works around buffer underruns with directsound and SDL2, but also
	   will make Sleep()/SDL_Dleay() accurate to 1ms which should help framerate
	   stability.
	*/
	timeBeginPeriod (1);
}

void Sys_Init (void)
{
	SetUnhandledExceptionFilter (Sys_CrashLogFilter); // write crash.log on hard crashes

	Sys_SetTimerResolution ();
	Sys_SetDPIAware ();

	memset (cwd, 0, sizeof (cwd));
	Sys_GetBasedir (NULL, cwd, sizeof (cwd));
	host_parms->basedir = cwd;

	/* userdirs not really necessary for windows guys.
	 * can be done if necessary, though... */
	host_parms->userdir = host_parms->basedir; /* code elsewhere relies on this ! */

	SYSTEM_INFO info;
	GetSystemInfo (&info);
	host_parms->numcpus = info.dwNumberOfProcessors;
	if (host_parms->numcpus < 1)
	{
		host_parms->numcpus = 1;
	}
	Sys_Printf ("Detected %d CPUs.\n", host_parms->numcpus);

	if (isDedicated)
	{
		if (!AllocConsole ())
		{
			isDedicated = false; /* so that we have a graphical error dialog */
			Sys_Error ("Couldn't create dedicated server console");
		}

		hinput = GetStdHandle (STD_INPUT_HANDLE);
		houtput = GetStdHandle (STD_OUTPUT_HANDLE);
	}

	counter_freq = (double)SDL_GetPerformanceFrequency ();
}

void Sys_mkdir (const char *path)
{
	if (CreateDirectory (path, NULL) != 0)
		return;
	if (GetLastError () != ERROR_ALREADY_EXISTS)
		Sys_Error ("Unable to create directory %s", path);
}

static const char errortxt1[] = "\nERROR-OUT BEGIN\n\n";
static const char errortxt2[] = "\nQUAKE ERROR: ";

void Sys_Error (const char *error, ...)
{
	va_list argptr;
	char    text[1024];
	DWORD   dummy;

	host_parms->errstate++;

	va_start (argptr, error);
	q_vsnprintf (text, sizeof (text), error, argptr);
	va_end (argptr);

	PR_SwitchQCVM (NULL);

	if (isDedicated)
		WriteFile (houtput, errortxt1, strlen (errortxt1), &dummy, NULL);
	/* SDL will put these into its own stderr log,
	   so print to stderr even in graphical mode. */
	fputs (errortxt1, stderr);
	fputs (errortxt2, stderr);
	fputs (text, stderr);
	fputs ("\n\n", stderr);
	if (!isDedicated)
		PL_ErrorDialog (text);
	else
	{
		WriteFile (houtput, errortxt2, strlen (errortxt2), &dummy, NULL);
		WriteFile (houtput, text, strlen (text), &dummy, NULL);
		WriteFile (houtput, "\r\n", 2, &dummy, NULL);
		SDL_Delay (3000); /* show the console 3 more seconds */
	}

#ifdef _DEBUG
	__debugbreak ();
#endif

	exit (1);
}

void Sys_Printf (const char *fmt, ...)
{
	va_list argptr;
	char    text[1024];
	DWORD   dummy;

	va_start (argptr, fmt);
	q_vsnprintf (text, sizeof (text), fmt, argptr);
	va_end (argptr);

	if (isDedicated)
	{
		WriteFile (houtput, text, strlen (text), &dummy, NULL);
	}
	else
	{
		/* SDL will put these into its own stdout log,
		   so print to stdout even in graphical mode. */
		fputs (text, stdout);
		OutputDebugStringA (text);
	}
}

void Sys_Quit (void)
{
	Host_Shutdown ();

	if (isDedicated)
		FreeConsole ();

	exit (0);
}

double Sys_DoubleTime (void)
{
	return (double)SDL_GetPerformanceCounter () / counter_freq;
}

const char *Sys_ConsoleInput (void)
{
	static char  con_text[256];
	static int   textlen;
	INPUT_RECORD recs[1024];
	int          ch;
	DWORD        dummy, numread, numevents;

	for (;;)
	{
		if (GetNumberOfConsoleInputEvents (hinput, &numevents) == 0)
			Sys_Error ("Error getting # of console events");

		if (!numevents)
			break;

		if (ReadConsoleInput (hinput, recs, 1, &numread) == 0)
			Sys_Error ("Error reading console input");

		if (numread != 1)
			Sys_Error ("Couldn't read console input");

		if (recs[0].EventType == KEY_EVENT)
		{
			if (recs[0].Event.KeyEvent.bKeyDown == TRUE)
			{
				ch = recs[0].Event.KeyEvent.uChar.AsciiChar;
				if (ch && recs[0].Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED)
				{
					BYTE keyboard[256] = {0};
					WORD   output;
					keyboard[SHIFT_PRESSED] = 0x80;
					if (ToAscii (VkKeyScan (ch), 0, keyboard, &output, 0) == 1)
						ch = output;
				}

				switch (ch)
				{
				case '\r':
					WriteFile (houtput, "\r\n", 2, &dummy, NULL);

					if (textlen != 0)
					{
						con_text[textlen] = 0;
						textlen = 0;
						return con_text;
					}

					break;

				case '\b':
					WriteFile (houtput, "\b \b", 3, &dummy, NULL);
					if (textlen != 0)
						textlen--;

					break;

				default:
					if (ch >= ' ')
					{
						WriteFile (houtput, &ch, 1, &dummy, NULL);
						con_text[textlen] = ch;
						textlen = (textlen + 1) & 0xff;
					}

					break;
				}
			}
		}
	}

	return NULL;
}

void Sys_Sleep (unsigned long msecs)
{
	/*	Sleep (msecs);*/
	SDL_Delay (msecs);
}

void Sys_SendKeyEvents (void)
{
	IN_Commands (); // ericw -- allow joysticks to add keys so they can be used to confirm SCR_ModalMessage
	IN_SendKeyEvents ();
}
