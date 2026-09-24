/* Native Original Xbox XID controller service. */
#include "xbox_input.h"

#include <string.h>

#include <usb.h>
#include <usbh_lib.h>
#include <xid_driver.h>
#include <usbh_hid.h>
#include <xboxkrnl/xboxkrnl.h>

#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"

#define XBOX_CONTROLLER_PORTS 4
/* Analog face buttons report noise near 0; sticks drift near center. */
#define XBOX_BUTTON_THRESHOLD 30
#define XBOX_STICK_DEADZONE 7849
#define XBOX_STICK_MAX 32767
#define XBOX_MENU_CURSOR_SPEED 400.0f
#define XBOX_FRAME_MAX_MS 50
#define XBOX_HID_MAX_DEVICES 8
#define HID_BOOT_KEYBOARD 1
#define HID_BOOT_MOUSE 2
#define HID_PROTOCOL_BOOT 0
#define HID_PROTOCOL_REPORT 1
#define HID_REPORT_MAX 16
#define HID_DESCRIPTOR_MAX 1024
#define HID_MOUSE_BUTTONS 5
#define HID_PARSE_USAGES 32
#define HID_PARSE_STACK 4
/* Usages are (page << 16) | id. */
#define HID_USAGE_BUTTON_1 0x00090001u
#define HID_USAGE_X 0x00010030u
#define HID_USAGE_Y 0x00010031u
#define HID_USAGE_WHEEL 0x00010038u

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

typedef struct
{
	qboolean present;
	qboolean isSigned;
	uint8_t reportId;
	uint8_t bitSize;
	uint32_t bitOffset;
} xboxHidField_t;

typedef struct
{
	qboolean reportIds;
	xboxHidField_t buttons[HID_MOUSE_BUTTONS];
	xboxHidField_t x, y, wheel;
} xboxHidMouseLayout_t;

typedef struct
{
	int dx, dy, wheel;
	int buttons, buttonMask;
} xboxHidMouseSample_t;

typedef struct
{
	uint32_t usagePage;
	int32_t logicalMin;
	uint32_t reportSize;
	uint32_t reportCount;
	uint32_t reportId;
} xboxHidGlobals_t;

typedef struct
{
	HID_DEV_T *device;
	uint8_t protocol;
	qboolean connected;
	uint8_t reports[8][HID_REPORT_MAX];
	uint8_t reportLengths[8];
	uint8_t reportHead, reportCount;
	int mouseX, mouseY;
	xboxHidMouseLayout_t mouse;
	uint8_t oldButtons;
	qboolean capsLock;
	uint8_t oldUsages[32];
	int modifierKeys[4];
} xboxHidState_t;

static xboxHidState_t xboxHid[XBOX_HID_MAX_DEVICES];
static void Sys_XboxInputKey(int time, int key, qboolean down);

static void Sys_XboxHidSetField(xboxHidField_t *field, uint32_t reportId,
	uint32_t bitOffset, uint32_t bitSize, qboolean isSigned)
{
	field->present = qtrue;
	field->isSigned = isSigned;
	field->reportId = (uint8_t)reportId;
	field->bitSize = (uint8_t)bitSize;
	field->bitOffset = bitOffset;
}

static void Sys_XboxHidBootMouseLayout(xboxHidMouseLayout_t *layout)
{
	int i;
	memset(layout, 0, sizeof(*layout));
	for (i = 0; i < HID_MOUSE_BUTTONS; ++i)
		Sys_XboxHidSetField(&layout->buttons[i], 0, i, 1, qfalse);
	Sys_XboxHidSetField(&layout->x, 0, 8, 8, qtrue);
	Sys_XboxHidSetField(&layout->y, 0, 16, 8, qtrue);
	Sys_XboxHidSetField(&layout->wheel, 0, 24, 8, qtrue);
}

static void Sys_XboxHidMatchMouseField(xboxHidMouseLayout_t *layout, uint32_t usage,
	const xboxHidGlobals_t *globals, uint32_t bitOffset, qboolean relative)
{
	xboxHidField_t *field = NULL;
	if (usage >= HID_USAGE_BUTTON_1 && usage < HID_USAGE_BUTTON_1 + HID_MOUSE_BUTTONS)
		field = &layout->buttons[usage - HID_USAGE_BUTTON_1];
	else if (relative && usage == HID_USAGE_X)
		field = &layout->x;
	else if (relative && usage == HID_USAGE_Y)
		field = &layout->y;
	else if (relative && usage == HID_USAGE_WHEEL)
		field = &layout->wheel;
	if (field && !field->present)
		Sys_XboxHidSetField(field, globals->reportId, bitOffset,
			globals->reportSize, globals->logicalMin < 0);
}

/* Finds the button, X, Y and wheel fields in a HID 1.11 report descriptor. */
static qboolean Sys_XboxHidParseMouse(const uint8_t *desc, int length,
	xboxHidMouseLayout_t *layout)
{
	xboxHidGlobals_t globals, stack[HID_PARSE_STACK];
	uint32_t usages[HID_PARSE_USAGES];
	uint32_t inputBits[256];
	uint32_t usageMin = 0;
	int usageCount = 0, stackDepth = 0, pos = 0;

	memset(layout, 0, sizeof(*layout));
	memset(&globals, 0, sizeof(globals));
	memset(inputBits, 0, sizeof(inputBits));
	while (pos < length)
	{
		uint8_t prefix = desc[pos++];
		int size = (prefix & 3) == 3 ? 4 : prefix & 3;
		int type = (prefix >> 2) & 3;
		int tag = prefix >> 4;
		uint32_t value = 0;
		int32_t signedValue;
		int i;

		if (prefix == 0xFE)
		{
			/* Long item: a data size byte and a tag byte come before the data. */
			if (pos >= length)
				return qfalse;
			pos += 2 + desc[pos];
			continue;
		}
		if (pos + size > length)
			return qfalse;
		for (i = 0; i < size; ++i)
			value |= (uint32_t)desc[pos + i] << (8 * i);
		pos += size;
		signedValue = size == 1 ? (int8_t)value : size == 2 ? (int16_t)value : (int32_t)value;

		if (type == 1)
		{
			switch (tag)
			{
			case 0x0: globals.usagePage = value; break;
			case 0x1: globals.logicalMin = signedValue; break;
			case 0x7: globals.reportSize = value; break;
			case 0x8: globals.reportId = value & 0xFF; layout->reportIds = qtrue; break;
			case 0x9: globals.reportCount = value; break;
			case 0xA: if (stackDepth < HID_PARSE_STACK) stack[stackDepth++] = globals; break;
			case 0xB: if (stackDepth > 0) globals = stack[--stackDepth]; break;
			default: break;
			}
		}
		else if (type == 2)
		{
			if (tag == 0x0 && usageCount < HID_PARSE_USAGES)
				usages[usageCount++] = value;
			else if (tag == 0x1)
				usageMin = value;
			else if (tag == 0x2)
			{
				uint32_t usage;
				for (usage = usageMin; usage <= value && usageCount < HID_PARSE_USAGES; ++usage)
					usages[usageCount++] = usage;
			}
		}
		else if (type == 0)
		{
			/* Input flags: bit 0 constant, bit 1 variable, bit 2 relative. */
			if (tag == 0x8)
			{
				if (!(value & 1) && (value & 2) && usageCount &&
					globals.reportSize >= 1 && globals.reportSize <= 32)
				{
					for (i = 0; i < (int)globals.reportCount && i < HID_PARSE_USAGES; ++i)
					{
						uint32_t usage = usages[i < usageCount ? i : usageCount - 1];
						/* A usage shorter than 4 bytes takes the page in effect here. */
						if (!(usage >> 16))
							usage |= globals.usagePage << 16;
						Sys_XboxHidMatchMouseField(layout, usage, &globals,
							inputBits[globals.reportId] + (uint32_t)i * globals.reportSize,
							(value & 4) != 0);
					}
				}
				inputBits[globals.reportId] += globals.reportSize * globals.reportCount;
			}
			usageCount = 0;
			usageMin = 0;
		}
	}
	return layout->x.present && layout->y.present;
}

static qboolean Sys_XboxHidReadMouseLayout(HID_DEV_T *device, xboxHidMouseLayout_t *layout)
{
	/* Control transfers need DMA-safe memory from the USB pool. */
	uint8_t *desc = (uint8_t *)usbh_alloc_mem(HID_DESCRIPTOR_MAX);
	qboolean parsed = qfalse;

	if (desc)
	{
		int length = usbh_hid_get_report_descriptor(device, desc, HID_DESCRIPTOR_MAX);
		parsed = length > 0 && Sys_XboxHidParseMouse(desc, length, layout);
		usbh_free_mem(desc, HID_DESCRIPTOR_MAX);
	}
	if (!parsed)
		Sys_XboxHidBootMouseLayout(layout);
	return parsed;
}

static qboolean Sys_XboxHidFieldValue(const xboxHidField_t *field, qboolean reportIds,
	const uint8_t *report, uint32_t length, int *value)
{
	uint32_t raw = 0;
	uint32_t i;

	if (!field->present)
		return qfalse;
	if (reportIds)
	{
		if (!length || report[0] != field->reportId)
			return qfalse;
		++report;
		--length;
	}
	if (field->bitOffset >= length * 8 || field->bitSize > length * 8 - field->bitOffset)
		return qfalse;
	for (i = 0; i < field->bitSize; ++i)
	{
		uint32_t bit = field->bitOffset + i;
		if (report[bit >> 3] & (1 << (bit & 7)))
			raw |= 1u << i;
	}
	if (field->isSigned && field->bitSize < 32 && (raw & (1u << (field->bitSize - 1))))
		raw |= ~0u << field->bitSize;
	*value = (int)raw;
	return qtrue;
}

/* Also runs in the USB DPC, so it only reads the report and the fixed layout. */
static void Sys_XboxHidMouseDecode(const xboxHidMouseLayout_t *layout,
	const uint8_t *report, uint32_t length, xboxHidMouseSample_t *sample)
{
	int value;
	int i;

	memset(sample, 0, sizeof(*sample));
	if (Sys_XboxHidFieldValue(&layout->x, layout->reportIds, report, length, &value))
		sample->dx = value;
	if (Sys_XboxHidFieldValue(&layout->y, layout->reportIds, report, length, &value))
		sample->dy = value;
	if (Sys_XboxHidFieldValue(&layout->wheel, layout->reportIds, report, length, &value))
		sample->wheel = value;
	for (i = 0; i < HID_MOUSE_BUTTONS; ++i)
	{
		if (Sys_XboxHidFieldValue(&layout->buttons[i], layout->reportIds, report, length, &value))
		{
			sample->buttonMask |= 1 << i;
			if (value)
				sample->buttons |= 1 << i;
		}
	}
}

static int Sys_XboxHidSlot(HID_DEV_T *device, qboolean allocate)
{
	int i;
	for (i = 0; i < XBOX_HID_MAX_DEVICES; ++i)
		if (xboxHid[i].device == device && (!allocate || xboxHid[i].connected))
			return i;
	if (allocate)
		for (i = 0; i < XBOX_HID_MAX_DEVICES; ++i)
			if (!xboxHid[i].device)
			{
				memset(&xboxHid[i], 0, sizeof(xboxHid[i]));
				xboxHid[i].device = device;
				xboxHid[i].protocol = device->bProtocolCode;
				xboxHid[i].connected = qtrue;
				device->user_data = &xboxHid[i];
				return i;
			}
	return -1;
}

static void Sys_XboxHidRead(HID_DEV_T *device, uint16_t endpoint, int status,
	uint8_t *report, uint32_t length)
{
	xboxHidState_t *state = device ? (xboxHidState_t *)device->user_data : NULL;
	(void)endpoint;
	if (!state || status || !report || !length)
		return;
	if (length > HID_REPORT_MAX)
		length = HID_REPORT_MAX;
	if (state->reportCount == 8)
	{
		state->reportHead = (state->reportHead + 1) & 7;
		--state->reportCount;
	}
	{
		int tail = (state->reportHead + state->reportCount) & 7;
		memcpy(state->reports[tail], report, length);
		state->reportLengths[tail] = (uint8_t)length;
		++state->reportCount;
	}
	if (state->protocol == HID_BOOT_MOUSE)
	{
		xboxHidMouseSample_t sample;
		Sys_XboxHidMouseDecode(&state->mouse, report, length, &sample);
		state->mouseX += sample.dx;
		state->mouseY += sample.dy;
	}
}

static void Sys_XboxHidConnected(HID_DEV_T *device, int param)
{
	int slot;
	uint8_t protocol = HID_PROTOCOL_BOOT;
	(void)param;
	if (!device || (device->bProtocolCode != HID_BOOT_KEYBOARD &&
		device->bProtocolCode != HID_BOOT_MOUSE))
		return;
	slot = Sys_XboxHidSlot(device, qtrue);
	if (slot < 0)
		return;
	/* Some mice ignore boot protocol, so decode their own layout in report protocol. */
	if (device->bProtocolCode == HID_BOOT_MOUSE &&
		Sys_XboxHidReadMouseLayout(device, &xboxHid[slot].mouse))
		protocol = HID_PROTOCOL_REPORT;
	/* Boot protocol gives the standard 8-byte keyboard and 3-byte mouse reports. */
	(void)usbh_hid_set_protocol(device, protocol);
	(void)usbh_hid_start_int_read(device, 0, Sys_XboxHidRead);
}

static void Sys_XboxHidDisconnected(HID_DEV_T *device, int param)
{
	int slot = Sys_XboxHidSlot(device, qfalse);
	(void)param;
	if (slot >= 0)
	{
		xboxHid[slot].connected = qfalse;
		xboxHid[slot].reportHead = 0;
		xboxHid[slot].reportCount = 0;
		xboxHid[slot].mouseX = xboxHid[slot].mouseY = 0;
	}
	if (device)
		device->user_data = NULL;
}

static int Sys_XboxHidKey(uint8_t usage)
{
	if (usage >= 4 && usage <= 29)
		return 'a' + usage - 4;
	if (usage >= 30 && usage <= 38)
		return '1' + usage - 30;
	if (usage == 39) return '0';
	if (usage >= 40 && usage <= 52)
	{
		static const int keys[] = { K_ENTER, K_ESCAPE, K_BACKSPACE, K_TAB, K_SPACE,
			'-', '=', '[', ']', '\\', '#', ';', '\'' };
		return keys[usage - 40];
	}
	if (usage >= 58 && usage <= 69) return K_F1 + usage - 58;
	switch (usage)
	{
	case 70: return K_PRINT; case 71: return K_SCROLLOCK; case 72: return K_PAUSE;
	case 73: return K_INS; case 74: return K_HOME; case 75: return K_PGUP;
	case 76: return K_DEL; case 77: return K_END; case 78: return K_PGDN;
	case 79: return K_RIGHTARROW; case 80: return K_LEFTARROW;
	case 81: return K_DOWNARROW; case 82: return K_UPARROW;
	case 83: return K_KP_NUMLOCK; case 84: return K_KP_SLASH;
	case 85: return K_KP_STAR; case 86: return K_KP_MINUS;
	case 87: return K_KP_PLUS; case 88: return K_KP_ENTER;
	case 89: return K_KP_END; case 90: return K_KP_DOWNARROW;
	case 91: return K_KP_PGDN; case 92: return K_KP_LEFTARROW;
	case 93: return K_KP_5; case 94: return K_KP_RIGHTARROW;
	case 95: return K_KP_HOME; case 96: return K_KP_UPARROW;
	case 97: return K_KP_PGUP; case 98: return K_KP_INS;
	case 99: return K_KP_DEL; case 103: return K_KP_EQUALS;
	case 53: return K_CONSOLE;
	case 57: return K_CAPSLOCK;
	case 54: return ','; case 55: return '.'; case 56: return '/';
	default: return 0;
	}
}

static void Sys_XboxHidKeyEvent(int time, int key, qboolean down, qboolean character)
{
	if (!key) return;
	Com_QueueEvent(time, SE_KEY, key, down, 0, NULL);
	if (down && character && key >= 32 && key < 127)
		Com_QueueEvent(time, SE_CHAR, key, 0, 0, NULL);
}

static int Sys_XboxHidChar(uint8_t usage, qboolean shifted, qboolean caps)
{
	int key = Sys_XboxHidKey(usage);
	/* ioq3 text fields erase on ctrl-h, which sdl_input.c sends for Backspace. */
	if (usage == 42)
		return 'h' - 'a' + 1;
	if (usage >= 4 && usage <= 29)
		return (shifted ^ caps) ? key - 'a' + 'A' : key;
	if (usage >= 30 && usage <= 39)
	{
		static const char shiftedDigits[] = "!@#$%^&*()";
		return shifted ? shiftedDigits[usage - 30] : (usage == 39 ? '0' : '1' + usage - 30);
	}
	if (usage >= 45 && usage <= 56)
	{
		static const char plain[] = "-=[]\\#;'`,./";
		static const char shiftedChars[] = "_+{}|~:\"~<>?";
		return shifted ? shiftedChars[usage - 45] : plain[usage - 45];
	}
	return key >= 32 && key < 127 ? key : 0;
}

static void Sys_XboxHidFrame(int time)
{
	int device;
	for (device = 0; device < XBOX_HID_MAX_DEVICES; ++device)
	{
		xboxHidState_t *state = &xboxHid[device];
		uint8_t reports[8][HID_REPORT_MAX];
		uint8_t reportLengths[8];
		uint8_t protocol;
		int reportCount, reportHead, mouseX, mouseY;
		int reportNumber;
		qboolean connected, devicePresent;
		KIRQL oldIrql;
		int i;
		oldIrql = KeRaiseIrqlToDpcLevel();
		devicePresent = state->device != NULL;
		connected = devicePresent && state->connected;
		protocol = state->protocol;
		reportHead = state->reportHead;
		reportCount = state->reportCount;
		mouseX = state->mouseX;
		mouseY = state->mouseY;
		state->reportHead = 0;
		state->reportCount = 0;
		state->mouseX = state->mouseY = 0;
		for (i = 0; i < reportCount; ++i)
		{
			int index = (reportHead + i) & 7;
			memcpy(reports[i], state->reports[index], HID_REPORT_MAX);
			reportLengths[i] = state->reportLengths[index];
		}
		KfLowerIrql(oldIrql);
		if (!devicePresent) continue;
		if (!connected)
		{
			for (i = 0; i < 32; ++i)
			{
				int bit;
				for (bit = 0; bit < 8; ++bit)
					if (state->oldUsages[i] & (1 << bit))
						Sys_XboxHidKeyEvent(time, (i * 8 + bit) == 53 ? K_CONSOLE : Sys_XboxHidKey((uint8_t)(i * 8 + bit)), qfalse, qfalse);
			}
			for (i = 0; i < 4; ++i)
				Sys_XboxHidKeyEvent(time, state->modifierKeys[i], qfalse, qfalse);
			for (i = 0; i < 5; ++i)
			{
				if (state->oldButtons & (1 << i))
					Sys_XboxInputKey(time, K_MOUSE1 + i, qfalse);
			}
			oldIrql = KeRaiseIrqlToDpcLevel();
			memset(state, 0, sizeof(*state));
			KfLowerIrql(oldIrql);
			continue;
		}
		for (reportNumber = 0; reportNumber < reportCount; ++reportNumber)
		{
			uint8_t *report = reports[reportNumber];
			uint32_t reportLength = reportLengths[reportNumber];
			if (protocol == HID_BOOT_KEYBOARD && reportLength >= 8)
			{
				uint8_t usages[32];
				qboolean shift = (report[0] & 0x22) != 0;
				memset(usages, 0, sizeof(usages));
				for (i = 0; i < 6; ++i)
				{
					int usage = report[2 + i];
					if (usage >= 1 && usage <= 3)
						break;
					usages[usage >> 3] |= (uint8_t)(1 << (usage & 7));
				}
				if (i != 6) continue;
				for (i = 0; i < 4; ++i)
				{
					static const int keys[] = { K_CTRL, K_SHIFT, K_ALT, K_COMMAND };
					int modifierMask = (1 << i) | (1 << (i + 4));
					int key = (report[0] & modifierMask) ? keys[i] : 0;
					if (key != state->modifierKeys[i])
					{
						Sys_XboxHidKeyEvent(time, state->modifierKeys[i], qfalse, qfalse);
						Sys_XboxHidKeyEvent(time, key, qtrue, qfalse);
						state->modifierKeys[i] = key;
					}
				}
				if ((usages[57 >> 3] & (1 << (57 & 7))) && !(state->oldUsages[57 >> 3] & (1 << (57 & 7))))
					state->capsLock = !state->capsLock;
				for (i = 0; i < 256; ++i)
				{
					uint8_t mask = (uint8_t)(1 << (i & 7));
					qboolean wasDown = (state->oldUsages[i >> 3] & mask) != 0;
					qboolean isDown = (usages[i >> 3] & mask) != 0;
					int key = i == 53 ? K_CONSOLE : Sys_XboxHidKey((uint8_t)i);
					if (wasDown != isDown)
					{
						Sys_XboxHidKeyEvent(time, key, isDown, qfalse);
						if (isDown && i != 53)
						{
							int character = Sys_XboxHidChar((uint8_t)i, shift, state->capsLock);
							if (character) Com_QueueEvent(time, SE_CHAR, character, 0, 0, NULL);
						}
					}
				}
				memcpy(state->oldUsages, usages, sizeof(usages));
			}
			else if (protocol == HID_BOOT_MOUSE)
			{
				xboxHidMouseSample_t sample;
				uint8_t buttons;
				int wheel;
				Sys_XboxHidMouseDecode(&state->mouse, report, reportLength, &sample);
				/* Keep buttons this report does not carry, e.g. another report ID. */
				buttons = (uint8_t)((state->oldButtons & ~sample.buttonMask) | sample.buttons);
				for (i = 0; i < 5; ++i)
					if ((buttons ^ state->oldButtons) & (1 << i))
						Sys_XboxInputKey(time, K_MOUSE1 + i, (buttons & (1 << i)) != 0);
				state->oldButtons = buttons;
				wheel = sample.wheel;
				while (wheel > 0)
				{
					Sys_XboxInputKey(time, K_MWHEELUP, qtrue);
					Sys_XboxInputKey(time, K_MWHEELUP, qfalse);
					--wheel;
				}
				while (wheel < 0)
				{
					Sys_XboxInputKey(time, K_MWHEELDOWN, qtrue);
					Sys_XboxInputKey(time, K_MWHEELDOWN, qfalse);
					++wheel;
				}
			}
		}
		if (protocol == HID_BOOT_MOUSE && (mouseX || mouseY))
		{
			Com_QueueEvent(time, SE_MOUSE, mouseX, mouseY, 0, NULL);
		}
	}
}

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
	memset(xboxHid, 0, sizeof(xboxHid));
	memset(xboxSentKey, 0, sizeof(xboxSentKey));
	memset(xboxSentAxis, 0, sizeof(xboxSentAxis));
	xboxCursorX = 0.0f;
	xboxCursorY = 0.0f;
	xboxLastFrameTime = Sys_Milliseconds();
	xboxInputStopping = qfalse;
	/* USB core initialization clears the driver table, so initialize it first. */
	usbh_core_init();
	usbh_xid_init();
	usbh_hid_init();
	usbh_install_xid_conn_callback(Sys_XboxInputConnected,
		Sys_XboxInputDisconnected);
	usbh_install_hid_conn_callback(Sys_XboxHidConnected,
		Sys_XboxHidDisconnected);

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
	Sys_XboxHidFrame(time);
}

void Sys_XboxInputShutdown(void)
{
	if (!xboxInputStarted)
		return;
	xboxInputStopping = qtrue;
	{
		int i, j, time = Sys_Milliseconds();
		for (i = 0; i < XBOX_HID_MAX_DEVICES; ++i)
		{
			for (j = 0; j < 32; ++j)
			{
				int bit;
				for (bit = 0; bit < 8; ++bit)
					if (xboxHid[i].oldUsages[j] & (1 << bit))
						Sys_XboxHidKeyEvent(time, (j * 8 + bit) == 53 ? K_CONSOLE : Sys_XboxHidKey((uint8_t)(j * 8 + bit)), qfalse, qfalse);
			}
			for (j = 0; j < 4; ++j)
				Sys_XboxHidKeyEvent(time, xboxHid[i].modifierKeys[j], qfalse, qfalse);
			for (j = 0; j < 5; ++j)
				if (xboxHid[i].oldButtons & (1 << j))
					Sys_XboxInputKey(time, K_MOUSE1 + j, qfalse);
		}
	}
	usbh_install_xid_conn_callback(NULL, NULL);
	usbh_install_hid_conn_callback(NULL, NULL);
	usbh_core_deinit();
	memset(xboxControllers, 0, sizeof(xboxControllers));
	memset(xboxSentKey, 0, sizeof(xboxSentKey));
	memset(xboxSentAxis, 0, sizeof(xboxSentAxis));
	memset(xboxHid, 0, sizeof(xboxHid));
	xboxInputStarted = qfalse;
}

const xboxControllerState_t *Sys_XboxInputState(int port)
{
	if (port < 0 || port >= XBOX_CONTROLLER_PORTS)
		return NULL;
	return &xboxControllers[port];
}
