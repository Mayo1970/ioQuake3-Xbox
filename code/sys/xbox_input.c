/* Native Original Xbox XID controller service. */
#include "xbox_input.h"

#include <string.h>

#include <usb.h>
#include <usbh_lib.h>
#include <xid_driver.h>

#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"

#define XBOX_CONTROLLER_PORTS 4

static xboxControllerState_t xboxControllers[XBOX_CONTROLLER_PORTS];
static xboxControllerState_t xboxPreviousControllers[XBOX_CONTROLLER_PORTS];
static qboolean xboxInputStarted;
static qboolean xboxInputStopping;

#define XBOX_DPAD_UP        0x0001
#define XBOX_DPAD_DOWN      0x0002
#define XBOX_DPAD_LEFT      0x0004
#define XBOX_DPAD_RIGHT     0x0008
#define XBOX_START          0x0010
#define XBOX_BACK           0x0020
#define XBOX_LEFT_THUMB     0x0040
#define XBOX_RIGHT_THUMB    0x0080

typedef struct
{
	uint16_t mask;
	int key;
} xboxButtonBinding_t;

static const xboxButtonBinding_t xboxButtonBindings[] =
{
	{ XBOX_DPAD_UP,     K_UPARROW },
	{ XBOX_DPAD_DOWN,   K_DOWNARROW },
	{ XBOX_DPAD_LEFT,   K_LEFTARROW },
	{ XBOX_DPAD_RIGHT,  K_RIGHTARROW },
	{ XBOX_START,       K_ENTER },
	{ XBOX_BACK,        K_ESCAPE },
	{ XBOX_LEFT_THUMB,  K_JOY7 },
	{ XBOX_RIGHT_THUMB, K_JOY8 }
};

static int Sys_XboxInputAxis(int value)
{
	value /= 256;
	if (value < -127)
		return -127;
	if (value > 127)
		return 127;
	return value;
}

static int Sys_XboxInputTrigger(int value)
{
	return Sys_XboxInputAxis((value * 256) - (128 * 256));
}

static void Sys_XboxInputKey(int time, int key, qboolean down)
{
	Com_QueueEvent(time, SE_KEY, key, down, 0, NULL);
}

static void Sys_XboxInputAxisEvent(int time, int axis, int value)
{
	Com_QueueEvent(time, SE_JOYSTICK_AXIS, axis, value, 0, NULL);
}

static void Sys_XboxInputButton(int time, uint16_t oldButtons,
	uint16_t newButtons, uint16_t mask, int key)
{
	qboolean oldDown = (oldButtons & mask) != 0;
	qboolean newDown = (newButtons & mask) != 0;

	if (oldDown != newDown)
		Sys_XboxInputKey(time, key, newDown);
}

static void Sys_XboxInputByteButton(int time, uint8_t oldValue,
	uint8_t newValue, int key)
{
	qboolean oldDown = oldValue != 0;
	qboolean newDown = newValue != 0;

	if (oldDown != newDown)
		Sys_XboxInputKey(time, key, newDown);
}

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
	device->user_data = NULL;
}

qboolean Sys_XboxInputInit(void)
{
	if (xboxInputStarted)
		return qtrue;

	memset(xboxControllers, 0, sizeof(xboxControllers));
	memset(xboxPreviousControllers, 0, sizeof(xboxPreviousControllers));
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

void Sys_XboxInputFrame(void)
{
	int port;
	int i;
	int time;

	if (!xboxInputStarted)
		return;

	Sys_XboxInputPoll();
	time = Sys_Milliseconds();
	for (port = 0; port < XBOX_CONTROLLER_PORTS; ++port)
	{
		const xboxControllerState_t *current = &xboxControllers[port];
		xboxControllerState_t *previous = &xboxPreviousControllers[port];
		int oldAxis;
		int newAxis;

		for (i = 0; i < (int)(sizeof(xboxButtonBindings) /
			sizeof(xboxButtonBindings[0])); ++i)
		{
			Sys_XboxInputButton(time, previous->buttons, current->buttons,
				xboxButtonBindings[i].mask, xboxButtonBindings[i].key);
		}
		Sys_XboxInputByteButton(time, previous->a, current->a, K_JOY1);
		Sys_XboxInputByteButton(time, previous->b, current->b, K_JOY2);
		Sys_XboxInputByteButton(time, previous->x, current->x, K_JOY3);
		Sys_XboxInputByteButton(time, previous->y, current->y, K_JOY4);
		Sys_XboxInputByteButton(time, previous->black, current->black, K_JOY5);
		Sys_XboxInputByteButton(time, previous->white, current->white, K_JOY6);

		oldAxis = Sys_XboxInputAxis(previous->leftStickX);
		newAxis = Sys_XboxInputAxis(current->leftStickX);
		if (oldAxis != newAxis)
			Sys_XboxInputAxisEvent(time, 0, newAxis);
		oldAxis = Sys_XboxInputAxis(previous->leftStickY);
		newAxis = Sys_XboxInputAxis(current->leftStickY);
		if (oldAxis != newAxis)
			Sys_XboxInputAxisEvent(time, 1, newAxis);
		oldAxis = Sys_XboxInputAxis(previous->rightStickX);
		newAxis = Sys_XboxInputAxis(current->rightStickX);
		if (oldAxis != newAxis)
			Sys_XboxInputAxisEvent(time, 2, newAxis);
		oldAxis = Sys_XboxInputAxis(previous->rightStickY);
		newAxis = Sys_XboxInputAxis(current->rightStickY);
		if (oldAxis != newAxis)
			Sys_XboxInputAxisEvent(time, 3, newAxis);
		oldAxis = Sys_XboxInputTrigger(previous->leftTrigger);
		newAxis = Sys_XboxInputTrigger(current->leftTrigger);
		if (oldAxis != newAxis)
			Sys_XboxInputAxisEvent(time, 4, newAxis);
		oldAxis = Sys_XboxInputTrigger(previous->rightTrigger);
		newAxis = Sys_XboxInputTrigger(current->rightTrigger);
		if (oldAxis != newAxis)
			Sys_XboxInputAxisEvent(time, 5, newAxis);

		*previous = *current;
	}
}

void Sys_XboxInputShutdown(void)
{
	if (!xboxInputStarted)
		return;
	xboxInputStopping = qtrue;
	usbh_install_xid_conn_callback(NULL, NULL);
	usbh_core_deinit();
	memset(xboxControllers, 0, sizeof(xboxControllers));
	memset(xboxPreviousControllers, 0, sizeof(xboxPreviousControllers));
	xboxInputStarted = qfalse;
}

const xboxControllerState_t *Sys_XboxInputState(int port)
{
	if (port < 0 || port >= XBOX_CONTROLLER_PORTS)
		return NULL;
	return &xboxControllers[port];
}
