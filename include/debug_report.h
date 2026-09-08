/* The per-session debug report: one text file per play session.
 *
 * The audience is two readers at once, and the format is chosen for both.
 *
 * A player who hits a problem should be able to find one file, see plainly
 * whether anything went wrong, and send it on without editing it. An assistant
 * reading that same file should be able to answer "what was running, on what
 * hardware, in what order, and what failed" without asking a single follow-up
 * question -- because the follow-up questions are what a bug report normally
 * dies of.
 *
 * That is why the file is written even when the session is uneventful, why it
 * opens with the build and machine it ran on rather than with the first event,
 * and why every line carries a level, a category and a timestamp: it can be read
 * top to bottom by a person and grepped by a machine.
 *
 * Everything the port and its libraries print is mirrored into the file as it
 * happens, so a report contains the renderer's own messages and not only the
 * ones this port thought to record. Errors and crashes are written straight to
 * the file as well, so they survive a mirror that has stopped pumping.
 */

#ifndef RAYMAN2_DEBUG_REPORT_H
#define RAYMAN2_DEBUG_REPORT_H

#include <filesystem>
#include <string>

namespace rayman2::report {

/* Open this session's report and start mirroring stdout and stderr into it.
 *
 * `preferred_dir` is where the debug-report folder is wanted -- the directory
 * holding the executable, so a player finds it next to the game. `fallback_dir`
 * is used when that is not writable, which is the normal case for an install
 * under Program Files. Safe to call once, early; every other function here is a
 * no-op until it has been.
 */
void begin_session(const std::filesystem::path& preferred_dir,
                   const std::filesystem::path& fallback_dir);

/* Close the session, appending the summary block. `clean` distinguishes
 * reaching the end of main from being killed, which is the first thing anyone
 * reading the file wants to know. */
void end_session(bool clean);

/* The report file, or an empty path if none was opened. */
std::filesystem::path path();

/* Add a key/value line to the header. Call before the first event for it to
 * appear in the header block; afterwards it is recorded as an event instead, so
 * that late-arriving facts (the renderer's device, the loaded ROM) are never
 * silently dropped. */
void add_context(const std::string& key, const std::string& value);

/* Events. `category` is a short lowercase noun -- "audio", "rom", "pak",
 * "input" -- and is what an assistant will group by. */
void info(const char* category, const char* fmt, ...);
void warn(const char* category, const char* fmt, ...);
void error(const char* category, const char* fmt, ...);

/* A crash block. These write directly to the file and to the real console,
 * bypassing the mirror, because a crashing process is exactly when the mirror
 * cannot be relied on. Called from the unhandled-exception filter and the
 * terminate handler in src/crash_report.cpp. */
void crash_begin(const char* kind);
void crash_line(const char* fmt, ...);
void crash_end();

/* How many of each have been recorded so far. */
int error_count();
int crash_count();

} // namespace rayman2::report

#endif /* RAYMAN2_DEBUG_REPORT_H */
