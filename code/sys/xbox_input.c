/* Native Original Xbox XID controller service. */
#include "xbox_input.h"

#include <string.h>

#include <usb.h>
#include <usbh_lib.h>
#include <xid_driver.h>

#include "../qcommon/qcommon.h"

#define XBOX_CONTROLLER_PORTS 4

static xboxControllerState_t xboxControllers[XBOX_CONTROLLER_PORTS];
static qboolean xboxInputStarted;
static qboolean xboxInputStopping;

static xboxControllerState_t *Sys_XboxInputSlot(xid_dev_t *device)
{
	int i;

	if (device->user_data)
		return (xboxControllerState_t *)device->user_data;

	for (i = 0; i < XBOX_CONTROLLER_PORTS; ++i)
	{
		if (!xboxControllers[i].connected)
		{
			device->user_data = &xboxControllers[i];
			xboxControllers[i].connected = qtrue;
			return &xboxControllers[i];
		}
	}

	return NULL;
}

static void Sys_XboxInputReadComplete(UTR_T *transfer)
{
	xid_dev_t *device = transfer ? (xid_dev_t *)transfer->context : NULL;
	xboxControllerState_t *state;
	xid_gamepad_in report;

	if (!device || !transfer || transfer->status != USBH_OK ||
		transfer->xfer_len < sizeof(report))
		return;
	if (device->xid_desc.bType != XID_TYPE_GAMECONTROLLER)
		return;

	state = (xboxControllerState_t *)device->user_data;
	if (!state)
		return;

	memcpy(&report, transfer->buff, sizeof(report));
	state->buttons = report.dButtons;
	state->a = report.a;
	state->b = report.b;
	state->x = report.x;
	state->y = report.y;
	state->black = report.black;
	state->white = report.white;
	state->leftTrigger = report.l;
	state->rightTrigger = report.r;
	state->leftStickX = report.leftStickX;
	state->leftStickY = report.leftStickY;
	state->rightStickX = report.rightStickX;
	state->rightStickY = report.rightStickY;

	if (!xboxInputStopping)
	{
		/* nxdk's XID path reuses the completed interrupt transfer. */
		transfer->xfer_len = 0;
		transfer->bIsTransferDone = 0;
		(void)usbh_int_xfer(transfer);
	}
}

static void Sys_XboxInputConnected(xid_dev_t *device, int param)
{
	xboxControllerState_t *state;
	(void)param;

	if (!device || usbh_xid_get_type(device) == XID_UNKNOWN)
		return;
	state = Sys_XboxInputSlot(device);
	if (state)
		(void)usbh_xid_read(device, 0, (void *)Sys_XboxInputReadComplete);
}

static void Sys_XboxInputDisconnected(xid_dev_t *device, int param)
{
	xboxControllerState_t *state;
	(void)param;

	if (!device)
		return;
	state = (xboxControllerState_t *)device->user_data;
	if (state)
	{
		memset(state, 0, sizeof(*state));
		state->connected = qfalse;
	}
}

qboolean Sys_XboxInputInit(void)
{
	if (xboxInputStarted)
		return qtrue;

	memset(xboxControllers, 0, sizeof(xboxControllers));
	xboxInputStopping = qfalse;
	/* USB core initialization clears the driver table, so initialize it first. */
	usbh_core_init();
	usbh_xid_init();
	usbh_install_xid_conn_callback(Sys_XboxInputConnected,
		Sys_XboxInputDisconnected);

	/* Enumerate controllers that were already connected at startup. */
	for (int i = 0; i < 500; ++i)
	{
		usbh_pooling_hubs();
		Sys_Sleep(1);
	}

	xboxInputStarted = qtrue;
	return qtrue;
}

void Sys_XboxInputPoll(void)
{
	if (!xboxInputStarted)
		return;
	/* Required by nxdk for enumeration and hot-plug detection. */
	usbh_pooling_hubs();
}

void Sys_XboxInputShutdown(void)
{
	if (!xboxInputStarted)
		return;
	xboxInputStopping = qtrue;
	usbh_install_xid_conn_callback(NULL, NULL);
	usbh_core_deinit();
	memset(xboxControllers, 0, sizeof(xboxControllers));
	xboxInputStarted = qfalse;
}

const xboxControllerState_t *Sys_XboxInputState(int port)
{
	if (port < 0 || port >= XBOX_CONTROLLER_PORTS)
		return NULL;
	return &xboxControllers[port];
}
