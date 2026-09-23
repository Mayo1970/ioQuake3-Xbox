/* Native Original Xbox XID controller service. */
#include "xbox_input.h"

#include <string.h>

#include <usb.h>
#include <usbh_lib.h>
#include <xid_driver.h>

#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"

#define XBOX_CONTROLLER_PORTS 4
/* Analog face buttons report noise near 0; sticks drift near center. */
#define XBOX_BUTTON_THRESHOLD 30
#define XBOX_STICK_DEADZONE 7849
#define XBOX_STICK_MAX 32767
#define XBOX_MENU_CURSOR_SPEED 400.0f
#define XBOX_FRAME_MAX_MS 50

extern int Key_GetCatcher(void);
extern void Key_SetBinding(int keynum, const char *binding);
extern char *Key_GetBinding(int keynum);

/* The first eight indices match bits 0-7 of the XID digital button mask. */
enum
{
	XBOX_BTN_DPAD_UP,
	XBOX_BTN_DPAD_DOWN,
	XBOX_BTN_DPAD_LEFT,
	XBOX_BTN_DPAD_RIGHT,
	XBOX_BTN_START,
	XBOX_BTN_BACK,
	XBOX_BTN_LEFT_THUMB,
	XBOX_BTN_RIGHT_THUMB,
	XBOX_BTN_A,
	XBOX_BTN_B,
	XBOX_BTN_X,
	XBOX_BTN_Y,
	XBOX_BTN_BLACK,
	XBOX_BTN_WHITE,
	XBOX_BTN_LEFT_TRIGGER,
	XBOX_BTN_RIGHT_TRIGGER,
	XBOX_BTN_COUNT
};

static const int xboxButtonKeys[XBOX_BTN_COUNT] =
{
	K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW,
	K_ESCAPE, K_JOY11, K_JOY9, K_JOY10,
	K_JOY1, K_JOY2, K_JOY3, K_JOY4, K_JOY5, K_JOY6, K_JOY7, K_JOY8
};

static xboxControllerState_t xboxControllers[XBOX_CONTROLLER_PORTS];
/* Key sent on press, so the release matches even if the menu state changed. */
static int xboxSentKey[XBOX_CONTROLLER_PORTS][XBOX_BTN_COUNT];
static int xboxSentAxis[XBOX_CONTROLLER_PORTS][4];
static float xboxCursorX;
static float xboxCursorY;
static int xboxLastFrameTime;
static qboolean xboxInputStarted;
static qboolean xboxInputStopping;

static int Sys_XboxInputStick(int value)
{
	int magnitude = value < 0 ? -value : value;

	if (magnitude <= XBOX_STICK_DEADZONE)
		return 0;
	if (magnitude > XBOX_STICK_MAX)
		magnitude = XBOX_STICK_MAX;
	/* Rescale so output starts at 0 right after the deadzone. */
	magnitude = (magnitude - XBOX_STICK_DEADZONE) * XBOX_STICK_MAX /
		(XBOX_STICK_MAX - XBOX_STICK_DEADZONE);
	return value < 0 ? -magnitude : magnitude;
}

static float Sys_XboxInputCursorStep(int stick, int frameMs)
{
	float f = (float)stick / (float)XBOX_STICK_MAX;

	/* Squared response gives finer control near center. */
	return f * (f < 0.0f ? -f : f) * XBOX_MENU_CURSOR_SPEED *
		(float)frameMs * 0.001f;
}

static void Sys_XboxInputKey(int time, int key, qboolean down)
{
	Com_QueueEvent(time, SE_KEY, key, down, 0, NULL);
}

static void Sys_XboxInputAxisEvent(int time, int axis, int value)
{
	Com_QueueEvent(time, SE_JOYSTICK_AXIS, axis, value, 0, NULL);
}

static qboolean Sys_XboxInputButtonDown(const xboxControllerState_t *state,
	int button)
{
	switch (button)
	{
	case XBOX_BTN_A:
		return state->a >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_B:
		return state->b >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_X:
		return state->x >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_Y:
		return state->y >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_BLACK:
		return state->black >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_WHITE:
		return state->white >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_LEFT_TRIGGER:
		return state->leftTrigger >= XBOX_BUTTON_THRESHOLD;
	case XBOX_BTN_RIGHT_TRIGGER:
		return state->rightTrigger >= XBOX_BUTTON_THRESHOLD;
	default:
		return (state->buttons & (1 << button)) != 0;
	}
}

static int Sys_XboxInputKeyFor(int button, qboolean inMenu)
{
	/* The menu UI only understands Enter and Escape for accept and back. */
	if (inMenu && button == XBOX_BTN_A)
		return K_ENTER;
	if (inMenu && button == XBOX_BTN_B)
		return K_ESCAPE;
	return xboxButtonKeys[button];
}

static void Sys_XboxInputDefaultBind(int key, const char *binding)
{
	char *existing = Key_GetBinding(key);

	if (!existing || !existing[0])
		Key_SetBinding(key, binding);
}

static void Sys_XboxInputDefaultCvar(const char *name, const char *value)
{
	cvar_t *cvar = Cvar_Get(name, value, CVAR_ARCHIVE);

	/* Keep any value the user or q3config.cfg already changed. */
	if (!strcmp(cvar->string, cvar->resetString))
		Cvar_Set(name, value);
}

static void Sys_XboxInputApplyDefaults(void)
{
	Sys_XboxInputDefaultBind(K_JOY1, "+moveup");
	Sys_XboxInputDefaultBind(K_JOY2, "+movedown");
	Sys_XboxInputDefaultBind(K_JOY3, "weapprev");
	Sys_XboxInputDefaultBind(K_JOY4, "weapnext");
	Sys_XboxInputDefaultBind(K_JOY5, "+button2");
	Sys_XboxInputDefaultBind(K_JOY6, "+speed");
	Sys_XboxInputDefaultBind(K_JOY7, "+zoom");
	Sys_XboxInputDefaultBind(K_JOY8, "+attack");
	Sys_XboxInputDefaultBind(K_JOY11, "+scores");

	/* Stock joystick scaling assumes a +-32767 axis and is far too fast. */
	Sys_XboxInputDefaultCvar("j_yaw", "-0.006");
	Sys_XboxInputDefaultCvar("j_pitch", "0.005");
	Sys_XboxInputDefaultCvar("j_forward", "-0.0039");
	Sys_XboxInputDefaultCvar("j_side", "0.0039");
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
	memset(xboxSentKey, 0, sizeof(xboxSentKey));
	memset(xboxSentAxis, 0, sizeof(xboxSentAxis));
	xboxCursorX = 0.0f;
	xboxCursorY = 0.0f;
	xboxLastFrameTime = Sys_Milliseconds();
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

	Sys_XboxInputApplyDefaults();
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
	int frameMs;
	qboolean inMenu;

	if (!xboxInputStarted)
		return;

	Sys_XboxInputPoll();
	time = Sys_Milliseconds();
	frameMs = time - xboxLastFrameTime;
	xboxLastFrameTime = time;
	if (frameMs < 0)
		frameMs = 0;
	if (frameMs > XBOX_FRAME_MAX_MS)
		frameMs = XBOX_FRAME_MAX_MS;
	inMenu = (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CGAME)) != 0;
	if (!inMenu)
	{
		xboxCursorX = 0.0f;
		xboxCursorY = 0.0f;
	}

	for (port = 0; port < XBOX_CONTROLLER_PORTS; ++port)
	{
		const xboxControllerState_t *state = &xboxControllers[port];
		int stick[4];

		for (i = 0; i < XBOX_BTN_COUNT; ++i)
		{
			qboolean down = Sys_XboxInputButtonDown(state, i);
			int *sent = &xboxSentKey[port][i];

			if (down && !*sent)
			{
				*sent = Sys_XboxInputKeyFor(i, inMenu);
				Sys_XboxInputKey(time, *sent, qtrue);
			}
			else if (!down && *sent)
			{
				Sys_XboxInputKey(time, *sent, qfalse);
				*sent = 0;
			}
		}

		/* XID reports Y up as positive; Quake expects down as positive. */
		stick[0] = Sys_XboxInputStick(state->leftStickX);
		stick[1] = -Sys_XboxInputStick(state->leftStickY);
		stick[2] = Sys_XboxInputStick(state->rightStickX);
		stick[3] = -Sys_XboxInputStick(state->rightStickY);

		if (inMenu)
		{
			int dx;
			int dy;

			/* The left stick moves the menu cursor; zero the game axes. */
			xboxCursorX += Sys_XboxInputCursorStep(stick[0], frameMs);
			xboxCursorY += Sys_XboxInputCursorStep(stick[1], frameMs);
			dx = (int)xboxCursorX;
			dy = (int)xboxCursorY;
			xboxCursorX -= (float)dx;
			xboxCursorY -= (float)dy;
			if (dx || dy)
				Com_QueueEvent(time, SE_MOUSE, dx, dy, 0, NULL);
			memset(stick, 0, sizeof(stick));
		}

		for (i = 0; i < 4; ++i)
		{
			if (stick[i] != xboxSentAxis[port][i])
			{
				xboxSentAxis[port][i] = stick[i];
				Sys_XboxInputAxisEvent(time, i, stick[i]);
			}
		}
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
	memset(xboxSentKey, 0, sizeof(xboxSentKey));
	memset(xboxSentAxis, 0, sizeof(xboxSentAxis));
	xboxInputStarted = qfalse;
}

const xboxControllerState_t *Sys_XboxInputState(int port)
{
	if (port < 0 || port >= XBOX_CONTROLLER_PORTS)
		return NULL;
	return &xboxControllers[port];
}
