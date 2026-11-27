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
    OrbisPadData data = {};
    previousButtons = currentButtons;

    // Keep the same polling function that the original working controller used.
    // OpenOrbis OrbisPadData includes the analogue axes in leftStick.
    if (scePadReadState(pad, &data) == 0)
    {
        currentButtons = data.buttons;

        // DS4 stick axes are unsigned bytes centred around 128.
        // DELTA 123 corresponds to near-full deflection.
        // Activate before the stick reaches its physical edge.
        // Values are 0-255, with approximately 128 at rest.
        // Adjust these two values to tune sensitivity:
        // lower STICK_LOW / higher STICK_HIGH = later activation.
        // higher STICK_LOW / lower STICK_HIGH = earlier activation.
        const int STICK_LOW = 80;
        const int STICK_HIGH = 176;

        if (data.leftStick.y < STICK_LOW)
            currentButtons |= ORBIS_PAD_BUTTON_UP;
        else if (data.leftStick.y > STICK_HIGH)
            currentButtons |= ORBIS_PAD_BUTTON_DOWN;

        if (data.leftStick.x < STICK_LOW)
            currentButtons |= ORBIS_PAD_BUTTON_LEFT;
        else if (data.leftStick.x > STICK_HIGH)
            currentButtons |= ORBIS_PAD_BUTTON_RIGHT;
    }
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
