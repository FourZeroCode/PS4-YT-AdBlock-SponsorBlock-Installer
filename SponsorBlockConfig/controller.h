#ifndef SPONSORBLOCK_CONTROLLER_H
#define SPONSORBLOCK_CONTROLLER_H

#include <orbis/Pad.h>
#include <orbis/UserService.h>

class Controller
{
    int pad;
    int userID;
    int previousButtons;
    int currentButtons;

public:
    Controller();
    bool Init(int controllerUserID = -1);
    void Poll();
    bool Pressed(int buttonMask) const;
    bool Held(int buttonMask) const;
};

#endif
