/* Native Original Xbox XID controller service. */
#ifndef IOQUAKE3_XBOX_INPUT_H
#define IOQUAKE3_XBOX_INPUT_H

#include <stdint.h>

#include "../qcommon/q_shared.h"

typedef struct
{
	qboolean connected;
	uint16_t buttons;
	uint8_t a;
	uint8_t b;
	uint8_t x;
	uint8_t y;
	uint8_t black;
	uint8_t white;
	uint8_t leftTrigger;
	uint8_t rightTrigger;
	int16_t leftStickX;
	int16_t leftStickY;
	int16_t rightStickX;
	int16_t rightStickY;
} xboxControllerState_t;

qboolean Sys_XboxInputInit(void);
void Sys_XboxInputPoll(void);
void Sys_XboxInputFrame(void);
void Sys_XboxInputShutdown(void);
const xboxControllerState_t *Sys_XboxInputState(int port);

#endif
