/******************************************************************************
*
* Copyright (C) 2019-2020 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*
*
*
******************************************************************************/
#include "xpm_defs.h"
#include "xplmi_dma.h"
#include "xpm_regs.h"
#include "xpm_device.h"
#include "xpm_mem.h"
#include "xpm_rpucore.h"

#define XPM_TCM_BASEADDRESS_MODE_OFFSET	0x80000U

#define XPM_NODEIDX_DEV_DDRMC_MIN	XPM_NODEIDX_DEV_DDRMC_0
#define XPM_NODEIDX_DEV_DDRMC_MAX	XPM_NODEIDX_DEV_DDRMC_3
#define DDRMC_TIMEOUT			300U


static const XPm_StateCap XPmDDRDeviceStates[] = {
	{
		.State = XPM_DEVSTATE_UNUSED,
		.Cap = XPM_MIN_CAPABILITY,
	}, {
		.State = XPM_DEVSTATE_RUNTIME_SUSPEND,
		.Cap = PM_CAP_CONTEXT,
	}, {
		.State = XPM_DEVSTATE_RUNNING,
		.Cap = XPM_MAX_CAPABILITY | PM_CAP_UNUSABLE,
	},
};

static const XPm_StateTran XPmDDRDevTransitions[] = {
	{
		.FromState = XPM_DEVSTATE_RUNNING,
		.ToState = XPM_DEVSTATE_UNUSED,
		.Latency = XPM_DEF_LATENCY,
	}, {
		.FromState = XPM_DEVSTATE_UNUSED,
		.ToState = XPM_DEVSTATE_RUNNING,
		.Latency = XPM_DEF_LATENCY,
	}, {
		.FromState = XPM_DEVSTATE_RUNTIME_SUSPEND,
		.ToState = XPM_DEVSTATE_RUNNING,
		.Latency = XPM_DEF_LATENCY,
	}, {
		.FromState = XPM_DEVSTATE_RUNNING,
		.ToState = XPM_DEVSTATE_RUNTIME_SUSPEND,
		.Latency = XPM_DEF_LATENCY,
	},
};

static XStatus XPmDDRDevice_EnterSelfRefresh(void)
{
	XStatus Status = XST_FAILURE;
	XPm_Device *Device;
	u32 BaseAddress;
	u32 Reg;
	u8 i;

	for (i = XPM_NODEIDX_DEV_DDRMC_MIN; i <= XPM_NODEIDX_DEV_DDRMC_MAX;
	     i++) {
		Device = XPmDevice_GetById(DDRMC_DEVID(i));
		BaseAddress = Device->Node.BaseAddress;

		/* Unlock DDRMC UB */
		Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
		XPm_Out32(Reg, NPI_PCSR_UNLOCK_VAL);

		/* Enable self-refresh */
		Reg = BaseAddress + DDRMC_UB_PMC2UB_INTERRUPT_OFFSET;
		XPm_Out32(Reg, DDRMC_UB_PMC2UB_INTERRUPT_SPARE_0_MASK);
		Reg = BaseAddress + DDRMC_UB_UB2PMC_ACK_OFFSET;
		Status = XPm_PollForMask(Reg, DDRMC_UB_UB2PMC_ACK_SPARE_0_MASK,
					DDRMC_TIMEOUT);
		if (XPM_NODEIDX_DEV_DDRMC_MIN == i && XST_SUCCESS != Status) {
			PmErr("Failed to enter self-refresh!\r\n");
			Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
			XPm_Out32(Reg, 0);
			goto done;
		}
		XPm_Out32(Reg, 0);

		Reg = BaseAddress + DDRMC_UB_UB2PMC_DONE_OFFSET;
		Status = XPm_PollForMask(Reg, DDRMC_UB_UB2PMC_DONE_SPARE_0_MASK,
					DDRMC_TIMEOUT);
		if (XPM_NODEIDX_DEV_DDRMC_MIN == i && XST_SUCCESS != Status) {
			PmErr("Failed to enter self-refresh!\r\n");
			Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
			XPm_Out32(Reg, 0);
			goto done;
		}
		XPm_Out32(Reg, 0);

		Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
		XPm_Out32(Reg, 0);
	}

	Status = XST_SUCCESS;

done:
	return Status;
}

static XStatus XPmDDRDevice_ExitSelfRefresh(void)
{
	XStatus Status = XST_FAILURE;
	XPm_Device *Device;
	u32 BaseAddress;
	u32 Reg;
	u8 i;

	for (i = XPM_NODEIDX_DEV_DDRMC_MIN; i <= XPM_NODEIDX_DEV_DDRMC_MAX;
	     i++) {
		Device = XPmDevice_GetById(DDRMC_DEVID(i));
		BaseAddress = Device->Node.BaseAddress;

		/* Unlock DDRMC UB */
		Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
		XPm_Out32(Reg, NPI_PCSR_UNLOCK_VAL);

		/* Disable self-refresh */
		Reg = BaseAddress + DDRMC_UB_PMC2UB_INTERRUPT_OFFSET;
		XPm_Out32(Reg, DDRMC_UB_PMC2UB_INTERRUPT_SR_EXIT_MASK);
		Reg = BaseAddress + DDRMC_UB_UB2PMC_ACK_OFFSET;
		Status = XPm_PollForMask(Reg, DDRMC_UB_UB2PMC_ACK_SR_EXIT_MASK,
					DDRMC_TIMEOUT);
		if (XPM_NODEIDX_DEV_DDRMC_MIN == i && XST_SUCCESS != Status) {
			PmErr("Failed to exit self-refresh!\r\n");
			Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
			XPm_Out32(Reg, 0);
			goto done;
		}
		XPm_Out32(Reg, 0);

		Reg = BaseAddress + DDRMC_UB_UB2PMC_DONE_OFFSET;
		Status = XPm_PollForMask(Reg, DDRMC_UB_UB2PMC_DONE_SR_EXIT_MASK,
					DDRMC_TIMEOUT);
		if (XPM_NODEIDX_DEV_DDRMC_MIN == i && XST_SUCCESS != Status) {
			PmErr("Failed to exit self-refresh!\r\n");
			Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
			XPm_Out32(Reg, 0);
			goto done;
		}
		XPm_Out32(Reg, 0);

		Reg = BaseAddress + NPI_PCSR_LOCK_OFFSET;
		XPm_Out32(Reg, 0);
	}

	Status = XST_SUCCESS;

done:
	return Status;
}

static XStatus HandleDDRDeviceState(XPm_Device* const Device, const u32 NextState)
{
	XStatus Status = XST_FAILURE;

	switch (Device->Node.State) {
	case XPM_DEVSTATE_UNUSED:
		if (XPM_DEVSTATE_RUNNING == NextState) {
			Status = XPmDevice_BringUp(Device);
		} else {
			Status = XST_SUCCESS;
		}
		break;
	case XPM_DEVSTATE_RUNNING:
		if (XPM_DEVSTATE_UNUSED == NextState) {
			Status = Device->HandleEvent(&Device->Node,
						     XPM_DEVEVENT_SHUTDOWN);
		} else {
			Status = XST_SUCCESS;
		}
		if (XPM_DEVSTATE_RUNTIME_SUSPEND == NextState) {
			Status = XPmDDRDevice_EnterSelfRefresh();
		}
		break;
	case XPM_DEVSTATE_RUNTIME_SUSPEND:
		if (XPM_DEVSTATE_RUNNING == NextState) {
			Status = XPmDDRDevice_ExitSelfRefresh();
		}
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

	return Status;
}

static const XPm_DeviceFsm XPmDDRDeviceFsm = {
	DEFINE_DEV_STATES(XPmDDRDeviceStates),
	DEFINE_DEV_TRANS(XPmDDRDevTransitions),
	.EnterState = HandleDDRDeviceState,
};

static const XPm_StateCap XPmMemDeviceStates[] = {
	{
		.State = XPM_DEVSTATE_UNUSED,
		.Cap = XPM_MIN_CAPABILITY,
	}, {
		.State = XPM_DEVSTATE_RUNNING,
		.Cap = PM_CAP_ACCESS | PM_CAP_CONTEXT,
	},
};

static const XPm_StateTran XPmMemDevTransitions[] = {
	{
		.FromState = XPM_DEVSTATE_RUNNING,
		.ToState = XPM_DEVSTATE_UNUSED,
		.Latency = XPM_DEF_LATENCY,
	}, {
		.FromState = XPM_DEVSTATE_UNUSED,
		.ToState = XPM_DEVSTATE_RUNNING,
		.Latency = XPM_DEF_LATENCY,
	},
};

static void TcmEccInit(XPm_MemDevice *Tcm, u32 Mode)
{
	u32 Size = Tcm->EndAddress - Tcm->StartAddress;
	u32 Id = Tcm->Device.Node.Id;
	u32 Base = Tcm->StartAddress;

	if (PM_DEV_TCM_1_A == Id || PM_DEV_TCM_1_B == Id) {
		if (XPM_RPU_MODE_LOCKSTEP == Mode)
			Base -= XPM_TCM_BASEADDRESS_MODE_OFFSET;
	}
	if (Size) {
		XPlmi_EccInit(Base, Size);
	}
	return;
}

static XStatus HandleTcmDeviceState(XPm_Device* Device, u32 NextState)
{
	XStatus Status = XST_FAILURE;
	XPm_Device *Rpu0Device = XPmDevice_GetById(PM_DEV_RPU0_0);
	XPm_Device *Rpu1Device = XPmDevice_GetById(PM_DEV_RPU0_1);
	u32 Id = Device->Node.Id;
	u32 Mode;

	switch (Device->Node.State) {
	case XPM_DEVSTATE_UNUSED:
		if (XPM_DEVSTATE_RUNNING == NextState) {
			Status = XPmDevice_BringUp(Device);
			if (XST_SUCCESS != Status) {
				goto done;
			}
			/* TCM is only accessible when the RPU is powered on and out of reset and is in halted state
			 * so bring up RPU too when TCM is requested*/
			XPm_RpuGetOperMode(PM_DEV_RPU0_0, &Mode);
			if (XPM_RPU_MODE_SPLIT == Mode) {
				if ((PM_DEV_TCM_0_A == Id ||
				     PM_DEV_TCM_0_B == Id) &&
				    (XPM_DEVSTATE_RUNNING !=
				     Rpu0Device->Node.State)) {
					Status = XPmRpuCore_Halt(Rpu0Device);
					if (XST_SUCCESS != Status) {
						goto done;
					}
				}
				if ((PM_DEV_TCM_1_A == Id ||
				     PM_DEV_TCM_1_B == Id) &&
				    (XPM_DEVSTATE_RUNNING !=
				     Rpu1Device->Node.State)) {
					Status = XPmRpuCore_Halt(Rpu1Device);
					if (XST_SUCCESS != Status) {
						goto done;
					}
				}
			}
			if (XPM_RPU_MODE_LOCKSTEP == Mode)
			{
				if ((PM_DEV_TCM_0_A == Id ||
				     PM_DEV_TCM_0_B == Id ||
				     PM_DEV_TCM_1_A == Id ||
				     PM_DEV_TCM_1_B == Id) &&
				     (XPM_DEVSTATE_RUNNING !=
				      Rpu0Device->Node.State)) {
					Status = XPmRpuCore_Halt(Rpu0Device);
					if (XST_SUCCESS != Status) {
						goto done;
					}
				}
			}
			/* Tcm should be ecc initialized */
			TcmEccInit((XPm_MemDevice *)Device, Mode);
		}
		Status = XST_SUCCESS;
		break;
	case XPM_DEVSTATE_RUNNING:
		if (XPM_DEVSTATE_UNUSED == NextState) {
			Status = Device->HandleEvent(&Device->Node,
						     XPM_DEVEVENT_SHUTDOWN);
			if (XST_SUCCESS != Status) {
				goto done;
			}
		}
		Status = XST_SUCCESS;
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

done:
	return Status;
}

static const XPm_DeviceFsm XPmTcmDeviceFsm = {
	DEFINE_DEV_STATES(XPmMemDeviceStates),
	DEFINE_DEV_TRANS(XPmMemDevTransitions),
	.EnterState = HandleTcmDeviceState,
};

static XStatus HandleMemDeviceState(XPm_Device* const Device, const u32 NextState)
{
	XStatus Status = XST_FAILURE;

	switch (Device->Node.State) {
	case XPM_DEVSTATE_UNUSED:
		if (XPM_DEVSTATE_RUNNING == NextState) {
			Status = XPmDevice_BringUp(Device);
		} else {
			Status = XST_SUCCESS;
		}
		break;
	case XPM_DEVSTATE_RUNNING:
		if (XPM_DEVSTATE_UNUSED == NextState) {
			Status = Device->HandleEvent(&Device->Node,
						     XPM_DEVEVENT_SHUTDOWN);
		} else {
			Status = XST_SUCCESS;
		}
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

	return Status;
}

static const XPm_DeviceFsm XPmMemDeviceFsm = {
	DEFINE_DEV_STATES(XPmMemDeviceStates),
	DEFINE_DEV_TRANS(XPmMemDevTransitions),
	.EnterState = HandleMemDeviceState,
};

XStatus XPmMemDevice_Init(XPm_MemDevice *MemDevice,
		u32 Id,
		u32 BaseAddress,
		XPm_Power *Power, XPm_ClockNode *Clock, XPm_ResetNode *Reset, u32 MemStartAddress,
		u32 MemEndAddress)
{
	XStatus Status = XST_FAILURE;
	u32 Type = NODETYPE(Id);

	Status = XPmDevice_Init(&MemDevice->Device, Id, BaseAddress, Power, Clock,
				Reset);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	MemDevice->StartAddress = MemStartAddress;
	MemDevice->EndAddress = MemEndAddress;

	switch (Type) {
	case XPM_NODETYPE_DEV_DDR:
		MemDevice->Device.DeviceFsm = &XPmDDRDeviceFsm;
		break;
	case XPM_NODETYPE_DEV_TCM:
		MemDevice->Device.DeviceFsm = &XPmTcmDeviceFsm;
		break;
	default:
		MemDevice->Device.DeviceFsm = &XPmMemDeviceFsm;
		break;
	}

done:
	return Status;
}
