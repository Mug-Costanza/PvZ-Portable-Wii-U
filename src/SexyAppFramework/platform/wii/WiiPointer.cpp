#include <wiiuse/wpad.h>

#include "WiiPointer.h"

bool WiiAnyPointerOnScreen()
{
	for (int i = 0; i < WPAD_MAX_WIIMOTES; i++)
		if (WPAD_Data(i)->ir.valid)
			return true;
	return false;
}

bool WiiRemoteIsSideways(int theChannel)
{
	if (theChannel < 0 || theChannel >= WPAD_MAX_WIIMOTES)
		return false;
	u32 aType;
	if (WPAD_Probe(theChannel, &aType) != WPAD_ERR_NONE)
		return false;
	WPADData* aData = WPAD_Data(theChannel);
	return aData->exp.type == WPAD_EXP_NONE && !aData->ir.valid;
}
