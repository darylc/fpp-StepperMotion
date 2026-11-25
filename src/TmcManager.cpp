#include "fpp-pch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "CurlManager.h"
#include "FileMonitor.h"
#include "common.h"
#include "log.h"
#include "Warnings.h"
#include "settings.h"
#include "overlays/PixelOverlay.h"
#include "overlays/PixelOverlayModel.h"

#include "TmcUtil.h"
#include "TmcManager.h"

namespace tmc {


TmcManager& TmcManager::instance() {
    static TmcManager inst;
    return inst;
}

std::string TmcManager::dataDir() const {
    return FPP_DIR_MEDIA(std::string("/plugindata/") + PLUGIN_NAME);
}

// Anything that would require rebuilding an axis or the channel output. These
// only take effect at fppd startup, so the UI has to tell the user a restart
// is needed rather than silently doing half the change.
bool TmcManager::structurallyDifferent(const PluginConfig& a,
                                       const PluginConfig& b) const {
    if (a.axes.size() != b.axes.size() || !(a.channelBlock == b.channelBlock))
        return true;
    for (size_t i = 0; i < a.axes.size(); i++) {
        const AxisConfig& x = a.axes[i];
        const AxisConfig& y = b.axes[i];
        if (x.name != y.name || x.enabled != y.enabled || !(x.channel == y.channel) ||
            x.driver != y.driver || x.spi.bus != y.spi.bus || x.spi.cs != y.spi.cs ||
            x.spi.speedHz != y.spi.speedHz || x.spi.fake != y.spi.fake)
            return true;
    }
    return false;
}

void TmcManager::applyConfig(const PluginConfig& cfg) {
    std::lock_guard<std::mutex> lk(m_lock);

    if (m_started && structurallyDifferent(m_config, cfg)) {
        // Deliberately not applied live. Swapping a chip select or driver type
        // under a running axis means closing a chip select mid-move, and the
        // channel output's range is fixed until fppd rebuilds it anyway.
        // structuralChangePending() reports it.
        LogInfo(VB_PLUGIN,
                "%s: stepper structure changed; an fppd restart is needed before it "
                "takes effect\n",
                LOGPFX);
        // Still take the tunables from the new config for the axes that match.
    }

    m_config.idleTimeoutMs = cfg.idleTimeoutMs;
    if (!m_started) {
        m_config = cfg;
        return;
    }

    for (const auto& ac : cfg.axes) {
        for (auto& w : m_axes) {
            if (w->name() == ac.name) {
                w->applyTunables(ac, cfg.idleTimeoutMs);
                break;
            }
        }
    }
    // Keep the stored copy in step so the next comparison is against what we
    // actually have.
    for (auto& stored : m_config.axes) {
        for (const auto& ac : cfg.axes) {
            if (stored.name == ac.name)
                copyTunables(ac, stored);
        }
    }
}

std::string TmcManager::configFilePath() const {
    return FPP_DIR_CONFIG(std::string("/plugin.") + PLUGIN_NAME + ".json");
}

// The revision is the file's own content, hashed: the same file means the
// same revision across fppd restarts, and any writer -- a save here, FPP's
// backup restore, the File Manager, an editor -- changes it. 53 bits so the
// page's JavaScript holds it exactly. 0 is "no file".
// GetFileContents() cannot tell an empty file from a missing one.
static bool readWholeFile(const std::string& path, std::string& bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    bytes = ss.str();
    return true;
}

static int64_t contentRevision(const std::string& bytes) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h &= (1ULL << 53) - 1;
    return h ? (int64_t)h : 1;
}

void TmcManager::reloadConfigFile() {
    std::lock_guard<std::mutex> slk(m_saveLock);
    m_fileChanged = false;
    loadConfigLocked(false);
}

// Picks up a configuration file written by something other than this
// plugin, before anything reads or saves the saved configuration: so the
// page shows what is really on disk, and a save made against what fppd
// held before cannot overwrite a restore. force reads the disk even
// without a change notice (the saves, which must never miss one).
void TmcManager::refreshFromDiskLocked(bool force) {
    if (!m_fileChanged.exchange(false) && !force)
        return;
    std::string bytes;
    const std::string path = configFilePath();
    const int64_t rev = readWholeFile(path, bytes) ? contentRevision(bytes) : 0;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (rev == m_revision)
            return; // our own write, or nothing changed
    }
    LogInfo(VB_PLUGIN, "%s: %s was changed outside the plugin; reloading it\n", LOGPFX,
            path.c_str());
    loadConfigLocked(true);
}

// m_saveLock held. external: fppd is running (a restore or a hand edit):
// the structural part waits for a restart, so FPP is told one is needed.
void TmcManager::loadConfigLocked(bool external) {
    std::string path = configFilePath();

    PluginConfig cfg;
    std::vector<std::string> errors;

    std::string bytes;
    if (!readWholeFile(path, bytes)) {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            m_revision = 0;
        }
        if (external) {
            // Deleted while running: keep what is running; a save writes it again.
            LogWarn(VB_PLUGIN, "%s: %s was removed; the running configuration stays "
                               "until the next save\n", LOGPFX, path.c_str());
            return;
        }
        // Not commissioned yet. A plugin with no steppers is a valid, quiet state,
        // not an error -- the setup page is where axes get created.
        LogInfo(VB_PLUGIN, "%s: no configuration at %s yet\n", LOGPFX,
                path.c_str());
        applyConfig(cfg);
        return;
    }
    const int64_t rev = contentRevision(bytes);

    Json::Value root;
    if (!LoadJsonFromString(bytes, root)) {
        std::lock_guard<std::mutex> lk(m_lock);
        // Adopted, so a save from the page (which shows the error) may
        // replace the broken file.
        m_revision = rev;
        m_configErrors = { path + " is not valid JSON" };
        LogErr(VB_PLUGIN, "%s: %s is not valid JSON; keeping the previous "
                              "configuration\n",
               LOGPFX, path.c_str());
        return;
    }

    if (!parsePluginConfig(root, cfg, errors)) {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            m_revision = rev;
            m_configErrors = errors;
        }
        for (const auto& e : errors)
            LogErr(VB_PLUGIN, "%s: config: %s\n", LOGPFX, e.c_str());
        // The previous configuration stays in force. Refusing a bad config
        // outright is better than half-applying one to something with a motor
        // attached.
        return;
    }

    {
        // Non-fatal messages (a preset scoped to a missing stepper, a bad
        // preset dropped) still reach the setup page.
        std::lock_guard<std::mutex> lk(m_lock);
        m_configErrors = errors;
        m_saved = cfg;
        m_revision = rev;
    }
    for (const auto& e : errors)
        LogWarn(VB_PLUGIN, "%s: config: %s\n", LOGPFX, e.c_str());
    applyConfig(cfg);
    if (external) {
        syncOutputEntry(false);
        if (structuralChangePending())
            setSetting("restartFlag", "1", true);
    }
}

static bool staleRevision(int64_t expect, int64_t now, std::vector<std::string>& errors,
                          bool* stale) {
    if (expect < 0 || expect == now)
        return false;
    errors.push_back("the configuration was changed elsewhere since this page loaded it "
                     "(another browser tab, or Set Home storing a homing direction); "
                     "reload the page and make the change again");
    if (stale)
        *stale = true;
    return true;
}

bool TmcManager::saveConfig(const Json::Value& root, std::vector<std::string>& errors,
                            bool& restartNeeded, int64_t expectRevision, bool* stale,
                            int64_t* revisionOut) {
    std::lock_guard<std::mutex> slk(m_saveLock);
    restartNeeded = false;
    refreshFromDiskLocked(true);
    if (staleRevision(expectRevision, configRevision(), errors, stale))
        return false;
    if (!saveConfigLocked(root, errors, restartNeeded, true))
        return false;
    if (revisionOut)
        *revisionOut = configRevision();
    return true;
}

bool TmcManager::saveConfigLocked(const Json::Value& root, std::vector<std::string>& errors,
                                  bool& restartNeeded, bool mayAddEntry) {
    restartNeeded = false;
    PluginConfig cfg;
    // Refused on ANY message, not only the fatal ones: a preset the daemon
    // would drop on load is one the user just typed, and they should hear
    // now, with the form still on screen.
    parsePluginConfig(root, cfg, errors);
    if (!errors.empty())
        return false;

    // Atomic replace, as TmcAxis::saveReference does: the file is re-read by
    // reloadConfigFile() and a torn read would look like a corrupt config.
    const std::string path = configFilePath();
    const std::string tmp = path + ".tmp";
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "    ";
    wb["precision"] = 15; // 0.15, not 0.14999999999999999
    const std::string bytes = Json::writeString(wb, cfg.toJson());
    if (!PutFileContents(tmp, bytes) ||
        rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        errors.push_back("could not write " + path);
        return false;
    }
    // fppd runs as root; FPP's backup, restore and file manager run as fpp.
    SetFilePerms(path);

    std::vector<std::pair<std::string, std::string>> renames;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        restartNeeded = m_started && structurallyDifferent(m_config, cfg);
        renames = renamedSteppers(m_saved, cfg);
        m_pendingRenames.insert(m_pendingRenames.end(), renames.begin(), renames.end());
        m_saved = cfg;
        m_revision = contentRevision(bytes);
        m_configErrors.clear();
    }
    // Copied now so the new name has the reference even if fppd never gets
    // to stopAxes() (a crash, a power cut); moved there, with the old-named
    // worker's last update, on a normal restart.
    for (const auto& r : renames)
        carryReference(r.first, r.second, true);
    applyConfig(cfg);
    // FPP's output entry follows the block just saved. Put back only by a
    // save from the page, never by a homing direction fppd stores itself.
    syncOutputEntry(mayAddEntry);
    if (restartNeeded) {
        // Channel outputs and axes are only built during fppd startup: set
        // the (persisted) flag and let FPP sequence the restart; never
        // restart fppd from here.
        setSetting("restartFlag", "1", true);
    }
    LogInfo(VB_PLUGIN, "%s: configuration saved: %d steppers, %d presets%s\n", LOGPFX,
            (int)cfg.axes.size(), (int)cfg.presets.size(),
            restartNeeded ? " (fppd restart required)" : "");
    return true;
}

bool TmcManager::savePresets(const Json::Value& presets, std::vector<std::string>& errors,
                             int64_t expectRevision, bool* stale, int64_t* revisionOut) {
    std::lock_guard<std::mutex> slk(m_saveLock);
    refreshFromDiskLocked(true);
    if (staleRevision(expectRevision, configRevision(), errors, stale))
        return false;
    Json::Value root;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        root = m_saved.toJson();
    }
    root["presets"] = presets;
    bool restartNeeded = false;
    if (!saveConfigLocked(root, errors, restartNeeded, true))
        return false;
    if (revisionOut)
        *revisionOut = configRevision();
    return true;
}

Json::Value TmcManager::configSnapshot(int64_t& revision) {
    std::lock_guard<std::mutex> slk(m_saveLock);
    refreshFromDiskLocked(false);
    std::lock_guard<std::mutex> lk(m_lock);
    revision = m_revision;
    return m_saved.toJson();
}

int64_t TmcManager::configRevision() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_revision;
}

Json::Value TmcManager::configJson() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_saved.toJson();
}

Json::Value TmcManager::defaultsEffectiveJson() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_saved.defaultsEffectiveJson();
}

Json::Value TmcManager::axesEffectiveJson() {
    std::lock_guard<std::mutex> lk(m_lock);
    Json::Value v(Json::arrayValue);
    for (const auto& ac : m_saved.axes)
        v.append(ac.effectiveJson());
    return v;
}

std::vector<std::string> TmcManager::configErrors() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_configErrors;
}

// Worked out each time rather than latched, so saving a structural change
// and then saving it back again clears it.
bool TmcManager::structuralChangePending() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_started && structurallyDifferent(m_config, m_saved);
}

Json::Value TmcManager::spiBusesJson() {
    Json::Value buses(Json::arrayValue);
    for (const SpiBusInfo& b : spiBuses()) {
        Json::Value v;
        v["bus"] = b.bus;
        v["path"] = "/dev/spidev" + std::to_string(b.spidev) + ".0";
        v["pins"]["miso"] = b.miso;
        v["pins"]["mosi"] = b.mosi;
        v["pins"]["sclk"] = b.sclk;
        v["available"] = spiBusAvailable(b);
        buses.append(v);
    }
    return buses;
}

void TmcManager::startAxes() {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_started)
        return;

    // Recreated as root if something removed it: hand it back to fpp, as the
    // installer made it.
    if (mkdir(dataDir().c_str(), 0775) == 0)
        SetFilePerms(dataDir(), true);

    int disabled = 0;
    for (const auto& ac : m_config.axes) {
        if (!ac.enabled) {
            // No worker: nothing opens its chip select or talks to its chip.
            LogInfo(VB_PLUGIN, "%s: stepper '%s' is disabled in its configuration; "
                                   "not started\n",
                    LOGPFX, ac.name.c_str());
            disabled++;
            continue;
        }
        auto w = std::make_unique<AxisWorker>(ac, dataDir(), m_config.idleTimeoutMs);
        const std::string name = ac.name;
        w->setHomingDirectionListener([this, name](bool ccw) {
            return saveHomingDirection(name, ccw);
        });
        std::string err;
        if (!w->start(err)) {
            LogErr(VB_PLUGIN, "%s: stepper '%s' failed to start: %s\n", LOGPFX,
                   ac.name.c_str(), err.c_str());
            WarningHolder::AddWarning("Stepper Motion '" + ac.name +
                                      "' failed to start: " + err);
            continue;
        }
        m_axes.push_back(std::move(w));
    }
    m_started = true;
    resolveChannelsLocked();
    LogInfo(VB_PLUGIN, "%s: %d of %d steppers running (%d disabled)\n", LOGPFX,
            (int)m_axes.size(), (int)m_config.axes.size() - disabled, disabled);
}

// Resolved here rather than at parse time so it can be redone whenever it
// might have changed, and never written back to config.json (see
// ChannelSource).
void TmcManager::resolveChannels() {
    std::lock_guard<std::mutex> lk(m_lock);
    resolveChannelsLocked();
}

void TmcManager::resolveChannelsLocked() {
    m_resolved.assign(m_axes.size(), 0);
    m_channelErrors.clear();

    // The block first: every slot is counted from its start.
    const ChannelBlock& blk = m_config.channelBlock;
    m_blockStart = 0;
    if (blk.source == ChannelBlock::Source::Model) {
        PixelOverlayModel* m = PixelOverlayManager::INSTANCE.getModel(blk.model);
        if (!m) {
            m_channelErrors.push_back(
                "the channel block's pixel overlay model '" + blk.model + "' does not exist. "
                "Steppers on a slot will not follow any channel until it does, or the block "
                "is pointed somewhere else.");
        } else {
            m_blockStart = m->getStartChannel() + 1; // 0-based in FPP; 1-based here
            if (m->getChannelCount() < CHANNEL_BLOCK_SIZE)
                m_channelErrors.push_back(
                    "the channel block's model '" + blk.model + "' has " +
                    std::to_string(m->getChannelCount()) + " channels; slots past that read "
                    "whatever follows it in the show. Make it " +
                    std::to_string(CHANNEL_BLOCK_SIZE) + " channels.");
        }
    } else {
        m_blockStart = blk.start;
    }

    for (size_t i = 0; i < m_axes.size(); i++) {
        const AxisConfig ac = m_axes[i]->config();
        switch (ac.channel.mode) {
        case ChannelSource::Mode::Slot:
            if (m_blockStart > 0 && ac.channel.slot >= 1)
                m_resolved[i] = m_blockStart + ac.channel.slot - 1;
            break;
        case ChannelSource::Mode::Absolute:
            m_resolved[i] = ac.channel.number;
            break;
        case ChannelSource::Mode::Model: {
            PixelOverlayModel* m =
                PixelOverlayManager::INSTANCE.getModel(ac.channel.model);
            if (!m) {
                m_channelErrors.push_back(
                    "stepper '" + ac.name + "': there is no pixel overlay model '" +
                    ac.channel.model + "'. It will not follow any channel until "
                    "the model exists or the stepper is pointed somewhere else.");
                break;
            }
            // getStartChannel() is 0-based; everything the user sees is 1-based.
            const int first = m->getStartChannel() + 1;
            if (ac.channel.offset >= m->getChannelCount()) {
                m_channelErrors.push_back(
                    "stepper '" + ac.name + "': offset " +
                    std::to_string(ac.channel.offset) + " is past the end of model '" +
                    ac.channel.model + "' (" + std::to_string(m->getChannelCount()) +
                    " channels).");
                break;
            }
            m_resolved[i] = first + ac.channel.offset;
            break;
        }
        }
        if (m_resolved[i] > 0) {
            LogInfo(VB_PLUGIN, "%s: stepper '%s' reads channel %d (%s)\n", LOGPFX,
                    ac.name.c_str(), m_resolved[i], ac.channel.describe().c_str());
        }
    }
    for (const auto& e : m_channelErrors)
        LogWarn(VB_PLUGIN, "%s: %s\n", LOGPFX, e.c_str());
}

// The plugin's name, and so its channel output's type in co-other.json (see
// TmcPlugin.cpp): also the owner tag on its HTTP requests, which is what FPP
// cancels when it shuts the plugin down.
static const char* CO_OTHER_URL = "http://127.0.0.1/api/channel/output/co-other";

// The first channel (1-based) of a block, 0 while its model is not loaded.
static int blockStartOf(const ChannelBlock& blk) {
    if (blk.source != ChannelBlock::Source::Model)
        return blk.start;
    PixelOverlayModel* m = PixelOverlayManager::INSTANCE.getModel(blk.model);
    return m ? m->getStartChannel() + 1 : 0; // 0-based in FPP
}

void TmcManager::syncOutputEntry(bool mayAdd) {
    ChannelBlock blk;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        blk = m_saved.channelBlock;
        // Nothing to feed yet: a fresh install puts nothing on the user's
        // Channel Outputs page until a stepper is saved.
        if (m_saved.axes.empty())
            mayAdd = false;
    }
    const int start = blockStartOf(blk);
    // This box's own FPP API, over 127.0.0.1: nothing leaves the device.
    CurlManager::INSTANCE.addGet(CO_OTHER_URL, [start, mayAdd](int rc, const std::string& resp) {
        // A box that has never had an "other" output has no co-other.json,
        // and FPP answers HTTP 404 with {"status": "ERROR: File not found"}:
        // nothing to keep. Anything else but a 200 is a real failure.
        Json::Value d;
        if ((rc != 200 && rc != 404) || !LoadJsonFromString(resp, d) || !d.isObject()) {
            LogWarn(VB_PLUGIN, "%s: could not read FPP's channel outputs (HTTP %d); output entry not checked\n",
                    LOGPFX, rc);
            return;
        }
        Json::Value outs = d["channelOutputs"];
        if (!outs.isArray()) {
            if (d["status"].asString().find("not found") == std::string::npos) {
                LogWarn(VB_PLUGIN, "%s: unexpected answer from FPP's channel outputs (%s); output entry not checked\n",
                        LOGPFX, d["status"].asString().c_str());
                return;
            }
            outs = Json::Value(Json::arrayValue);
        }
        int at = -1;
        for (Json::ArrayIndex i = 0; i < outs.size(); i++) {
            if (outs[i].isObject() && outs[i]["type"].asString() == PLUGIN_NAME) {
                at = (int)i;
                break;
            }
        }
        if (at < 0 && !mayAdd)
            return;
        Json::Value want;
        if (at >= 0) {
            want = outs[at];
        } else {
            want["type"] = PLUGIN_NAME;
            want["enabled"] = 1;
        }
        // A block on a model that is not loaded keeps the entry's start (1
        // for a new one): the count is known regardless.
        if (start > 0 || at < 0)
            want["startChannel"] = start > 0 ? start : 1;
        want["channelCount"] = CHANNEL_BLOCK_SIZE;
        if (at >= 0 && want == outs[at])
            return; // already right
        if (at >= 0)
            outs[at] = want;
        else
            outs.append(want);
        Json::Value doc;
        doc["channelOutputs"] = outs;
        LogInfo(VB_PLUGIN, "%s: %s FPP's channel output entry: channels %d-%d\n", LOGPFX,
                at >= 0 ? "updating" : "adding", want["startChannel"].asInt(),
                want["startChannel"].asInt() + CHANNEL_BLOCK_SIZE - 1);
        // 127.0.0.1 again: this box's own FPP API.
        CurlManager::INSTANCE.addPost(CO_OTHER_URL, SaveJsonToString(doc), "application/json",
            [](int rc, const std::string&) {
                if (rc != 200)
                    LogWarn(VB_PLUGIN, "%s: could not write FPP's channel output entry (HTTP %d)\n",
                            LOGPFX, rc);
            }, PLUGIN_NAME);
    }, PLUGIN_NAME);
}

void TmcManager::watchOutputEntry() {
    // A write of our own comes back here too, finds the entry right and
    // stops. Never re-adds a removed entry (see syncOutputEntry()).
    FileMonitor::INSTANCE.AddFile(PLUGIN_NAME, FPP_DIR_CONFIG("/co-other.json"),
                                  []() { TmcManager::instance().syncOutputEntry(false); });
    // The configuration file: only noted here (this runs on fppd's main
    // loop, which must never wait on a save); read on the next API call.
    FileMonitor::INSTANCE.AddFile(PLUGIN_NAME, configFilePath(),
                                  []() { TmcManager::instance().m_fileChanged = true; });
}

void TmcManager::unwatchOutputEntry() {
    FileMonitor::INSTANCE.RemoveFile(PLUGIN_NAME, FPP_DIR_CONFIG("/co-other.json"));
    FileMonitor::INSTANCE.RemoveFile(PLUGIN_NAME, configFilePath());
}

Json::Value TmcManager::channelBlockJson() {
    std::lock_guard<std::mutex> lk(m_lock);
    const ChannelBlock& blk = m_config.channelBlock;
    Json::Value v;
    v["source"] = blk.source == ChannelBlock::Source::Model ? "model" : "channel";
    if (blk.source == ChannelBlock::Source::Model)
        v["model"] = blk.model;
    else
        v["start"] = blk.start;
    v["resolvedStart"] = m_blockStart;
    v["size"] = CHANNEL_BLOCK_SIZE;
    return v;
}

std::vector<int> TmcManager::resolvedChannels() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_resolved;
}

void TmcManager::stopAxes() {
    // Stopped outside m_lock: a worker finishing Set Home saves through
    // saveHomingDirection(), which takes m_lock, and stop() joins it.
    std::vector<std::unique_ptr<AxisWorker>> axes;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        axes.swap(m_axes);
        m_started = false;
    }
    for (auto& w : axes)
        w->requestStop();
    for (auto& w : axes)
        w->stop();

    // Every worker has written its last reference under its old name; carry
    // each saved rename across, in the order the saves made them.
    std::vector<std::pair<std::string, std::string>> renames;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        renames.swap(m_pendingRenames);
    }
    for (const auto& r : renames)
        carryReference(r.first, r.second, false);
}

// A stepper is taken as renamed when it holds the same place in the list,
// on the same hardware (driver, bus and chip select, or both simulated), and
// its old name is gone from the new list while its new name was not in the
// old one -- so removing a stepper, reordering the list or swapping two names
// never hands one stepper's reference to another.
std::vector<std::pair<std::string, std::string>>
TmcManager::renamedSteppers(const PluginConfig& before, const PluginConfig& after) {
    auto has = [](const PluginConfig& c, const std::string& name) {
        for (const auto& a : c.axes)
            if (a.name == name)
                return true;
        return false;
    };
    std::vector<std::pair<std::string, std::string>> out;
    const size_t n = std::min(before.axes.size(), after.axes.size());
    for (size_t i = 0; i < n; i++) {
        const AxisConfig& x = before.axes[i];
        const AxisConfig& y = after.axes[i];
        if (x.name == y.name || x.driver != y.driver || x.spi.fake != y.spi.fake)
            continue;
        if (!x.spi.fake && (x.spi.bus != y.spi.bus || x.spi.cs != y.spi.cs))
            continue;
        if (has(after, x.name) || has(before, y.name))
            continue;
        out.emplace_back(x.name, y.name);
    }
    return out;
}

// The reference file of `from`, copied (keepSource) or moved to `to`'s,
// replacing whatever `to` had: the file belongs to the hardware, and this is
// the same hardware under a new name. Nothing to do if `from` has none.
void TmcManager::carryReference(const std::string& from, const std::string& to,
                                bool keepSource) {
    const std::string src = TmcAxis::statePathFor(dataDir(), from);
    const std::string dst = TmcAxis::statePathFor(dataDir(), to);
    if (src == dst || !FileExists(src))
        return;
    bool ok;
    if (keepSource) {
        const std::string tmp = dst + ".tmp";
        std::string body = GetFileContents(src);
        ok = !body.empty() && PutFileContents(tmp, body) && rename(tmp.c_str(), dst.c_str()) == 0;
        if (!ok)
            unlink(tmp.c_str());
    } else {
        ok = rename(src.c_str(), dst.c_str()) == 0;
    }
    if (ok)
        LogInfo(VB_PLUGIN, "%s: stepper '%s' renamed to '%s': position reference %s\n",
                LOGPFX, from.c_str(), to.c_str(), keepSource ? "copied" : "moved");
    else
        LogWarn(VB_PLUGIN, "%s: stepper '%s' renamed to '%s': could not carry the "
                               "position reference from %s; it will need a rehome\n",
                LOGPFX, from.c_str(), to.c_str(), src.c_str());
}

Json::Value TmcManager::saveHomingDirection(const std::string& name, bool ccw) {
    // Read-modify-write of the saved configuration: held across the save so
    // a page save cannot land in between and be overwritten. A restored
    // file is picked up first, so this edits the restore, not what it
    // replaced.
    std::lock_guard<std::mutex> slk(m_saveLock);
    refreshFromDiskLocked(true);
    Json::Value root;
    Json::Value result; // null: nothing saved
    const int64_t before = configRevision();
    {
        std::lock_guard<std::mutex> lk(m_lock);
        PluginConfig cfg = m_saved;
        bool found = false;
        for (auto& ac : cfg.axes) {
            if (ac.name == name) {
                ac.rehome.ccw = ccw;
                found = true;
            }
        }
        if (!found)
            return result; // removed from the saved configuration since it started
        root = cfg.toJson();
    }
    std::vector<std::string> errors;
    bool restartNeeded = false;
    if (saveConfigLocked(root, errors, restartNeeded, false)) {
        LogInfo(VB_PLUGIN, "%s: %s: homing direction saved as %s\n", LOGPFX,
                name.c_str(), ccw ? "ccw" : "cw");
        // The page adopts the new revision only if it held the old one.
        result["revisionBefore"] = (Json::Int64)before;
        result["revision"] = (Json::Int64)configRevision();
    } else {
        for (const auto& e : errors)
            LogWarn(VB_PLUGIN, "%s: %s: could not save the homing direction: %s\n",
                    LOGPFX, name.c_str(), e.c_str());
    }
    return result;
}

// Called on the fppd output thread once per frame. Nothing here allocates,
// locks, or touches SPI: it stores two atomics per axis and returns.
void TmcManager::distribute(const unsigned char* data, int dataStartChannel,
                            int count) {
    int64_t now = monotonicMs();
    // Reading m_axes unlocked is safe by ordering: fppd's teardown calls
    // CloseChannelOutputs() before PluginManager::Cleanup(), so the output
    // thread is stopped before stopAxes() clears the vector. This thread
    // must not take m_lock.
    for (size_t i = 0; i < m_axes.size(); i++) {
        // m_resolved is likewise sized and written by resolveChannels() before
        // the output starts and never resized afterwards.
        const int resolved = i < m_resolved.size() ? m_resolved[i] : 0;
        if (resolved > 0) {
            // Bounded by the channel buffer, not this output's range: fppd
            // hands SendData() a pointer into its whole channel buffer at the
            // output's start, and the steppers read wherever they were pointed
            // (declared through GetRequiredChannelRanges()) -- a stepper on an
            // override may sit before the block. So index from the buffer's
            // start.
            if (resolved > MAX_CHANNEL_NUMBER)
                continue;
            m_axes[i]->setChannelValue((data - dataStartChannel)[resolved - 1]);
            m_axes[i]->noteFrame(now);
        }
        // Unresolved (its model is missing): the axis sees no frames and
        // stays where it is; resolveChannels() has already reported it.
    }
}

void TmcManager::showStarting() {
    for (auto& w : m_axes)
        w->showStarting();
}

void TmcManager::showStopping() {
    for (auto& w : m_axes)
        w->showStopping();
}

int TmcManager::axisCount() {
    std::lock_guard<std::mutex> lk(m_lock);
    return (int)m_axes.size();
}

AxisWorker* TmcManager::axis(int index) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (index < 0 || index >= (int)m_axes.size())
        return nullptr;
    return m_axes[index].get();
}

AxisWorker* TmcManager::axisByName(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto& w : m_axes) {
        if (w->name() == name)
            return w.get();
    }
    return nullptr;
}

Json::Value TmcManager::statusJson() {
    Json::Value v;
    v["structuralChangePending"] = structuralChangePending();
    v["configRevision"] = (Json::Int64)configRevision();
    {
        std::lock_guard<std::mutex> lk(m_lock);
        for (const auto& e : m_configErrors)
            v["configErrors"].append(e);
        for (const auto& e : m_channelErrors)
            v["configErrors"].append(e);
        v["idleTimeoutMs"] = m_config.idleTimeoutMs;
    }
    v["channelBlock"] = channelBlockJson();
    const std::vector<int> resolved = resolvedChannels(); // a copy, under the lock
    int n = axisCount();
    v["axisCount"] = n;
    for (int i = 0; i < n; i++) {
        AxisWorker* w = axis(i);
        if (!w)
            continue;
        const AxisConfig cfg = w->config();
        Json::Value a = snapshotToJson(w->snapshot(), cfg);
        a["index"] = i;
        // Both a readable description and the structured source, so the
        // setup page can tell what fppd is running from what is in the file.
        const ChannelSource& cs = cfg.channel;
        a["channelSource"] = cs.describe();
        a["resolvedChannel"] = (i < (int)resolved.size()) ? resolved[i] : 0;
        a["channelMode"] = ChannelSource::modeName(cs.mode);
        a["channelSlot"] = cs.slot;
        a["channelNumber"] = cs.number;
        a["channelModel"] = cs.model;
        a["channelModelOffset"] = cs.offset;
        a["job"] = jobToJson(w->lastJob());
        v["axes"].append(a);
    }
    return v;
}

} // namespace tmc
