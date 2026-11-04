#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sstream>
#include <string>

#include <orbis/Pad.h>
#include <orbis/libkernel.h>
#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/Net.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>

#include "../_common/graphics.h"
#include "../_common/log.h"
#include "controller.h"
#include "version.h"

static char downloadFailureReason[128] = "not attempted";

// OpenOrbis exposes the progress-bar target as an integer value (0).
#define ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT 0

#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_DEPTH 4
#define CATEGORY_COUNT 12
#define CONFIG_DIRECTORY "/data/youtube"
#define CONFIG_PATH "/data/youtube/config.json"

#define LATEST_APP_RELEASE_API_URL "https://api.github.com/repos/FourZeroCode/PS4-YT-AdBlock-SponsorBlock-Installer/releases/latest"
#define APP_UPDATE_TEMP_PATH "/data/pkg/SponsorBlockConfig-update.pkg.download"
#define APP_UPDATE_PKG_PATH "/data/pkg/SponsorBlockConfig-update.pkg"

std::stringstream debugLogStream;
int frameID = 0;

struct Category
{
    const char *id;
    const char *label;
};

static Category categories[CATEGORY_COUNT] = {
    { "sponsor", "Sponsor" },
    { "selfpromo", "Unpaid / self-promotion" },
    { "interaction", "Interaction reminders" },
    { "intro", "Intro / intermission" },
    { "outro", "Endcards / credits" },
    { "preview", "Preview / recap" },
    { "music_offtopic", "Non-music section in music video" },
    { "filler", "Filler / tangents" },
    { "hook", "Hook / greeting" },
    { "poi_highlight", "Point of interest / highlight" },
    { "exclusive_access", "Exclusive access" },
    { "chapter", "Chapter" }
};

static bool selected[CATEGORY_COUNT] = { false };

// Default to confirm for existing configs that do not yet contain skipMode.
static bool skipModeAuto = false;
static const int MENU_ITEM_COUNT = CATEGORY_COUNT + 1;

static void drawText(Scene2D *scene, const char *text, FT_Face font,
                     int x, int y, Color background, Color foreground)
{
    char mutableText[256];
    snprintf(mutableText, sizeof(mutableText), "%s", text);
    scene->DrawText(mutableText, font, x, y, background, foreground);
}

// Draw the button icons.
static void drawLine(Scene2D *scene, int x0, int y0, int x1, int y1, Color color)
{
    int dx = x1 - x0;
    if (dx < 0) dx = -dx;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 - y0;
    if (dy < 0) dy = -dy;
    dy = -dy;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;)
    {
        scene->DrawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int twiceError = 2 * error;
        if (twiceError >= dy)
        {
            error += dy;
            x0 += sx;
        }
        if (twiceError <= dx)
        {
            error += dx;
            y0 += sy;
        }
    }
}

static void drawControllerIcon(Scene2D *scene, const char *icon, int x, int y)
{
    Color triangleGreen = { 0, 190, 110 };
    Color squarePink = { 245, 80, 150 };
    Color crossBlue = { 75, 155, 255 };
    Color arrowWhite = { 225, 232, 240 };

    if (strcmp(icon, "triangle") == 0)
    {
        drawLine(scene, x + 14, y, x + 1, y + 25, triangleGreen);
        drawLine(scene, x + 1, y + 25, x + 27, y + 25, triangleGreen);
        drawLine(scene, x + 27, y + 25, x + 14, y, triangleGreen);
    }
    else if (strcmp(icon, "square") == 0)
    {
        drawLine(scene, x + 2, y + 2, x + 26, y + 2, squarePink);
        drawLine(scene, x + 26, y + 2, x + 26, y + 26, squarePink);
        drawLine(scene, x + 26, y + 26, x + 2, y + 26, squarePink);
        drawLine(scene, x + 2, y + 26, x + 2, y + 2, squarePink);
    }
    else if (strcmp(icon, "cross") == 0)
    {
        // Make the cross a bit thicker.
        for (int offset = -1; offset <= 1; ++offset)
        {
            drawLine(scene, x + 4 + offset, y + 4, x + 24 + offset, y + 24, crossBlue);
            drawLine(scene, x + 24 + offset, y + 4, x + 4 + offset, y + 24, crossBlue);
        }
    }
    else if (strcmp(icon, "arrows") == 0)
    {
        // D-pad arrows.
        drawLine(scene, x + 7, y + 11, x + 7, y + 1, arrowWhite);
        drawLine(scene, x + 1, y + 7, x + 7, y + 1, arrowWhite);
        drawLine(scene, x + 13, y + 7, x + 7, y + 1, arrowWhite);
        drawLine(scene, x + 20, y + 14, x + 20, y + 25, arrowWhite);
        drawLine(scene, x + 14, y + 19, x + 20, y + 25, arrowWhite);
        drawLine(scene, x + 26, y + 19, x + 20, y + 25, arrowWhite);
    }
}

static bool writeAll(int fd, const char *data, size_t length)
{
    size_t written = 0;
    while (written < length)
    {
        ssize_t result = write(fd, data + written, length - written);
        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (result == 0)
            return false;
        written += (size_t)result;
    }
    return true;
}


static bool loadConfig(char *errorText, size_t errorTextSize)
{
	// If config.json is missing, leave all categories unselected and default to skipMode "confirm".
    int fd = open(CONFIG_PATH, O_RDONLY);
    if (fd < 0)
    {
        if (errno == ENOENT)
        {
            skipModeAuto = false;
            errorText[0] = '\0';
            return true;
        }
        snprintf(errorText, errorTextSize, "config open failed (errno %d)", errno);
        return false;
    }

    std::string contents;
    char buffer[1024];
    for (;;)
    {
        ssize_t bytesRead = read(fd, buffer, sizeof(buffer));
        if (bytesRead == 0)
            break;
        if (bytesRead < 0)
        {
            if (errno == EINTR)
                continue;
            int savedErrno = errno;
            close(fd);
            snprintf(errorText, errorTextSize, "config read failed (errno %d)", savedErrno);
            return false;
        }
        contents.append(buffer, (size_t)bytesRead);
        // The config should never be this big.
        if (contents.size() > 65536)
        {
            close(fd);
            snprintf(errorText, errorTextSize, "config file is unexpectedly large");
            return false;
        }
    }
    if (close(fd) != 0)
    {
        snprintf(errorText, errorTextSize, "config close failed (errno %d)", errno);
        return false;
    }

    // Find the category IDs in the JSON.
    // This avoids depending on an additional JSON library.
    size_t categoriesKey = contents.find("\"categories\"");
    if (categoriesKey == std::string::npos)
    {
        snprintf(errorText, errorTextSize, "config has no categories array");
        return false;
    }
    size_t arrayStart = contents.find('[', categoriesKey);
    size_t arrayEnd = (arrayStart == std::string::npos) ? std::string::npos
                                                        : contents.find(']', arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos)
    {
        snprintf(errorText, errorTextSize, "config categories array is invalid");
        return false;
    }

    for (int i = 0; i < CATEGORY_COUNT; ++i)
        selected[i] = false;

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd)
    {
        size_t quoteStart = contents.find('"', pos);
        if (quoteStart == std::string::npos || quoteStart >= arrayEnd)
            break;
        size_t quoteEnd = contents.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos || quoteEnd > arrayEnd)
        {
            snprintf(errorText, errorTextSize, "config contains an invalid category entry");
            return false;
        }

        std::string id = contents.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        for (int i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (id == categories[i].id)
                selected[i] = true;
        }
        pos = quoteEnd + 1;
    }

    // Optional setting for compatibility with configs created before skipMode existed.
    // Missing or unrecognized values safely default to "confirm".
    skipModeAuto = false;
    size_t skipModeKey = contents.find("\"skipMode\"");
    if (skipModeKey != std::string::npos)
    {
        size_t colon = contents.find(':', skipModeKey + 10);
        size_t valueStart = (colon == std::string::npos) ? std::string::npos
                                                         : contents.find('\"', colon + 1);
        size_t valueEnd = (valueStart == std::string::npos) ? std::string::npos
                                                            : contents.find('\"', valueStart + 1);
        if (valueStart != std::string::npos && valueEnd != std::string::npos)
        {
            std::string mode = contents.substr(valueStart + 1, valueEnd - valueStart - 1);
            skipModeAuto = (mode == "auto");
        }
    }

    errorText[0] = '\0';
    return true;
}

static bool saveConfig(char *errorText, size_t errorTextSize, int *savedCount)
{
    if (mkdir(CONFIG_DIRECTORY, 0777) != 0 && errno != EEXIST)
    {
        snprintf(errorText, errorTextSize, "mkdir failed (errno %d)", errno);
        return false;
    }

    char json[1024];
    size_t used = 0;
    int count = 0;
    for (int i = 0; i < CATEGORY_COUNT; ++i)
        if (selected[i]) ++count;

    int n = snprintf(json, sizeof(json), "{\n  \"categories\": [\n");
    if (n < 0 || (size_t)n >= sizeof(json))
    {
        snprintf(errorText, errorTextSize, "JSON buffer error");
        return false;
    }
    used = (size_t)n;

    int emitted = 0;
    for (int i = 0; i < CATEGORY_COUNT; ++i)
    {
        if (!selected[i])
            continue;
        ++emitted;
        n = snprintf(json + used, sizeof(json) - used,
                     "    \"%s\"%s\n", categories[i].id,
                     emitted < count ? "," : "");
        if (n < 0 || (size_t)n >= sizeof(json) - used)
        {
            snprintf(errorText, errorTextSize, "JSON buffer too small");
            return false;
        }
        used += (size_t)n;
    }

    n = snprintf(json + used, sizeof(json) - used,
                 "  ],\n  \"skipMode\": \"%s\"\n}\n",
                 skipModeAuto ? "auto" : "confirm");
    if (n < 0 || (size_t)n >= sizeof(json) - used)
    {
        snprintf(errorText, errorTextSize, "JSON buffer too small");
        return false;
    }
    used += (size_t)n;

    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        snprintf(errorText, errorTextSize, "open failed (errno %d)", errno);
        return false;
    }

    bool ok = writeAll(fd, json, used);
    int savedErrno = errno;
    if (close(fd) != 0 && ok)
    {
        ok = false;
        savedErrno = errno;
    }
    if (!ok)
    {
        snprintf(errorText, errorTextSize, "write failed (errno %d)", savedErrno);
        return false;
    }

    *savedCount = count;
    errorText[0] = '\0';
    return true;
}

static bool ensureDialogServices(char *errorText, size_t errorTextSize)
{
    // Only initialize the dialog services once.
    static bool messageModuleLoaded = false;
    static bool commonDialogReady = false;
    int rc;

    if (!messageModuleLoaded)
    {
        rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
        if (rc < 0)
        {
            snprintf(errorText, errorTextSize, "Could not load message dialog module (%d)", rc);
            return false;
        }
        messageModuleLoaded = true;
    }

    if (!commonDialogReady)
    {
        rc = sceCommonDialogInitialize();
        if (rc < 0)
        {
            snprintf(errorText, errorTextSize, "Common dialog initialization failed (%d)", rc);
            return false;
        }
        commonDialogReady = true;
    }

    return true;
}

static int askInstallPrx(char *errorText, size_t errorTextSize)
{
    int rc;
    if (!ensureDialogServices(errorText, errorTextSize))
        return -1;

    rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }

    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));

	// Set up the OpenOrbis dialog.
    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER +
                                       (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;

    struct stat existingPrxInfo;
    bool existingPrx = stat("/data/GoldHEN/plugins/yt_adblock.prx", &existingPrxInfo) == 0 &&
                       S_ISREG(existingPrxInfo.st_mode);
    userMsgParam.msg = existingPrx
        ? "PS4 YT AdBlock + SponsorBlock already exists. Do you want to update it?"
        : "Install PS4 YT AdBlock + SponsorBlock?";
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_YESNO_FOCUS_NO;
    param.userMsgParam = &userMsgParam;

    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open install dialog (%d)", rc);
        return -1;
    }

    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Install dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
            usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();

    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read dialog result (%d)", rc);
        return -1;
    }

    errorText[0] = '\0';
    return result.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_YES ? 1 : 0;
}



static int askUninstallConfirmation(char *errorText, size_t errorTextSize)
{
    int rc;
    if (!ensureDialogServices(errorText, errorTextSize))
        return -1;

    rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }

    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));

    // Set up the OpenOrbis dialog.
    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER +
                                       (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;

    userMsgParam.msg =
        "Uninstall PS4 YT AdBlock + SponsorBlock? This will delete the PRX, SponsorBlock config, and this app's plugins.ini entry.";
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_YESNO_FOCUS_NO;
    param.userMsgParam = &userMsgParam;

    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open uninstall dialog (%d)", rc);
        return -1;
    }

    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Uninstall dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
            usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();

    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read uninstall dialog result (%d)", rc);
        return -1;
    }

    errorText[0] = '\0';
    return result.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_YES ? 1 : 0;
}


static int showSuccessDialog(bool wasUpdate, bool usedBundledFallback, char *errorText, size_t errorTextSize)
{
    int rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }

    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));

    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER +
                                       (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    char successMessage[320];
    if (usedBundledFallback)
        snprintf(successMessage, sizeof(successMessage), "%s using bundled PRX. GitHub download failed: %.120s",
                 wasUpdate ? "Update completed" : "Installed", downloadFailureReason);
    else
        snprintf(successMessage, sizeof(successMessage), "%s from the latest GitHub release.",
                 wasUpdate ? "PS4 YT AdBlock + SponsorBlock updated" : "PS4 YT AdBlock + SponsorBlock installed");
    userMsgParam.msg = successMessage;
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    param.userMsgParam = &userMsgParam;

    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open success dialog (%d)", rc);
        return -1;
    }

    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Success dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
            usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read success dialog result (%d)", rc);
        return -1;
    }

    errorText[0] = '\0';
    return result.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_INVALID ? 0 : 1;
}

static int showConfigSavedDialog(char *errorText, size_t errorTextSize)
{
    // Triangle might be pressed before the install dialog, so make sure
    // the dialog services are ready.
    int rc;
    if (!ensureDialogServices(errorText, errorTextSize))
        return -1;

    rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }

    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));

    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER +
                                       (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    userMsgParam.msg = "Configuration saved successfully.";
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    param.userMsgParam = &userMsgParam;

    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open save dialog (%d)", rc);
        return -1;
    }

    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Save dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
            usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read save dialog result (%d)", rc);
        return -1;
    }

    errorText[0] = '\0';
    return result.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_INVALID ? 0 : 1;
}

static bool ensurePluginsIniEntry(char *errorText, size_t errorTextSize, bool *wasAdded)
{
    const char *iniPath = "/data/GoldHEN/plugins.ini";
    const std::string sectionLine = "[CUSA01015]";
    const std::string pluginLine = "/data/GoldHEN/plugins/yt_adblock.prx";
    if (wasAdded)
        *wasAdded = false;

    std::string contents;
    int readFd = open(iniPath, O_RDONLY);
    if (readFd >= 0)
    {
        char buffer[4096];
        for (;;)
        {
            ssize_t bytesRead = read(readFd, buffer, sizeof(buffer));
            if (bytesRead == 0)
                break;
            if (bytesRead < 0)
            {
                if (errno == EINTR) continue;
                int savedErrno = errno;
                close(readFd);
                snprintf(errorText, errorTextSize, "plugins.ini read failed (errno %d)", savedErrno);
                return false;
            }
            contents.append(buffer, (size_t)bytesRead);
        }
        if (close(readFd) != 0)
        {
            snprintf(errorText, errorTextSize, "plugins.ini close failed (errno %d)", errno);
            return false;
        }
    }
    else if (errno != ENOENT)
    {
        snprintf(errorText, errorTextSize, "plugins.ini open failed (errno %d)", errno);
        return false;
    }

    // If the two exact lines are already adjacent, do not modify the file.
    std::string previousLine;
    size_t position = 0;
    while (position < contents.size())
    {
        size_t newline = contents.find('\n', position);
        size_t lineEnd = (newline == std::string::npos) ? contents.size() : newline;
        std::string currentLine = contents.substr(position, lineEnd - position);
        if (!currentLine.empty() && currentLine[currentLine.size() - 1] == '\r')
            currentLine.erase(currentLine.size() - 1);
        if ((previousLine == sectionLine || previousLine == "[CUSA01015]\\") && currentLine == pluginLine)
        {
            errorText[0] = '\0';
            return true;
        }
        previousLine = currentLine;
        if (newline == std::string::npos) break;
        position = newline + 1;
    }

	// Add entry at the end of plugins.ini.
    int appendFd = open(iniPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (appendFd < 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini append open failed (errno %d)", errno);
        return false;
    }

    std::string addition;
    if (!contents.empty() && contents[contents.size() - 1] != '\n')
        addition += "\n";
    addition += "[CUSA01015]\n";
    addition += "/data/GoldHEN/plugins/yt_adblock.prx\n";

    bool ok = writeAll(appendFd, addition.c_str(), addition.size());
    int savedErrno = errno;
    if (ok && fsync(appendFd) != 0)
    {
        ok = false;
        savedErrno = errno;
    }
    if (close(appendFd) != 0 && ok)
    {
        ok = false;
        savedErrno = errno;
    }
    if (!ok)
    {
        snprintf(errorText, errorTextSize, "plugins.ini append failed (errno %d)", savedErrno);
        return false;
    }

    // Verify the appended entry can be read back.
    int verifyFd = open(iniPath, O_RDONLY);
    if (verifyFd < 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini verification open failed (errno %d)", errno);
        return false;
    }
    std::string verifyContents;
    for (;;)
    {
        char buffer[4096];
        ssize_t bytesRead = read(verifyFd, buffer, sizeof(buffer));
        if (bytesRead == 0) break;
        if (bytesRead < 0)
        {
            if (errno == EINTR) continue;
            int savedErrno = errno;
            close(verifyFd);
            snprintf(errorText, errorTextSize, "plugins.ini verification read failed (errno %d)", savedErrno);
            return false;
        }
        verifyContents.append(buffer, (size_t)bytesRead);
    }
    close(verifyFd);

    if (verifyContents.find(sectionLine + "\n" + pluginLine) == std::string::npos &&
        verifyContents.find(sectionLine + "\r\n" + pluginLine) == std::string::npos &&
        verifyContents.find("[CUSA01015]\\\n" + pluginLine) == std::string::npos &&
        verifyContents.find("[CUSA01015]\\\r\n" + pluginLine) == std::string::npos)
    {
        snprintf(errorText, errorTextSize, "plugins.ini write could not be verified");
        return false;
    }

    if (wasAdded) *wasAdded = true;
    errorText[0] = '\0';
    return true;
}

static bool removePluginsIniEntry(char *errorText, size_t errorTextSize)
{
    const char *iniPath = "/data/GoldHEN/plugins.ini";
    const char *tempPath = "/data/GoldHEN/plugins.ini.sponsorblock.tmp";
    const std::string targetSection = "[CUSA01015]";
    const std::string targetPlugin = "/data/GoldHEN/plugins/yt_adblock.prx";

    struct stat originalInfo;
    if (stat(iniPath, &originalInfo) != 0)
    {
        if (errno == ENOENT)
        {
            errorText[0] = '\0';
            return true;
        }
        snprintf(errorText, errorTextSize, "plugins.ini stat failed (errno %d)", errno);
        return false;
    }

    int fd = open(iniPath, O_RDONLY);
    if (fd < 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini open failed (errno %d)", errno);
        return false;
    }

    std::string contents;
    char buffer[4096];
    for (;;)
    {
        ssize_t bytesRead = read(fd, buffer, sizeof(buffer));
        if (bytesRead == 0)
            break;
        if (bytesRead < 0)
        {
            if (errno == EINTR) continue;
            int savedErrno = errno;
            close(fd);
            snprintf(errorText, errorTextSize, "plugins.ini read failed (errno %d)", savedErrno);
            return false;
        }
        contents.append(buffer, (size_t)bytesRead);
    }
    if (close(fd) != 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini close failed (errno %d)", errno);
        return false;
    }

    // Only remove our section if it has no other plugins in it.
    // Leave everything else untouched.
    std::string filtered;
    filtered.reserve(contents.size());
    bool removedEntry = false;
    size_t position = 0;
    while (position < contents.size())
    {
        size_t sectionStart = position;
        size_t sectionEnd = contents.size();
        size_t nextPosition = position;
        bool isTargetSection = false;
        bool sectionHasOtherEntries = false;
        bool sectionHasPluginEntry = false;

        size_t firstNewline = contents.find('\n', position);
        size_t firstEnd = (firstNewline == std::string::npos) ? contents.size() : firstNewline + 1;
        std::string firstLine = contents.substr(position, firstEnd - position);
        std::string firstCompare = firstLine;
        if (!firstCompare.empty() && firstCompare[firstCompare.size() - 1] == '\n')
            firstCompare.erase(firstCompare.size() - 1);
        if (!firstCompare.empty() && firstCompare[firstCompare.size() - 1] == '\r')
            firstCompare.erase(firstCompare.size() - 1);

        if (!firstCompare.empty() && firstCompare[0] == '[')
        {
            isTargetSection = (firstCompare == targetSection || firstCompare == "[CUSA01015]\\");
            nextPosition = firstEnd;
            while (nextPosition < contents.size())
            {
                size_t newline = contents.find('\n', nextPosition);
                size_t end = (newline == std::string::npos) ? contents.size() : newline + 1;
                std::string line = contents.substr(nextPosition, end - nextPosition);
                std::string compareLine = line;
                if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\n')
                    compareLine.erase(compareLine.size() - 1);
                if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\r')
                    compareLine.erase(compareLine.size() - 1);

                if (!compareLine.empty() && compareLine[0] == '[')
                {
                    sectionEnd = nextPosition;
                    break;
                }
                if (isTargetSection && !compareLine.empty())
                {
                    if (compareLine == targetPlugin)
                        sectionHasPluginEntry = true;
                    else
                        sectionHasOtherEntries = true;
                }
                nextPosition = end;
            }
            if (sectionEnd == contents.size())
                nextPosition = contents.size();
        }
        else
        {
            // Preserve lines before the first section header.
            nextPosition = firstEnd;
        }

        if (isTargetSection && sectionHasPluginEntry && !sectionHasOtherEntries)
        {
            removedEntry = true;
            // Drop the entire now-empty section, including its header.
        }
        else
        {
            filtered.append(contents, sectionStart, nextPosition - sectionStart);
            if (isTargetSection && sectionHasPluginEntry)
                removedEntry = true;
        }

        position = nextPosition;
        if (position <= sectionStart)
            break;
    }

    if (!removedEntry)
    {
        errorText[0] = '\0';
        return true;
    }

    int tempFd = open(tempPath, O_WRONLY | O_CREAT | O_TRUNC, originalInfo.st_mode & 0777);
    if (tempFd < 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini temp open failed (errno %d)", errno);
        return false;
    }

    bool ok = fchmod(tempFd, originalInfo.st_mode & 0777) == 0 &&
              writeAll(tempFd, filtered.c_str(), filtered.size());
    int savedErrno = errno;
    if (ok && fsync(tempFd) != 0)
    {
        ok = false;
        savedErrno = errno;
    }
    if (close(tempFd) != 0 && ok)
    {
        ok = false;
        savedErrno = errno;
    }
    if (!ok)
    {
        unlink(tempPath);
        snprintf(errorText, errorTextSize, "plugins.ini rewrite failed (errno %d)", savedErrno);
        return false;
    }

    if (rename(tempPath, iniPath) != 0)
    {
        int renameErrno = errno;
        unlink(tempPath);
        snprintf(errorText, errorTextSize, "plugins.ini replace failed (errno %d)", renameErrno);
        return false;
    }

    errorText[0] = '\0';
    return true;
}

static bool removeIfPresent(const char *path, char *errorText, size_t errorTextSize)
{
    if (unlink(path) == 0 || errno == ENOENT)
        return true;
    snprintf(errorText, errorTextSize, "Could not delete %.70s (errno %d)", path, errno);
    return false;
}

static bool uninstallManagedFiles(char *errorText, size_t errorTextSize)
{
    char iniError[160] = "";
    char prxError[160] = "";
    char configError[160] = "";
    bool iniOk = removePluginsIniEntry(iniError, sizeof(iniError));
    bool prxOk = removeIfPresent("/data/GoldHEN/plugins/yt_adblock.prx", prxError, sizeof(prxError));
    bool configOk = removeIfPresent(CONFIG_PATH, configError, sizeof(configError));

    if (!iniOk || !prxOk || !configOk)
    {
        const char *first = !iniOk ? iniError : (!prxOk ? prxError : configError);
        snprintf(errorText, errorTextSize, "%s", first);
        return false;
    }

    errorText[0] = '\0';
    return true;
}

// The latest download URL redirects to the actual PRX file.
static const char *LATEST_PRX_URL =
    "https://github.com/FourZeroCode/yt_adblock_sponsorblock_ps4/releases/latest/download/yt_adblock.prx";
static const char *DOWNLOADED_PRX_PATH =
    "/data/GoldHEN/plugins/yt_adblock.prx.download";

// Only accept certificates that pass verification.
static int verifyGitHubCertificate(int libsslId, unsigned int verifyErr,
                                   void * const sslCert[], int certNum, void *userArg)
{
    (void)libsslId;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    return verifyErr == 0 ? 1 : 0;
}

static bool extractRedirectLocation(int requestId, char *location, size_t locationSize)
{
    char *headers = NULL;
    size_t headerSize = 0;
    if (sceHttpGetAllResponseHeaders(requestId, &headers, &headerSize) < 0 ||
        headers == NULL || headerSize == 0)
        return false;

    // Headers are not guaranteed to end with a null byte.
    size_t pos = 0;
    while (pos < headerSize)
    {
        size_t lineStart = pos;
        while (pos < headerSize && headers[pos] != '\n') ++pos;
        size_t lineEnd = pos;
        if (pos < headerSize) ++pos;
        if (lineEnd > lineStart && headers[lineEnd - 1] == '\r') --lineEnd;
        static const char key[] = "location:";
        if (lineEnd - lineStart >= sizeof(key) - 1)
        {
            bool match = true;
            for (size_t i = 0; i < sizeof(key) - 1; ++i)
            {
                char c = headers[lineStart + i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != key[i]) { match = false; break; }
            }
            if (match)
            {
                size_t valueStart = lineStart + sizeof(key) - 1;
                while (valueStart < lineEnd &&
                       (headers[valueStart] == ' ' || headers[valueStart] == '\t'))
                    ++valueStart;
                size_t valueLength = lineEnd - valueStart;
                if (valueLength == 0 || valueLength >= locationSize)
                    return false;
                memcpy(location, headers + valueStart, valueLength);
                location[valueLength] = '\0';
                // HTTPS redirects only.
                return strncmp(location, "https://", 8) == 0;
            }
        }
    }
    return false;
}

static bool downloadLatestPrx(const char *outputPath)
{
    int netPoolId = -1;
    int sslContextId = -1;
    int httpContextId = -1;
    int templateId = -1;
    int connectionId = -1;
    int requestId = -1;
    int outputFd = -1;
    int statusCode = 0;
    int contentLengthType = 0;
    typedef int32_t (*HttpSetResponseHeaderMaxSizeFn)(int32_t, uint32_t);
    HttpSetResponseHeaderMaxSizeFn setResponseHeaderMaxSize =
        reinterpret_cast<HttpSetResponseHeaderMaxSizeFn>(&sceHttpSetResponseHeaderMaxSize);
    int headerLimitResult = -1;
    size_t contentLength = 0;
    uint64_t totalRead = 0;
    bool ok = false;
    bool gotFinalResponse = false;
    char buffer[16384];
    char currentUrl[2048];

    snprintf(downloadFailureReason, sizeof(downloadFailureReason), "starting download");
    snprintf(currentUrl, sizeof(currentUrl), "%s", LATEST_PRX_URL);
    unlink(outputPath);

    snprintf(downloadFailureReason, sizeof(downloadFailureReason), "loading network modules");
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0)
        goto cleanup;

    // Network setup for PRX download.
    sceNetInit();
    netPoolId = sceNetPoolCreate("SponsorBlockConfigNet", 16 * 1024, 0);
    if (netPoolId < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "network pool init failed (%d)", netPoolId);
        goto cleanup;
    }

    sslContextId = sceSslInit(512 * 1024);
    if (sslContextId < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "SSL init failed (%d)", sslContextId);
        goto cleanup;
    }

    httpContextId = sceHttpInit(netPoolId, sslContextId, 512 * 1024);
    if (httpContextId < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "HTTP init failed (%d)", httpContextId);
        goto cleanup;
    }

    templateId = sceHttpCreateTemplate(httpContextId, "SponsorBlockConfig/1.1",
                                       ORBIS_HTTP_VERSION_1_1, 1);
    if (templateId < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "HTTP template failed (%d)", templateId);
        goto cleanup;
    }
    if (sceHttpsSetSslCallback(templateId, verifyGitHubCertificate, NULL) < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "HTTPS certificate callback failed");
        goto cleanup;
    }

    // OpenOrbis Http.h declares this without parameters, but the actual
    // function takes an ID and size. Use the correct signature here.
    // Increase the header limit from 5 KB to 64 KB.
    // GitHub redirects sometimes have large headers.
    headerLimitResult = setResponseHeaderMaxSize(templateId, 64 * 1024);
    if (headerLimitResult < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason),
                 "HTTP header limit setup failed (%d)", headerLimitResult);
        goto cleanup;
    }

    // Auto-redirect is declared incorrectly in OpenOrbis,
    // so follow the GitHub redirects.
    for (int redirectCount = 0; redirectCount <= 5; ++redirectCount)
    {
        connectionId = sceHttpCreateConnectionWithURL(templateId, currentUrl, 1);
        if (connectionId < 0)
        {
            snprintf(downloadFailureReason, sizeof(downloadFailureReason), "HTTP connection failed (%d)", connectionId);
            goto cleanup;
        }
        requestId = sceHttpCreateRequestWithURL(connectionId, ORBIS_METHOD_GET, currentUrl, 0);
        if (requestId < 0)
        {
            snprintf(downloadFailureReason, sizeof(downloadFailureReason), "HTTP request failed (%d)", requestId);
            goto cleanup;
        }
        if (sceHttpSendRequest(requestId, NULL, 0) < 0)
        {
            int err = 0;
            sceHttpGetLastErrno(requestId, &err);

            // 0x80431073 means the PS4 HTTP stack rejected an oversized response
            // header. Let the installer use the bundled PRX if the download fails.
            if ((unsigned int)err == 0x80431073U)
                snprintf(downloadFailureReason, sizeof(downloadFailureReason),
                         "response header too large (0x80431073)");
            else
                snprintf(downloadFailureReason, sizeof(downloadFailureReason),
                         "HTTP send failed (%d)", err);
            goto cleanup;
        }
        if (sceHttpGetStatusCode(requestId, &statusCode) < 0)
        {
            snprintf(downloadFailureReason, sizeof(downloadFailureReason), "could not read HTTP status");
            goto cleanup;
        }

        if (statusCode == 200)
        {
            gotFinalResponse = true;
            break;
        }
        if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
            statusCode == 307 || statusCode == 308)
        {
            char redirectUrl[2048];
            if (redirectCount == 5 || !extractRedirectLocation(requestId, redirectUrl, sizeof(redirectUrl)))
            {
                snprintf(downloadFailureReason, sizeof(downloadFailureReason), "redirect %d had no safe Location", statusCode);
                goto cleanup;
            }
            sceHttpDeleteRequest(requestId);
            requestId = -1;
            sceHttpDeleteConnection(connectionId);
            connectionId = -1;
            snprintf(currentUrl, sizeof(currentUrl), "%s", redirectUrl);
            continue;
        }
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "GitHub returned HTTP %d", statusCode);
        goto cleanup;
    }
    if (!gotFinalResponse)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "redirect limit exceeded");
        goto cleanup;
    }

    if (sceHttpGetResponseContentLength(requestId, &contentLengthType, &contentLength) == 0 &&
        contentLength > 0 && (contentLength < 65536 || contentLength > 20 * 1024 * 1024))
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "unexpected PRX size (%lu bytes)", (unsigned long)contentLength);
        goto cleanup;
    }

    outputFd = open(outputPath, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (outputFd < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "cannot create download file (errno %d)", errno);
        goto cleanup;
    }

    for (;;)
    {
        int bytesRead = sceHttpReadData(requestId, buffer, sizeof(buffer));
        if (bytesRead == 0) break;
        if (bytesRead < 0)
        {
            snprintf(downloadFailureReason, sizeof(downloadFailureReason), "HTTP read failed (%d)", bytesRead);
            goto cleanup;
        }
        if (totalRead + (uint64_t)bytesRead > 20 * 1024 * 1024)
        {
            snprintf(downloadFailureReason, sizeof(downloadFailureReason), "download exceeded size limit");
            goto cleanup;
        }
        if (!writeAll(outputFd, buffer, (size_t)bytesRead))
        {
            snprintf(downloadFailureReason, sizeof(downloadFailureReason), "writing download failed (errno %d)", errno);
            goto cleanup;
        }
        totalRead += (uint64_t)bytesRead;
    }

    if (totalRead < 65536 || (contentLength > 0 && totalRead != contentLength))
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "download size check failed (%lu bytes)", (unsigned long)totalRead);
        goto cleanup;
    }
    if (fsync(outputFd) != 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "download flush failed (errno %d)", errno);
        goto cleanup;
    }
    if (close(outputFd) != 0)
    {
        outputFd = -1;
        snprintf(downloadFailureReason, sizeof(downloadFailureReason), "download close failed (errno %d)", errno);
        goto cleanup;
    }
    outputFd = -1;
    ok = true;
    snprintf(downloadFailureReason, sizeof(downloadFailureReason), "downloaded successfully");

cleanup:
    if (outputFd >= 0) close(outputFd);
    if (requestId >= 0) sceHttpDeleteRequest(requestId);
    if (connectionId >= 0) sceHttpDeleteConnection(connectionId);
    if (templateId >= 0) sceHttpDeleteTemplate(templateId);
    if (httpContextId >= 0) sceHttpTerm(httpContextId);
    if (sslContextId >= 0) sceSslTerm();
    if (netPoolId >= 0) sceNetPoolDestroy(netPoolId);

    if (!ok) unlink(outputPath);
    return ok;
}

static bool jsonReadString(const std::string &json, size_t quotePosition, std::string &value)
{
    if (quotePosition >= json.size() || json[quotePosition] != '"')
        return false;

    value.clear();
    for (size_t i = quotePosition + 1; i < json.size(); ++i)
    {
        char c = json[i];
        if (c == '"')
            return true;
        if (c == '\\')
        {
            if (++i >= json.size()) return false;
            char escaped = json[i];
            if (escaped == '"' || escaped == '\\' || escaped == '/') value += escaped;
            else if (escaped == 'n') value += '\n';
            else if (escaped == 'r') value += '\r';
            else if (escaped == 't') value += '\t';
            else return false;
        }
        else
        {
            value += c;
        }
    }
    return false;
}

static bool jsonGetString(const std::string &json, const char *key, std::string &value)
{
    std::string quotedKey = "\"";
    quotedKey += key;
    quotedKey += "\"";
    size_t position = json.find(quotedKey);
    if (position == std::string::npos) return false;
    position = json.find(':', position + quotedKey.size());
    if (position == std::string::npos) return false;
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\n' ||
            json[position] == '\r' || json[position] == '\t'))
        ++position;
    return jsonReadString(json, position, value);
}

static bool parseReleaseVersion(const std::string &version, unsigned int parts[3])
{
    size_t position = 0;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
        position = 1;

    parts[0] = parts[1] = parts[2] = 0;
    for (int part = 0; part < 2; ++part)
    {
        if (position >= version.size() || version[position] < '0' || version[position] > '9')
            return false;
        unsigned int value = 0;
        while (position < version.size() && version[position] >= '0' && version[position] <= '9')
        {
            unsigned int digit = (unsigned int)(version[position] - '0');
            if (value > (0xFFFFFFFFU - digit) / 10U) return false;
            value = value * 10U + digit;
            ++position;
        }
        parts[part] = value;
        if (part == 1 && position == version.size())
            return true;
        if (position >= version.size() || version[position] != '.') return false;
        ++position;
    }

    if (position == version.size())
        return false;

    if (version[position] < '0' || version[position] > '9')
        return false;
    unsigned int patch = 0;
    while (position < version.size() && version[position] >= '0' && version[position] <= '9')
    {
        unsigned int digit = (unsigned int)(version[position] - '0');
        if (patch > (0xFFFFFFFFU - digit) / 10U) return false;
        patch = patch * 10U + digit;
        ++position;
    }
    if (position != version.size()) return false;
    parts[2] = patch;
    return true;
}

static bool isVersionNewer(const std::string &remoteVersion)
{
    unsigned int localParts[3];
    unsigned int remoteParts[3];
    if (!parseReleaseVersion(APP_RELEASE_VERSION, localParts) ||
        !parseReleaseVersion(remoteVersion, remoteParts))
        return false;

    for (int i = 0; i < 3; ++i)
    {
        if (remoteParts[i] > localParts[i]) return true;
        if (remoteParts[i] < localParts[i]) return false;
    }
    return false;
}

static bool extractPkgAsset(const std::string &json, std::string &downloadUrl)
{
    size_t assetsKey = json.find("\"assets\"");
    if (assetsKey == std::string::npos) return false;
    size_t arrayStart = json.find('[', assetsKey);
    if (arrayStart == std::string::npos) return false;

    bool inString = false;
    bool escaped = false;
    int objectDepth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < json.size(); ++i)
    {
        char c = json[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"')
        {
            inString = true;
            continue;
        }
        if (c == '{')
        {
            if (objectDepth == 0) objectStart = i;
            ++objectDepth;
        }
        else if (c == '}' && objectDepth > 0)
        {
            --objectDepth;
            if (objectDepth == 0 && objectStart != std::string::npos)
            {
                std::string asset = json.substr(objectStart, i - objectStart + 1);
                std::string name;
                std::string url;
                if (jsonGetString(asset, "name", name) &&
                    jsonGetString(asset, "browser_download_url", url) &&
                    name.size() >= 4 &&
                    (name.compare(name.size() - 4, 4, ".pkg") == 0 ||
                     name.compare(name.size() - 4, 4, ".PKG") == 0) &&
                    url.compare(0, 8, "https://") == 0)
                {
                    downloadUrl = url;
                    return true;
                }
                objectStart = std::string::npos;
            }
        }
        else if (c == ']' && objectDepth == 0)
        {
            break;
        }
    }
    return false;
}

static bool httpGetToString(const char *url, std::string &response,
                            char *errorText, size_t errorTextSize)
{
    int netPoolId = -1;
    int sslContextId = -1;
    int httpContextId = -1;
    int templateId = -1;
    int connectionId = -1;
    int requestId = -1;
    int statusCode = 0;
    bool ok = false;
    bool gotFinalResponse = false;
    typedef int32_t (*HttpSetResponseHeaderMaxSizeFn)(int32_t, uint32_t);
    HttpSetResponseHeaderMaxSizeFn setResponseHeaderMaxSize =
        reinterpret_cast<HttpSetResponseHeaderMaxSizeFn>(&sceHttpSetResponseHeaderMaxSize);
    char currentUrl[2048];
    char buffer[4096];
    response.clear();
    snprintf(currentUrl, sizeof(currentUrl), "%s", url);

    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0)
    {
        snprintf(errorText, errorTextSize, "network modules could not be loaded");
        goto cleanup;
    }
    sceNetInit();
    netPoolId = sceNetPoolCreate("SponsorBlockUpdateNet", 16 * 1024, 0);
    if (netPoolId < 0)
    {
        snprintf(errorText, errorTextSize, "network pool failed (%d)", netPoolId);
        goto cleanup;
    }
    sslContextId = sceSslInit(512 * 1024);
    if (sslContextId < 0)
    {
        snprintf(errorText, errorTextSize, "SSL init failed (%d)", sslContextId);
        goto cleanup;
    }
    httpContextId = sceHttpInit(netPoolId, sslContextId, 512 * 1024);
    if (httpContextId < 0)
    {
        snprintf(errorText, errorTextSize, "HTTP init failed (%d)", httpContextId);
        goto cleanup;
    }
    templateId = sceHttpCreateTemplate(httpContextId, "SponsorBlockConfig-Updater/1.0",
                                       ORBIS_HTTP_VERSION_1_1, 1);
    if (templateId < 0 || sceHttpsSetSslCallback(templateId, verifyGitHubCertificate, NULL) < 0)
    {
        snprintf(errorText, errorTextSize, "HTTP template or certificate setup failed");
        goto cleanup;
    }
    if (setResponseHeaderMaxSize(templateId, 64 * 1024) < 0)
    {
        snprintf(errorText, errorTextSize, "HTTP response header limit setup failed");
        goto cleanup;
    }

    for (int redirectCount = 0; redirectCount <= 5; ++redirectCount)
    {
        connectionId = sceHttpCreateConnectionWithURL(templateId, currentUrl, 1);
        if (connectionId < 0)
        {
            snprintf(errorText, errorTextSize, "HTTP connection failed (%d)", connectionId);
            goto cleanup;
        }
        requestId = sceHttpCreateRequestWithURL(connectionId, ORBIS_METHOD_GET, currentUrl, 0);
        if (requestId < 0)
        {
            snprintf(errorText, errorTextSize, "HTTP request failed (%d)", requestId);
            goto cleanup;
        }
        if (sceHttpAddRequestHeader(requestId, "User-Agent", "SponsorBlockConfig-Updater", 0) < 0 ||
            sceHttpAddRequestHeader(requestId, "Accept", "application/vnd.github+json", 0) < 0)
        {
            snprintf(errorText, errorTextSize, "could not set GitHub API request headers");
            goto cleanup;
        }
        if (sceHttpSendRequest(requestId, NULL, 0) < 0)
        {
            int httpError = 0;
            sceHttpGetLastErrno(requestId, &httpError);
            snprintf(errorText, errorTextSize, "HTTP send failed (%d)", httpError);
            goto cleanup;
        }
        if (sceHttpGetStatusCode(requestId, &statusCode) < 0)
        {
            snprintf(errorText, errorTextSize, "could not read HTTP status");
            goto cleanup;
        }
        if (statusCode == 200)
        {
            gotFinalResponse = true;
            break;
        }
        if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
            statusCode == 307 || statusCode == 308)
        {
            char redirectUrl[2048];
            if (redirectCount == 5 || !extractRedirectLocation(requestId, redirectUrl, sizeof(redirectUrl)))
            {
                snprintf(errorText, errorTextSize, "unsafe or excessive HTTP redirects");
                goto cleanup;
            }
            sceHttpDeleteRequest(requestId);
            requestId = -1;
            sceHttpDeleteConnection(connectionId);
            connectionId = -1;
            snprintf(currentUrl, sizeof(currentUrl), "%s", redirectUrl);
            continue;
        }
        snprintf(errorText, errorTextSize, "GitHub returned HTTP %d", statusCode);
        goto cleanup;
    }
    if (!gotFinalResponse)
    {
        snprintf(errorText, errorTextSize, "HTTP redirect limit exceeded");
        goto cleanup;
    }

    for (;;)
    {
        int bytesRead = sceHttpReadData(requestId, buffer, sizeof(buffer));
        if (bytesRead == 0) break;
        if (bytesRead < 0)
        {
            snprintf(errorText, errorTextSize, "release information read failed (%d)", bytesRead);
            goto cleanup;
        }
        if (response.size() + (size_t)bytesRead > 256 * 1024)
        {
            snprintf(errorText, errorTextSize, "GitHub release response was too large");
            goto cleanup;
        }
        response.append(buffer, (size_t)bytesRead);
    }
    if (response.empty())
    {
        snprintf(errorText, errorTextSize, "GitHub returned an empty release response");
        goto cleanup;
    }
    ok = true;
    errorText[0] = '\0';

cleanup:
    if (requestId >= 0) sceHttpDeleteRequest(requestId);
    if (connectionId >= 0) sceHttpDeleteConnection(connectionId);
    if (templateId >= 0) sceHttpDeleteTemplate(templateId);
    if (httpContextId >= 0) sceHttpTerm(httpContextId);
    if (sslContextId >= 0) sceSslTerm();
    if (netPoolId >= 0) sceNetPoolDestroy(netPoolId);
    return ok;
}

// Native PS4 progress dialog used only by the SponsorBlockConfig PKG updater.
static bool openAppUpdateProgressDialog(char *errorText, size_t errorTextSize)
{
    if (!ensureDialogServices(errorText, errorTextSize))
        return false;

    int rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Progress dialog initialization failed (%d)", rc);
        return false;
    }

    OrbisMsgDialogParam param;
    OrbisMsgDialogProgressBarParam progressParam;
    memset(&param, 0, sizeof(param));
    memset(&progressParam, 0, sizeof(progressParam));

    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER +
                                       (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_PROGRESS_BAR;
    progressParam.barType = ORBIS_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    progressParam.msg = "Preparing SponsorBlockConfig update...";
    param.progBarParam = &progressParam;

    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open progress dialog (%d)", rc);
        return false;
    }

    sceMsgDialogProgressBarSetMsg(ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT,
                                  "Preparing SponsorBlockConfig update...");
    sceMsgDialogProgressBarSetValue(ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, 0);
    return true;
}

static void updateAppUpdateProgress(const char *message, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    sceMsgDialogProgressBarSetMsg(ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, message);
    sceMsgDialogProgressBarSetValue(ORBIS_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, percent);
}

static void closeAppUpdateProgressDialog(void)
{
    sceMsgDialogClose();
    OrbisCommonDialogStatus status;
    do
    {
        status = sceMsgDialogUpdateStatus();
        if (status != ORBIS_COMMON_DIALOG_STATUS_FINISHED && (int)status >= 0)
            usleep(10000);
        else
            break;
    } while (status != ORBIS_COMMON_DIALOG_STATUS_FINISHED);
    sceMsgDialogTerminate();
}

static bool downloadAppUpdatePkg(const char *url, char *errorText, size_t errorTextSize)
{
    int netPoolId = -1;
    int sslContextId = -1;
    int httpContextId = -1;
    int templateId = -1;
    int connectionId = -1;
    int requestId = -1;
    int outputFd = -1;
    int statusCode = 0;
    int contentLengthType = 0;
    size_t contentLength = 0;
    uint64_t totalRead = 0;
    int lastProgressPercent = 10;
    bool ok = false;
    bool gotFinalResponse = false;
    typedef int32_t (*HttpSetResponseHeaderMaxSizeFn)(int32_t, uint32_t);
    HttpSetResponseHeaderMaxSizeFn setResponseHeaderMaxSize =
        reinterpret_cast<HttpSetResponseHeaderMaxSizeFn>(&sceHttpSetResponseHeaderMaxSize);
    char currentUrl[2048];
    char buffer[16384];
    unlink(APP_UPDATE_TEMP_PATH);
    snprintf(currentUrl, sizeof(currentUrl), "%s", url);

    updateAppUpdateProgress("Initializing network...", 2);
    if (mkdir("/data/pkg", 0777) != 0 && errno != EEXIST)
    {
        snprintf(errorText, errorTextSize, "could not create /data/pkg (errno %d)", errno);
        goto cleanup;
    }
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0)
    {
        snprintf(errorText, errorTextSize, "network modules could not be loaded");
        goto cleanup;
    }
    sceNetInit();
    netPoolId = sceNetPoolCreate("SponsorBlockPkgUpdateNet", 16 * 1024, 0);
    if (netPoolId < 0)
    {
        snprintf(errorText, errorTextSize, "network pool failed (%d)", netPoolId);
        goto cleanup;
    }
    sslContextId = sceSslInit(512 * 1024);
    if (sslContextId < 0)
    {
        snprintf(errorText, errorTextSize, "SSL init failed (%d)", sslContextId);
        goto cleanup;
    }
    httpContextId = sceHttpInit(netPoolId, sslContextId, 512 * 1024);
    if (httpContextId < 0)
    {
        snprintf(errorText, errorTextSize, "HTTP init failed (%d)", httpContextId);
        goto cleanup;
    }
    templateId = sceHttpCreateTemplate(httpContextId, "SponsorBlockConfig-Updater/1.0",
                                       ORBIS_HTTP_VERSION_1_1, 1);
    if (templateId < 0 || sceHttpsSetSslCallback(templateId, verifyGitHubCertificate, NULL) < 0)
    {
        snprintf(errorText, errorTextSize, "HTTP template or certificate setup failed");
        goto cleanup;
    }
    if (setResponseHeaderMaxSize(templateId, 64 * 1024) < 0)
    {
        snprintf(errorText, errorTextSize, "HTTP response header limit setup failed");
        goto cleanup;
    }

    for (int redirectCount = 0; redirectCount <= 5; ++redirectCount)
    {
        connectionId = sceHttpCreateConnectionWithURL(templateId, currentUrl, 1);
        if (connectionId < 0)
        {
            snprintf(errorText, errorTextSize, "HTTP connection failed (%d)", connectionId);
            goto cleanup;
        }
        requestId = sceHttpCreateRequestWithURL(connectionId, ORBIS_METHOD_GET, currentUrl, 0);
        if (requestId < 0)
        {
            snprintf(errorText, errorTextSize, "HTTP request failed (%d)", requestId);
            goto cleanup;
        }
        if (sceHttpSendRequest(requestId, NULL, 0) < 0)
        {
            int httpError = 0;
            sceHttpGetLastErrno(requestId, &httpError);
            snprintf(errorText, errorTextSize, "PKG download request failed (%d)", httpError);
            goto cleanup;
        }
        if (sceHttpGetStatusCode(requestId, &statusCode) < 0)
        {
            snprintf(errorText, errorTextSize, "could not read PKG download status");
            goto cleanup;
        }
        if (statusCode == 200)
        {
            gotFinalResponse = true;
            break;
        }
        if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
            statusCode == 307 || statusCode == 308)
        {
            char redirectUrl[2048];
            if (redirectCount == 5 || !extractRedirectLocation(requestId, redirectUrl, sizeof(redirectUrl)))
            {
                snprintf(errorText, errorTextSize, "unsafe or excessive PKG download redirects");
                goto cleanup;
            }
            sceHttpDeleteRequest(requestId);
            requestId = -1;
            sceHttpDeleteConnection(connectionId);
            connectionId = -1;
            snprintf(currentUrl, sizeof(currentUrl), "%s", redirectUrl);
            continue;
        }
        snprintf(errorText, errorTextSize, "GitHub returned HTTP %d for the PKG", statusCode);
        goto cleanup;
    }
    if (!gotFinalResponse)
    {
        snprintf(errorText, errorTextSize, "PKG download redirect limit exceeded");
        goto cleanup;
    }

    if (sceHttpGetResponseContentLength(requestId, &contentLengthType, &contentLength) == 0 &&
        contentLength > 0 && contentLength > 512ULL * 1024ULL * 1024ULL)
    {
        snprintf(errorText, errorTextSize, "PKG file is larger than the 512 MB safety limit");
        goto cleanup;
    }
    updateAppUpdateProgress("Preparing update file...", 10);
    outputFd = open(APP_UPDATE_TEMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outputFd < 0)
    {
        snprintf(errorText, errorTextSize, "could not create PKG download (errno %d)", errno);
        goto cleanup;
    }
    updateAppUpdateProgress("Downloading SponsorBlockConfig update...", 10);
    for (;;)
    {
        int bytesRead = sceHttpReadData(requestId, buffer, sizeof(buffer));
        if (bytesRead == 0) break;
        if (bytesRead < 0)
        {
            snprintf(errorText, errorTextSize, "PKG download read failed (%d)", bytesRead);
            goto cleanup;
        }
        if (totalRead + (uint64_t)bytesRead > 512ULL * 1024ULL * 1024ULL)
        {
            snprintf(errorText, errorTextSize, "PKG download exceeded the 512 MB safety limit");
            goto cleanup;
        }
        if (!writeAll(outputFd, buffer, (size_t)bytesRead))
        {
            snprintf(errorText, errorTextSize, "could not write PKG download (errno %d)", errno);
            goto cleanup;
        }
        totalRead += (uint64_t)bytesRead;
        if (contentLength > 0)
        {
            int percent = 10 + (int)((totalRead * 85ULL) / contentLength);
            char progressMessage[160];
            if (percent > 95) percent = 95;
            snprintf(progressMessage, sizeof(progressMessage),
                     "Downloading... %llu / %llu MB",
                     (unsigned long long)(totalRead / (1024ULL * 1024ULL)),
                     (unsigned long long)(contentLength / (1024ULL * 1024ULL)));
            if (percent != lastProgressPercent)
            {
                updateAppUpdateProgress(progressMessage, percent);
                lastProgressPercent = percent;
            }
        }
        else
        {
            static const uint64_t PROGRESS_STEP_BYTES = 1024ULL * 1024ULL;
            if (totalRead / PROGRESS_STEP_BYTES > (totalRead - (uint64_t)bytesRead) / PROGRESS_STEP_BYTES)
            {
                char progressMessage[128];
                snprintf(progressMessage, sizeof(progressMessage),
                         "Downloading... %llu MB",
                         (unsigned long long)(totalRead / PROGRESS_STEP_BYTES));
                updateAppUpdateProgress(progressMessage, 50);
            }
        }
    }
    if (totalRead < 4096 || (contentLength > 0 && totalRead != contentLength))
    {
        snprintf(errorText, errorTextSize, "PKG download size check failed (%lu bytes)", (unsigned long)totalRead);
        goto cleanup;
    }
    updateAppUpdateProgress("Verifying downloaded PKG...", 96);
    if (fsync(outputFd) != 0)
    {
        snprintf(errorText, errorTextSize, "could not flush PKG download (errno %d)", errno);
        goto cleanup;
    }
    if (close(outputFd) != 0)
    {
        outputFd = -1;
        snprintf(errorText, errorTextSize, "could not close PKG download (errno %d)", errno);
        goto cleanup;
    }
    outputFd = -1;

    {
        int verifyFd = open(APP_UPDATE_TEMP_PATH, O_RDONLY);
        unsigned char magic[4];
        ssize_t magicRead = verifyFd >= 0 ? read(verifyFd, magic, sizeof(magic)) : -1;
        if (verifyFd >= 0) close(verifyFd);
        if (magicRead != 4 || magic[0] != 0x7F || magic[1] != 'C' ||
            magic[2] != 'N' || magic[3] != 'T')
        {
            snprintf(errorText, errorTextSize, "downloaded file does not have a valid PS4 PKG header");
            goto cleanup;
        }
    }
    updateAppUpdateProgress("Finalizing update...", 99);
    if (rename(APP_UPDATE_TEMP_PATH, APP_UPDATE_PKG_PATH) != 0)
    {
        snprintf(errorText, errorTextSize, "could not finalize PKG download (errno %d)", errno);
        goto cleanup;
    }
    ok = true;
    updateAppUpdateProgress("Update downloaded successfully!", 100);
    errorText[0] = '\0';

cleanup:
    if (outputFd >= 0) close(outputFd);
    if (requestId >= 0) sceHttpDeleteRequest(requestId);
    if (connectionId >= 0) sceHttpDeleteConnection(connectionId);
    if (templateId >= 0) sceHttpDeleteTemplate(templateId);
    if (httpContextId >= 0) sceHttpTerm(httpContextId);
    if (sslContextId >= 0) sceSslTerm();
    if (netPoolId >= 0) sceNetPoolDestroy(netPoolId);
    if (!ok) unlink(APP_UPDATE_TEMP_PATH);
    return ok;
}

static bool checkForAppUpdate(std::string &latestVersion, std::string &pkgUrl)
{
    std::string response;
    char errorText[160];
    if (!httpGetToString(LATEST_APP_RELEASE_API_URL, response, errorText, sizeof(errorText)))
        return false;
    unsigned int parsedParts[3];
    if (!jsonGetString(response, "tag_name", latestVersion) ||
        !parseReleaseVersion(latestVersion, parsedParts))
        return false;
    if (!isVersionNewer(latestVersion))
    {
        // The installed app is already up to date.
        // remove update PKG so it does not remain in /data/pkg/.
        unlink(APP_UPDATE_PKG_PATH);
        return false;
    }
    return extractPkgAsset(response, pkgUrl);
}

static int askDownloadAppUpdate(const char *latestVersion, char *errorText, size_t errorTextSize)
{
    if (!ensureDialogServices(errorText, errorTextSize)) return -1;
    int rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }

    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));
    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;

    char prompt[256];
    snprintf(prompt, sizeof(prompt),
             "A new version is available: %s You are currently using v%s Would you like to download the update now?",
             latestVersion, APP_RELEASE_VERSION);
    userMsgParam.msg = prompt;
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_YESNO_FOCUS_NO;
    param.userMsgParam = &userMsgParam;

    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open update dialog (%d)", rc);
        return -1;
    }
    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Update dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED) usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);

    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read update dialog result (%d)", rc);
        return -1;
    }
    errorText[0] = '\0';
    return result.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_YES ? 1 : 0;
}

static int showAppUpdateDownloadedDialog(char *errorText, size_t errorTextSize)
{
    int rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));
    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    userMsgParam.msg = "Update downloaded successfully!\n\nTo install the update:\n1. Open GoldHEN > Debug Settings.\n2. Under Package Installer, disable Enable Background Installation.\n3. Open Package Installer and install SponsorBlockConfig-update.pkg.\n4. Confirm the update/reinstall if prompted.\n\nAfter installation, you can enable Background Installation again.";
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    param.userMsgParam = &userMsgParam;
    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open download-complete dialog (%d)", rc);
        return -1;
    }
    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Download-complete dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED) usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);
    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read download-complete dialog result (%d)", rc);
        return -1;
    }
    errorText[0] = '\0';
    return 0;
}

static int showAppUpdateFailedDialog(const char *reason, char *errorText, size_t errorTextSize)
{
    int rc = sceMsgDialogInitialize();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Message dialog initialization failed (%d)", rc);
        return -1;
    }
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam userMsgParam;
    OrbisMsgDialogResult result;
    memset(&param, 0, sizeof(param));
    memset(&userMsgParam, 0, sizeof(userMsgParam));
    memset(&result, 0, sizeof(result));
    param.baseParam.size = (uint32_t)sizeof(OrbisCommonDialogBaseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&param.baseParam);
    param.size = sizeof(OrbisMsgDialogParam);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    char message[256];
    snprintf(message, sizeof(message), "Could not download the SponsorBlockConfig update: %.170s", reason);
    userMsgParam.msg = message;
    userMsgParam.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    param.userMsgParam = &userMsgParam;
    rc = sceMsgDialogOpen(&param);
    if (rc < 0)
    {
        sceMsgDialogTerminate();
        snprintf(errorText, errorTextSize, "Could not open update-error dialog (%d)", rc);
        return -1;
    }
    OrbisCommonDialogStatus dialogStatus;
    do
    {
        dialogStatus = sceMsgDialogUpdateStatus();
        if ((int)dialogStatus < 0)
        {
            sceMsgDialogClose();
            sceMsgDialogTerminate();
            snprintf(errorText, errorTextSize, "Update-error dialog failed (%d)", (int)dialogStatus);
            return -1;
        }
        if (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED) usleep(10000);
    } while (dialogStatus != ORBIS_COMMON_DIALOG_STATUS_FINISHED);
    sceMsgDialogClose();
    rc = sceMsgDialogGetResult(&result);
    sceMsgDialogTerminate();
    if (rc < 0)
    {
        snprintf(errorText, errorTextSize, "Could not read update-error dialog result (%d)", rc);
        return -1;
    }
    errorText[0] = '\0';
    return 0;
}

static bool installPrx(char *errorText, size_t errorTextSize, bool *iniEntryAdded, bool *usedBundledFallback)
{
    const char *bundledSourcePath = "/app0/assets/misc/yt_adblock.prx";
    const char *destinationPath = "/data/GoldHEN/plugins/yt_adblock.prx";
    const char *sourcePath = bundledSourcePath;
    if (usedBundledFallback)
        *usedBundledFallback = false;

    if (mkdir("/data/GoldHEN", 0777) != 0 && errno != EEXIST)
    {
        snprintf(errorText, errorTextSize, "mkdir GoldHEN failed (errno %d)", errno);
        return false;
    }
    if (mkdir("/data/GoldHEN/plugins", 0777) != 0 && errno != EEXIST)
    {
        snprintf(errorText, errorTextSize, "mkdir plugins failed (errno %d)", errno);
        return false;
    }

    // Try downloading the latest PRX first.
    // If it fails, use the PRX bundled with the app.
    if (downloadLatestPrx(DOWNLOADED_PRX_PATH))
        sourcePath = DOWNLOADED_PRX_PATH;
    else if (usedBundledFallback)
        *usedBundledFallback = true;

    int sourceFd = open(sourcePath, O_RDONLY);
    if (sourceFd < 0)
    {
        if (sourcePath == DOWNLOADED_PRX_PATH)
            unlink(DOWNLOADED_PRX_PATH);
        snprintf(errorText, errorTextSize, "PRX source open failed (errno %d)", errno);
        return false;
    }

    // Copy the PRX into place.
    int destinationFd = open(destinationPath, O_WRONLY | O_CREAT, 0777);
    if (destinationFd < 0)
    {
        int savedErrno = errno;
        close(sourceFd);
        if (sourcePath == DOWNLOADED_PRX_PATH) unlink(DOWNLOADED_PRX_PATH);
        snprintf(errorText, errorTextSize, "PRX destination open failed (errno %d)", savedErrno);
        return false;
    }

    if (fchmod(destinationFd, 0777) != 0)
    {
        int chmodErrno = errno;
        struct stat destinationInfo;
        if (chmodErrno != EPERM ||
            fstat(destinationFd, &destinationInfo) != 0 ||
            (destinationInfo.st_mode & 0777) != 0777)
        {
            close(sourceFd);
            close(destinationFd);
            if (sourcePath == DOWNLOADED_PRX_PATH) unlink(DOWNLOADED_PRX_PATH);
            snprintf(errorText, errorTextSize, "PRX permission change failed (errno %d)", chmodErrno);
            return false;
        }
        // If chmod fails but permissions are already 0777,
        // we can still update the PRX.
    }

    if (ftruncate(destinationFd, 0) != 0)
    {
        int savedErrno = errno;
        close(sourceFd);
        close(destinationFd);
        if (sourcePath == DOWNLOADED_PRX_PATH) unlink(DOWNLOADED_PRX_PATH);
        snprintf(errorText, errorTextSize, "PRX truncate failed (errno %d)", savedErrno);
        return false;
    }

    char buffer[16384];
    bool ok = true;
    int savedErrno = 0;
    for (;;)
    {
        ssize_t bytesRead = read(sourceFd, buffer, sizeof(buffer));
        if (bytesRead == 0)
            break;
        if (bytesRead < 0)
        {
            if (errno == EINTR)
                continue;
            ok = false;
            savedErrno = errno;
            break;
        }
        if (!writeAll(destinationFd, buffer, (size_t)bytesRead))
        {
            ok = false;
            savedErrno = errno;
            break;
        }
    }

    if (close(sourceFd) != 0 && ok)
    {
        ok = false;
        savedErrno = errno;
    }
    if (close(destinationFd) != 0 && ok)
    {
        ok = false;
        savedErrno = errno;
    }

    if (sourcePath == DOWNLOADED_PRX_PATH)
        unlink(DOWNLOADED_PRX_PATH);

    if (!ok)
    {
        snprintf(errorText, errorTextSize, "PRX copy failed (errno %d)", savedErrno);
        return false;
    }

    // Register the plugin only after the PRX copy succeeds.
    if (!ensurePluginsIniEntry(errorText, errorTextSize, iniEntryAdded))
        return false;

    errorText[0] = '\0';
    return true;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    Color black = { 0, 0, 0 };
    Color white = { 245, 247, 250 };
    Color muted = { 165, 175, 190 };
    Color accent = { 38, 90, 140 };
    Color green = { 115, 220, 150 };
    Color red = { 255, 125, 125 };

    Scene2D *scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!scene->Init(0xC000000, 2))
    {
        DEBUGLOG << "Failed to initialize graphics";
        return 1;
    }

    FT_Face titleFont;
    FT_Face menuFont;
    FT_Face smallFont;
    const char *fontPath = "/app0/assets/fonts/Gontserrat-Regular.ttf";
    if (!scene->InitFont(&titleFont, fontPath, 56) ||
        !scene->InitFont(&menuFont, fontPath, 32) ||
        !scene->InitFont(&smallFont, fontPath, 20))
    {
        DEBUGLOG << "Failed to initialize font: " << fontPath;
        return 1;
    }

    Controller controller;
    if (!controller.Init(-1))
    {
        DEBUGLOG << "Failed to initialize controller";
        return 1;
    }

    int cursor = 0;
    // D-pad repeat timing
    int upHoldFrames = 0;
    int downHoldFrames = 0;
    const int NAV_REPEAT_DELAY_FRAMES = 3;
    const int NAV_REPEAT_INTERVAL_FRAMES = 1;
    char status[256] = "Choose categories, then save and install PS4 YT AdBlock + SponsorBlock.";
    bool statusIsError = false;
    char loadError[160];
    if (!loadConfig(loadError, sizeof(loadError)))
    {
        snprintf(status, sizeof(status), "Could not load saved categories: %.150s", loadError);
        statusIsError = true;
    }
    else
    {
        int initiallySelected = 0;
        for (int i = 0; i < CATEGORY_COUNT; ++i)
            if (selected[i]) ++initiallySelected;
        if (initiallySelected > 0)
            snprintf(status, sizeof(status), "Loaded %d saved categories. Adjust settings, then save when ready.", initiallySelected);
    }

    // Check stable GitHub releases once at startup. Network failures never block the app.
    std::string latestAppVersion;
    std::string latestAppPkgUrl;
    if (checkForAppUpdate(latestAppVersion, latestAppPkgUrl))
    {
        char updateDialogError[160] = "";
        int updateChoice = askDownloadAppUpdate(latestAppVersion.c_str(),
                                                updateDialogError, sizeof(updateDialogError));
        if (updateChoice == 1)
        {
            char downloadError[192] = "";
            if (!openAppUpdateProgressDialog(downloadError, sizeof(downloadError)))
            {
                showAppUpdateFailedDialog(downloadError, updateDialogError, sizeof(updateDialogError));
            }
            else
            {
                bool downloadSucceeded =
                    downloadAppUpdatePkg(latestAppPkgUrl.c_str(), downloadError, sizeof(downloadError));
                if (downloadSucceeded)
                    updateAppUpdateProgress("Update downloaded successfully!", 100);
                else
                    updateAppUpdateProgress("Download failed.", 100);
                usleep(500000);
                closeAppUpdateProgressDialog();

                if (downloadSucceeded)
                    showAppUpdateDownloadedDialog(updateDialogError, sizeof(updateDialogError));
                else
                    showAppUpdateFailedDialog(downloadError, updateDialogError, sizeof(updateDialogError));
            }
        }
    }

    for (;;)
    {
        controller.Poll();
        if (controller.Pressed(ORBIS_PAD_BUTTON_UP))
        {
            cursor = (cursor + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
            upHoldFrames = 0;
        }
        else if (controller.Held(ORBIS_PAD_BUTTON_UP))
        {
            ++upHoldFrames;
            if (upHoldFrames >= NAV_REPEAT_DELAY_FRAMES &&
                (upHoldFrames - NAV_REPEAT_DELAY_FRAMES) % NAV_REPEAT_INTERVAL_FRAMES == 0)
                cursor = (cursor + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        }
        else
        {
            upHoldFrames = 0;
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_DOWN))
        {
            cursor = (cursor + 1) % MENU_ITEM_COUNT;
            downHoldFrames = 0;
        }
        else if (controller.Held(ORBIS_PAD_BUTTON_DOWN))
        {
            ++downHoldFrames;
            if (downHoldFrames >= NAV_REPEAT_DELAY_FRAMES &&
                (downHoldFrames - NAV_REPEAT_DELAY_FRAMES) % NAV_REPEAT_INTERVAL_FRAMES == 0)
                cursor = (cursor + 1) % MENU_ITEM_COUNT;
        }
        else
        {
            downHoldFrames = 0;
        }
        if (controller.Pressed(ORBIS_PAD_BUTTON_SQUARE))
        {
            if (cursor == 0)
                skipModeAuto = !skipModeAuto;
            else
                selected[cursor - 1] = !selected[cursor - 1];
            status[0] = '\0';
        }

        // R2 selects everything unless everything is already checked.
        // If so, clear the selection instead.
        if (controller.Pressed(ORBIS_PAD_BUTTON_R2))
        {
            bool allSelected = true;
            for (int i = 0; i < CATEGORY_COUNT; ++i)
            {
                if (!selected[i])
                {
                    allSelected = false;
                    break;
                }
            }

            const bool selectAll = !allSelected;
            for (int i = 0; i < CATEGORY_COUNT; ++i)
                selected[i] = selectAll;

            status[0] = '\0';
            statusIsError = false;
        }
        if (controller.Pressed(ORBIS_PAD_BUTTON_OPTIONS))
        {
            char errorText[160];
            int uninstallChoice = askUninstallConfirmation(errorText, sizeof(errorText));
            if (uninstallChoice == 1)
            {
                if (uninstallManagedFiles(errorText, sizeof(errorText)))
                {
                    snprintf(status, sizeof(status), "SponsorBlock config and YT AdBlock plugin uninstalled.");
                    statusIsError = false;
                    for (int i = 0; i < CATEGORY_COUNT; ++i)
                        selected[i] = false;
                    skipModeAuto = false;
                }
                else
                {
                    snprintf(status, sizeof(status), "Uninstall incomplete: %.190s", errorText);
                    statusIsError = true;
                }
            }
            else if (uninstallChoice < 0)
            {
                snprintf(status, sizeof(status), "Uninstall dialog failed: %.180s", errorText);
                statusIsError = true;
            }
            else
            {
                snprintf(status, sizeof(status), "Uninstall cancelled.");
                statusIsError = false;
            }
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_TRIANGLE))
        {
            int count = 0;
            char errorText[160];
            if (saveConfig(errorText, sizeof(errorText), &count))
            {
                snprintf(status, sizeof(status), "SponsorBlock settings saved successfully.");
                statusIsError = false;

                int saveChoice = showConfigSavedDialog(errorText, sizeof(errorText));
                if (saveChoice == 1)
                {
                    sceSystemServiceLoadExec("exit", NULL);
                    return 0;
                }
                else if (saveChoice < 0)
                {
                    snprintf(status, sizeof(status), "Settings saved; dialog failed: %.120s", errorText);
                    statusIsError = true;
                }
                // Keep the saved message on screen when going back.
            }
            else
            {
                snprintf(status, sizeof(status), "Save failed: %s", errorText);
                statusIsError = true;
            }
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_CROSS))
        {
            int count = 0;
            char errorText[160];
            if (saveConfig(errorText, sizeof(errorText), &count))
            {
                int installChoice = askInstallPrx(errorText, sizeof(errorText));
                if (installChoice == 1)
                {
                    bool iniEntryAdded = false;
                    bool usedBundledFallback = false;
                    struct stat existingPrxBeforeInstall;
                    bool wasUpdate = stat("/data/GoldHEN/plugins/yt_adblock.prx",
                                          &existingPrxBeforeInstall) == 0 &&
                                     S_ISREG(existingPrxBeforeInstall.st_mode);
                    if (installPrx(errorText, sizeof(errorText), &iniEntryAdded, &usedBundledFallback))
                    {
                        snprintf(status, sizeof(status), iniEntryAdded ? "PS4 YT AdBlock + SponsorBlock installed; YouTube entry added" : "PS4 YT AdBlock + SponsorBlock installed");
                        statusIsError = false;
                        // Close the app after OK.
                        int successChoice = showSuccessDialog(wasUpdate, usedBundledFallback, errorText, sizeof(errorText));
                        if (successChoice == 1)
                            sceSystemServiceLoadExec("exit", NULL);
                        else if (successChoice < 0)
                        {
                            snprintf(status, sizeof(status), "Installed successfully; dialog failed: %.110s", errorText);
                            statusIsError = true;
                        }
                    }
                    else
                    {
                        snprintf(status, sizeof(status), "SponsorBlock Config saved; install failed: %.130s", errorText);
                        statusIsError = true;
                    }
                }
                else if (installChoice == 0)
                {
                    snprintf(status, sizeof(status), "SponsorBlock Config saved; PS4 YT AdBlock + SponsorBlock install skipped.");
                    statusIsError = false;
                }
                else
                {
                    snprintf(status, sizeof(status), "SponsorBlock Config saved; dialog failed: %.130s", errorText);
                    statusIsError = true;
                }
            }
            else
            {
                snprintf(status, sizeof(status), "Save failed: %s", errorText);
                statusIsError = true;
            }
        }
        scene->FrameBufferFill(black);
        drawText(scene, "SponsorBlock Categories", titleFont, 150, 100, black, white);
        drawText(scene, "YouTube AdBlock is always enabled.", menuFont, 150, 165, black, muted);

        // Segment Skip Mode row.
        int skipModeY = 250;
        if (cursor == 0)
            scene->DrawRectangle(135, skipModeY - 34, 1120, 43, accent);
        char skipModeText[128];
        snprintf(skipModeText, sizeof(skipModeText), "Segment Skip Mode: %s",
                 skipModeAuto ? "Auto Skip" : "Ask for Confirmation");
        drawText(scene, skipModeText, menuFont, 160, skipModeY, black,
                 cursor == 0 ? white : muted);

		// Categories selection rows.
        for (int i = 0; i < CATEGORY_COUNT; ++i)
        {
            int y = 304 + i * 54;
            if (i + 1 == cursor)
                scene->DrawRectangle(135, y - 34, 1120, 43, accent);

            char row[192];
            snprintf(row, sizeof(row), "%s  %s  (%s)",
                     selected[i] ? "[+]" : "[-]", categories[i].label, categories[i].id);
            drawText(scene, row, menuFont, 160, y, black, i + 1 == cursor ? white : muted);
        }

        int selectedCount = 0;
        for (int i = 0; i < CATEGORY_COUNT; ++i)
            if (selected[i]) ++selectedCount;
        char countText[96];
        snprintf(countText, sizeof(countText), "Selected: %d / %d", selectedCount, CATEGORY_COUNT);
        drawText(scene, countText, menuFont, 1320, 255, black, green);
        drawControllerIcon(scene, "arrows", 1325, 342);
        drawText(scene, "Move through options", menuFont, 1370, 365, black, muted);
        drawControllerIcon(scene, "square", 1325, 397);
        drawText(scene, "Toggle category / skip mode", menuFont, 1370, 420, black, muted);
        drawControllerIcon(scene, "triangle", 1325, 452);
        drawText(scene, "Update categories", menuFont, 1370, 475, black, muted);
        drawControllerIcon(scene, "cross", 1325, 508);
        drawText(scene, "Save and install", menuFont, 1370, 530, black, muted);
        drawText(scene, "OPTIONS: Uninstall All", smallFont, 1682, 1035, black, muted);
        char versionText[64];
        snprintf(versionText, sizeof(versionText), "Version: %s", APP_RELEASE_VERSION);
        drawText(scene, versionText, smallFont, 1765, 1065, black, muted);
        if (status[0] != '\0')
            drawText(scene, status, menuFont, 150, 1000, black, statusIsError ? red : green);

        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);
        scene->FrameBufferSwap();
        ++frameID;
    }
    return 0;
}
