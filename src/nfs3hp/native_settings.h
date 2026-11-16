#ifndef NFS3HP_NATIVE_SETTINGS_H_
#define NFS3HP_NATIVE_SETTINGS_H_

#include <string>

namespace nfs3hp
{

/* Port: the NFS_* switches as a file, `nfs3hp.ini` beside the game data.
 *
 * Every switch the port reads, it reads from the environment (SDL_getenv, once,
 * on its first use); this file is put into the environment before the game
 * starts, so a switch can be set for good rather than on a command line, and
 * the port's own menus write it back.  A variable already in the environment
 * wins over the file -- launch scripts and `adb shell am start --ez ...` still
 * have the last word (native_settings.cpp; TECHNICAL.md's Environment
 * variables). */

/* Read `<folder>nfs3hp.ini` and set every switch it names, then remember where
 * it was.  A file that is not there yet is written out with every switch in it,
 * commented out at the value the port gives it -- so the defaults have one
 * place to look at and one to change.  Called once, at startup. */
void settingsLoad(const std::string& folder);

/* The value of a switch: what the file set it to, or `otherwise` for none. */
std::string settingsGet(const std::string& key, const std::string& otherwise = std::string());

/* Set a switch now, in the environment, and in the file as it stands -- after
 * which settingsSave writes the file.  A menu changes a switch this way; one
 * the file has commented out is uncommented where it is, among its own group's. */
void settingsSet(const std::string& key, const std::string& value);

/* Write the file back as it stands: what settingsSet has changed since.  False
 * where it could not be written (a read-only install). */
bool settingsSave();

/* Where the switches were read from, folder and trailing '/' and all; empty
 * before settingsLoad. */
const std::string& settingsFile();

}  // namespace nfs3hp

#endif
