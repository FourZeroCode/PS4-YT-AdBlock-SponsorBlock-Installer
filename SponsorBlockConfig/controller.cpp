#include "controller.h"
#include "../_common/log.h"

Controller::Controller()
    : pad(-1), userID(-1), previousButtons(0), currentButtons(0)
{
}

bool Controller::Init(int controllerUserID)
{
    if (scePadInit() != 0)
    {
        DEBUGLOG << "Failed to initialize pad library";
        return false;
    }

    if (controllerUserID < 0)
    {
        OrbisUserServiceInitializeParams params;
        params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
        if (sceUserServiceInitialize(&params) != 0)
        {
            DEBUGLOG << "Failed to initialize user service";
            return false;
        }
        if (sceUserServiceGetInitialUser(&userID) != 0)
        {
            DEBUGLOG << "Failed to get initial user";
            return false;
        }
    }
    else
    {
        userID = controllerUserID;
    }

    pad = scePadOpen(userID, 0, 0, NULL);
    if (pad < 0)
    {
        DEBUGLOG << "Failed to open controller";
        return false;
    }
    return true;
}

void Controller::Poll()
{
    OrbisPadData data;
    previousButtons = currentButtons;
    if (scePadReadState(pad, &data) == 0)
        currentButtons = data.buttons;
}

bool Controller::Pressed(int buttonMask) const
{
    return (currentButtons & buttonMask) != 0 &&
           (previousButtons & buttonMask) == 0;
}


bool Controller::Held(int buttonMask) const
{
    return (currentButtons & buttonMask) != 0;
}
