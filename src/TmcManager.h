#pragma once
/*
 * TmcManager owns the axis workers.
 *
 * The manager, not the channel output, owns the axes: commissioning has to
 * work before any channel output is configured, so the axes come up with the
 * plugin and the channel output (TmcOutput.h) just feeds them values.
 */

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "fpp-json.h"

#include "AxisConfig.h"
#include "AxisWorker.h"

namespace tmc {

class TmcManager {
public:
    static TmcManager& instance();

    // Applies a new configuration. Tunables (currents, speeds, presets, rehome
    // staging) are pushed to running axes live. Structural changes -- the axis
    // list, SPI buses and chip selects, driver types -- are reported through
    // structuralChangePending() instead, because the channel output they feed
    // is only built during fppd startup.
    void applyConfig(const PluginConfig& cfg);
    // Reads and applies configFilePath(), when the plugin loads. Later
    // writes by anything else (FPP's backup restore, the File Manager, an
    // editor) are picked up by the next API call (see watchOutputEntry()).
    void reloadConfigFile();

    // The setup page's save. Validates root with the full parser and refuses
    // with the complete list if anything is wrong (nothing written).
    // Otherwise writes config.json atomically, applies the tunables live,
    // and if the change is structural sets FPP's restartFlag and reports
    // restartNeeded. expectRevision >= 0 refuses the save (stale = true)
    // when the file has changed since the page read that revision: another
    // tab, a Set Home that stored a homing direction, a restore. Saves are
    // serialised, so no two can interleave. revisionOut: the new revision,
    // read under the same lock. HTTP thread.
    bool saveConfig(const Json::Value& root, std::vector<std::string>& errors,
                    bool& restartNeeded, int64_t expectRevision = -1,
                    bool* stale = nullptr, int64_t* revisionOut = nullptr);
    // Saves one stepper's homing direction (rehome.dir) into the saved
    // configuration: what Set Home found the approach to be, so a rehome
    // after the live reference is lost goes to the same stop. Worker thread.
    // Returns {revisionBefore, revision} when it saved, else null.
    Json::Value saveHomingDirection(const std::string& name, bool ccw);
    // Replaces only the preset table of the saved configuration and saves.
    // The steppers come from what the daemon holds, never from the browser,
    // so a preset save cannot smuggle in a half-edited stepper change.
    bool savePresets(const Json::Value& presets, std::vector<std::string>& errors,
                     int64_t expectRevision = -1, bool* stale = nullptr,
                     int64_t* revisionOut = nullptr);
    // A hash of the configuration file's content (0: no file); the page
    // sends back the one it loaded so a stale save is refused rather than
    // winning.
    int64_t configRevision();
    // The saved configuration and its revision, read together after
    // picking up any outside change to the file. HTTP thread.
    Json::Value configSnapshot(int64_t& revision);
    // The saved configuration (what config.json holds, including a structural
    // change waiting on a restart) in file shape, for the setup page.
    Json::Value configJson();
    // The same configuration resolved: the defaults block with every
    // inheritable field filled in, and each stepper with its effective
    // values -- what the setup page shows as placeholders.
    Json::Value defaultsEffectiveJson();
    Json::Value axesEffectiveJson();
    // Non-fatal parse messages from the last load or save, for the page.
    std::vector<std::string> configErrors();
    bool structuralChangePending();

    // The SPI buses a stepper may be on, as [{bus, path, pins: {miso, mosi,
    // sclk}, available}] (spiBuses() in AxisConfig.h): the page's bus
    // choice, the wiring table's data-line columns, and which bus this FPP
    // does not have a device node for.
    static Json::Value spiBusesJson();

    // config/plugin.<name>.json: FPP's backup and restore include every
    // config/plugin.* file, so the wiring, channels and presets travel with
    // a backup. (Not WriteSettingToFile's config/plugin.<name>: that is
    // key = value lines split on every '='.) The per-stepper position files
    // stay in plugindata: they describe this box's hardware, not settings.
    std::string configFilePath() const;

    void startAxes();
    void stopAxes();

    // From the fppd output thread. Allocation- and lock-free. dataStartChannel
    // is the 0-based channel the data pointer begins at (the channel output's
    // own start), so an absolute channel indexes as channel - 1 - that.
    void distribute(const unsigned char* data, int dataStartChannel, int count);

    // Resolves every stepper's channel source to a 1-based channel number,
    // or 0 if it cannot be resolved right now (see ChannelSource).
    void resolveChannels();
    // 1-based channel per running stepper, 0 where unresolved. Index matches
    // axis(i). Safe to call from any thread.
    std::vector<int> resolvedChannels();
    // The running channel block as {source, start, model, resolvedStart,
    // size}, for the setup page (resolvedStart 0 while its model is
    // missing). Safe to call from any thread.
    Json::Value channelBlockJson();

    // Puts FPP's entry for the plugin's channel output (co-other.json, the
    // Channel Outputs page's Misc tab) in step with the saved channel
    // block: start = the block's first channel, count = CHANNEL_BLOCK_SIZE,
    // enabled and any other field kept. Through FPP's own API,
    // asynchronously (CurlManager, finished on fppd's main loop), and only
    // written when something differs. A missing entry is added only when
    // mayAdd and at least one stepper is configured: at plugin start and
    // after a save, never from the file watch -- the uninstaller removes the
    // entry while the plugin is still loaded, and a user may delete it on
    // purpose. Any thread.
    void syncOutputEntry(bool mayAdd);
    // Keeps an existing entry's start and count in step whenever
    // co-other.json changes. unwatchOutputEntry() at shutdown.
    void watchOutputEntry();
    void unwatchOutputEntry();
    void showStarting();
    void showStopping();

    int axisCount();
    AxisWorker* axis(int index);
    AxisWorker* axisByName(const std::string& name);

    Json::Value statusJson();

    std::string dataDir() const;

private:
    TmcManager() {}
    bool saveConfigLocked(const Json::Value& root, std::vector<std::string>& errors,
                          bool& restartNeeded, bool mayAddEntry);
    void loadConfigLocked(bool external);
    void refreshFromDiskLocked(bool force);
    bool structurallyDifferent(const PluginConfig& a, const PluginConfig& b) const;
    void resolveChannelsLocked();

    std::mutex m_lock;
    // Held across a whole save (read, validate, write, adopt), outside
    // m_lock, so two saves can never interleave or undo each other.
    std::mutex m_saveLock;
    int64_t m_revision = 0; // under m_lock
    // Set by the file watch on fppd's main loop; consumed under m_saveLock.
    std::atomic<bool> m_fileChanged { false };
    // What the steppers are running. After a structural save this keeps the
    // old structure (with the new tunables) until fppd restarts.
    PluginConfig m_config;
    // What config.json holds: the last configuration that loaded or saved
    // cleanly. This is what the setup page edits and what savePresets()
    // merges into, so a pending structural change is not lost by either.
    PluginConfig m_saved;
    // Parallel to m_axes: the channel each is reading, 1-based, 0 if its
    // source could not be resolved. Rebuilt by resolveChannels().
    std::vector<int> m_resolved;
    // The running configuration's channel block, resolved: its first
    // channel (1-based), 0 while its model is missing.
    int m_blockStart = 0;
    std::vector<std::string> m_channelErrors;
    std::vector<std::unique_ptr<AxisWorker>> m_axes;

    // Renames saved since fppd started (old name, new name), in save order.
    // A position reference is stored per stepper NAME, so a rename would
    // otherwise cost a rehome: saveConfig() copies the file straight away and
    // stopAxes() moves it once the old-named worker has written its last
    // update. See carryReference().
    std::vector<std::pair<std::string, std::string>> m_pendingRenames;
    static std::vector<std::pair<std::string, std::string>>
    renamedSteppers(const PluginConfig& before, const PluginConfig& after);
    void carryReference(const std::string& from, const std::string& to, bool keepSource);
    std::vector<std::string> m_configErrors;
    bool m_started = false;
};

} // namespace tmc
