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
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "backends/ahi.h"

#include <cstring>
#include <cstdio>

#include "alexcpt.h"
#include "alu.h"
#include "logging.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <devices/ahi.h>

namespace {

constexpr char DefaultName[] = "AHI Default";
std::string DefaultPlayback{"ahi.device:0"};

extern "C" ULONG ALCahiSignalPID_func(struct Hook *hook, ULONG pid, struct Process *process)
{
    if (process->pr_ProcessID != pid)
        return false;

    IExec->Signal(&process->pr_Task, (ULONG)hook->h_Data);
    return true;
}

bool ALCahiSignalPID(ULONG pid, ULONG signals)
{
    struct Hook hook;
    
    std::memset(&hook, 0, sizeof(struct Hook));
    hook.h_Entry = (HOOKFUNC)ALCahiSignalPID_func;
    hook.h_Data = (APTR)signals;
    
    if (IDOS->ProcessScan(&hook, (APTR)pid, 0))
        return true;
        
    return false;
}

struct AHIPlayback final : public BackendBase {
    AHIPlayback(ALCdevice *device) noexcept : BackendBase{device} { }
    ~AHIPlayback() override;
    
    int mixerProc();
    
    void open(const ALCchar *name) override;
    bool reset() override;
    bool start() override;
    void stop() override;

    al::vector<ALubyte> mMixData[2];
    
    struct MsgPort *mAhiPort{NULL};
    struct AHIRequest *mAhir[2]{NULL,NULL};
    ULONG mAhiFmt{AHIST_NOTYPE};
    ULONG mProcessID{0};
    
    DEF_NEWDEL(AHIPlayback)
};

AHIPlayback::~AHIPlayback()
{
    std::fprintf(stderr, "AHIPlayback::~AHIPlayback\n");

    if (mAhir[0])
    {
		if (mAhir[0]->ahir_Std.io_Device)
		{
		    IExec->CloseDevice((struct IORequest *)mAhir[0]);
		}
        IExec->FreeSysObject(ASOT_IOREQUEST, mAhir[0]);
        mAhir[0] = NULL;
    }
    if (mAhir[1])
    {
        IExec->FreeSysObject(ASOT_IOREQUEST, mAhir[1]);
        mAhir[1] = NULL;
    }

    if (mAhiPort)
    {
        IExec->FreeSysObject(ASOT_PORT, mAhiPort);
        mAhiPort = NULL;
    }
}

int AHIPlayback::mixerProc()
{
    APTR buffer;
    struct AHIRequest *ahir, *link{NULL};
    int db{0};

    std::fprintf(stderr, "AHIPlayback::mixerProc\n");

    mAhiPort->mp_SigTask = IExec->FindTask(NULL);
    mAhiPort->mp_SigBit = IExec->AllocSignal(-1);
    mAhiPort->mp_Flags = PA_SIGNAL;
    
    while (!IDOS->CheckSignal(SIGBREAKF_CTRL_C) &&
           mDevice->Connected.load(std::memory_order_acquire))
    {
        buffer = mMixData[db].data();
        ahir = mAhir[db];
        
        aluMixData(mDevice, buffer, mDevice->UpdateSize, mDevice->channelsFromFmt());
        
        ahir->ahir_Std.io_Command = CMD_WRITE;
        ahir->ahir_Std.io_Data = buffer;
        ahir->ahir_Std.io_Length = mDevice->UpdateSize * mDevice->frameSizeFromFmt();
        ahir->ahir_Frequency = mDevice->Frequency;
        ahir->ahir_Type = mAhiFmt;
        ahir->ahir_Volume = 0x10000UL;
        ahir->ahir_Position = 0x8000UL;
        ahir->ahir_Link = link;
        IExec->SendIO((struct IORequest *)ahir);
        
        if (link)
        {
            IExec->WaitIO((struct IORequest *)link);
        }
    
        link = ahir;
        db ^= 1;
    }
    
    if (link)
    {
        IExec->WaitIO((struct IORequest *)link);
    }
    
    mAhiPort->mp_Flags = PA_IGNORE;
    mAhiPort->mp_SigTask = NULL;
    IExec->FreeSignal(mAhiPort->mp_SigBit);
    mAhiPort->mp_SigBit = -1;

    return 0;
}

void AHIPlayback::open(const ALCchar *name)
{
    std::fprintf(stderr, "AHIPlayback::open\n");

    if (name)
    {
        throw al::backend_exception{ALC_INVALID_VALUE, "Device name \"%s\" not found", name};
    }
    else
    {
        name = DefaultName;
    }

	mAhiPort = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT,
	    ASOPORT_Action, PA_IGNORE,
	    ASOPORT_AllocSig, FALSE,
	    TAG_END);
	if (mAhiPort == NULL)
    {
        throw al::backend_exception{ALC_OUT_OF_MEMORY, "Out of memory"};
    }

    mAhir[0] = (struct AHIRequest *)IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, mAhiPort,
        ASOIOR_Size, sizeof(struct AHIRequest),
        TAG_END);
    if (mAhir[0] == NULL)
    {
        throw al::backend_exception{ALC_OUT_OF_MEMORY, "Out of memory"};
    }
    
    mAhir[0]->ahir_Version = 4;
    if (IExec->OpenDevice(AHINAME, AHI_DEFAULT_UNIT, (struct IORequest *)mAhir[0], 0)
        != IOERR_SUCCESS)
    {
        mAhir[0]->ahir_Std.io_Device = NULL;
        throw al::backend_exception{ALC_INVALID_VALUE, "Could not open \"%s\" unit %d", AHINAME, 0};
    }
    
    mAhir[1] = (struct AHIRequest *)IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_Duplicate, mAhir[0],
        TAG_END);
    if (mAhir[1] == NULL)
    {
        throw al::backend_exception{ALC_OUT_OF_MEMORY, "Out of memory"};
    }

    mDevice->DeviceName = name;
}

bool AHIPlayback::reset()
{
    std::fprintf(stderr, "AHIPlayback::reset\n");

    switch (mDevice->channelsFromFmt())
    {
        case 1:  // mono
            switch (mDevice->FmtType)
            {
                case DevFmtByte:
                    mAhiFmt = AHIST_M8S;
                    break;
                case DevFmtShort:
                    mAhiFmt = AHIST_M16S;
                    break;
                case DevFmtInt:
                    mAhiFmt = AHIST_M32S;
                    break;
                default:
                    mAhiFmt = AHIST_NOTYPE;
                    break;
            }
            break;
        case 2:  // stereo
            switch (mDevice->FmtType)
            {
                case DevFmtByte:
                    mAhiFmt = AHIST_S8S;
                    break;
                case DevFmtShort:
                    mAhiFmt = AHIST_S16S;
                    break;
                case DevFmtInt:
                    mAhiFmt = AHIST_S32S;
                    break;
                default:
                    mAhiFmt = AHIST_NOTYPE;
                    break;
            }
            break;
        default: // surround?
            mAhiFmt = AHIST_NOTYPE;
            break;
    }
    
    if (mAhiFmt == AHIST_NOTYPE)
    {
        ERR("Unsupported format type: %u channels: %u", mDevice->FmtType, mDevice->channelsFromFmt());
        return false;
    }
    
    SetDefaultChannelOrder(mDevice);
    
    mMixData[0].resize(mDevice->UpdateSize * mDevice->frameSizeFromFmt());
    mMixData[1].resize(mDevice->UpdateSize * mDevice->frameSizeFromFmt());
    
    return true;
}

extern "C" int ALCahiMixerProc(void)
{
    struct AHIPlayback *playback = (struct AHIPlayback *)IExec->FindTask(NULL)->tc_UserData;
    
    return playback->mixerProc();
}

bool AHIPlayback::start()
{
    struct Process *process;
    
    std::fprintf(stderr, "AHIPlayback::start\n");

    if (mProcessID != 0)
    {
        ERR("Playback process already running");
        return false;
    }
    
    process = IDOS->CreateNewProcTags(
        NP_Name, MIXER_THREAD_NAME,
        NP_Entry, ALCahiMixerProc,
        NP_Priority, 5,
        NP_Child, TRUE,
        NP_UserData, this,
        NP_CurrentDir, ZERO,
        NP_Path, ZERO,
        NP_CopyVars, FALSE,
        NP_Input, ZERO,
        NP_Output, ZERO,
        NP_Error, ZERO,
        NP_CloseInput, FALSE,
        NP_CloseOutput, FALSE,
        NP_CloseError, FALSE,
        TAG_END);
    if (process == NULL)
    {
        ERR("Could not create playback process");
        return false;
    }

    mProcessID = (ULONG)IDOS->IoErr();
    return true;
}

void AHIPlayback::stop()
{
    std::fprintf(stderr, "AHIPlayback::stop\n");

    if (mProcessID != 0)
    {
        while (ALCahiSignalPID(mProcessID, SIGBREAKF_CTRL_C))
            IDOS->Delay(1);
        
        mProcessID = 0;
    }
}

} // namespace

BackendFactory &AHIBackendFactory::getFactory()
{
    static AHIBackendFactory factory{};
    
    std::fprintf(stderr, "AHIBackendFactory::getFactory\n");
    
    return factory;
}

bool AHIBackendFactory::init()
{
    std::fprintf(stderr, "AHIBackendFactory::init\n");

    return true;
}

bool AHIBackendFactory::querySupport(BackendType type)
{
    std::fprintf(stderr, "AHIBackendFactory::querySupport\n");

	return (type == BackendType::Playback);
}

void AHIBackendFactory::probe(DevProbe type, std::string *outnames)
{
    std::fprintf(stderr, "AHIBackendFactory::probe\n");
    /* No-op */
}

BackendPtr AHIBackendFactory::createBackend(ALCdevice *device, BackendType type)
{
    std::fprintf(stderr, "AHIBackendFactory::createBackend\n");

    if (type == BackendType::Playback)
        return BackendPtr{new AHIPlayback{device}};
    return nullptr;
}

