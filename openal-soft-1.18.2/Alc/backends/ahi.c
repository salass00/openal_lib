/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "alMain.h"
#include "alu.h"

#include "backends/base.h"

#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>

static uint32 signal_pid_func(const struct Hook *hook, uint32 pid, struct Process *proc)
{
	uint32 sigmask = (uint32)hook->h_Data;
	BOOL   result  = FALSE;

	if (proc->pr_ProcessID == pid)
	{
		if (sigmask != 0)
			IExec->Signal(&proc->pr_Task, sigmask);

		result = TRUE;
	}

	return result;
}

static BOOL signal_pid(uint32 pid, uint32 sigmask)
{
	struct Hook hook;
	BOOL        result = FALSE;

	memset(&hook, 0, sizeof(hook));

	hook.h_Entry = (HOOKFUNC)signal_pid_func;
	hook.h_Data  = (APTR)sigmask;

	if (IDOS->ProcessScan(&hook, (APTR)pid, 0))
		result = TRUE;

	return result;
}

static const ALCchar ahi_device[] = "AHI Default";

typedef struct ALCplaybackAHI {
	DERIVE_FROM_TYPE(ALCbackend);

	uint32             ahi_fmt;
	struct MsgPort    *ahi_port;
	struct AHIRequest *ahi_req[2];

	ALubyte           *mix_data[2];
	uint32             data_size;

	uint32             proc_id;
} ALCplaybackAHI;

static int ALCplaybackAHI_mixerProc(void);
static void ALCplaybackAHI_Construct(ALCplaybackAHI *self, ALCdevice *device);
static DECLARE_FORWARD(ALCplaybackAHI, ALCbackend, void, Destruct)
static ALCenum ALCplaybackAHI_open(ALCplaybackAHI *self, const ALCchar *name);
static void ALCplaybackAHI_close(ALCplaybackAHI *self);
static ALCboolean ALCplaybackAHI_reset(ALCplaybackAHI *self);
static ALCboolean ALCplaybackAHI_start(ALCplaybackAHI *self);
static void ALCplaybackAHI_stop(ALCplaybackAHI *self);
static DECLARE_FORWARD2(ALCplaybackAHI, ALCbackend, ALCenum, captureSamples, ALCvoid*, ALCuint)
static DECLARE_FORWARD(ALCplaybackAHI, ALCbackend, ALCuint, availableSamples)
static DECLARE_FORWARD(ALCplaybackAHI, ALCbackend, ClockLatency, getClockLatency)
static DECLARE_FORWARD(ALCplaybackAHI, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCplaybackAHI, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCplaybackAHI)
DEFINE_ALCBACKEND_VTABLE(ALCplaybackAHI);

static int ALCplaybackAHI_mixerProc(void)
{
	struct Process    *me       = (struct Process *)IExec->FindTask(NULL);
	ALCplaybackAHI    *self     = me->pr_Task.tc_UserData;
	ALCdevice         *device   = STATIC_CAST(ALCbackend, self)->mDevice;
	int                db;
	uint32             data_size, frame_size;
	void              *buffer;
	struct AHIRequest *ahir, *link;

	/* Setup AHI port */
	self->ahi_port->mp_SigTask = &me->pr_Task;
	self->ahi_port->mp_SigBit  = IExec->AllocSignal(-1);
	self->ahi_port->mp_Flags   = PA_SIGNAL;

	frame_size = FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);

	link = NULL;
	db = 0;

	while (IDOS->CheckSignal(SIGBREAKF_CTRL_C) == 0 && device->Connected) {
		data_size = self->data_size;

		buffer = self->mix_data[db];
		ahir   = self->ahi_req[db];

		aluMixData(device, buffer, data_size / frame_size);

		ahir->ahir_Std.io_Command = CMD_WRITE;
		ahir->ahir_Std.io_Data    = buffer;
		ahir->ahir_Std.io_Length  = data_size;
		ahir->ahir_Frequency      = device->Frequency;
		ahir->ahir_Type           = self->ahi_fmt;
		ahir->ahir_Volume         = 0x10000;
		ahir->ahir_Position       = 0x8000;
		ahir->ahir_Link           = link;
	   	IExec->SendIO((struct IORequest*)ahir);

		if (link != NULL)
		{
			IExec->WaitIO((struct IORequest *)link);
		}

		link = ahir;
		db ^= 1;
	}

	if (link != NULL)
	{
		/* Should we AbortIO() as well here? */
		IExec->WaitIO((struct IORequest *)link);
	}

	/* Cleanup AHI port */
	self->ahi_port->mp_Flags   = PA_IGNORE;
	self->ahi_port->mp_SigTask = NULL;

	IExec->FreeSignal(self->ahi_port->mp_SigBit);
	self->ahi_port->mp_SigBit  = -1;

	return RETURN_OK;
}

static void ALCplaybackAHI_Construct(ALCplaybackAHI *self, ALCdevice *device)
{
	ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
	SET_VTABLE2(ALCplaybackAHI, ALCbackend, self);
}

static int get_ahi_format(ALCdevice *device)
{
	switch(ChannelsFromDevFmt(device->FmtChans, device->AmbiOrder))
	{
	case 1:
		switch(device->FmtType)
		{
		case DevFmtUByte:
			device->FmtType = DevFmtByte;
		case DevFmtByte:
			return AHIST_M8S;
		case DevFmtUShort:
			device->FmtType = DevFmtShort;
		case DevFmtShort:
			return AHIST_M16S;
		case DevFmtUInt:
		case DevFmtFloat:
			device->FmtType = DevFmtInt;
		case DevFmtInt:
			return AHIST_M32S;
		default:
			break;
		}
		break;
	case 2:
		switch(device->FmtType)
		{
		case DevFmtUByte:
			device->FmtType = DevFmtByte;
		case DevFmtByte:
			return AHIST_S8S;
		case DevFmtUShort:
			device->FmtType = DevFmtShort;
		case DevFmtShort:
			return AHIST_S16S;
		case DevFmtUInt:
		case DevFmtFloat:
			device->FmtType = DevFmtInt;
		case DevFmtInt:
			return AHIST_S32S;
		default:
			break;
		}
		break;
	default:
		break;
	}
	ERR("Unknown format?! chans: %d type: %d\n", device->FmtChans, device->FmtType);
	return -1;
}

static ALCenum ALCplaybackAHI_open(ALCplaybackAHI *self, const ALCchar *name)
{
	ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

	if(name != NULL && strcmp(name, AHINAME) != 0)
	{
		return ALC_INVALID_VALUE;
	}

	self->ahi_fmt = get_ahi_format(device);
	if(self->ahi_fmt == -1)
		return ALC_INVALID_VALUE;

	self->ahi_port = IExec->AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Action,   PA_IGNORE,
		ASOPORT_AllocSig, FALSE,
		TAG_END);

	self->ahi_req[0] = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
		ASOIOR_ReplyPort, self->ahi_port,
		ASOIOR_Size,	  sizeof(struct AHIRequest),
		TAG_END);
	if(self->ahi_req[0] == NULL)
	{
		return ALC_OUT_OF_MEMORY;
	}

	self->ahi_req[0]->ahir_Version = 4;
	if(IExec->OpenDevice(AHINAME, AHI_DEFAULT_UNIT, (struct IORequest *)self->ahi_req[0], 0)
		!= IOERR_SUCCESS)
	{
		return ALC_OUT_OF_MEMORY;
	}

	self->ahi_req[1] = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
		ASOIOR_Duplicate, self->ahi_req[0],
		TAG_END);
	if(self->ahi_req[1] == NULL)
	{
		return ALC_OUT_OF_MEMORY;
	}

	alstr_copy_cstr(&device->DeviceName, name);

	return ALC_NO_ERROR;
}

static void ALCplaybackAHI_close(ALCplaybackAHI *self)
{
	IExec->CloseDevice((struct IORequest*)self->ahi_req[0]);

	IExec->FreeSysObject(ASOT_IOREQUEST, self->ahi_req[1]);
	IExec->FreeSysObject(ASOT_IOREQUEST, self->ahi_req[0]);

	self->ahi_req[0] = NULL;
	self->ahi_req[1] = NULL;

	IExec->FreeSysObject(ASOT_PORT, self->ahi_port);

	self->ahi_port = NULL;

	self->ahi_fmt = -1;
}

static ALCboolean ALCplaybackAHI_reset(ALCplaybackAHI *self)
{
	ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

	self->ahi_fmt = get_ahi_format(device);
	if(self->ahi_fmt == -1)
		return ALC_FALSE;

	SetDefaultChannelOrder(device);

	return ALC_TRUE;
}

static ALCboolean ALCplaybackAHI_start(ALCplaybackAHI *self)
{
	ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

	self->data_size = device->UpdateSize * FrameSizeFromDevFmt(device->FmtChans, device->FmtType, device->AmbiOrder);

	self->mix_data[0] = IExec->AllocVecTags(self->data_size,
		AVT_Type, MEMF_SHARED,
		TAG_END);
	self->mix_data[1] = IExec->AllocVecTags(self->data_size,
		AVT_Type, MEMF_SHARED,
		TAG_END);
	if(self->mix_data[0] != NULL && self->mix_data[1] != NULL)
	{
		struct Process *proc;

		proc = IDOS->CreateNewProcTags(
			NP_Name,                   "OpenAL mixer process",
			NP_Entry,                  ALCplaybackAHI_mixerProc,
			NP_Priority,               5,
			NP_Child,                  TRUE,
			NP_UserData,               self,
			NP_CurrentDir,             ZERO,
			NP_Path,                   ZERO,
			NP_CopyVars,               FALSE,
			NP_Input,                  ZERO,
			NP_Output,                 ZERO,
			NP_Error,                  ZERO,
			NP_CloseInput,             FALSE,
			NP_CloseOutput,            FALSE,
			NP_CloseError,             FALSE,
			NP_NotifyOnDeathSigTask,   NULL,
			NP_NotifyOnDeathSignalBit, SIGB_CHILD,
			TAG_END);
		if (proc != NULL)
		{
			self->proc_id = IDOS->IoErr();

			return ALC_TRUE;
		}
	}

	IExec->FreeVec(self->mix_data[1]);
	IExec->FreeVec(self->mix_data[0]);

	self->mix_data[0] = NULL;
	self->mix_data[1] = NULL;

	return ALC_FALSE;
}

static void ALCplaybackAHI_stop(ALCplaybackAHI *self)
{
	if (self->proc_id == 0)
		return;

	while (signal_pid(self->proc_id, SIGBREAKF_CTRL_C))
		IExec->Wait(SIGF_CHILD);

	self->proc_id = 0;

	IExec->FreeVec(self->mix_data[1]);
	IExec->FreeVec(self->mix_data[0]);

	self->mix_data[0] = NULL;
	self->mix_data[1] = NULL;
}

typedef struct ALCahiBackendFactory {
	DERIVE_FROM_TYPE(ALCbackendFactory);
} ALCahiBackendFactory;
#define ALCAHIBACKENDFACTORY_INITIALIZER { { GET_VTABLE2(ALCahiBackendFactory, ALCbackendFactory) } }

ALCbackendFactory *ALCahiBackendFactory_getFactory(void);

static ALCboolean ALCahiBackendFactory_init(ALCahiBackendFactory *self);
static DECLARE_FORWARD(ALCahiBackendFactory, ALCbackendFactory, void, deinit)
static ALCboolean ALCahiBackendFactory_querySupport(ALCahiBackendFactory *self, ALCbackend_Type type);
static void ALCahiBackendFactory_probe(ALCahiBackendFactory *self, enum DevProbe type);
static ALCbackend* ALCahiBackendFactory_createBackend(ALCahiBackendFactory *self, ALCdevice *device, ALCbackend_Type type);
DEFINE_ALCBACKENDFACTORY_VTABLE(ALCahiBackendFactory);

ALCbackendFactory *ALCahiBackendFactory_getFactory(void)
{
	static ALCahiBackendFactory factory = ALCAHIBACKENDFACTORY_INITIALIZER;
	return STATIC_CAST(ALCbackendFactory, &factory);
}

ALCboolean ALCahiBackendFactory_init(ALCahiBackendFactory* UNUSED(self))
{
	return ALC_TRUE;
}

ALCboolean ALCahiBackendFactory_querySupport(ALCahiBackendFactory* UNUSED(self), ALCbackend_Type type)
{
	if(type == ALCbackend_Playback)
		return ALC_TRUE;
	return ALC_FALSE;
}

void ALCahiBackendFactory_probe(ALCahiBackendFactory* UNUSED(self), enum DevProbe type)
{
	switch(type)
	{
		case ALL_DEVICE_PROBE:
			AppendAllDevicesList(ahi_device);
			break;
		case CAPTURE_DEVICE_PROBE:
			break;
	}
}

ALCbackend* ALCahiBackendFactory_createBackend(ALCahiBackendFactory* UNUSED(self), ALCdevice *device, ALCbackend_Type type)
{
	if(type == ALCbackend_Playback)
	{
		ALCplaybackAHI *backend;

		backend = ALCplaybackAHI_New(sizeof(*backend));
		if(!backend) return NULL;
		memset(backend, 0, sizeof(*backend));

		ALCplaybackAHI_Construct(backend, device);

		return STATIC_CAST(ALCbackend, backend);
    }

    return NULL;
}

