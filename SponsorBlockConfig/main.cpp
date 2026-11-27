#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
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
#define APP_UPDATE_TEMP_PATH "/data/pkg/AdlessTube-update.pkg.download"
#define APP_UPDATE_PKG_PATH "/data/pkg/AdlessTube-update.pkg"

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
    { "music_offtopic", "Non-music sections" },
    { "filler", "Filler / tangents" },
    { "hook", "Hook / greeting" },
    { "poi_highlight", "Point of interest / highlight" },
    { "exclusive_access", "Exclusive access" },
    { "chapter", "Chapter" }
};

static bool selected[CATEGORY_COUNT] = { false };

// Skip modes: 0 = ask for confirmation, 1 = auto skip, 2 = auto without notifications.
static int skipMode = 0;
static bool adBlockEnabled = true;
static bool sponsorBlockEnabled = true;
// Order: skip mode, plugin toggles, then the 12 categories.
static const int MENU_ITEM_COUNT = CATEGORY_COUNT + 3;

static void drawText(Scene2D *scene, const char *text, FT_Face font,
                     int x, int y, Color background, Color foreground)
{
    char mutableText[256];
    snprintf(mutableText, sizeof(mutableText), "%s", text);
    scene->DrawText(mutableText, font, x, y, background, foreground);
}

// Right-align the value so changing digits don't move it.
static int getTextWidth(const char *text, FT_Face font)
{
    int textWidth = 0;
    FT_UInt previousGlyph = 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    {
        FT_UInt glyph = FT_Get_Char_Index(font, *p);
        if (FT_HAS_KERNING(font) && previousGlyph && glyph)
        {
            FT_Vector kerning;
            if (FT_Get_Kerning(font, previousGlyph, glyph, FT_KERNING_DEFAULT, &kerning) == 0)
                textWidth += (int)(kerning.x >> 6);
        }
        if (FT_Load_Glyph(font, glyph, FT_LOAD_DEFAULT) == 0)
            textWidth += (int)(font->glyph->advance.x >> 6);
        previousGlyph = glyph;
    }

    return textWidth;
}

static void drawTextRightAligned(Scene2D *scene, const char *text, FT_Face font,
                                 int rightEdge, int y, Color background, Color foreground)
{
    int textWidth = 0;
    FT_UInt previousGlyph = 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    {
        FT_UInt glyph = FT_Get_Char_Index(font, *p);
        if (FT_HAS_KERNING(font) && previousGlyph && glyph)
        {
            FT_Vector kerning;
            if (FT_Get_Kerning(font, previousGlyph, glyph, FT_KERNING_DEFAULT, &kerning) == 0)
                textWidth += (int)(kerning.x >> 6);
        }
        if (FT_Load_Glyph(font, glyph, FT_LOAD_DEFAULT) == 0)
            textWidth += (int)(font->glyph->advance.x >> 6);
        previousGlyph = glyph;
    }

    drawText(scene, text, font, rightEdge - textWidth, y, background, foreground);
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
    Color circleRed = { 235, 65, 65 };

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
    else if (strcmp(icon, "circle") == 0)
    {
        // Circle button.
        const int centerX = x + 14;
        const int centerY = y + 14;
        int circleX = 12;
        int circleY = 0;
        int decision = 1 - circleX;

        while (circleX >= circleY)
        {
            scene->DrawPixel(centerX + circleX, centerY + circleY, circleRed);
            scene->DrawPixel(centerX + circleY, centerY + circleX, circleRed);
            scene->DrawPixel(centerX - circleY, centerY + circleX, circleRed);
            scene->DrawPixel(centerX - circleX, centerY + circleY, circleRed);
            scene->DrawPixel(centerX - circleX, centerY - circleY, circleRed);
            scene->DrawPixel(centerX - circleY, centerY - circleX, circleRed);
            scene->DrawPixel(centerX + circleY, centerY - circleX, circleRed);
            scene->DrawPixel(centerX + circleX, centerY - circleY, circleRed);

            ++circleY;
            if (decision < 0)
                decision += 2 * circleY + 1;
            else
            {
                --circleX;
                decision += 2 * (circleY - circleX) + 1;
            }
        }
    }

}

// Show a native PS4 notification.
static void sendSystemNotification(const char *message)
{
    OrbisNotificationRequest notification;
    memset(&notification, 0, sizeof(notification));

    notification.type = OrbisNotificationRequestType::NotificationRequest;
    notification.unk3 = 0;
    notification.useIconImageUri = true;
    notification.targetId = -1;

    snprintf(notification.iconUri, sizeof(notification.iconUri),
             "file://user/appmeta/SBCK00001/icon0.png");

    if (message != NULL)
        strncpy(notification.message, message, sizeof(notification.message) - 1);

    sceKernelSendNotificationRequest(0, &notification, sizeof(notification), 0);
}

static bool jsonGetBoolValue(const std::string &json, const char *key, bool defaultValue)
{
    std::string quotedKey = "\"";
    quotedKey += key;
    quotedKey += "\"";

    size_t position = json.find(quotedKey);
    if (position == std::string::npos)
        return defaultValue;

    position = json.find(':', position + quotedKey.size());
    if (position == std::string::npos)
        return defaultValue;
    ++position;

    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\n' ||
            json[position] == '\r' || json[position] == '\t'))
        ++position;

    if (json.compare(position, 4, "true") == 0)
        return true;
    if (json.compare(position, 5, "false") == 0)
        return false;
    return defaultValue;
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
            skipMode = 0;
            adBlockEnabled = true;
            sponsorBlockEnabled = true;
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
        // Guard against an oversized config file.
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
    skipMode = 0;
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
            if (mode == "auto")
                skipMode = 1;
            else if (mode == "silent")
                skipMode = 2;
            else
                skipMode = 0;
        }
    }

    // Enable new settings when their keys are missing.
    adBlockEnabled = jsonGetBoolValue(contents, "adBlockEnabled", true);
    sponsorBlockEnabled = jsonGetBoolValue(contents, "sponsorBlockEnabled", true);

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
                 "  ],\n  \"skipMode\": \"%s\",\n"
                 "  \"adBlockEnabled\": %s,\n"
                 "  \"sponsorBlockEnabled\": %s\n}\n",
                 skipMode == 1 ? "auto" : (skipMode == 2 ? "silent" : "confirm"),
                 adBlockEnabled ? "true" : "false",
                 sponsorBlockEnabled ? "true" : "false");
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
    bool existingPrx = stat("/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx", &existingPrxInfo) == 0 &&
                       S_ISREG(existingPrxInfo.st_mode);
    userMsgParam.msg = existingPrx
        ? "AdlessTube already exists. Do you want to update it?"
        : "Install AdlessTube?";
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
        "Uninstall AdlessTube? This will delete all related files and configurations.";
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


static int showSuccessDialog(bool wasUpdate, bool usedBundledFallback, bool usedCachedFallback, char *errorText, size_t errorTextSize)
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
        snprintf(successMessage, sizeof(successMessage), "GitHub check failed. %s using bundled PRX.",
                 wasUpdate ? "Update successful" : "Install successful");
    else if (usedCachedFallback)
        snprintf(successMessage, sizeof(successMessage), "GitHub check failed. %s using cached PRX.",
                 wasUpdate ? "Update successful" : "Install successful");
    else
        snprintf(successMessage, sizeof(successMessage), "%s.",
                 wasUpdate ? "Update successful" : "Install successful");
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

static bool ensurePluginsIniEntry(char *errorText, size_t errorTextSize, bool *wasAdded)
{
    const char *iniPath = "/data/GoldHEN/plugins.ini";
    const char *tempPath = "/data/GoldHEN/plugins.ini.adlesstube.tmp";
    const std::string sectionLine = "[CUSA01015]";
    const std::string legacySectionLine = "[CUSA01015]\\";
    const std::string pluginLine = "/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx";
    if (wasAdded)
        *wasAdded = false;

    std::string contents;
    struct stat originalInfo;
    bool fileExists = false;
    int readFd = open(iniPath, O_RDONLY);
    if (readFd >= 0)
    {
        fileExists = true;
        if (fstat(readFd, &originalInfo) != 0)
        {
            int savedErrno = errno;
            close(readFd);
            snprintf(errorText, errorTextSize, "plugins.ini stat failed (errno %d)", savedErrno);
            return false;
        }
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

    // First pass: determine whether the AdlessTube entry already exists
    // in any CUSA01015 section, including repeated section headers.
    bool pluginAlreadyPresent = false;
    bool inTargetSection = false;
    size_t position = 0;
    while (position < contents.size())
    {
        size_t newline = contents.find('\n', position);
        size_t lineEnd = (newline == std::string::npos) ? contents.size() : newline + 1;
        std::string line = contents.substr(position, lineEnd - position);
        std::string compareLine = line;
        if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\n')
            compareLine.erase(compareLine.size() - 1);
        if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\r')
            compareLine.erase(compareLine.size() - 1);

        if (!compareLine.empty() && compareLine[0] == '[')
        {
            inTargetSection = (compareLine == sectionLine || compareLine == legacySectionLine);
        }
        else if (inTargetSection && compareLine == pluginLine)
        {
            pluginAlreadyPresent = true;
        }
        position = lineEnd;
    }

    // Second pass: combine repeated CUSA01015 headers into one section,
    // preserve all other lines, and add our PRX path only once.
    std::string updated;
    updated.reserve(contents.size() + pluginLine.size() + 32);
    bool keptTargetHeader = false;
    bool inKeptTargetSection = false;
    bool pluginWritten = false;
    bool changed = false;
    position = 0;
    while (position < contents.size())
    {
        size_t newline = contents.find('\n', position);
        size_t lineEnd = (newline == std::string::npos) ? contents.size() : newline + 1;
        std::string line = contents.substr(position, lineEnd - position);
        std::string compareLine = line;
        if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\n')
            compareLine.erase(compareLine.size() - 1);
        if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\r')
            compareLine.erase(compareLine.size() - 1);

        if (!compareLine.empty() && compareLine[0] == '[')
        {
            bool isTargetHeader = (compareLine == sectionLine || compareLine == legacySectionLine);
            if (isTargetHeader)
            {
                if (!keptTargetHeader || !inKeptTargetSection)
                {
                    // Keep a target header when another section appeared
                    // in between; removing it would reassign its entries.
                    updated.append(line);
                    keptTargetHeader = true;
                    inKeptTargetSection = true;
                }
                else
                {
                    // Collapse only adjacent repeated target headers.
                    // Their entries remain in the same section.
                    changed = true;
                    inKeptTargetSection = true;
                }
            }
            else
            {
                if (inKeptTargetSection && !pluginAlreadyPresent && !pluginWritten)
                {
                    if (!updated.empty() && updated[updated.size() - 1] != '\n')
                        updated += "\n";
                    updated += pluginLine + "\n";
                    pluginWritten = true;
                    changed = true;
                }
                updated.append(line);
                inKeptTargetSection = false;
            }
        }
        else if (inKeptTargetSection && compareLine == pluginLine)
        {
            if (!pluginWritten)
            {
                updated.append(line);
                pluginWritten = true;
            }
            else
            {
                // Remove duplicate copies of only our own exact PRX line.
                changed = true;
            }
        }
        else
        {
            updated.append(line);
        }
        position = lineEnd;
    }

    if (keptTargetHeader)
    {
        if (!pluginAlreadyPresent && !pluginWritten)
        {
            if (!updated.empty() && updated[updated.size() - 1] != '\n')
                updated += "\n";
            updated += pluginLine + "\n";
            changed = true;
        }
    }
    else
    {
        if (!updated.empty() && updated[updated.size() - 1] != '\n')
            updated += "\n";
        updated += sectionLine + "\n" + pluginLine + "\n";
        changed = true;
    }

    // No file changes are needed when the entry already exists.
    if (!changed)
    {
        errorText[0] = '\0';
        return true;
    }

    int tempFd = open(tempPath, O_WRONLY | O_CREAT | O_TRUNC,
                      fileExists ? (originalInfo.st_mode & 0777) : 0666);
    if (tempFd < 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini temp open failed (errno %d)", errno);
        return false;
    }

    bool ok = true;
    if (fileExists && fchmod(tempFd, originalInfo.st_mode & 0777) != 0)
        ok = false;
    if (ok)
        ok = writeAll(tempFd, updated.c_str(), updated.size());
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

    // Verify that the PRX path is present under a CUSA01015 section.
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
        if (bytesRead == 0)
            break;
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
    if (close(verifyFd) != 0)
    {
        snprintf(errorText, errorTextSize, "plugins.ini verification close failed (errno %d)", errno);
        return false;
    }

    bool verifiedEntry = false;
    bool verifyInTargetSection = false;
    size_t verifyPosition = 0;
    while (verifyPosition < verifyContents.size())
    {
        size_t newline = verifyContents.find('\n', verifyPosition);
        size_t lineEnd = (newline == std::string::npos) ? verifyContents.size() : newline + 1;
        std::string line = verifyContents.substr(verifyPosition, lineEnd - verifyPosition);
        std::string compareLine = line;
        if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\n')
            compareLine.erase(compareLine.size() - 1);
        if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\r')
            compareLine.erase(compareLine.size() - 1);
        if (!compareLine.empty() && compareLine[0] == '[')
            verifyInTargetSection = (compareLine == sectionLine || compareLine == legacySectionLine);
        else if (verifyInTargetSection && compareLine == pluginLine)
            verifiedEntry = true;
        verifyPosition = lineEnd;
    }
    if (!verifiedEntry)
    {
        snprintf(errorText, errorTextSize, "plugins.ini write could not be verified");
        return false;
    }

    if (wasAdded)
        *wasAdded = !pluginAlreadyPresent;
    errorText[0] = '\0';
    return true;
}

static bool removePluginsIniEntry(const char *pluginPath, char *errorText, size_t errorTextSize)
{
    const char *iniPath = "/data/GoldHEN/plugins.ini";
    const char *tempPath = "/data/GoldHEN/plugins.ini.sponsorblock.tmp";
    const std::string targetSection = "[CUSA01015]";
    const std::string targetPlugin = pluginPath;

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

    // Remove only our plugin entry from section.
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

        if (isTargetSection && sectionHasPluginEntry)
        {
            removedEntry = true;

            if (sectionHasOtherEntries)
            {
                // Keep the section and its other entries; remove only
                // exact matches for this plugin path.
                filtered.append(contents, sectionStart, firstEnd - sectionStart);

                size_t linePosition = firstEnd;
                while (linePosition < nextPosition)
                {
                    size_t newline = contents.find('\n', linePosition);
                    size_t lineEnd = (newline == std::string::npos || newline >= nextPosition)
                                         ? nextPosition : newline + 1;
                    std::string line = contents.substr(linePosition, lineEnd - linePosition);
                    std::string compareLine = line;
                    if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\n')
                        compareLine.erase(compareLine.size() - 1);
                    if (!compareLine.empty() && compareLine[compareLine.size() - 1] == '\r')
                        compareLine.erase(compareLine.size() - 1);

                    if (compareLine != targetPlugin)
                        filtered.append(contents, linePosition, lineEnd - linePosition);

                    linePosition = lineEnd;
                }
            }
            // If this was the section’s only entry, remove the
            // empty section and its header.
        }
        else
        {
            filtered.append(contents, sectionStart, nextPosition - sectionStart);
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

// Remove a directory tree without following symlinks:
// unlink files and symlinks directly; open only directories.
static bool removeDirectoryTree(const char *path, char *errorText, size_t errorTextSize)
{
    DIR *directory = opendir(path);
    if (directory == NULL)
    {
        if (errno == ENOENT)
            return true;
        snprintf(errorText, errorTextSize, "Could not open directory %.70s (errno %d)", path, errno);
        return false;
    }

    bool ok = true;
    int readError = 0;
    for (;;)
    {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL)
        {
            readError = errno;
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char childPath[1024];
        int pathLength = snprintf(childPath, sizeof(childPath), "%s/%s", path, entry->d_name);
        if (pathLength <= 0 || (size_t)pathLength >= sizeof(childPath))
        {
            snprintf(errorText, errorTextSize, "Directory path is too long during uninstall");
            ok = false;
            break;
        }

        if (unlink(childPath) == 0 || errno == ENOENT)
            continue;

        int removeErrno = errno;
        if (removeErrno == EISDIR || removeErrno == EPERM)
        {
            if (!removeDirectoryTree(childPath, errorText, errorTextSize))
            {
                ok = false;
                break;
            }
        }
        else
        {
            snprintf(errorText, errorTextSize, "Could not delete %.70s (errno %d)",
                     childPath, removeErrno);
            ok = false;
            break;
        }
    }

    if (closedir(directory) != 0 && ok)
    {
        snprintf(errorText, errorTextSize, "Could not close directory %.70s (errno %d)", path, errno);
        ok = false;
    }
    if (readError != 0 && ok)
    {
        snprintf(errorText, errorTextSize, "Could not read directory %.70s (errno %d)", path, readError);
        ok = false;
    }
    if (!ok)
        return false;

    if (rmdir(path) != 0 && errno != ENOENT)
    {
        snprintf(errorText, errorTextSize, "Could not remove directory %.70s (errno %d)", path, errno);
        return false;
    }
    return true;
}

static bool uninstallManagedFiles(char *errorText, size_t errorTextSize)
{
    char oldIniError[160] = "";
    char newIniError[160] = "";
    char oldPrxError[160] = "";
    char newPrxError[160] = "";
    char configError[160] = "";
    char cacheError[160] = "";

    bool oldIniOk = removePluginsIniEntry("/data/GoldHEN/plugins/yt_adblock.prx",
                                          oldIniError, sizeof(oldIniError));
    bool newIniOk = removePluginsIniEntry("/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx",
                                          newIniError, sizeof(newIniError));
    bool oldPrxOk = removeIfPresent("/data/GoldHEN/plugins/yt_adblock.prx",
                                    oldPrxError, sizeof(oldPrxError));
    bool newPrxOk = removeIfPresent("/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx",
                                    newPrxError, sizeof(newPrxError));
    bool configOk = removeIfPresent(CONFIG_PATH, configError, sizeof(configError));
    bool cacheOk = removeDirectoryTree("/data/ytadsponsorblock", cacheError, sizeof(cacheError));

    if (!oldIniOk || !newIniOk || !oldPrxOk || !newPrxOk || !configOk || !cacheOk)
    {
        const char *first = !oldIniOk ? oldIniError :
                            (!newIniOk ? newIniError :
                            (!oldPrxOk ? oldPrxError :
                            (!newPrxOk ? newPrxError :
                            (!configOk ? configError : cacheError))));
        snprintf(errorText, errorTextSize, "%s", first);
        return false;
    }

    errorText[0] = '\0';
    return true;
}

// Check for a PRX update and download it if needed.
#define LATEST_PRX_RELEASE_API_URL "https://api.github.com/repos/FourZeroCode/ps4_yt_adsponsorblock/releases/latest"
static const char *LATEST_PRX_URL =
    "https://github.com/FourZeroCode/ps4_yt_adsponsorblock/releases/latest/download/ps4_yt_adsponsorblock.prx";
#define PRX_CACHE_DIRECTORY "/data/ytadsponsorblock"
#define PRX_DOWNLOAD_TEMP_PATH "/data/ytadsponsorblock/prx-download.tmp"
static const char *DOWNLOADED_PRX_PATH = PRX_DOWNLOAD_TEMP_PATH;

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

static bool downloadLatestPrx(const char *outputPath, uint64_t *downloadedSize)
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

    // OpenOrbis declares this without arguments, but the actual
    // function takes an ID and size.
    // Raise the header limit to 64 KB.
    // GitHub can return large redirect headers.
    headerLimitResult = setResponseHeaderMaxSize(templateId, 64 * 1024);
    if (headerLimitResult < 0)
    {
        snprintf(downloadFailureReason, sizeof(downloadFailureReason),
                 "HTTP header limit setup failed (%d)", headerLimitResult);
        goto cleanup;
    }

    // Follow GitHub redirects.
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

            // 0x80431073 indicates the PS4 HTTP stack rejected a large
            // response header. Fall back to the bundled PRX.
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
    if (downloadedSize)
        *downloadedSize = totalRead;
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


// Compare numeric versions such as 1.01 or 1.2.3.
static int compareReleaseVersions(const std::string &left, const std::string &right)
{
    unsigned int a[3], b[3];
    if (!parseReleaseVersion(left, a) || !parseReleaseVersion(right, b))
        return 0;
    for (int i = 0; i < 3; ++i)
    {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

static bool isUsableCachedPrx(const char *path)
{
    // Existing cache files are keyed by release tag. New downloads
    // are validated before being moved into the cache.
    // Leave existing cached files alone.
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    // A one-byte read checks that the path is a readable file.
    // It does not check the file size.
    unsigned char probe;
    ssize_t readResult = read(fd, &probe, 1);
    close(fd);
    return readResult >= 0;
}

static bool buildCachedPrxPath(const std::string &releaseTag, char *path, size_t pathSize)
{
    unsigned int parts[3];
    if (!parseReleaseVersion(releaseTag, parts))
        return false;

    // Use the release tag for the cache filename:
    // v1.2.3 becomes /data/ytadsponsorblock/v1.2.3.prx
    for (size_t i = 0; i < releaseTag.size(); ++i)
    {
        char c = releaseTag[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'v' || c == 'V'))
            return false;
    }

    int written = snprintf(path, pathSize, PRX_CACHE_DIRECTORY "/%s.prx",
                           releaseTag.c_str());
    return written > 0 && (size_t)written < pathSize;
}

// Look only for the filename matching this exact tag.
static bool findCachedPrxForVersion(const std::string &releaseTag,
                                    char *path, size_t pathSize)
{
    char taggedPath[512] = "";
    if (!buildCachedPrxPath(releaseTag, taggedPath, sizeof(taggedPath)))
        return false;

    // Cache filename: the exact GitHub tag, e.g. v1.0.0.prx.
    if (isUsableCachedPrx(taggedPath))
    {
        int written = snprintf(path, pathSize, "%s", taggedPath);
        return written > 0 && (size_t)written < pathSize;
    }

    // Move files from the old version1.0.0.prx naming scheme
    // into the tag-based cache without downloading again.
    std::string version = releaseTag;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
        version.erase(0, 1);

    char legacyPath[512];
    int legacyWritten = snprintf(legacyPath, sizeof(legacyPath),
                                 PRX_CACHE_DIRECTORY "/version%s.prx", version.c_str());
    if (legacyWritten <= 0 || (size_t)legacyWritten >= sizeof(legacyPath) ||
        !isUsableCachedPrx(legacyPath))
        return false;

    // Rename the cache entry to the tag-based filename.
    // If rename fails, keep using the old path rather than
    // downloading the PRX again.
    if (rename(legacyPath, taggedPath) == 0)
        legacyPath[0] = '\0';

    const char *selectedPath = legacyPath[0] == '\0' ? taggedPath : legacyPath;
    int written = snprintf(path, pathSize, "%s", selectedPath);
    return written > 0 && (size_t)written < pathSize;
}

static bool findNewestCachedPrx(char *path, size_t pathSize, std::string *foundVersion = NULL)
{
    DIR *directory = opendir(PRX_CACHE_DIRECTORY);
    if (!directory) return false;

    bool found = false;
    std::string newestVersion;
    char newestPath[512] = "";
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        const char *name = entry->d_name;
        size_t length = strlen(name);
        if (length <= 4 || strcmp(name + length - 4, ".prx") != 0)
            continue;

        std::string version;
        if (strncmp(name, "version", 7) == 0 && length > 11)
            version.assign(name + 7, length - 11); // Old cache filename
        else
            version.assign(name, length - 4);      // Release-tag cache filename

        unsigned int parts[3];
        if (!parseReleaseVersion(version, parts))
            continue;

        char candidate[512];
        int written = snprintf(candidate, sizeof(candidate), PRX_CACHE_DIRECTORY "/%s", name);
        if (written <= 0 || (size_t)written >= sizeof(candidate) ||
            !isUsableCachedPrx(candidate))
            continue;

        if (!found || compareReleaseVersions(version, newestVersion) > 0)
        {
            found = true;
            newestVersion = version;
            snprintf(newestPath, sizeof(newestPath), "%s", candidate);
        }
    }
    closedir(directory);

    if (!found || strlen(newestPath) >= pathSize)
        return false;
    snprintf(path, pathSize, "%s", newestPath);
    if (foundVersion) *foundVersion = newestVersion;
    return true;
}

static bool getLatestPrxReleaseTag(std::string &releaseTag)
{
    // The leading # intentionally disables network checks in GUI builds.
    if (LATEST_PRX_RELEASE_API_URL[0] == '#')
        return false;

    std::string response;
    char errorText[160] = "";
    if (!httpGetToString(LATEST_PRX_RELEASE_API_URL, response, errorText, sizeof(errorText)))
        return false;

    return jsonGetString(response, "tag_name", releaseTag);
}

// Native PS4 progress dialog used by the SponsorBlockConfig PKG updater.
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
    userMsgParam.msg = "Update downloaded successfully!\n\nTo install the update:\n1. Open GoldHEN > Debug Settings.\n2. Under Package Installer, disable Enable Background Installation.\n3. Open Package Installer and install AdlessTube-update.pkg.\n4. Confirm the update/reinstall if prompted.\n\nAfter installation, you can enable Background Installation again.";
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

static bool installPrx(char *errorText, size_t errorTextSize, bool *iniEntryAdded, bool *usedBundledFallback, bool *usedCachedFallback)
{
    const char *bundledSourcePath = "/app0/assets/misc/ps4_yt_adsponsorblock.prx";
    const char *destinationPath = "/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx";
    char selectedCachePath[512] = "";
    const char *sourcePath = bundledSourcePath;
    bool usedCache = false;
    bool sourceIsFreshDownload = false;
    uint64_t expectedPrxSize = 0;
    if (usedBundledFallback) *usedBundledFallback = false;
    if (usedCachedFallback) *usedCachedFallback = false;

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

    // Remove the old AdBlock PRX and its GoldHEN entry before
    // installing SponsorBlock. Leave other plugin files untouched.
    if (!removePluginsIniEntry("/data/GoldHEN/plugins/yt_adblock.prx",
                               errorText, errorTextSize))
        return false;
    if (!removeIfPresent("/data/GoldHEN/plugins/yt_adblock.prx",
                         errorText, errorTextSize))
        return false;

    // Create the PRX cache directory.
    if (mkdir(PRX_CACHE_DIRECTORY, 0777) != 0 && errno != EEXIST)
    {
        snprintf(errorText, errorTextSize, "PRX cache directory creation failed (errno %d)", errno);
        return false;
    }

    // Get the latest stable PRX tag from GitHub. If the request fails,
    // use the newest valid cache, then the bundled PRX.
    std::string latestTag;
    char matchingCachePath[512] = "";
    bool haveLatestTag = getLatestPrxReleaseTag(latestTag);
    bool haveMatchingCache = haveLatestTag &&
                             findCachedPrxForVersion(latestTag, matchingCachePath,
                                                     sizeof(matchingCachePath));

    if (haveMatchingCache)
    {
        sourcePath = matchingCachePath;
        usedCache = true;
    }
    else if (haveLatestTag && downloadLatestPrx(DOWNLOADED_PRX_PATH, &expectedPrxSize))
    {
        char newCachePath[512] = "";
        if (!buildCachedPrxPath(latestTag, newCachePath, sizeof(newCachePath)))
        {
            snprintf(errorText, errorTextSize, "GitHub release tag is not a valid cache version");
            unlink(DOWNLOADED_PRX_PATH);
            return false;
        }

        // Remove an existing cache file before replacing it. If the
        // path is a stale directory from an earlier run, unlink() fails;
        // remove only the known temp file, then call rmdir().
        // Avoid lstat(); it can return EPERM
        if (unlink(newCachePath) != 0 && errno != ENOENT)
        {
            int removeTargetErrno = errno;
            char misplacedDownload[640];
            int misplacedLength = snprintf(misplacedDownload, sizeof(misplacedDownload),
                                           "%s/prx-download.tmp", newCachePath);
            if (misplacedLength <= 0 || (size_t)misplacedLength >= sizeof(misplacedDownload))
            {
                snprintf(errorText, errorTextSize, "old cache directory path is too long");
                return false;
            }

            // Try this cleanup only for errors typically returned when
            // unlink() is called on a directory. Don’t recurse or
            // remove anything else.
            if (removeTargetErrno != EISDIR && removeTargetErrno != EPERM)
            {
                snprintf(errorText, errorTextSize,
                         "could not prepare versioned cache path (errno %d)", removeTargetErrno);
                return false;
            }
            if (unlink(misplacedDownload) != 0 && errno != ENOENT)
            {
                snprintf(errorText, errorTextSize,
                         "could not remove misplaced PRX from old cache directory (errno %d)", errno);
                return false;
            }
            if (rmdir(newCachePath) != 0)
            {
                snprintf(errorText, errorTextSize,
                         "could not remove stale versioned cache directory (errno %d)", errno);
                return false;
            }
        }

        // The download passed the byte-count, content-length, and
        // 64 KiB–20 MiB checks. Rename it to the cache file.
        if (rename(DOWNLOADED_PRX_PATH, newCachePath) == 0)
        {
            // The downloader already checked the byte count and
            // content length. Don’t check st_size again: on this PS4
            // filesystem, stat() reports 544 for a valid renamed PRX.
            snprintf(selectedCachePath, sizeof(selectedCachePath), "%s", newCachePath);
            sourcePath = selectedCachePath;
            sourceIsFreshDownload = true;
        }
        else
        {
            int cacheRenameErrno = errno;
            // Cache promotion failed; keep using the downloaded file
            // for this install.
            // The temp file was validated during download. If moving it
            // into the cache fails, install from the temp path.
            sourcePath = DOWNLOADED_PRX_PATH;
            sourceIsFreshDownload = true;
            (void)cacheRenameErrno;
        }
    }
    else
    {
        std::string cachedVersion;
        if (findNewestCachedPrx(selectedCachePath, sizeof(selectedCachePath), &cachedVersion))
        {
            sourcePath = selectedCachePath;
            usedCache = true;
        }
        else if (!haveLatestTag && usedBundledFallback)
        {
            *usedBundledFallback = true;
        }
    }

    // Keep the bundled PRX fallback: open the packaged file
    // directly instead of applying cache checks to it.
    // This also keeps GUI builds working with GitHub checks off.

    // Use the cache as a fallback only if the release check failed.
    // A cache for the checked release is a normal install.
    if (usedCachedFallback) *usedCachedFallback = usedCache && !haveLatestTag;
    int sourceFd = open(sourcePath, O_RDONLY);
    if (sourceFd < 0)
    {
        if (strcmp(sourcePath, DOWNLOADED_PRX_PATH) == 0)
            unlink(DOWNLOADED_PRX_PATH);
        snprintf(errorText, errorTextSize, "PRX source open failed (errno %d)", errno);
        return false;
    }

    // Copy to a temp file beside the destination, then rename it
    // into place so a partial copy never replaces the live PRX.
    const char *destinationTempPath = "/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx.tmp";
    unlink(destinationTempPath);
    int destinationFd = open(destinationTempPath, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (destinationFd < 0)
    {
        int savedErrno = errno;
        close(sourceFd);
        if (strcmp(sourcePath, DOWNLOADED_PRX_PATH) == 0) unlink(DOWNLOADED_PRX_PATH);
        snprintf(errorText, errorTextSize, "PRX temporary destination open failed (errno %d)", savedErrno);
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
            unlink(destinationTempPath);
            if (strcmp(sourcePath, DOWNLOADED_PRX_PATH) == 0) unlink(DOWNLOADED_PRX_PATH);
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
        unlink(destinationTempPath);
        if (strcmp(sourcePath, DOWNLOADED_PRX_PATH) == 0) unlink(DOWNLOADED_PRX_PATH);
        snprintf(errorText, errorTextSize, "PRX truncate failed (errno %d)", savedErrno);
        return false;
    }

    char buffer[16384];
    bool ok = true;
    uint64_t copiedPrxSize = 0;
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
        copiedPrxSize += (uint64_t)bytesRead;
    }

    if (ok && fsync(destinationFd) != 0)
    {
        ok = false;
        savedErrno = errno;
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

    if (strcmp(sourcePath, DOWNLOADED_PRX_PATH) == 0)
        unlink(DOWNLOADED_PRX_PATH);

    if (!ok)
    {
        unlink(destinationTempPath);
        snprintf(errorText, errorTextSize, "PRX copy failed (errno %d)", savedErrno);
        return false;
    }

    // Check the copied file size before replacing the installed PRX.
    if (sourceIsFreshDownload)
    {
        if (copiedPrxSize != expectedPrxSize)
        {
            unlink(destinationTempPath);
            snprintf(errorText, errorTextSize,
                     "PRX copy size mismatch (copied %llu, downloaded %llu)",
                     (unsigned long long)copiedPrxSize,
                     (unsigned long long)expectedPrxSize);
            return false;
        }
    }
    else if (strcmp(sourcePath, bundledSourcePath) != 0 &&
             !isUsableCachedPrx(destinationTempPath))
    {
        unlink(destinationTempPath);
        snprintf(errorText, errorTextSize, "copied PRX failed size validation");
        return false;
    }

    if (rename(destinationTempPath, destinationPath) != 0)
    {
        int renameErrno = errno;
        unlink(destinationTempPath);
        snprintf(errorText, errorTextSize, "PRX final rename failed (errno %d)", renameErrno);
        return false;
    }

    // Register the plugin only after the PRX copy succeeds.
    if (!ensurePluginsIniEntry(errorText, errorTextSize, iniEntryAdded))
        return false;

    errorText[0] = '\0';
    return true;
}

// Close the screen with two black shutters, leaving the UI visible
// until they cover it. FrameWait keeps the animation paced.
static void playClosingAnimation(Scene2D *scene, int *frameID, Color black)
{
    const int animationFrames = 45;

    for (int frame = 0; frame < animationFrames; ++frame)
    {
        const int barHeight = ((FRAME_HEIGHT / 2) * (frame + 1)) / animationFrames;

        scene->DrawRectangle(0, 0, FRAME_WIDTH, barHeight, black);
        scene->DrawRectangle(0, FRAME_HEIGHT - barHeight, FRAME_WIDTH, barHeight, black);

        scene->SubmitFlip(*frameID);
        scene->FrameWait(*frameID);
        scene->FrameBufferSwap();
        ++(*frameID);
    }
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
    FT_Face categoryFont;
    FT_Face smallFont;
    FT_Face legendFont;
    const char *fontPath = "/app0/assets/fonts/Gontserrat-Regular.ttf";
    if (!scene->InitFont(&titleFont, fontPath, 50) ||
        !scene->InitFont(&menuFont, fontPath, 32) ||
        !scene->InitFont(&categoryFont, fontPath, 32) ||
        !scene->InitFont(&smallFont, fontPath, 20) ||
        !scene->InitFont(&legendFont, fontPath, 24))
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
    char status[256] = "Select your categories and install AdlessTube.";
    bool statusIsError = false;
    int preferencesStatusFramesRemaining = 0;
    bool closeConfirmPending = false;
    bool exitRequested = false;
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
            snprintf(status, sizeof(status), "Loaded %d saved categories. Configure AdlessTube to your liking.", initiallySelected);
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

    // Put the three main settings in one row above the categories.
    auto moveCursorUp = [&]()
    {
        if (cursor <= 2)
        {
            cursor = MENU_ITEM_COUNT - 1;
        }
        else
        {
            const int row = (cursor - 3) % 6;
            if (row == 0)
                cursor = (cursor >= 9) ? 2 : 1;
            else
                --cursor;
        }
    };
    auto moveCursorDown = [&]()
    {
        if (cursor <= 2)
        {
            cursor = (cursor == 2) ? 9 : 3;
        }
        else
        {
            const int row = (cursor - 3) % 6;
            if (row == 5)
                cursor = (cursor >= 9) ? 2 : 1;
            else
                ++cursor;
        }
    };

    // Save each setting or category change immediately.
    auto saveAfterChange = [&]()
    {
        int savedCount = 0;
        char saveError[160];
        if (saveConfig(saveError, sizeof(saveError), &savedCount))
        {
            snprintf(status, sizeof(status), "Preferences updated successfully.");
            statusIsError = false;
            preferencesStatusFramesRemaining = 20; // About two seconds at the current frame rate.
        }
        else
        {
            snprintf(status, sizeof(status), "Save failed: %.210s", saveError);
            statusIsError = true;
        }
    };

    for (;;)
    {
        if (preferencesStatusFramesRemaining > 0)
        {
            --preferencesStatusFramesRemaining;
            if (preferencesStatusFramesRemaining == 0 &&
                strcmp(status, "Preferences updated successfully.") == 0)
                status[0] = '\0';
        }

        controller.Poll();

		// Press Circle twice to exit. Any other button cancels.
        if (controller.Pressed(ORBIS_PAD_BUTTON_CIRCLE))
        {
            if (closeConfirmPending)
            {
                exitRequested = true;
            }
            else
            {
                closeConfirmPending = true;
                // Keep the in-app message as a fallback. Also try the
                // native system notification.
                status[0] = '\0';
                statusIsError = false;
                preferencesStatusFramesRemaining = 0;
                sendSystemNotification("Press again to exit");
            }
        }
        else if (closeConfirmPending &&
                 (controller.Pressed(ORBIS_PAD_BUTTON_UP) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_DOWN) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_LEFT) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_RIGHT) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_CROSS) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_OPTIONS) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_TRIANGLE) ||
                  controller.Pressed(ORBIS_PAD_BUTTON_SQUARE)))
        {
            closeConfirmPending = false;
        }

        if (!exitRequested)
        {
        if (controller.Pressed(ORBIS_PAD_BUTTON_UP))
        {
            moveCursorUp();
            upHoldFrames = 0;
        }
        else if (controller.Held(ORBIS_PAD_BUTTON_UP))
        {
            ++upHoldFrames;
            if (upHoldFrames >= NAV_REPEAT_DELAY_FRAMES &&
                (upHoldFrames - NAV_REPEAT_DELAY_FRAMES) % NAV_REPEAT_INTERVAL_FRAMES == 0)
                moveCursorUp();
        }
        else
        {
            upHoldFrames = 0;
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_DOWN))
        {
            moveCursorDown();
            downHoldFrames = 0;
        }
        else if (controller.Held(ORBIS_PAD_BUTTON_DOWN))
        {
            ++downHoldFrames;
            if (downHoldFrames >= NAV_REPEAT_DELAY_FRAMES &&
                (downHoldFrames - NAV_REPEAT_DELAY_FRAMES) % NAV_REPEAT_INTERVAL_FRAMES == 0)
                moveCursorDown();
        }
        else
        {
            downHoldFrames = 0;
        }

        // Move across the top-row settings, or between category
        // columns on the same row.
        if (controller.Pressed(ORBIS_PAD_BUTTON_LEFT))
        {
            if (cursor == 1)
                cursor = 0;
            else if (cursor == 2)
                cursor = 1;
            else if (cursor >= 9 && cursor <= 14)
                cursor -= 6;
        }
        else if (controller.Pressed(ORBIS_PAD_BUTTON_RIGHT))
        {
            if (cursor == 0)
                cursor = 1;
            else if (cursor == 1)
                cursor = 2;
            else if (cursor >= 3 && cursor <= 8)
                cursor += 6;
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_CROSS))
        {
            if (cursor == 0)
                skipMode = (skipMode + 1) % 3;
            else if (cursor == 1)
                adBlockEnabled = !adBlockEnabled;
            else if (cursor == 2)
                sponsorBlockEnabled = !sponsorBlockEnabled;
            else
                selected[cursor - 3] = !selected[cursor - 3];

            saveAfterChange();
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_OPTIONS))
        {
            char errorText[160];
            int uninstallChoice = askUninstallConfirmation(errorText, sizeof(errorText));
            if (uninstallChoice == 1)
            {
                if (uninstallManagedFiles(errorText, sizeof(errorText)))
                {
                    snprintf(status, sizeof(status), "AdlessTube uninstalled.");
                    statusIsError = false;
                    for (int i = 0; i < CATEGORY_COUNT; ++i)
                        selected[i] = false;
                    skipMode = 0;
                    adBlockEnabled = true;
                    sponsorBlockEnabled = true;
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

            saveAfterChange();
        }

        if (controller.Pressed(ORBIS_PAD_BUTTON_SQUARE))
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
                    bool usedCachedFallback = false;
                    struct stat existingPrxBeforeInstall;
                    bool wasUpdate = stat("/data/GoldHEN/plugins/ps4_yt_adsponsorblock.prx",
                                          &existingPrxBeforeInstall) == 0 &&
                                     S_ISREG(existingPrxBeforeInstall.st_mode);
                    if (installPrx(errorText, sizeof(errorText), &iniEntryAdded, &usedBundledFallback, &usedCachedFallback))
                    {
                        snprintf(status, sizeof(status), iniEntryAdded ? "AdlessTube installed, YouTube entry added" : "AdlessTube installed");
                        statusIsError = false;
                        // Close the app after OK.
                        int successChoice = showSuccessDialog(wasUpdate, usedBundledFallback, usedCachedFallback, errorText, sizeof(errorText));
                        if (successChoice == 1)
                            exitRequested = true;
                        else if (successChoice < 0)
                        {
                            snprintf(status, sizeof(status), "Installed successfully, dialog failed: %.110s", errorText);
                            statusIsError = true;
                        }
                    }
                    else
                    {
                        snprintf(status, sizeof(status), "Settings saved, install failed: %.130s", errorText);
                        statusIsError = true;
                    }
                }
                else if (installChoice == 0)
                {
                    snprintf(status, sizeof(status), "Settings saved, AdlessTube install skipped.");
                    statusIsError = false;
                }
                else
                {
                    snprintf(status, sizeof(status), "Settings saved, dialog failed: %.130s", errorText);
                    statusIsError = true;
                }
            }
            else
            {
                snprintf(status, sizeof(status), "Save failed: %s", errorText);
                statusIsError = true;
            }
        }
        }

        scene->FrameBufferFill(black);
        drawText(scene, "AdlessTube - AdBlock & SponsorBlock for YouTube", titleFont, 150, 100, black, white);
        drawText(scene, "All these settings can now be configured directly in YouTube", menuFont, 150, 165, black, muted);

		// Top right corner: app version and the uninstall option.
        char versionText[64];
        snprintf(versionText, sizeof(versionText), "Version: %s", APP_RELEASE_VERSION);
        drawTextRightAligned(scene, versionText, smallFont, 1900, 30, black, muted);
        drawText(scene, "OPTIONS: Uninstall All", smallFont, 1682, 60, black, muted);

        // All three main settings share one horizontal row.
        const int settingsRowY = 285;
        if (cursor == 0)
            scene->DrawRectangle(135, settingsRowY - 34, 645, 43, accent);
        if (cursor == 1)
            scene->DrawRectangle(805, settingsRowY - 34, 500, 43, accent);
        if (cursor == 2)
            scene->DrawRectangle(1375, settingsRowY - 34, 450, 43, accent);

        char skipModeText[128];
        char adBlockText[96];
        char sponsorBlockText[96];
        snprintf(skipModeText, sizeof(skipModeText), "Skip Mode: %s",
                 skipMode == 1 ? "Auto Skip" :
                 (skipMode == 2 ? "Auto without notifications" : "Ask for Confirmation"));
        snprintf(adBlockText, sizeof(adBlockText), "AdBlock: %s",
                 adBlockEnabled ? "ON" : "OFF");
        snprintf(sponsorBlockText, sizeof(sponsorBlockText), "SponsorBlock: %s",
                 sponsorBlockEnabled ? "ON" : "OFF");

        drawText(scene, skipModeText, menuFont, 160, settingsRowY, black,
                 cursor == 0 ? white : muted);
        drawText(scene, adBlockText, menuFont, 830, settingsRowY, black,
                 cursor == 1 ? white : muted);
        drawText(scene, sponsorBlockText, menuFont, 1400, settingsRowY, black,
                 cursor == 2 ? white : muted);

        int selectedCount = 0;
        for (int i = 0; i < CATEGORY_COUNT; ++i)
            if (selected[i]) ++selectedCount;
        char countText[32];
        snprintf(countText, sizeof(countText), "%d / %d", selectedCount, CATEGORY_COUNT);
        // Keep the label fixed and right-align the changing count.
        drawText(scene, "Selected:", menuFont, 1645, 165, black, green);
        drawTextRightAligned(scene, countText, menuFont, 1900, 165, black, green);

        // Show categories in two columns, six per column.
        // Navigation stays linear: down the first column, then the second.
        for (int i = 0; i < CATEGORY_COUNT; ++i)
        {
            const int column = i / 6;
            const int rowIndex = i % 6;
            const int x = column == 0 ? 160 : 960;
            const int highlightX = column == 0 ? 135 : 935;
            const int highlightWidth = column == 0 ? 760 : 850;
            const int y = 400 + rowIndex * 64;

            if (i + 3 == cursor)
                scene->DrawRectangle(highlightX, y - 32, highlightWidth, 42, accent);

            char row[192];
            snprintf(row, sizeof(row), "%s (%s): %s",
                     categories[i].label, categories[i].id,
                     selected[i] ? "ON" : "OFF");
            drawText(scene, row, categoryFont, x, y, black,
                     i + 3 == cursor ? white : muted);
        }

        // Controller legend.
        const char *legendLabels[] = { "Select", "Select all / Clear all", "Close App", "Install" };
        const char *legendIcons[] = { "cross", "triangle", "circle", "square" };
        const int legendTextGap = 8;
        const int legendIconWidth = 28;
        int legendGroupWidths[4];
        int totalLegendWidth = 0;

        for (int i = 0; i < 4; ++i)
        {
            legendGroupWidths[i] = legendIconWidth + legendTextGap + getTextWidth(legendLabels[i], legendFont);
            totalLegendWidth += legendGroupWidths[i];
        }

        // Keep the gaps equal; outer margins are half the gap.
        const int legendGap = (FRAME_WIDTH - totalLegendWidth) / 4;
        const int legendOuterMargin = legendGap / 2;
        int legendX = legendOuterMargin;
        for (int i = 0; i < 4; ++i)
        {
            drawControllerIcon(scene, legendIcons[i], legendX, 1038);
            drawText(scene, legendLabels[i], legendFont, legendX + legendIconWidth + legendTextGap,
                     1060, black, muted);
            legendX += legendGroupWidths[i] + legendGap;
        }

        if (status[0] != '\0')
            drawText(scene, status, menuFont, 150, 955, black, statusIsError ? red : green);

        if (exitRequested)
        {
            playClosingAnimation(scene, &frameID, black);
            sceSystemServiceLoadExec("exit", NULL);
            return 0;
        }

        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);
        scene->FrameBufferSwap();
        ++frameID;
    }
    return 0;
}
